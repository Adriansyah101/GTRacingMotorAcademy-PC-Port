#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "gl.h"
#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_517608(CPU *c);
void __real_F_515980(CPU *c);

static float ext_x0, ext_x1;
static float stage_w;
static int in_anchored;

void __wrap_F_515980(CPU *c)
{
    if (ext_x0 == 0 && ext_x1 == 0) {
        __real_F_515980(c);
        return;
    }
    float r[4], h;
    memcpy(r, GP(c->r[1]), sizeof r);
    memcpy(&h, GP(c->r[0] + 0x34c), 4);
    if (in_anchored)
        c->r[0] = !(r[0] > ext_x1 || r[1] < ext_x0 || r[3] < 0 || r[2] > h);
    else
        c->r[0] = !(r[0] > stage_w || r[1] < 0 || r[3] < 0 || r[2] > h);
}

#define VT_BOUND 0x12cu

static float stage_h;

#define PC_SLOTS 512
typedef struct { u32 pass, ch; float m[6]; u8 has_m; s8 card; } PassEntry;
static PassEntry pc[PC_SLOTS];
static u32 pass_no = 1;
static int pc_on = -1;

static PassEntry *pass_entry(u32 ch)
{
    if (pc_on < 0) {
        const char *e = getenv("GT_WIDE_CACHE");
        pc_on = !(e && *e == '0');
    }
    if (!pc_on || !ch)
        return NULL;
    u32 h = (ch >> 2) * 0x9E3779B1u >> 23 & (PC_SLOTS - 1);
    for (int n = 0; n < 8; n++, h = (h + 1) & (PC_SLOTS - 1)) {
        PassEntry *e = &pc[h];
        if (e->pass != pass_no) {
            e->pass = pass_no, e->ch = ch, e->has_m = 0, e->card = -1;
            return e;
        }
        if (e->ch == ch)
            return e;
    }
    return NULL;
}

static void parent_matrix(u32 parent, float m[6])
{
    PassEntry *e = pass_entry(parent);
    if (e && e->has_m) {
        memcpy(m, e->m, sizeof e->m);
        return;
    }
    static const float id[6] = {1, 0, 0, 0, 1, 0};
    u32 w = parent ? rt_call1(GT_FN_WORLD_MATRIX, parent) : 0;
    memcpy(m, w ? GP(w) : (const void *)id, 6 * sizeof(float));
    if (e) {
        memcpy(e->m, m, sizeof e->m);
        e->has_m = 1;
    }
}

static int world_box(u32 ch, float b[4])
{
    static float rect[4];
    rect[0] = 1, rect[1] = 0;
    rt_call2(rd32(rd32(ch) + VT_BOUND), ch, GA(rect));
    if (!(rect[0] <= rect[1] && rect[2] <= rect[3]))
        return 0;
    float m[6];
    parent_matrix(rd32(ch + 0x40), m);
    b[0] = b[2] = 1e30f, b[1] = b[3] = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = rect[k & 1], v = rect[2 + (k >> 1)];
        float x = m[0] * u + m[1] * v + m[2], y = m[3] * u + m[4] * v + m[5];
        b[0] = x < b[0] ? x : b[0], b[1] = x > b[1] ? x : b[1];
        b[2] = y < b[2] ? y : b[2], b[3] = y > b[3] ? y : b[3];
    }
    return 1;
}

static int off_stage(const float b[4])
{
    float on = (b[1] < stage_w ? b[1] : stage_w) - (b[0] > 0 ? b[0] : 0);
    if (on <= 0)
        return 1;
    return on < 0.03f * stage_w && on < 0.05f * (b[1] - b[0]);
}

static const char *char_name(u32 ch);

enum { KEEP, STRETCH, ZOOM, EXTEND };

