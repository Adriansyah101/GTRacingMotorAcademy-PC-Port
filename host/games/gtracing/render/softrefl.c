#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include "compat.h"
#endif

#include "gl.h"
#include "rt.h"
#include "features.h"

#define FB_BINDING     0x8CA6u
#define TEX_BINDING_2D 0x8069u
#define ACTIVE_TEXTURE 0x84E0u
#define TEXTURE0       0x84C0u
#define DEPTH_TEST     0x0B71u
#define BLEND          0x0BE2u
#define MIN_FILTER     0x2801u
#define MIN_LOD        0x813Au
#define LINEAR_MIPMAP_LINEAR 0x2703u

void __real_F_2a4ba4(CPU *c);

static int level = -1;
static int soft_track;
static u32 env_fb;
static int in_map;
static u32 sky_tex;
static u32 sky_filter;
static u32 mipped_tex;
static volatile unsigned map_frame;

static int want_level(void)
{
    if (level < 0) {
        const char *e = getenv("GT_SOFT_REFLECTION");
        level = e && *e ? atoi(e) : 3;
        if (level < 0)
            level = 0;
        if (level > 6)
            level = 6;
        rt_log(level ? "[softrefl] the backdrop of the cars' reflection on the listed tracks read from level %d "
                       "(SOFT_REFLECTION)\n"
                     : "[softrefl] off: the original reflection (GT_SOFT_REFLECTION=0)\n", level);
    }
    return level;
}

static int plain_name(const char *code, size_t n, char *out, size_t cap)
{
    static const char *const pairs[] = {
        "BUa", "BWb", "C4c", "DSd", "EZe", "FAf", "GOg", "HIh", "K0i", "LOj", "MPk", "NNl", "QYm", "RAn", "REo", "ROp",
        "SIq", "TAr", "UKs", "VWt", "WUu", "WVv", "XAw", "Y3x", "ZXy", "ZZz", "A10", "AA1", "AD2", "AF3", "AZ4", "B05",
        "BB6", "BR7", "BS8", "BT9", "UT_"};
    const size_t np = sizeof pairs / sizeof *pairs;
    size_t k = 0;
    if (n % 2 || n / 2 >= cap)
        return 0;
    for (size_t i = 0; i < n; i += 2) {
        size_t j = 0;
        while (j < np && strncmp(pairs[j], code + i, 2))
            j++;
        if (j == np)
            return 0;
        out[k++] = pairs[j][2];
    }
    out[k] = 0;
    return 1;
}

static int listed(const char *track)
{
    static char list[256];
    static int read;
    if (!read) {
        read = 1;
        const char *e = getenv("GT_SOFT_REFLECTION_TRACKS");
        snprintf(list, sizeof list, ",%s,", e && *e ? e : "snowrally,snowrallyalt");
        for (char *q = list; *q; q++)
            *q = *q == ' ' ? ',' : (char)tolower((unsigned char)*q);
        rt_log("[softrefl] tracks: %s\n", e && *e ? e : "snowrally,snowrallyalt (the two Scandinavia tracks)");
    }
    if (strstr(list, ",all,"))
        return 1;
    char key[48];
    snprintf(key, sizeof key, ",%s,", track);
    return strstr(list, key) != NULL;
}

void softrefl_note_file(const char *path)
{
    size_t n = strlen(path);
    if (n < 4 || _stricmp(path + n - 4, ".ba2"))
        return;
    const char *b = path + n;
    while (b > path && b[-1] != '/' && b[-1] != '\\')
        b--;
    static char last[64];
    static int logged;
    char track[32];
    soft_track = plain_name(b, (size_t)(path + n - 4 - b), track, sizeof track) && listed(track);
    mipped_tex = 0;
    if (eagl_frames() > 10 && strcmp(last, b) && logged++ < 40)
        rt_log("[softrefl] track %s loaded (frame %u): %s\n", b, eagl_frames(),
               soft_track ? "the reflection's backdrop made soft" : "the original reflection");
    snprintf(last, sizeof last, "%s", b);
}

void __wrap_F_2a4ba4(CPU *c)
{
    __real_F_2a4ba4(c);
    if (c->r[0] != 1)
        return;
    u32 fb = 0;
    p_glGetIntegerv(FB_BINDING, GA(&fb));
    env_fb = fb;
    in_map = 1;
    map_frame = eagl_frames();
}

static void on_tex(u32 tex, void (*f)(u32 tex))
{
    u32 unit = 0, old = 0;
    p_glGetIntegerv(ACTIVE_TEXTURE, GA(&unit));
    if (unit != TEXTURE0)
        p_glActiveTexture(TEXTURE0);
    p_glGetIntegerv(TEX_BINDING_2D, GA(&old));
    if (old != tex)
        p_glBindTexture(GL_TEXTURE_2D, tex);
    f(tex);
    if (old != tex)
        p_glBindTexture(GL_TEXTURE_2D, old);
    if (unit != TEXTURE0)
        p_glActiveTexture(unit);
}

