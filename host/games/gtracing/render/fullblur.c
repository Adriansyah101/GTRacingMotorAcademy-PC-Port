/* fullblur.c -- FULL_BLUR: the iPad 3 (Retina) mode keeps the iPad 2's full motion blur.

   Without it, the iPad 3 mode draws the lighter iPad 3 blur, as the original does.
   An enhancement, not a port fault: the original cuts the blur on purpose on an iPad 3.

   The original (read from the code): the device table F_28d72c puts a device class at 0x7a2da0 (iPad 2 = 9,
   iPad 3 = 10; both take the same branch for draw distances and assets).  Only two places treat 10 apart:
   - F_34a85c names the motion blur shader: gtr_motionBlur_fs_iPad3.glsl on an iPad 3 (two of the blur's
     samples dropped, no darkening of the screen's edges at speed), gtr_motionBlur_fs.glsl otherwise;
   - F_2a4f38 makes the blur's render target: x0.65 of the screen on an iPad 3, the full screen otherwise.
   Both are wrapped (GNU ld --wrap; their only callers are in code_010.c, and the dispatch table) and run
   with the class read as 9 or 10 as the choice asks; nothing they call reads the class.  The screen stays Retina.
   GT_FULL_BLUR=0 keeps the original iPad 3 blur.

   GPU_PROFILE (optimisation pass 5, gpuprofile.c: built-in GPUs get MSAA 4x and the 0.65x blur): GT_FULL_BLUR
   missing, blank or "auto" = full on a graphics card, the iPad 3's lighter blur on a built-in GPU.  And 0 (light)
   holds in the iPad 2 mode too (the iPad 3's path, its shader file is in every copy of the game), so a built-in
   GPU gets the lighter blur whichever art it shows.  1 in the iPad 2 mode is its own blur.

   Changed while the game runs (Options > Display > Motion Blur, fullblur_choose):
   - the shader: the game compiles each post effect's shader once, the first time it draws it (F_150ce4 keeps it),
     so F_34a85c always names the full gtr_motionBlur_fs.glsl and its source is given one host uniform,
     gt_blur_light (gt_blur_shader_patch, at glShaderSource; gles.c sets it at glUseProgram).  At 1 it is the
     iPad 3 file's code, term for term: the samples vTexcoordBlur1 and vTexcoordBlur6 weighted 0, the blend's t
     scaled by motionBlurRatio, no darkening of the edges.  At 0 it is the full file as shipped;
   - the target: the post effect object is made once (F_14ba30 makes it the first time a race asks for it, then
     F_2a4f38 its targets), so its blur target (+0x144) keeps the size it was made at.  At the end of a frame
     (gt_frame_end, display.c) the target's texture and its depth / stencil renderbuffer get the size the choice
     asks (the screen's, or x0.65 as F_2a4f38 computes it) and the target's own width and height (+4, +8), which
     F_2a36ec sets its viewport from.  Nothing else keeps the size: the texture's coordinates are 0..1. */
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "rt.h"
#include "features.h"
#include "game.h"

#define DEVICE_CLASS 0x7a2da0u      /* F_28d72c: 9 iPad 2, 10 iPad 3 */
#define POST_BLUR 0x144u            /* the post effect object's blur target (F_2a4f38) */
/* a render target (F_2a46c0, 0x9c bytes) */
#define RT_TEX 0x00u
#define RT_W 0x04u
#define RT_H 0x08u
#define RT_FORMAT 0x0cu
#define RT_DEPTH 0x94u
#define RT_HAS_DEPTH 0x98u

void __real_F_34a85c(CPU *c);      /* the motion blur shader's name */
void __real_F_2a4f38(CPU *c);      /* the post effects' render targets */

void h_glTexImage2D(u32 target, u32 level, u32 internal, u32 w, u32 h, u32 border, u32 format, u32 type, u32 pixels);
void h_glRenderbufferStorage(u32 target, u32 internal, u32 w, u32 h);

static int full = -1;               /* 1 the full blur (the iPad 2's), 0 the lighter one (the iPad 3's) */
static u32 post_obj;                /* the post effect object (F_2a4f38's), 0 before the first race */