static int cover_kind(u32 ch, const float b[4])
{
    float tol = 0.02f * stage_w;
    if (b[0] > tol || b[1] < stage_w - tol)
        return KEEP;
    if (b[0] < -0.1f * stage_w || b[1] > 1.1f * stage_w)
        return KEEP;
    u32 def = rd32(ch + 0xb0), vt = def ? rd32(def) : 0;
    if (vt != 0x78d448u && vt != 0x78da08u)
        return KEEP;
    u32 fills = rd32(def + 0x24), n = rd32(def + 0x28), type = n && fills ? rd32(fills + 4) : 99;
    int picture = type >= 0x40 && type <= 0x43 && n <= 3;
    if (b[2] > tol || b[3] < stage_h - tol) {
        if (!n)
            return KEEP;
        if (b[3] - b[2] > 0.25f * stage_h)
            return picture ? EXTEND : KEEP;
        return n == 1 && (type == 0 || type == 0x10) ? STRETCH : EXTEND;
    }
    if (picture)
        return n == 1 && !strcmp(char_name(rd32(ch + 0x40)), "newBG") ? ZOOM : EXTEND;
    if (n != 1)
        return KEEP;
    if (type == 0)
        return STRETCH;
    return KEEP;
}

static int world_box(u32 ch, float b[4]);
static int event_sides = -1, upgrade_sides = -1;

static int card_off(u32 ch)
{
    u32 p = rd32(ch + 0x40);
    for (int d = 0; p && d < 8; d++, p = rd32(p + 0x40)) {
        const char *n = char_name(p);
        size_t len = strlen(n);
        if (!strstr(n, "File") || n[len - 1] == 's')
            continue;
        if (event_sides < 0) {
            const char *e = getenv("GT_EVENT_SIDES");
            event_sides = !(e && *e == '0');
            e = getenv("GT_UPGRADE_SIDES");
            upgrade_sides = !(e && *e == '0');
        }
        if ((event_sides && !strncmp(n, "EventFile", 9)) || (upgrade_sides && !strncmp(n, "MarketFile", 10)))
            return 0;
        PassEntry *e = pass_entry(p);
        if (e && e->card >= 0)
            return e->card;
        float b[4];
        int off = 0;
        if (world_box(p, b) && b[1] > b[0]) {
            float on = (b[1] < stage_w ? b[1] : stage_w) - (b[0] > 0 ? b[0] : 0);
            off = on < 0.5f * (b[1] - b[0]);
        }
        if (e)
            e->card = (s8)off;
        return off;
    }
    return 0;
}

static const char *char_name(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    return tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
}

static u32 interior_of(u32 ch)
{
    for (int depth = 0; ch && depth < 8; depth++, ch = rd32(ch + 0x40))
        if (!strcmp(char_name(ch), "RenderInterior"))
            return ch;
    return 0;
}

#define INTERIOR_ASPECT 1.5f

static float interior_scale(void)
{
    float s = (ext_x1 - ext_x0) / stage_h / INTERIOR_ASPECT;
    s = s < 1 ? 1 : s > 1.5f ? 1.5f : s;
    static float told;
    if (s != told) {
        told = s;
        rt_log("[wide] cockpit picture enlarged %gx\n", s);
    }
    return s;
}

static int dump_frames[16], ndump, frame_no, dumping;

static void dump(const char *what, u32 ch, const float b[4])
{
    char path[256] = "";
    u32 p = ch;
    for (int d = 0; p && d < 6; d++, p = rd32(p + 0x40)) {
        size_t n = strlen(path);
        snprintf(path + n, sizeof path - n, "%s%s", d ? "<" : "", char_name(p));
    }
    u32 def = rd32(ch + 0xb0), vt = def ? rd32(def) : 0, nf = 0, t = 99;
    if (vt == 0x78d448u || vt == 0x78da08u) {
        nf = rd32(def + 0x28);
        t = nf ? rd32(rd32(def + 0x24) + 4) : 99;
    }
    dev_log("[wdump] %u %s %08x %s box %.0f..%.0f x %.0f..%.0f vt %x fills %u type %u\n", frame_no, what, ch, path,
           b[0] / 20, b[1] / 20, b[2] / 20, b[3] / 20, vt, nf, t);
}

static void extend_sides(CPU *c, void (*real)(CPU *), u32 ch, const float b[4])
{
    float band = -ext_x0, edge = 0.002f * stage_w;
    if (band <= 0)
        return;
    u32 wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
    float keep[6], m[6], k = (band + fabsf(b[0])) / edge;
    memcpy(keep, GP(wm), sizeof keep);
    for (int side = 0; side < 2; side++) {
        float a = side ? b[1] : b[0];
        memcpy(m, keep, sizeof m);
        m[0] *= -k, m[1] *= -k, m[2] = a - (m[2] - a) * k;
        memcpy(GP(wm), m, sizeof m);
        c->r[0] = ch;
        real(c);
    }
    memcpy(GP(wm), keep, sizeof keep);
}

