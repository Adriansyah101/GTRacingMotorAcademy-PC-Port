#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "rt.h"
#include "features.h"
#include "game.h"

static int mode_ = -1, making;

#define GAME_OWN(m) ((m) == 1 || (m) == 4)
#define POSTFX_DEFAULT 4

static int gt_postfx_mode(void)
{
    if (mode_ < 0) {
        const char *e = getenv("GT_POSTFX");
        mode_ = e && *e >= '0' && *e <= '4' && !e[1] ? *e - '0' : POSTFX_DEFAULT;
        rt_log(mode_ == 0   ? "[postfx] off (POSTFX=0)\n"
               : mode_ == 1 ? "[postfx] the game's own bloom on (POSTFX=1)\n"
               : mode_ == 2 ? "[postfx] the iPad 2010 bloom on (POSTFX=2)\n"
               : mode_ == 3 ? "[postfx] the iPad 2010 bloom, the world's colours kept (POSTFX=3)\n"
                            : "[postfx] Enhanced: the game's own bloom balanced, and Classic Natural (POSTFX=4)\n");
    }
    return mode_;
}

int gt_postfx_wanted(void) { return gt_postfx_mode(); }

static int classic(void)
{
    int m = gt_postfx_mode();
    return m >= 2 && m <= 4;
}

void gt_postfx_set(int m)
{
    gt_postfx_mode();
    mode_ = m >= 0 && m <= 4 ? m : 0;
    rt_log("[postfx] %s (Options > Display)\n", mode_ == 4   ? "Enhanced"
                                                 : mode_ == 3 ? "Classic Natural"
                                                 : mode_ == 2 ? "Classic"
                                                 : mode_ == 1 ? "the game's own bloom"
                                                              : "off");
}

void gt_postfx_targets(int on) { making = on; }

int gt_postfx_gate(void)
{
    return making || GAME_OWN(gt_postfx_mode());
}

static float gt_shader_balance(void)
{
    return gt_postfx_mode() == 4 ? 1.0f : 0.0f;
}

#define BAL_STATIC 0.30f
#define BAL_CAR 1.15f
#define BAL_POST 1.10f
#define BAL_SCREEN 1
static void balance_tune(float *st, float *car, float *post, int *screen)
{
    static float v[3] = {-1.0f};
    static int s;
    if (v[0] < 0.0f) {
        v[0] = BAL_STATIC, v[1] = BAL_CAR, v[2] = BAL_POST, s = BAL_SCREEN;
        const char *e = dev_getenv("GT_POSTFX_BALANCE_TUNE");
        float a, b, d = BAL_CAR;
        int c, n = e ? sscanf(e, "%f %f %d %f", &a, &b, &c, &d) : 0;
        if (n >= 3 && a >= 0.0f && a <= 2.0f && b >= 0.0f && b <= 2.0f && d >= 0.0f && d <= 2.0f) {
            v[0] = a, v[1] = d, v[2] = b, s = c != 0;
            rt_log("[postfx] Balanced: static highlights %.2f, car highlights %.2f, glow %.2f%s "
                   "(GT_POSTFX_BALANCE_TUNE)\n", (double)a, (double)d, (double)b, s ? " as a screen" : " added");
        }
    }
    *st = v[0], *car = v[1], *post = v[2], *screen = s;
}

