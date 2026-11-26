#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#ifndef _WIN32
#include <spawn.h>
extern char **environ;
#endif

#include "features.h"
#include "game.h"

#define AOM_EVENT_FN 0x24b484u
#define SWF_VISIBLE 0xab

int glx_msaa_now(void);
int glx_msaa_max(void);
float glx_aniso_now(void);
float glx_aniso_max(void);
void glx_set_msaa(int n);
void glx_set_aniso(float n);

typedef struct { const char *label, *value; } Opt;
static const Opt res_opts[] = {{"Device", "0"}, {"720p", "720"}, {"1080p", "1080"}, {"1440p", "1440"},
                               {"2160p", "2160"}, {"Auto", "monitor"}};
static const Opt wide_opts[] = {{"4:3 (iPad)", "1024"}, {"16:10", "1229"}, {"16:9", "1366"}, {"21:9", "1792"},
                                {"Auto", "monitor"}};
#define NRES ((int)(sizeof res_opts / sizeof res_opts[0]))
#define NWIDE ((int)(sizeof wide_opts / sizeof wide_opts[0]))
static const int aa_opts[] = {0, 2, 4, 8};
static const int af_opts[] = {1, 2, 4, 8, 16};
static const Opt blur_opts[] = {{"Auto", "auto"}, {"Full", "1"}, {"Light", "0"}};
#define NBLUR 3
static const Opt postfx_opts[] = {{"Off", "0"}, {"Classic", "2"}, {"Classic Natural", "3"}, {"Enhanced", "4"}};
#define NPOSTFX 4
static const char *postfx_text(void)
{
    int m = gt_postfx_wanted();
    return m == 4 ? "Enhanced" : m == 3 ? "Classic Natural" : m == 2 ? "Classic" : m == 1 ? "Game's own" : "Off";
}

static int on = -1;
static int aa_auto;
static int blur_sel = 0;
static int res_run = -1, res_sel = -1, wide_run = -1, wide_sel = -1;
static char res_custom[32], wide_custom[32];
static u32 pressed_at;

static int find_opt(const Opt *o, int n, const char *v)
{
    if (!v)
        v = "";
    for (int i = 0; i < n; i++)
        if (!_stricmp(o[i].value, v))
            return i;
    return -1;
}

static SRWLOCK ini_lock = SRWLOCK_INIT;
static void ini_set_locked(const char *key, const char *value, const char *comment);
static void ini_set(const char *key, const char *value, const char *comment)
{
    AcquireSRWLockExclusive(&ini_lock);
    ini_set_locked(key, value, comment);
    ReleaseSRWLockExclusive(&ini_lock);
}

static void ini_set_locked(const char *key, const char *value, const char *comment)
{
    const char *path = gt_ini_path();
    static char buf[65536], out[65536 + 1024];
    size_t n = 0, o = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    size_t kl = strlen(key);
    int done = 0;
    for (char *line = buf; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char *k = line;
        while (*k == ' ' || *k == '\t')
            k++;
        char *e = k + kl;
        while (*e == ' ' || *e == '\t')
            e++;
        if (!done && !strncmp(k, key, kl) && *e == '=') {
            o += (size_t)snprintf(out + o, sizeof out - o, "%s=%s%s", key, value,
                                  len > 1 && line[len - 2] == '\r' ? "\r\n" : "\n");
            done = 1;
        } else if (o + len < sizeof out) {
            memcpy(out + o, line, len);
            o += len;
        }
        line += len;
    }
    if (!done)
        o += (size_t)snprintf(out + o, sizeof out - o, "%s\n# %s\n%s=%s\n", o && out[o - 1] != '\n' ? "\n" : "",
                              comment, key, value);
    f = fopen(path, "wb");
    if (!f) {
        rt_log("[display] cannot write %s\n", path);
        return;
    }
    fwrite(out, 1, o, f);
    fclose(f);
    rt_log("[display] %s: %s=%s\n", path, key, value);
}

static volatile LONG fs_want = -1;