enum { G_HUD, G_NAV, NGROUPS };
#define MAX_PARTS 32
typedef struct { u32 ch; float shift; float box[4]; } Part;
static Part parts[NGROUPS][MAX_PARTS];
static int nparts[NGROUPS], group_frame[NGROUPS] = {-9, -9};
static int hud_on = -1;

static int edges_on(void)
{
    if (hud_on < 0) {
        const char *e = getenv("GT_WIDE_HUD");
        hud_on = ui_wide_ui && !(e && *e == '0');
    }
    return hud_on > 0 && (ext_x0 != 0 || ext_x1 != 0);
}

static void add_part(int g, u32 ch, float shift, const float b[4])
{
    if (nparts[g] >= MAX_PARTS || shift == 0)
        return;
    Part *p = &parts[g][nparts[g]++];
    p->ch = ch, p->shift = shift;
    memcpy(p->box, b, sizeof p->box);
    p->box[0] += shift, p->box[1] += shift;
}

static float hud_shift(u32 ch)
{
    if (hud_on <= 0)
        return 0;
    for (int d = 0; ch && d < 10; d++, ch = rd32(ch + 0x40))
        for (int g = 0; g < NGROUPS; g++)
            if (group_frame[g] + 2 >= frame_no)
                for (int i = 0; i < nparts[g]; i++)
                    if (parts[g][i].ch == ch)
                        return parts[g][i].shift;
    return 0;
}

float wide_draw_shift(u32 ch)
{
    return ext_x0 != 0 || ext_x1 != 0 ? hud_shift(ch) / 20 : 0;
}

static const char *const hud_parts[] = {
    "HUD.MovingElements.btnRenderTrack", "HUD.MovingElements.btnTimeStats", "HUD.MovingElements.btnSpeedOMeter",
    "HUD.MovingElements.Laps", "HUD.MovingElements.btnPosition", "HUD.MovingElements.btnLap",
    "HUD.MovingElements.btnTopTimes", "HUD.MovingElements.Home", "HUD.MovingElements.OpenBigPlayer",
    "HUD.MovingElements.btnOpenSong", "HUD.Gears", "HUD.AccelBrake.btnAccel", "HUD.AccelBrake.btnBrake",
    "HUD.AccelBrake.btnBrakeLeft", "HUD.AccelBrake.btnBrakeRight", "HUD.btnBack", "HUD.btnReset", NULL};

void wide_hud(u32 pl)
{
    if (!edges_on())
        return;
    float band = -ext_x0;
    group_frame[G_HUD] = frame_no, nparts[G_HUD] = 0;
    pass_no++;
    u32 hud = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"HUD"));
    for (int i = 0; hud_parts[i]; i++) {
        u32 ch = swf_find_under(pl, hud, hud_parts[i] + 4);
        float b[4];
        if (!ch || !*(u8 *)GP(ch + 0xab) || !world_box(ch, b) || b[1] <= b[0])
            continue;
        float mid = (b[0] + b[1]) / 2;
        add_part(G_HUD, ch, mid < 0.4f * stage_w ? -band : mid > 0.6f * stage_w ? band : 0, b);
    }
    pass_no++;
    static int told;
    if (!told++)
        rt_log("[wide] race HUD: its left and right parts drawn at the screen's edges (%.0f px)\n", band / 20);
}

#define CH_LIST 0xb8

void wide_nav(u32 pl)
{
    if (!edges_on())
        return;
    float band = -ext_x0;
    group_frame[G_NAV] = frame_no, nparts[G_NAV] = 0;
    u32 nm = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"NavigationMenu"));
    u32 wm = nm && *(u8 *)GP(nm + 0xab) ? rt_call1(GT_FN_WORLD_MATRIX, nm) : 0;
    if (!wm)
        return;
    float tx = *(float *)GP(wm + 8), away = fabsf(tx);
    float shift = away < band ? -(band - away) : 0;
    if (shift == 0)
        return;
    u32 arr = rd32(nm + CH_LIST);
    int n = (int)rd32(nm + CH_LIST + 4);
    pass_no++;
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        float b[4];
        if (!k || !*(u8 *)GP(k + 0xab) || !world_box(k, b) || b[1] <= b[0])
            continue;
        if (b[0] - tx >= -0.05f * stage_w && b[1] - tx <= 0.4f * stage_w)
            add_part(G_NAV, k, shift, b);
    }
    pass_no++;
    static int told;
    if (!told++)
        rt_log("[wide] main menu: its left bar drawn at the screen's left edge (%.0f px)\n", band / 20);
}

