/* softrefl.c -- SOFT_REFLECTION: the cars' live reflection made soft on the two Scandinavia tracks (by default) or
   on the tracks GT_SOFT_REFLECTION_TRACKS names.

   An enhancement, not a port fault: the original looks the same (identical with every GL optimisation, MSAA,
   full blur and widescreen off).

   The original (read from the code and traced in GL): in a race on every track the car shader
   (gtr_high_light_reflection, texture1 = a sphere map read at reflect(V, N)) reads a render target the game
   redraws each frame.  F_2a4ba4 (its only caller is the race frame F_16eee8, 0x16ef9c, in another generated
   file) counts the frames, makes the target once with F_2a46c0 (a texture, GL_LINEAR, its size a power of two
   from the screen: 512 on an iPad 2, 1024 here) into its +0x138, binds it (F_2a36ec) and returns 1 when it is
   drawn this frame; the caller then draws the scene into it as a wide view from the car.  On France that view
   is mostly sky over the road.  On Scandinavia (snowrally.ba2) and Scandinavia - Snowfield (snowrallyalt.ba2)
   orange walls, the start gantry (its checks) and cliffs stand right beside the car and fill it: the windows
   and the body show sharp streaks that change every frame.

   Here, on those two tracks (or the ones named): when the game leaves the framebuffer it drew the map into
   (glenhance.c h_glBindFramebuffer), the map's mipmaps are made and the reflection is read from level n
   (GL_TEXTURE_MIN_LOD), a smaller, blurred copy: the same live reflection, its colours without the streaks.
   On other tracks the map is put back as the game made it (GL_LINEAR, level 0).  The game's files and
   textures are not changed.
   GT_SOFT_REFLECTION=n: the level (1 a little softer .. 6; default 3); 0 = the original.
   GT_SOFT_REFLECTION_TRACKS: the tracks, by their file names in plain letters, comma-separated (blank: the two
   Scandinavia tracks, snowrally,snowrallyalt), or all.  The fifteen: blackforest, drag, france, lagunaseca,
   montecarlo, montreal, nurburg, snowrally, snowrallyalt, sydney, testoval, testring, us, usalt, valley.  Other
   tracks with walls or rock close to the road can show the same kind of streaks, less often; they are left to
   the player's choice. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "rt.h"
#include "features.h"

#define FB_BINDING     0x8CA6u
#define TEX_BINDING_2D 0x8069u
#define ATT_NAME       0x8CD1u   /* GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME */
#define MIN_FILTER     0x2801u
#define MIN_LOD        0x813Au
#define LINEAR         0x2601u
#define LINEAR_MIPMAP_LINEAR 0x2703u

void __real_F_2a4ba4(CPU *c);

static int level = -1;
static int soft_track;              /* the track loaded last is one the reflection is made soft on */
static u32 env_fb, env_tex;         /* the map's framebuffer and texture, as the game drew it last */
static u32 soft_tex;                /* the texture given a level here (put back off the snow tracks) */

static int want_level(void)
{
    if (level < 0) {
        const char *e = getenv("GT_SOFT_REFLECTION");
        level = e && *e ? atoi(e) : 3;
        if (level < 0)
            level = 0;
        if (level > 6)
            level = 6;
        rt_log(level ? "[softrefl] the cars' reflection on the listed tracks read from level %d "
                       "(SOFT_REFLECTION)\n"
                     : "[softrefl] off: the original reflection (GT_SOFT_REFLECTION=0)\n", level);
    }
    return level;
}

/* a bundle name in the two-letter code to plain letters ("UKRAREXATABUNNNNZX" -> "snowrally"); 0 if not one */
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

/* is the track (plain letters) one GT_SOFT_REFLECTION_TRACKS names? */
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

/* imp_libc.c: every file the game opens.  A track is <name>.ba2 ("snowrally" / "snowrallyalt" in the bundle's
   two-letter code both start UKRAREXATABUNNNNZX).  The game opens every track's file at its start (a scan),
   then a race's own track while the race loads, just before its render targets are made: the track opened
   last is the race's. */
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
    if (eagl_frames() > 10 && strcmp(last, b) && logged++ < 40)
        rt_log("[softrefl] track %s loaded (frame %u): %s\n", b, eagl_frames(),
               soft_track ? "the reflection made soft" : "the original reflection");
    snprintf(last, sizeof last, "%s", b);
}

/* the race frame's reflection map: drawn this frame when it returns 1 (its framebuffer bound) */
void __wrap_F_2a4ba4(CPU *c)
{
    __real_F_2a4ba4(c);
    if (c->r[0] != 1)
        return;
    u32 fb = 0, tex = 0;
    p_glGetIntegerv(FB_BINDING, GA(&fb));
    p_glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ATT_NAME, GA(&tex));
    env_fb = fb, env_tex = tex;
}

static void on_tex(u32 tex, void (*f)(u32 tex))
{
    u32 old = 0;                    /* on the active unit, as it was after */
    p_glGetIntegerv(TEX_BINDING_2D, GA(&old));
    p_glBindTexture(GL_TEXTURE_2D, tex);
    f(tex);
    p_glBindTexture(GL_TEXTURE_2D, old);
}

static void make_soft(u32 tex)
{
    p_glGenerateMipmap(GL_TEXTURE_2D);
    if (soft_tex != tex) {
        p_glTexParameteri(GL_TEXTURE_2D, MIN_FILTER, LINEAR_MIPMAP_LINEAR);
        p_glTexParameterf(GL_TEXTURE_2D, MIN_LOD, (float)level);
        soft_tex = tex;
    }
}

static void put_back(u32 tex)
{
    p_glTexParameterf(GL_TEXTURE_2D, MIN_LOD, -1000.0f);   /* GL's own default */
    p_glTexParameteri(GL_TEXTURE_2D, MIN_FILTER, LINEAR);
    (void)tex;
}

/* glenhance.c: the game binds framebuffer fb; when it leaves the map's, the map is drawn */
void softrefl_bind_fb(u32 fb)
{
    if (!env_fb || fb == env_fb)
        return;
    u32 cur = 0;
    p_glGetIntegerv(FB_BINDING, GA(&cur));
    if (cur != env_fb)
        return;
    if (soft_track && want_level() > 0 && env_tex)
        on_tex(env_tex, make_soft);
    else if (soft_tex) {
        on_tex(soft_tex, put_back);
        soft_tex = 0;
    }
}