static int fs_now(void)
{
    LONG w = fs_want;
    if (w >= 0)
        return (int)w;
    return ui_window && (SDL_GetWindowFlags(ui_window) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;
}

static void display_fullscreen(int fs)
{
    fs = fs ? 1 : 0;
    ini_set("GT_FULLSCREEN", fs ? "1" : "0", "1 full screen, 0 a window (Options > Display > Full Screen, Alt+Enter)");
    rt_log("[display] full screen %s\n", fs ? "on" : "off");
    if (ui_is_main_thread()) {
        InterlockedExchange(&fs_want, -1);
        SDL_SetWindowFullscreen(ui_window, fs);
    } else
        InterlockedExchange(&fs_want, fs);
}

void display_fullscreen_toggle(void) { display_fullscreen(!fs_now()); }

void display_fullscreen_pump(void)
{
    LONG w = InterlockedExchange(&fs_want, -1);
    if (w >= 0 && ui_window)
        SDL_SetWindowFullscreen(ui_window, w != 0);
}

static u32 find(u32 pl, const char *path) { return rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)); }

static void set_text(u32 pl, const char *path, const char *text)
{
    char p[96];
    snprintf(p, sizeof p, "ControlsMenu.PCDisplay.%s", path);
    if (find(pl, p))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(p), GA((void *)text), 0);
}

static int pending(void) { return res_sel != res_run || wide_sel != wide_run; }

static const char *blur_text(char *t, size_t n)
{
    if (blur_sel == 0)
        snprintf(t, n, "Auto (%s)", gpu_auto_full_blur() ? "Full" : "Light");
    else
        snprintf(t, n, "%s", blur_opts[blur_sel].label);
    return t;
}

static const char *aa_text(char *t, size_t n, int aa)
{
    if (aa_auto)
        snprintf(t, n, "Auto (%dx)", gpu_auto_msaa());
    else if (aa)
        snprintf(t, n, "%dx MSAA", aa);
    else
        snprintf(t, n, "Off");
    return t;
}

static int aa_index(void)
{
    int now = glx_msaa_now(), best = 0;
    for (int i = 0; i < 4; i++)
        if (aa_opts[i] <= now)
            best = i;
    return best;
}

static int af_index(void)
{
    float now = glx_aniso_now();
    int best = 0;
    for (int i = 0; i < 5; i++)
        if ((float)af_opts[i] <= now)
            best = i;
    return best;
}

static int fs_shown = -1;

static void values(u32 pl)
{
    char t[48];
    if (res_sel >= 0) {
        const char *dev = getenv("GT_DEVICE");
        if (res_sel == 0)
            snprintf(t, sizeof t, "iPad (%s)", dev && !strcmp(dev, "ipad3") ? "1536p" : "768p");
        else
            snprintf(t, sizeof t, "%s", res_opts[res_sel].label);
    } else
        snprintf(t, sizeof t, "%s", res_custom);
    set_text(pl, "Sel1.txtSelectorType", t);
    set_text(pl, "Sel2.txtSelectorType", wide_sel >= 0 ? wide_opts[wide_sel].label : wide_custom);
    fs_shown = fs_now();
    set_text(pl, "Sel3.txtSelectorType", fs_shown ? "On" : "Off");
    set_text(pl, "Sel4.txtSelectorType", aa_text(t, sizeof t, aa_opts[aa_index()]));
    set_text(pl, "Sel6.txtSelectorType", blur_text(t, sizeof t));
    int af = af_opts[af_index()];
    if (af > 1)
        snprintf(t, sizeof t, "%dx", af);
    set_text(pl, "Sel5.txtSelectorType", af > 1 ? t : "Off");
    set_text(pl, "Sel7.txtSelectorType", postfx_text());
    set_text(pl, "Note", "All but Resolution and Aspect Ratio: at once");
}

void display_menu_fill(u32 pl)
{
    set_text(pl, "Title1.typefield", "Resolution:");
    set_text(pl, "Title2.typefield", "Aspect Ratio:");
    set_text(pl, "Title3.typefield", "Full Screen:");
    set_text(pl, "Title4.typefield", "Anti-aliasing:");
    set_text(pl, "Title5.typefield", "Anisotropic:");
    set_text(pl, "Title6.typefield", "Motion Blur:");
    set_text(pl, "Title7.typefield", "PostFX:");
    set_text(pl, "CellRestart.txtKey", "Restart to Apply");
    char p[96] = "ControlsMenu.PCDisplay.CellRestart.Hi";
    u32 hi = find(pl, p);
    if (hi)
        *(volatile u8 *)GP(hi + SWF_VISIBLE) = 0;
    values(pl);
}