static int device_ok(void)
{
    u32 cls = rd32(DEVICE_CLASS);
    return cls == 9 || cls == 10;
}

static int from_setting(const char *v, int *was_auto)
{
    int a = !v || !*v || !_stricmp(v, "auto");
    if (was_auto)
        *was_auto = a;
    return a ? gpu_auto_full_blur() : *v != '0';
}

/* 1 the full blur, 0 the lighter one: as chosen last (GT_FULL_BLUR at the start, then the Display menu) */
static int fullblur_full(void)
{
    if (full < 0) {
        int a;
        full = from_setting(gpu_setting_auto("GT_FULL_BLUR") ? "auto" : getenv("GT_FULL_BLUR"), &a);
        rt_log("[blur] %s motion blur (%s, %s art)%s\n", full ? "the iPad 2's full" : "the iPad 3's lighter",
               a ? "GT_FULL_BLUR auto: GPU_PROFILE" : "GT_FULL_BLUR", rd32(DEVICE_CLASS) == 10 ? "iPad 3" : "iPad 2",
               full == (rd32(DEVICE_CLASS) == 9) ? ", the device's own" : "");
    }
    return full;
}

/* Options > Display > Motion Blur (display.c): "auto", "1" or "0", taken at once (the target at the frame's end) */
void fullblur_choose(const char *v)
{
    int a;
    fullblur_full();
    full = from_setting(v, &a);
    rt_log("[blur] %s motion blur%s (Options > Display)\n", full ? "the iPad 2's full" : "the iPad 3's lighter",
           a ? " (auto)" : "");
}

/* the shaders' gt_blur_light (gles.c, at glUseProgram) */
static float gt_shader_blur_light(void) { return device_ok() && !fullblur_full() ? 1.0f : 0.0f; }

/* the full motion blur shader given gt_blur_light (gles.c, at glShaderSource; NULL: another shader, as it is) */
static char *gt_blur_shader_patch(const char *src)
{
    if (!strstr(src, "motionBlurRatio") || !strstr(src, "gl_FragColor") || !strstr(src, "vTexcoordBlur6"))
        return NULL;                    /* not the blur's fragment shader */
    static const struct { const char *was, *now; } edits[] = {
        {"sum += texture2D( texture0, vTexcoordBlur1);",
         "sum += (1.0 - gt_blur_light) * texture2D( texture0, vTexcoordBlur1);"},
        {"sum += texture2D( texture0, vTexcoordBlur6);",
         "sum += (1.0 - gt_blur_light) * texture2D( texture0, vTexcoordBlur6);"},
        {"* sampleStrength;", "* sampleStrength * mix(1.0, motionBlurRatio, gt_blur_light);"},
        {"* (1.0-dist*1.6*motionBlurRatio);", "* mix(1.0-dist*1.6*motionBlurRatio, 1.0, gt_blur_light);"},
    };
    enum { N = sizeof edits / sizeof edits[0] };
    size_t n = strlen(src);
    char *out = malloc(n + 512);
    if (!out)
        return NULL;
    static const char head[] = "uniform float gt_blur_light;\n";
    memcpy(out, head, sizeof head - 1);
    char *o = out + sizeof head - 1;
    int which = 0;
    for (const char *p = src; *p;) {
        int k = 0;
        while (k < N && strncmp(p, edits[k].was, strlen(edits[k].was)))
            k++;
        if (k < N && !(which & 1 << k)) {
            size_t l = strlen(edits[k].now);
            memcpy(o, edits[k].now, l);
            o += l;
            p += strlen(edits[k].was);
            which |= 1 << k;
        } else
            *o++ = *p++;
    }
    *o = 0;
    if (which != (1 << N) - 1) {        /* not the file this was written for: as it is */
        rt_log("[blur] the motion blur shader is not the one expected (edits 0x%x): left as it is\n", which);
        free(out);
        return NULL;
    }
    rt_log("[blur] the motion blur shader given gt_blur_light (the lighter blur switched at once)\n");
    return out;
}