static void put_back(u32 tex)
{
    p_glTexParameterf(GL_TEXTURE_2D, MIN_LOD, -1000.0f);
    p_glTexParameteri(GL_TEXTURE_2D, MIN_FILTER, sky_filter);
    (void)tex;
}

static void make_soft(u32 tex)
{
    if (mipped_tex != tex) {
        p_glGenerateMipmap(GL_TEXTURE_2D);
        mipped_tex = tex;
    }
    p_glGetTexParameteriv(GL_TEXTURE_2D, MIN_FILTER, GA(&sky_filter));
    p_glTexParameteri(GL_TEXTURE_2D, MIN_FILTER, LINEAR_MIPMAP_LINEAR);
    p_glTexParameterf(GL_TEXTURE_2D, MIN_LOD, (float)level);
}

void softrefl_draw(void)
{
    if (!in_map || !soft_track || want_level() <= 0)
        return;
    u32 depth = 0, blend = 0, unit = 0, tex = 0;
    p_glGetIntegerv(DEPTH_TEST, GA(&depth));
    p_glGetIntegerv(BLEND, GA(&blend));
    if ((depth & 0xff) || (blend & 0xff))
        return;
    p_glGetIntegerv(ACTIVE_TEXTURE, GA(&unit));
    if (unit != TEXTURE0)
        p_glActiveTexture(TEXTURE0);
    p_glGetIntegerv(TEX_BINDING_2D, GA(&tex));
    if (unit != TEXTURE0)
        p_glActiveTexture(unit);
    if (!tex || tex == sky_tex)
        return;
    if (sky_tex)
        on_tex(sky_tex, put_back);
    sky_tex = tex;
    on_tex(tex, make_soft);
}

void softrefl_bind_fb(u32 fb)
{
    if (!in_map || fb == env_fb)
        return;
    in_map = 0;
    if (sky_tex) {
        on_tex(sky_tex, put_back);
        sky_tex = 0;
    }
}

static const char glass_head[] =
    "uniform mediump float gt_glass_soft;\n";
static const char glass_func[] =
    "#if COLORSWAP == 0\n"
    "lowp vec4 gt_glass_env(highp vec2 uv, lowp vec4 c)\n"
    "{\n"
    "\tif (gt_glass_soft <= 0.0)\n"
    "\t\treturn c;\n"
    "\tlowp vec4 s = c;\n"
    "\tfor (int i = 0; i < 8; i++) {\n"
    "\t\thighp float a = float(i) * 0.785398;\n"
    "\t\thighp vec2 d = vec2(cos(a), sin(a)) * gt_glass_soft;\n"
    "\t\ts += texture2D(texture1, uv + d);\n"
    "\t\ts += texture2D(texture1, uv + 0.5 * vec2(d.x - d.y, d.x + d.y) * 0.7071068);\n"
    "\t}\n"
    "\treturn s / 17.0;\n"
    "}\n"
    "#endif\n\n";
static const char glass_mark[] = "reflectivity *= 0.5;";
static const char glass_call[] = "envColor = gt_glass_env(vTexCoord1, envColor);\n\t";

static char *glass_patch(const char *src)
{
    const char *at = strstr(src, glass_mark), *m = strstr(src, "void main()");
    if (!at || !m || m > at || !strstr(src, "vTexCoord1"))
        return NULL;
    size_t n = strlen(src);
    char *out = malloc(n + sizeof glass_head + sizeof glass_func + sizeof glass_call);
    if (!out)
        return NULL;
    char *o = out;
    memcpy(o, glass_head, sizeof glass_head - 1), o += sizeof glass_head - 1;
    memcpy(o, src, (size_t)(m - src)), o += m - src;
    memcpy(o, glass_func, sizeof glass_func - 1), o += sizeof glass_func - 1;
    memcpy(o, m, (size_t)(at - m)), o += at - m;
    memcpy(o, glass_call, sizeof glass_call - 1), o += sizeof glass_call - 1;
    strcpy(o, at);
    static int told;
    if (!told++)
        rt_log("[softrefl] the cars' glass shader given gt_glass_soft (SOFT_REFLECTION)\n");
    return out;
}

static float glass_soft(void)
{
    if (!soft_track || eagl_frames() - map_frame > 4 || want_level() <= 0)
        return 0.0f;
    return 0.004f * (float)level;
}

__attribute__((constructor)) static void softrefl_hooks(void)
{
    gles_add_shader_patch(glass_patch);
    gles_add_host_uniform("gt_glass_soft", glass_soft);
}