void display_menu_layout(u32 pl)
{
    u32 r = find(pl, "ControlsMenu.PCDisplay.CellRestart");
    if (r && *(volatile u8 *)GP(r + SWF_VISIBLE) != (u8)pending())
        *(volatile u8 *)GP(r + SWF_VISIBLE) = (u8)pending();
    u32 n = find(pl, "ControlsMenu.PCDisplay.Note");
    if (n && *(volatile u8 *)GP(n + SWF_VISIBLE) != (u8)!pending())
        *(volatile u8 *)GP(n + SWF_VISIBLE) = (u8)!pending();
    int fs = fs_now();
    if (fs != fs_shown) {
        fs_shown = fs;
        set_text(pl, "Sel3.txtSelectorType", fs ? "On" : "Off");
    }
}

static void restart(void)
{
    char exe[MAX_PATH], cwd[MAX_PATH], pid[16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetCurrentDirectoryA(sizeof cwd, cwd);
    gt_ini_forget_env();
    _putenv_s("GT_RESOLUTION", res_sel >= 0 ? res_opts[res_sel].value : res_custom);
    _putenv_s("GT_WIDE", wide_sel >= 0 ? wide_opts[wide_sel].value : wide_custom);
    _putenv_s("GT_FULLSCREEN", fs_now() ? "1" : "0");
    _putenv_s("GT_FULL_BLUR", blur_opts[blur_sel].value);
    char bm[4];
    snprintf(bm, sizeof bm, "%d", gt_postfx_wanted());
    _putenv_s("GT_POSTFX", bm);
    static const char *const scripted[] = {"GT_TAPS", "GT_KEYNAV", "GT_PADNAV", "GT_PEDALS", "GT_PAUSE", NULL};
    for (int i = 0; GT_DEV && scripted[i]; i++)
        _putenv_s(scripted[i], "");
    snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    _putenv_s("GT_WAIT_PID", pid);
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    char *cmd = _strdup(GetCommandLineA());
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        rt_log("[display] restart: new run %lu, this one ends\n", pi.dwProcessId);
        ui_quit = 1;
    } else
        rt_log("[display] restart: CreateProcess failed (%lu)\n", GetLastError());
    free(cmd);
#else
    static char line[8192];
    char *argv[64];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(line, 1, sizeof line - 1, f) : 0;
    if (f)
        fclose(f);
    line[n] = 0;
    for (size_t i = 0; i < n && argc < 63; i += strlen(line + i) + 1)
        argv[argc++] = line + i;
    argv[argc] = NULL;
    pid_t child;
    int e = argc ? posix_spawn(&child, exe, NULL, NULL, argv, environ) : ENOENT;
    if (!e) {
        rt_log("[display] restart: new run %d, this one ends\n", (int)child);
        ui_quit = 1;
    } else
        rt_log("[display] restart: posix_spawn failed (%d)\n", e);
    (void)cwd;
#endif
    _putenv_s("GT_WAIT_PID", "");
}