static void as_blur(CPU *c, void (*f)(CPU *), int want)
{
    u32 cls = rd32(DEVICE_CLASS);
    if ((cls == 9 || cls == 10) && cls != (u32)want) {
        wr32(DEVICE_CLASS, (u32)want);
        f(c);
        wr32(DEVICE_CLASS, cls);
    } else
        f(c);
}

/* (?, effect, &vertex shader name, &fragment shader name): the shader files of one post effect.  Always the full
   blur's file: its lighter variant is gt_blur_light's. */
void __wrap_F_34a85c(CPU *c)
{
    static int told;
    u32 fs = c->r[3];
    fullblur_full();
    as_blur(c, __real_F_34a85c, 9);
    const char *name = rd32(fs) ? (const char *)GP(rd32(fs)) : "";
    static int trace = -1, n;
    if (trace < 0)
        trace = dev_getenv("GT_SHADER_LOG") != NULL;
    if (GT_DEV && trace && n < 60) {
        n++;
        rt_log("[shader] %s\n", name);
    }
    if (!told && strstr(name, "motionBlur")) {
        told = 1;
        rt_log("[blur] motion blur shader: %s\n", name);
    }
}

/* the post effect object's targets, made once.  The game's own bloom's targets are made whatever PostFX is
   (postfx.c gt_postfx_targets), so the game's own bloom can be switched on at once. */
void __wrap_F_2a4f38(CPU *c)
{
    static int n;
    if (n++ < 8)
        rt_log("[blur] post effect targets made (frame %u)\n", eagl_frames());
    post_obj = c->r[0];
    gt_postfx_targets(1);
    as_blur(c, __real_F_2a4f38, fullblur_full() ? 9 : 10);
    gt_postfx_targets(0);
}

/* the end of a frame (eagl.c, through display.c's gt_frame_end): the blur target at the size the choice asks */
void fullblur_frame_end(void)
{
    if (!post_obj || !device_ok())
        return;
    u32 t = rd32(post_obj + POST_BLUR);
    if (!t)
        return;
    u32 w = rd32(GT_SCREEN_PX_W), h = rd32(GT_SCREEN_PX_H);
    if (!fullblur_full())               /* as F_2a4f38 computes it */
        w = (u32)(int)((float)(int)w * 0.65f), h = (u32)(int)((float)(int)h * 0.65f);
    if (!w || !h || (rd32(t + RT_W) == w && rd32(t + RT_H) == h))
        return;
    u32 active, tex0, rb;
    gles_state(GL_ACTIVE_TEXTURE, &active);
    gles_state(GL_RENDERBUFFER_BINDING, &rb);
    p_glActiveTexture(GL_TEXTURE0);
    gles_state(GL_TEXTURE_BINDING_2D, &tex0);       /* unit 0's */
    u32 fmt = rd32(t + RT_FORMAT);
    p_glBindTexture(GL_TEXTURE_2D, rd32(t + RT_TEX));
    h_glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, 0);
    p_glBindTexture(GL_TEXTURE_2D, tex0);
    p_glActiveTexture(active);
    if (*(volatile u8 *)GP(t + RT_HAS_DEPTH) && rd32(t + RT_DEPTH)) {
        gles_bind_rb(rd32(t + RT_DEPTH));
        h_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);    /* GL_DEPTH24_STENCIL8, as F_2a46c0 makes it */
        gles_bind_rb(rb);
    }
    rt_log("[blur] the blur target %u x %u -> %u x %u (frame %u)\n", rd32(t + RT_W), rd32(t + RT_H), w, h,
           eagl_frames());
    wr32(t + RT_W, w);
    wr32(t + RT_H, h);
}

/* the hooks into the GL layer (gl.h): the motion blur shader's patch and gt_blur_light, before any shader is made */
__attribute__((constructor)) static void fullblur_hooks(void)
{
    gles_add_shader_patch(gt_blur_shader_patch);
    gles_add_host_uniform("gt_blur_light", gt_shader_blur_light);
}