void wide_hud_touch(float *x, float y)
{
    if (!edges_on())
        return;
    float tx = *x * 20, ty = y * 20;
    for (int g = 0; g < NGROUPS; g++) {
        if (group_frame[g] + 2 < frame_no)
            continue;
        for (int i = 0; i < nparts[g]; i++) {
            const float *b = parts[g][i].box;
            if (tx >= b[0] && tx <= b[1] && ty >= b[2] && ty <= b[3]) {
                *x -= parts[g][i].shift / 20;
                return;
            }
        }
    }
}

static void leaf(CPU *c, void (*real)(CPU *), int shape)
{
    u32 ch = c->r[0];
    float b[4], keep[6];
    u32 wm = 0;
    if (shape && menu3d_hide_backdrop(ch))
        return;
    u32 par = rd32(ch + 0x40);
    if (par && !strcmp(char_name(par), "cashNew")) {
        if (!shop_cash_new())
            return;
    }
    float shift = ext_x0 != 0 || ext_x1 != 0 ? hud_shift(ch) : 0;
    if (shift != 0) {
        if (world_box(ch, b) && (b[1] + shift < ext_x0 || b[0] + shift > ext_x1))
            return;
        wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
        memcpy(keep, GP(wm), sizeof keep);
        float m[6];
        memcpy(m, keep, sizeof m);
        m[2] += shift;
        memcpy(GP(wm), m, sizeof m);
        c->r[0] = ch;
        in_anchored = 1;
        real(c);
        in_anchored = 0;
        memcpy(GP(wm), keep, sizeof keep);
        return;
    }
    if ((ext_x0 != 0 || ext_x1 != 0) && world_box(ch, b)) {
        if (dumping)
            dump(shape ? "shape" : "text", ch, b);
        if (off_stage(b) || card_off(ch))
            return;
        float m[6], s = 1;
        int interior = interior_of(ch) != 0;
        int kind = shape && !interior ? cover_kind(ch, b) : KEEP;
        if (kind == EXTEND) {
            c->r[0] = ch;
            real(c);
            extend_sides(c, real, ch, b);
            return;
        }
        if (kind == ZOOM)
            s = (ext_x1 - ext_x0) / (b[1] - b[0]);
        else if (interior)
            s = interior_scale();
        if (s > 1 && kind != STRETCH) {
            wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
            memcpy(keep, GP(wm), sizeof keep);
            float cx = stage_w / 2, cy = stage_h / 2;
            memcpy(m, keep, sizeof m);
            m[0] *= s, m[1] *= s, m[2] = cx + (m[2] - cx) * s;
            m[3] *= s, m[4] *= s, m[5] = cy + (m[5] - cy) * s;
            memcpy(GP(wm), m, sizeof m);
        } else if (kind == STRETCH) {
            wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
            memcpy(keep, GP(wm), sizeof keep);
            s = (ext_x1 - ext_x0) / (b[1] - b[0]);
            memcpy(m, keep, sizeof m);
            m[0] *= s, m[1] *= s, m[2] = ext_x0 + (m[2] - b[0]) * s;
            memcpy(GP(wm), m, sizeof m);
        }
    }
    c->r[0] = ch;
    real(c);
    if (wm)
        memcpy(GP(wm), keep, sizeof keep);
}

void __real_F_4eb4ac(CPU *c);
void __real_F_500b58(CPU *c);
void __wrap_F_4eb4ac(CPU *c) { leaf(c, __real_F_4eb4ac, 1); }
void __wrap_F_500b58(CPU *c) { leaf(c, __real_F_500b58, 0); }

void __real_F_4ca684(CPU *c);