static char *gt_shader_patch(const char *src)
{
    float st, car, post;
    int screen;
    balance_tune(&st, &car, &post, &screen);
    char e_static[96], e_car[96], e_post[128];
    snprintf(e_static, sizeof e_static, "specularIntensity *= highLightRatio * mix(1.0, %.4f, gt_balance);",
             (double)st);
    snprintf(e_car, sizeof e_car, "specularIntensity = 4.0 * mix(1.0, %.4f, gt_balance);", (double)car);
    if (screen)
        snprintf(e_post, sizeof e_post, "+ color1 * mix(vec4(1.0), vec4(%.4f * (vec3(1.0) - color0.rgb), 1.0), "
                 "gt_balance);", (double)post);
    else
        snprintf(e_post, sizeof e_post, "+ color1 * mix(vec4(1.0), vec4(vec3(%.4f), 1.0), gt_balance);",
                 (double)post);
    const struct { const char *was, *now; int highlights_only; } edits[] = {
        {"color.a * 0.33333", "mix(color.a, color.b, gt_balance) * 0.33333", 0},
        {"vec3(1.28, 1.0, 1.0)", "mix(vec3(1.28, 1.0, 1.0), vec3(1.0), gt_balance)", 1},
        {"specularIntensity *= highLightRatio;", e_static, 1},
        {"specularIntensity = 4.0;", e_car, 1},
        {"color / 3.0", "color / mix(3.0, 3.462, gt_balance)", 0},
        {"color0 * 2.0", "color0 * mix(2.0, 1.0, gt_balance)", 0},
        {"+ color1;", e_post, 0},
    };
    int highlights = strstr(src, "specularColor") && strstr(src, "vec4(0.0, 0.0, 0.0, 1.0)");
    if (!strstr(src, "luminanceCoef") && !highlights && !strstr(src, "blurWeight") && !strstr(src, "color0 * 2.0"))
        return NULL;
    size_t n = strlen(src);
    char *out = malloc(n + 1024);
    if (!out)
        return NULL;
    static const char head[] = "uniform float gt_balance;\n";
    memcpy(out, head, sizeof head - 1);
    char *o = out + sizeof head - 1;
    int changed = 0, which = 0;
    for (const char *p = src; *p;) {
        size_t k = 0;
        while (k < sizeof edits / sizeof *edits &&
               (strncmp(p, edits[k].was, strlen(edits[k].was)) || (edits[k].highlights_only && !highlights)))
            k++;
        if (k < sizeof edits / sizeof *edits && (size_t)(o - out) + strlen(edits[k].now) < n + 1000) {
            size_t l = strlen(edits[k].now);
            memcpy(o, edits[k].now, l);
            o += l;
            p += strlen(edits[k].was);
            changed++;
            which |= 1 << k;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    if (!changed) {
        free(out);
        return NULL;
    }
    static int logs;
    if (logs++ < 8)
        rt_log("[postfx] a bloom shader given gt_balance (%d change%s, edits 0x%02x, POSTFX=4)\n", changed,
               changed > 1 ? "s" : "", which);
    return out;
}

void __real_F_2a36e8(CPU *c);

typedef struct {
    u32 fb, tex, w, h;
} Target;
static Target scene, bright, blur1, blur2;

enum { P_BRIGHT, P_BLUR, P_ADD, P_FLAT, P_TONE, NPROG };
static u32 prog[NPROG], loc_k[NPROG], loc_a, loc_b, loc_add, loc_sat;
static int progs_ok = -1;

static const char *const vs_src =
    "#version 120\n"
    "varying vec2 v;\n"
    "void main() { v = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";
static const char *const fs_src[NPROG] = {
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; varying vec2 v;\n"
    "void main() { gl_FragColor = vec4(max(texture2D(t, v).rgb - k, 0.0), 1.0); }\n",
    "#version 120\n"
    "uniform sampler2D t; uniform vec2 a, b; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb + texture2D(t, v + a).rgb + texture2D(t, v - a).rgb\n"
    "         + texture2D(t, v + b).rgb + texture2D(t, v - b).rgb;\n"
    "  gl_FragColor = vec4(0.2 * c, 1.0);\n"
    "}\n",
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; uniform float add; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb;\n"
    "  gl_FragColor = vec4(clamp(add > 0.5 ? c + k : c * k, 0.0, 1.0), 1.0);\n"
    "}\n",
    "#version 120\n"
    "uniform vec3 k;\n"
    "void main() { gl_FragColor = vec4(k, 1.0); }\n",
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; uniform float sat; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb * k;\n"
    "  float y = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  gl_FragColor = vec4(clamp(mix(vec3(y), c, sat), 0.0, 1.0), 1.0);\n"
    "}\n",
};

static u32 compile(u32 kind, const char *src)
{
    u32 s = p_glCreateShader(kind), ok = 0;
    const char *srcs[1] = {src};
    p_glShaderSource(s, 1, GA(srcs), 0);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, GA(&ok));
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(s, sizeof log, 0, GA(log));
        rt_log("[postfx] 2010 bloom: a shader did not compile: %s\n", log);
        return 0;
    }
    return s;
}