void display_menu_click(u32 pl, const char *ps)
{
    if (strstr(ps, "PCDisplay.CellRestart")) {
        if (pending())
            restart();
        return;
    }
    const char *s = strstr(ps, "PCDisplay.Sel");
    if (!s)
        return;
    int row = s[13] - '0';
    int dir = strstr(ps, "btnLeftArrow") ? -1 : strstr(ps, "btnRightArrow") ? 1 : 0;
    if (!dir)
        return;
    if (row == 1) {
        res_sel = res_sel < 0 ? 0 : (res_sel + dir + NRES) % NRES;
        ini_set("GT_RESOLUTION", res_opts[res_sel].value,
                "the height in pixels the game draws its picture at (Options > Display > Resolution)");
    } else if (row == 2) {
        wide_sel = wide_sel < 0 ? 0 : (wide_sel + dir + NWIDE) % NWIDE;
        ini_set("GT_WIDE", wide_opts[wide_sel].value,
                "the screen's shape: 1024 4:3, 1229 16:10, 1366 16:9, 1792 21:9, monitor (Options > Display)");
    } else if (row == 3) {
        int fs = !fs_now();
        display_fullscreen(fs);
        fs_shown = fs;
        set_text(pl, "Sel3.txtSelectorType", fs ? "On" : "Off");
        return;
    } else if (row == 4) {
        int n = 4, mx = glx_msaa_max();
        while (n > 1 && mx && aa_opts[n - 1] > mx)
            n--;
        int i = aa_auto ? n : aa_index();
        i = (i + dir + n + 1) % (n + 1);
        aa_auto = i == n;
        int samples = aa_auto ? gpu_auto_msaa() : aa_opts[i];
        if (mx && samples > mx)
            samples = mx;
        glx_set_msaa(samples);
        char v[8];
        snprintf(v, sizeof v, "%d", samples);
        ini_set("GT_MSAA", aa_auto ? "auto" : v, "anti-aliasing samples: 0 off, 2, 4, 8, auto (Options > Display)");
        char t[24];
        set_text(pl, "Sel4.txtSelectorType", aa_text(t, sizeof t, samples));
        rt_log("[display] anti-aliasing %s\n", t);
        return;
    } else if (row == 5) {
        int n = 5, i = af_index();
        float mx = glx_aniso_max();
        while (n > 1 && mx > 0 && (float)af_opts[n - 1] > mx)
            n--;
        i = (i + dir + n) % n;
        glx_set_aniso((float)af_opts[i]);
        char v[8];
        snprintf(v, sizeof v, "%d", af_opts[i]);
        ini_set("GT_ANISOTROPIC", v, "anisotropic filtering: 1 off, 2, 4, 8, 16 (Options > Display)");
        char t[24];
        if (af_opts[i] > 1)
            snprintf(t, sizeof t, "%dx", af_opts[i]);
        set_text(pl, "Sel5.txtSelectorType", af_opts[i] > 1 ? t : "Off");
        rt_log("[display] anisotropic %s\n", af_opts[i] > 1 ? t : "off");
        return;
    } else if (row == 6) {
        blur_sel = (blur_sel + dir + NBLUR) % NBLUR;
        ini_set("GT_FULL_BLUR", blur_opts[blur_sel].value,
                "motion blur: auto (light on built-in graphics), 1 full, 0 light (Options > Display)");
        fullblur_choose(blur_opts[blur_sel].value);
        char t[24];
        set_text(pl, "Sel6.txtSelectorType", blur_text(t, sizeof t));
        rt_log("[display] motion blur %s\n", t);
        return;
    } else if (row == 7) {
        int m = gt_postfx_wanted();
        int i = m == 4 ? 3 : m == 3 ? 2 : m == 2 ? 1 : 0;
        if (m != 1)
            i = (i + dir + NPOSTFX) % NPOSTFX;
        gt_postfx_set(atoi(postfx_opts[i].value));
        ini_set("GT_POSTFX", postfx_opts[i].value,
                "PostFX: 0 Off (none, as shipped), 2 Classic (the paid iPad 2010 build's bloom), 3 Classic Natural "
                "(the same, the world's colours kept), 4 Enhanced (the game's own bloom balanced + Classic Natural, "
                "the default), 1 the game's own bloom (Options > Display)");
        set_text(pl, "Sel7.txtSelectorType", postfx_text());
        return;
    }
    values(pl);
    display_menu_layout(pl);
}

static struct { u32 frame; char what; char value[8]; } tests[32];
static int ntests = -1, next_test;

static void test_parse(void)
{
    ntests = 0;
    const char *e = dev_getenv("GT_TEST_DISPLAY");
    for (const char *s = e; s && *s && ntests < 32;) {
        unsigned f;
        char w[8], v[8];
        if (sscanf(s, "%u:%7[a-z]=%7[^;,]", &f, w, v) == 3 && (!strcmp(w, "postfx") || !strcmp(w, "blur"))) {
            tests[ntests].frame = f, tests[ntests].what = w[0];
            snprintf(tests[ntests].value, sizeof tests[ntests].value, "%s", v);
            ntests++;
        }
        s += strcspn(s, ";,");
        if (*s)
            s++;
    }
    if (ntests)
        dev_log("[display] test: %d change%s (GT_TEST_DISPLAY)\n", ntests, ntests > 1 ? "s" : "");
}