void __wrap_F_4ca684(CPU *c)
{
    u32 ch = c->r[0];
    float b[4];
    if ((ext_x0 != 0 || ext_x1 != 0) && hud_shift(ch) == 0 && world_box(ch, b)) {
        if (dumping) {
            u32 cb = rd32(ch + 0x64);
            dump("callback", ch, b);
            dev_log("[wdump] %u   its function 0x%x (data 0x%x)\n", frame_no, cb ? rd32(cb + 0x4c) : 0,
                   cb ? rd32(cb + 0x50) : 0);
        }
        if (off_stage(b) || card_off(ch))
            return;
    }
    c->r[0] = ch;
    __real_F_4ca684(c);
}

void gt_wheel_proj(u32 m)
{
    if (ext_x0 == 0 && ext_x1 == 0)
        return;
    float s = interior_scale(), v[8];
    if (s <= 1)
        return;
    memcpy(v, GP(m), sizeof v);
    for (int i = 0; i < 8; i++)
        v[i] *= s;
    memcpy(GP(m), v, sizeof v);
    static int told;
    if (!told++)
        rt_log("[wide] cockpit wheel: enlarged %gx with the picture\n", s);
}

static float bx_sx = 1, bx_sy = 1, bx_dx;

#define BACKDROP_ASPECT 1.64f

static float backdrop_scale(void)
{
    float s = (ext_x1 - ext_x0) / stage_h / BACKDROP_ASPECT;
    return s < 1 ? 1 : s;
}

void gt_wide_bounds(u32 ch, u32 rect)
{
    if (bx_sx == bx_sy)
        return;
    float r[4], s = 1, cx = stage_w / 2, cy = stage_h / 2;
    if (interior_of(ch))
        s = interior_scale();
    else if (!strcmp(char_name(ch), "RenderBackground"))
        s = backdrop_scale();
    memcpy(r, GP(rect), sizeof r);
    if (dumping) {
        float d[4] = {r[0] / bx_sx, r[1] / bx_sx, r[2] / bx_sx, r[3] / bx_sx};
        dump("bounds", ch, d);
    }
    float shift = hud_shift(ch);
    for (int k = 0; k < 4; k++) {
        float t = r[k] / bx_sx, mid = k < 2 ? cx : cy;
        t = mid + (t - mid) * s;
        r[k] = (t + (k < 2 ? shift : 0)) * bx_sy + (k < 2 ? bx_dx : 0);
    }
    memcpy(GP(rect), r, sizeof r);
}

void gt_wide_fit(u32 rect, u32 *out)
{
    float r[4];
    memcpy(r, GP(rect), sizeof r);
    float W = (float)(int)rd32(GT_SCREEN_PX_H), H = (float)(int)rd32(GT_SCREEN_PX_W), w = r[1] - r[0], h = r[3] - r[2];
    if (bx_sx != bx_sy && W > 0 && H > 0 && W / H > 4.0f / 3 * 1.001f && w > 0 && h > 0) {
        float vx, vy, vw, vh;
        if (w / h > 4.0f / 3)
            vw = w, vh = w * 0.75f, vx = r[0], vy = r[2] - (h - vh) * 0.5f;
        else
            vh = h, vw = h * 4.0f / 3, vx = r[0] - (w - vw) * 0.5f, vy = r[2];
        float mid = vx + vw / 2, nw = vh * W / H;
        r[0] = mid - nw / 2, r[1] = mid + nw / 2, r[2] = vy, r[3] = vy + vh;
        if (dumping)
            dev_log("[wdump] %u fit %.0f..%.0f x %.0f..%.0f -> %.0f..%.0f x %.0f..%.0f (px)\n", frame_no,
                   (vx) / 20, (vx + vw) / 20, vy / 20, (vy + vh) / 20, r[0] / 20, r[1] / 20, r[2] / 20, r[3] / 20);
    }
    memcpy(out, r, sizeof r);
}

void gt_wide_clip(u32 rect)
{
    if (bx_sx == bx_sy)
        return;
    float r[4], x0 = bx_dx, x1 = bx_dx + stage_w * bx_sy, tol = 0.02f * stage_w * bx_sy;
    memcpy(r, GP(rect), sizeof r);
    if (r[0] <= x0 + tol && r[1] >= x1 - tol) {
        if (dumping)
            dev_log("[wdump] %u clip %.0f..%.0f widened (px)\n", frame_no, r[0] / 20, r[1] / 20);
        r[0] = 0, r[1] = stage_w * bx_sx;
        memcpy(GP(rect), r, sizeof r);
    }
}