static int make_progs(void)
{
    if (progs_ok >= 0)
        return progs_ok;
    progs_ok = 0;
    u32 vs = compile(GL_VERTEX_SHADER, vs_src);
    if (!vs)
        return 0;
    for (int i = 0; i < NPROG; i++) {
        u32 fs = compile(GL_FRAGMENT_SHADER, fs_src[i]), ok = 0;
        if (!fs)
            return 0;
        prog[i] = p_glCreateProgram();
        p_glAttachShader(prog[i], vs);
        p_glAttachShader(prog[i], fs);
        p_glLinkProgram(prog[i]);
        p_glGetProgramiv(prog[i], GL_LINK_STATUS, GA(&ok));
        if (!ok) {
            char log[512] = "";
            p_glGetProgramInfoLog(prog[i], sizeof log, 0, GA(log));
            rt_log("[postfx] 2010 bloom: a program did not link: %s\n", log);
            return 0;
        }
        loc_k[i] = p_glGetUniformLocation(prog[i], GA("k"));
        p_glUseProgram(prog[i]);
        p_glUniform1i(p_glGetUniformLocation(prog[i], GA("t")), 0);
    }
    loc_a = p_glGetUniformLocation(prog[P_BLUR], GA("a"));
    loc_b = p_glGetUniformLocation(prog[P_BLUR], GA("b"));
    loc_add = p_glGetUniformLocation(prog[P_ADD], GA("add"));
    loc_sat = p_glGetUniformLocation(prog[P_TONE], GA("sat"));
    progs_ok = 1;
    return 1;
}