void gt_frame_end(void)
{
    if (ntests < 0)
        test_parse();
    u32 now = eagl_frames();
    while (next_test < ntests && tests[next_test].frame <= now) {
        const char *v = tests[next_test].value;
        dev_log("[display] test, frame %u: %s %s\n", now, tests[next_test].what == 'p' ? "PostFX" : "motion blur", v);
        if (tests[next_test].what == 'p')
            gt_postfx_set(atoi(v));
        else {
            int i = find_opt(blur_opts, NBLUR, v);
            blur_sel = i >= 0 ? i : blur_sel;
            fullblur_choose(v);
        }
        next_test++;
    }
    fullblur_frame_end();
}

static void (*aom_event_fn)(CPU *), (*aom_update_fn)(CPU *);

static void aom_update_hook(CPU *c)
{
    u32 self = c->r[0];
    aom_update_fn(c);
    u32 pl = rd32(self + 4);
    static u32 last;
    u32 now = eagl_frames();
    if (pl && now - last >= 30) {
        last = now;
        static const char path[] = "AllOptionsMenu.btnCustomSupport.txtTitle.field";
        if (find(pl, path))
            rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)path), GA("Display"), 0);
    }
}

static void aom_event_hook(CPU *c)
{
    u32 self = c->r[0], ev = c->r[1];
    u32 np = rd32(ev + 4);
    const char *name = np ? (const char *)GP(np) : "";
    if (!strcmp(name, "btnCustomSupport")) {
        pressed_at = eagl_frames();
        if (rd32(ev + 8) == 2)
            rt_log("[display] the Display tile (frame %u)\n", pressed_at);
        rt_call2(GT_FN_MENU_BASE_EVENT, self, ev);
        return;
    }
    if (!strncmp(name, "btn", 3))
        pressed_at = 0;
    aom_event_fn(c);
}

void display_test_page(void) { pressed_at = eagl_frames(); }

int display_take_page(void)
{
    int page = on > 0 && pressed_at && eagl_frames() - pressed_at < 240;
    pressed_at = 0;
    return page;
}

void display_init(void)
{
    const char *e = getenv("GT_DISPLAY_MENU");
    on = !(e && *e == '0');
    res_run = res_sel = find_opt(res_opts, NRES, getenv("GT_RESOLUTION"));
    wide_run = wide_sel = find_opt(wide_opts, NWIDE, getenv("GT_WIDE"));
    snprintf(res_custom, sizeof res_custom, "%s", getenv("GT_RESOLUTION") ? getenv("GT_RESOLUTION") : "");
    snprintf(wide_custom, sizeof wide_custom, "%s", getenv("GT_WIDE") ? getenv("GT_WIDE") : "");
    aa_auto = gpu_setting_auto("GT_MSAA");
    blur_sel = gpu_setting_auto("GT_FULL_BLUR") ? 0 : *getenv("GT_FULL_BLUR") == '0' ? 2 : 1;
    if (!on) {
        rt_log("[display] off (GT_DISPLAY_MENU=0)\n");
        return;
    }
    u32 slot = 0;
    for (u32 a = 0x780000u; a < 0x7a0000u; a += 4)
        if (rd32(a) == AOM_EVENT_FN) {
            slot = a;
            break;
        }
    aom_event_fn = rt_lookup(AOM_EVENT_FN);
    if (!slot || !aom_event_fn) {
        rt_log("[display] AllOptionsMenu's event handler not found: no Display menu\n");
        on = 0;
        return;
    }
    gt_unprotect(slot, 4);
    wr32(slot, rt_host_addr(aom_event_hook, "display aom_event_hook"));
    u32 upd = slot - GT_VT_EVENT + GT_VT_UPDATE;
    if (!gt_hook_slot(upd, rd32(upd), &aom_update_fn, aom_update_hook, "display aom_update_hook"))
        rt_log("[display] AllOptionsMenu's update unknown: the tile keeps its name\n");
    rt_log("[display] Display menu on (AllOptionsMenu handler at 0x%x); resolution %s, aspect %s\n", slot,
           res_sel >= 0 ? res_opts[res_sel].label : res_custom, wide_sel >= 0 ? wide_opts[wide_sel].label : wide_custom);
}