static u32 draws_at_start;
static int bands_seen;

void wide_frame_start(void)
{
    draws_at_start = gles_draws();
    bands_seen = 0;
    if (!frame_no) {
        const char *s = dev_getenv("GT_WIDE_DUMP");
        while (s && *s && ndump < 16) {
            dump_frames[ndump++] = atoi(s);
            s = strchr(s, ',');
            if (s)
                s++;
        }
    }
    frame_no++;
    dumping = 0;
    for (int i = 0; i < ndump; i++)
        dumping |= dump_frames[i] == frame_no;
}

static void black_bands(int vw, int vh, float band)
{
    int b = (int)(band + 0.5f), sc = 0, box[4];
    float cc[4];
    if (b <= 0)
        return;
    static int timed = -1;
    static LARGE_INTEGER fq, t0, t1;
    static double acc;
    static unsigned calls;
    if (timed < 0) {
        timed = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
        QueryPerformanceFrequency(&fq);
    }
    if (GT_DEV && timed)
        QueryPerformanceCounter(&t0);
    gles_state(GL_SCISSOR_TEST, &sc);
    gles_state(GL_SCISSOR_BOX, box);
    gles_state(GL_COLOR_CLEAR_VALUE, cc);
    if (GT_DEV && timed) {
        QueryPerformanceCounter(&t1);
        acc += (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
        if (++calls == 600) {
            rt_log("[wide] side bands' state queries: %.3f ms a frame\n", acc * 1000 / 600);
            calls = 0, acc = 0;
        }
    }
    p_glEnable(GL_SCISSOR_TEST);
    p_glClearColor(0, 0, 0, 1);
    if (vh > vw) {
        p_glScissor(0, 0, (u32)vw, (u32)b);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glScissor(0, (u32)(vh - b), (u32)vw, (u32)b);
    } else {
        p_glScissor(0, 0, (u32)b, (u32)vh);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glScissor((u32)(vw - b), 0, (u32)b, (u32)vh);
    }
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glClearColor(cc[0], cc[1], cc[2], cc[3]);
    p_glScissor((u32)box[0], (u32)box[1], (u32)box[2], (u32)box[3]);
    if (!sc)
        p_glDisable(GL_SCISSOR_TEST);
}

void __wrap_F_517608(CPU *c)
{
    u32 sp = c->r[13];
    int vw = (int)rd32(sp), vh = (int)rd32(sp + 4);
    float f[4];
    memcpy(f, GP(sp + 8), sizeof f);
    float lw = (float)(vw > vh ? vw : vh), lh = (float)(vw > vh ? vh : vw);
    float fw = f[1] - f[0], fh = f[3] - f[2];
    pass_no++;
    int wide = ui_wide_ui && fh > 0 && fw > 0 && lh > 0 && lw / lh > fw / fh * 1.001f;
    ext_x0 = ext_x1 = 0;
    if (wide) {
        float nw = fh * lw / lh, ext = (nw - fw) / 2, g[2] = {f[0] - ext, f[1] + ext};
        static int told;
        if (!told) {
            told = 1;
            rt_log("[wide] gameswf: the %g x %g twips stage in a %g x %g frame (one scale, centred)\n", fw, fh, nw, fh);
        }
        memcpy(GP(sp + 8), g, sizeof g);
        ext_x0 = g[0], ext_x1 = g[1], stage_w = fw, stage_h = fh;
        float sx = lw * 20 / fw, sy = lh * 20 / fh;
        bx_sx = sx, bx_sy = sy, bx_dx = (lw * 20 - fw * sy) / 2;
        if (!bands_seen) {
            bands_seen = 1;
            if (gles_draws() == draws_at_start)
                black_bands(vw, vh, (lw - fw / 20 * sy) / 2);
        }
    } else {
        bx_sx = bx_sy = 1, bx_dx = 0;
    }
    __real_F_517608(c);
    if (wide)
        memcpy(GP(sp + 8), f, 2 * sizeof(float));
}