static void target(Target *t, u32 w, u32 h, int mip)
{
    if (t->tex && t->w == w && t->h == h)
        return;
    if (!t->tex) {
        p_glGenTextures(1, GA(&t->tex));
        p_glGenFramebuffers(1, GA(&t->fb));
    }
    p_glBindTexture(GL_TEXTURE_2D, t->tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mip)
        p_glGenerateMipmap(GL_TEXTURE_2D);
    gles_bind_fb(GL_FRAMEBUFFER, t->fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    t->w = w, t->h = h;
}

static void quad(void)
{
    p_glBegin(GL_QUADS);
    p_glTexCoord2f(0, 0), p_glVertex2f(-1, -1);
    p_glTexCoord2f(1, 0), p_glVertex2f(1, -1);
    p_glTexCoord2f(1, 1), p_glVertex2f(1, 1);
    p_glTexCoord2f(0, 1), p_glVertex2f(-1, 1);
    p_glEnd();
}

static void blur(const Target *src, const Target *dst, float radius)
{
    gles_bind_fb(GL_FRAMEBUFFER, dst->fb);
    p_glViewport(0, 0, dst->w, dst->h);
    p_glBindTexture(GL_TEXTURE_2D, src->tex);
    float dx = radius / (float)src->w, dy = radius / (float)src->h;
    p_glUniform2f(loc_a, 1.5f * dx, 0.5f * dy);
    p_glUniform2f(loc_b, -0.5f * dx, 1.5f * dy);
    quad();
}

typedef struct {
    unsigned char tint[3], thr[3], scn[3];
    float radius;
    u32 blend;
    int tint_scene, add_bloom;
    int screen;
    float sat;
} Params;

static void neutral(unsigned char v[3])
{
    unsigned char y = (unsigned char)((299 * v[0] + 587 * v[1] + 114 * v[2] + 500) / 1000);
    v[0] = v[1] = v[2] = y;
}

static void postfx_draw(const Params *p);

#define ENHANCED_GLOW 2.0f
#define ENHANCED_SCENE 0.85f
#define ENHANCED_SCREEN 1
#define ENHANCED_THRESHOLD 160
#define ENHANCED_SATURATION 1.15f
typedef struct {
    float glow, scene, sat;
    int screen, threshold;
} Tone;
static Tone enhanced_tone(void)
{
    static Tone t;
    static int made;
    if (!made) {
        made = 1;
        t.glow = ENHANCED_GLOW, t.scene = ENHANCED_SCENE, t.screen = ENHANCED_SCREEN;
        t.threshold = ENHANCED_THRESHOLD, t.sat = ENHANCED_SATURATION;
        const char *e = dev_getenv("GT_POSTFX_ENHANCED_GLOW");
        float a, c = t.scene;
        int b = t.screen;
        if (e && sscanf(e, "%f %d %f", &a, &b, &c) >= 1 && a >= 0.0f && a <= 4.0f && c >= 0.0f && c <= 1.0f) {
            t.glow = a, t.screen = b != 0, t.scene = c;
            rt_log("[postfx] Enhanced: glow %.2f%s, scene darkening %.2f (GT_POSTFX_ENHANCED_GLOW)\n", (double)a,
                   t.screen ? " as a screen" : " added", (double)c);
        }
        e = dev_getenv("GT_POSTFX_ENHANCED_TONE");
        int h;
        float s;
        if (e && sscanf(e, "%d %f", &h, &s) == 2 && h >= 0 && h <= 255 && s >= 0.5f && s <= 2.0f) {
            t.threshold = h, t.sat = s;
            rt_log("[postfx] Enhanced: threshold %d, saturation %.2f (GT_POSTFX_ENHANCED_TONE)\n", h, (double)s);
        }
    }
    return t;
}

#define ENHANCED_MENU_TINT 255
#define ENHANCED_MENU_THRESHOLD ENHANCED_THRESHOLD
#define ENHANCED_MENU_SCENE 250
#define ENHANCED_MENU_SCREEN 1
static Params enhanced_menu(void)
{
    static Params p;
    static int made;
    if (!made) {
        made = 1;
        int t = ENHANCED_MENU_TINT, h = ENHANCED_MENU_THRESHOLD, c = ENHANCED_MENU_SCENE, s = ENHANCED_MENU_SCREEN;
        const char *e = dev_getenv("GT_POSTFX_ENHANCED_MENU");
        int a[4];
        if (e && sscanf(e, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4 && a[0] >= 0 && a[0] <= 255 &&
            a[1] >= 0 && a[1] <= 255 && a[2] >= 0 && a[2] <= 255) {
            t = a[0], h = a[1], c = a[2], s = a[3] != 0;
            rt_log("[postfx] Enhanced: the menus' 3D scene: tint %d, threshold %d, scene %d%s "
                   "(GT_POSTFX_ENHANCED_MENU)\n", t, h, c, s ? ", as a screen" : "");
        }
        for (int i = 0; i < 3; i++)
            p.tint[i] = (unsigned char)t, p.thr[i] = (unsigned char)h, p.scn[i] = (unsigned char)c;
        p.radius = 1.5f, p.blend = 2, p.tint_scene = c < 255, p.add_bloom = 1, p.screen = s;
        p.sat = enhanced_tone().sat;
    }
    return p;
}

static void postfx_with(Params p, const char *where, int enhanced, int race)
{
    static Params last;
    static int told;
    if (memcmp(&last, &p, sizeof p) && told < 20) {
        told++;
        last = p;
        rt_log("[postfx] %s: tint %d %d %d, threshold %d %d %d, scene %d %d %d, radius %.2f, mode %u%s%s%s\n", where,
               p.tint[0], p.tint[1], p.tint[2], p.thr[0], p.thr[1], p.thr[2], p.scn[0], p.scn[1], p.scn[2],
               (double)p.radius, p.blend, p.tint_scene ? "" : ", no scene tint", p.add_bloom ? "" : ", no bloom",
               mode_ == 3 ? " (POSTFX=3: drawn grey)" : enhanced ? " (Enhanced: its own tone)" : "");
    }
    if (mode_ == 3 || mode_ == 4) {
        if (!enhanced)
            neutral(p.tint);
        neutral(p.thr);
        neutral(p.scn);
    }
    if (enhanced && race) {
        Tone t = enhanced_tone();
        float top = p.tint[0] > p.tint[1] ? p.tint[0] : p.tint[1];
        top = top > p.tint[2] ? top : p.tint[2];
        float g = top * t.glow > 255.0f ? 255.0f / top : t.glow;
        for (int i = 0; i < 3; i++) {
            p.tint[i] = (unsigned char)(p.tint[i] * g + 0.5f);
            p.scn[i] = (unsigned char)(255 - (255 - p.scn[i]) * t.scene + 0.5f);
            if (p.thr[i] < t.threshold)
                p.thr[i] = (unsigned char)t.threshold;
        }
        p.screen = t.screen, p.sat = t.sat;
    }
    postfx_draw(&p);
}

static void race_postfx(u32 o)
{
    const unsigned char *m = GP(o);
    if (!m[0xf6])
        return;
    Params p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 3; i++) {
        p.tint[i] = m[0xdc + 3 * i];
        p.thr[i] = m[0xdd + 3 * i];
        p.scn[i] = m[0xde + 3 * i];
    }
    memcpy(&p.radius, m + 0xf0, 4);
    p.blend = rd32(o + 0xe8);
    p.tint_scene = m[0xf4], p.add_bloom = m[0xf5];
    postfx_with(p, "a race", mode_ == 4, 1);
}

static void postfx_draw(const Params *p)
{
    float tint[3], thr[3], scn[3], radius = p->radius;
    for (int i = 0; i < 3; i++)
        tint[i] = p->tint[i] / 255.0f, thr[i] = p->thr[i] / 255.0f, scn[i] = p->scn[i] / 255.0f;
    u32 blend = p->blend;
    int tint_scene = p->tint_scene, add_bloom = p->add_bloom;

    u32 vp[4] = {0};
    p_glGetIntegerv(GL_VIEWPORT, GA(vp));
    if (vp[2] < 16 || vp[3] < 16)
        return;
    u32 cur_prog, rfbo, dfbo, active, tex0;
    gles_state(GL_CURRENT_PROGRAM, &cur_prog);
    gles_state(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    gles_state(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    gles_state(GL_ACTIVE_TEXTURE, &active);
    gles_state(GL_TEXTURE_BINDING_2D, &tex0);
    p_glActiveTexture(GL_TEXTURE0);
    p_glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_ALPHA_TEST);
    p_glColorMask(1, 1, 1, 1);
    p_glDepthMask(0);

    if (make_progs()) {
        target(&scene, vp[2], vp[3], 1);
        target(&bright, 512, 512, 0);
        target(&blur1, 256, 256, 0);
        target(&blur2, 256, 256, 0);

        gles_bind_fb(GL_READ_FRAMEBUFFER, dfbo);
        gles_bind_fb(GL_DRAW_FRAMEBUFFER, scene.fb);
        p_glBlitFramebuffer(vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], 0, 0, vp[2], vp[3], GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
        p_glBindTexture(GL_TEXTURE_2D, scene.tex);
        p_glGenerateMipmap(GL_TEXTURE_2D);

        gles_bind_fb(GL_FRAMEBUFFER, bright.fb);
        p_glViewport(0, 0, bright.w, bright.h);
        p_glUseProgram(prog[P_BRIGHT]);
        p_glUniform3f(loc_k[P_BRIGHT], thr[0], thr[1], thr[2]);
        quad();

        p_glUseProgram(prog[P_BLUR]);
        blur(&bright, &blur1, radius);
        blur(&blur1, &blur2, radius);

        gles_bind_fb(GL_FRAMEBUFFER, dfbo);
        p_glViewport(vp[0], vp[1], vp[2], vp[3]);
        p_glEnable(GL_BLEND);
        if (p->sat > 0.0f) {
            p_glDisable(GL_BLEND);
            p_glColorMask(1, 1, 1, 0);
            p_glUseProgram(prog[P_TONE]);
            p_glUniform3f(loc_k[P_TONE], tint_scene ? scn[0] : 1.0f, tint_scene ? scn[1] : 1.0f,
                          tint_scene ? scn[2] : 1.0f);
            p_glUniform1f(loc_sat, p->sat);
            p_glBindTexture(GL_TEXTURE_2D, scene.tex);
            quad();
            p_glColorMask(1, 1, 1, 1);
            p_glEnable(GL_BLEND);
        } else if (tint_scene) {
            p_glUseProgram(prog[P_FLAT]);
            p_glUniform3f(loc_k[P_FLAT], scn[0], scn[1], scn[2]);
            p_glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            quad();
        }
        if (add_bloom) {
            p_glUseProgram(prog[P_ADD]);
            p_glUniform3f(loc_k[P_ADD], tint[0], tint[1], tint[2]);
            p_glUniform1f(loc_add, blend == 4 ? 1.0f : 0.0f);
            if (blend == 4)
                p_glBlendFunc(GL_SRC_COLOR, GL_ZERO);
            else
                p_glBlendFunc(p->screen ? GL_ONE_MINUS_DST_COLOR : GL_ONE, GL_ONE);
            p_glBindTexture(GL_TEXTURE_2D, blur2.tex);
            quad();
        }
    }

    gles_bind_fb(GL_READ_FRAMEBUFFER, rfbo);
    gles_bind_fb(GL_DRAW_FRAMEBUFFER, dfbo);
    p_glPopAttrib();
    p_glUseProgram(cur_prog);
    p_glBindTexture(GL_TEXTURE_2D, tex0);
    p_glActiveTexture(active);
}

static u32 race_post;
void __wrap_F_2a36e8(CPU *c)
{
    u32 o = c->r[0];
    __real_F_2a36e8(c);
    race_post = 0;
    if (classic() && o) {
        if (gt_postfx_gate())
            race_post = o;
        else
            race_postfx(o);
    }
}

#define RET_RACE_AFTER_COMPOSITE 0x1700b4u
void __real_F_14a660(CPU *c);
void __wrap_F_14a660(CPU *c)
{
    if (c->r[14] == RET_RACE_AFTER_COMPOSITE && race_post) {
        u32 o = race_post;
        race_post = 0;
        if (classic())
            race_postfx(o);
    }
    if (c->r[14] == 0x25213cu && classic()) {
        u32 g = gt_game();
        if (g && *(volatile unsigned char *)GP(g + G_MENU_SCENE)) {
            Params p = {{210, 210, 210}, {90, 90, 90}, {255, 255, 255}, 1.5f, 2, 1, 1, 0, 0.0f};
            if (mode_ == 4)
                p = enhanced_menu();
            postfx_with(p, "the menus' 3D scene", mode_ == 4, 0);
        }
    }
    __real_F_14a660(c);
}

__attribute__((constructor)) static void postfx_hooks(void)
{
    gles_add_shader_patch(gt_shader_patch);
    gles_add_host_uniform("gt_balance", gt_shader_balance);
}
