#include <stdlib.h>

#include "gl.h"
#include "features.h"

#define FB_BINDING      0x8CA6u
#define READ_FB_BINDING 0x8CAAu
#define READ_FB         0x8CA8u
#define DRAW_FB         0x8CA9u
#define MAX_SAMPLES     0x8D57u
#define ANISO           0x84FEu
#define MAX_ANISO       0x84FFu
#define DEPTH24_STENCIL8 0x88F0u

static int samples = -1, max_samples;
static int pending_samples = -1;
static u32 main_fbo, ms_fbo, ms_color, ms_depth;
static SDL_GLContext main_ctx;
static int ms_w, ms_h, ms_failed;

static struct { u32 fbo, rb; SDL_GLContext ctx; } color_att[16];
static int ncolor_att;

static int want_samples(void)
{
    if (samples < 0) {
        int n = gpu_setting_auto("GT_MSAA") ? gpu_auto_msaa() : rt_setting_int("GT_MSAA", gpu_auto_msaa(), 0, 64), mx = 0;
        p_glGetIntegerv(MAX_SAMPLES, GA(&mx));
        max_samples = mx;
        if (n > mx)
            n = mx;
        samples = n > 1 ? n : 0;
        rt_log(samples ? "[msaa] %d samples (driver's most %d)\n" : "[msaa] off (driver's most %d)\n",
               samples ? samples : mx, mx);
    }
    return samples;
}

static u32 raw_binding(u32 which)
{
    u32 v = 0;
    p_glGetIntegerv(which, GA(&v));
    return v;
}

static int ms_ready(void)
{
    if (!main_fbo || ms_failed || SDL_GL_GetCurrentContext() != main_ctx || !want_samples())
        return 0;
    u32 rb;
    int w, h;
    eagl_drawable(&rb, &w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    if (ms_fbo && w == ms_w && h == ms_h)
        return 1;
    u32 old_fb = raw_binding(FB_BINDING), old_rb = raw_binding(GL_RENDERBUFFER_BINDING);
    if (!ms_fbo) {
        p_glGenFramebuffers(1, GA(&ms_fbo));
        p_glGenRenderbuffers(1, GA(&ms_color));
        p_glGenRenderbuffers(1, GA(&ms_depth));
    }
    gles_bind_rb(ms_color);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, (u32)samples, GL_RGBA8, (u32)w, (u32)h);
    gles_bind_rb(ms_depth);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, (u32)samples, DEPTH24_STENCIL8, (u32)w, (u32)h);
    gles_bind_fb(GL_FRAMEBUFFER, ms_fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms_color);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ms_depth);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ms_depth);
    u32 st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gles_bind_rb(old_rb);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("[msaa] framebuffer incomplete (0x%x): off\n", st);
        ms_failed = 1;
        gles_bind_fb(GL_FRAMEBUFFER, old_fb == ms_fbo ? main_fbo : old_fb);
        return 0;
    }
    ms_w = w;
    ms_h = h;
    rt_log("[msaa] framebuffer %u: %d x %d, %d samples, in place of the game's %u\n", ms_fbo, w, h, samples,
           main_fbo);
    gles_bind_fb(GL_FRAMEBUFFER, old_fb == main_fbo ? ms_fbo : old_fb);
    return 1;
}

void glx_drawable_changed(u32 rb)
{
    if (!main_fbo)
        for (int i = 0; i < ncolor_att; i++)
            if (color_att[i].rb == rb) {
                main_fbo = color_att[i].fbo;
                main_ctx = color_att[i].ctx;
                rt_log("[msaa] the game's main framebuffer is %u\n", main_fbo);
            }
    if (SDL_GL_GetCurrentContext() == main_ctx)
        ms_ready();
}

u32 glx_present_fbo(void)
{
    return ms_fbo && ms_w && samples > 0 && !ms_failed && SDL_GL_GetCurrentContext() == main_ctx ? ms_fbo : 0;
}

u32 glx_logical_fbo(u32 raw)
{
    return raw && raw == ms_fbo && glx_present_fbo() ? main_fbo : raw;
}

void h_glBindFramebuffer(u32 target, u32 fb)
{
    softrefl_bind_fb(fb);
    if (fb && fb == main_fbo && ms_ready())
        fb = ms_fbo;
    gles_bind_fb(target, fb);
}

#define ON_MAIN(call)                                                         \
    do {                                                                      \
        int swap_ = ms_fbo && raw_binding(FB_BINDING) == ms_fbo;              \
        if (swap_)                                                            \
            gles_bind_fb(GL_FRAMEBUFFER, main_fbo);                    \
        call;                                                                 \
        if (swap_)                                                            \
            gles_bind_fb(GL_FRAMEBUFFER, ms_fbo);                      \
    } while (0)

void h_glFramebufferRenderbuffer(u32 target, u32 att, u32 rbt, u32 rb)
{
    ON_MAIN(p_glFramebufferRenderbuffer(target, att, rbt, rb));
    if (att != GL_COLOR_ATTACHMENT0 || !rb)
        return;
    u32 fb = glx_logical_fbo(raw_binding(FB_BINDING));
    if (ncolor_att < 16) {
        color_att[ncolor_att].fbo = fb;
        color_att[ncolor_att].rb = rb;
        color_att[ncolor_att].ctx = SDL_GL_GetCurrentContext();
        ncolor_att++;
    }
    u32 drb;
    int w, h;
    eagl_drawable(&drb, &w, &h);
    if (!main_fbo && drb && rb == drb) {
        main_fbo = fb;
        main_ctx = SDL_GL_GetCurrentContext();
        rt_log("[msaa] the game's main framebuffer is %u\n", main_fbo);
        ms_ready();
    }
}

void h_glFramebufferTexture2D(u32 target, u32 att, u32 tt, u32 tex, u32 level)
{
    ON_MAIN(p_glFramebufferTexture2D(target, att, tt, tex, level));
}

void h_glGetFramebufferAttachmentParameteriv(u32 target, u32 att, u32 pname, u32 out)
{
    ON_MAIN(p_glGetFramebufferAttachmentParameteriv(target, att, pname, out));
}

void h_glReadPixels(u32 x, u32 y, u32 w, u32 h, u32 format, u32 type, u32 out)
{
    if (!ms_fbo || raw_binding(READ_FB_BINDING) != ms_fbo) {
        p_glReadPixels(x, y, w, h, format, type, out);
        return;
    }
    gles_bind_fb(READ_FB, ms_fbo);
    gles_bind_fb(DRAW_FB, main_fbo);
    p_glBlitFramebuffer(0, 0, (u32)ms_w, (u32)ms_h, 0, 0, (u32)ms_w, (u32)ms_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gles_bind_fb(READ_FB, main_fbo);
    p_glReadPixels(x, y, w, h, format, type, out);
    gles_bind_fb(GL_FRAMEBUFFER, ms_fbo);
}

static float aniso = -1, max_aniso;
static float pending_aniso = -1;
static unsigned aniso_game, aniso_host;
static u8 aniso_seen[65536 / 8];
#define TEX_BINDING_2D 0x8069u

static float aniso_level(float n)
{
    if (n <= 0)
        return 0;
    if (n <= 1)
        return 1;
    return n > max_aniso ? max_aniso : n;
}

static float want_aniso(void)
{
    if (aniso < 0) {
        p_glGetFloatv(MAX_ANISO, GA(&max_aniso));
        aniso = aniso_level(rt_setting_float("GT_ANISOTROPIC", 16, 0, 16));
        rt_log(aniso > 1 ? "[aniso] %gx on mipmapped textures (driver's most %g)\n"
               : aniso == 1 ? "[aniso] off (%g; driver's most %g)\n" : "[aniso] the game's own (%g; driver's most %g)\n",
               aniso, max_aniso);
    }
    return aniso;
}

static void seen(void)
{
    u32 t = 0;
    p_glGetIntegerv(TEX_BINDING_2D, GA(&t));
    if (t && t < 65536)
        aniso_seen[t >> 3] |= (u8)(1 << (t & 7));
}

static void set_aniso(u32 target)
{
    if (target != GL_TEXTURE_2D || want_aniso() < 1)
        return;
    p_glTexParameterf(target, ANISO, aniso);
    seen();
    if (!(aniso_host++ & 1023))
        rt_log("[aniso] %u textures set by the host, %u asked by the game\n", aniso_host, aniso_game);
}

static int mip_filter(u32 v) { return v >= GL_NEAREST_MIPMAP_NEAREST && v <= GL_LINEAR_MIPMAP_LINEAR; }

void h_glTexParameteri(u32 target, u32 pname, u32 v)
{
    p_glTexParameteri(target, pname, v);
    if (pname == GL_TEXTURE_MIN_FILTER && mip_filter(v))
        set_aniso(target);
}

void h_glTexParameterf(u32 target, u32 pname, float v)
{
    if (pname == ANISO) {
        aniso_game++;
        if (want_aniso() >= 1)
            v = aniso;
        if (target == GL_TEXTURE_2D)
            seen();
    }
    p_glTexParameterf(target, pname, v);
    if (pname == GL_TEXTURE_MIN_FILTER && mip_filter((u32)v))
        set_aniso(target);
}

void h_glGenerateMipmap(u32 target)
{
    p_glGenerateMipmap(target);
    set_aniso(target);
}

void glx_tex_level(u32 target, u32 level)
{
    if (level == 1)
        set_aniso(target);
}

int glx_msaa_now(void) { return samples > 0 ? samples : 0; }
int glx_msaa_max(void) { return max_samples; }
float glx_aniso_now(void) { return aniso; }
float glx_aniso_max(void) { return max_aniso; }
void glx_set_msaa(int n) { pending_samples = n < 0 ? 0 : n; }
void glx_set_aniso(float n) { pending_aniso = n < 0 ? 0 : n; }

void glx_frame_end(void)
{
    if (pending_samples >= 0 && max_samples && SDL_GL_GetCurrentContext() == main_ctx) {
        int n = pending_samples > max_samples ? max_samples : pending_samples;
        pending_samples = -1;
        n = n > 1 ? n : 0;
        if (n != samples) {
            samples = n;
            ms_w = ms_h = 0;
            ms_failed = 0;
            u32 cur = raw_binding(FB_BINDING);
            if (samples)
                ms_ready();
            else if (cur == ms_fbo)
                gles_bind_fb(GL_FRAMEBUFFER, main_fbo);
            rt_log(samples ? "[msaa] now %d samples\n" : "[msaa] now off\n", samples);
        }
    }
    if (pending_aniso >= 0 && max_aniso > 0) {
        float n = aniso_level(pending_aniso);
        pending_aniso = -1;
        if (n != aniso) {
            aniso = n;
            u32 active = 0, was = 0, count = 0;
            p_glGetIntegerv(GL_ACTIVE_TEXTURE, GA(&active));
            p_glActiveTexture(GL_TEXTURE0);
            p_glGetIntegerv(TEX_BINDING_2D, GA(&was));
            for (u32 t = 1; t < 65536; t++)
                if ((aniso_seen[t >> 3] & (1 << (t & 7))) && p_glIsTexture(t)) {
                    p_glBindTexture(GL_TEXTURE_2D, t);
                    p_glTexParameterf(GL_TEXTURE_2D, ANISO, n >= 1 ? n : 1);
                    count++;
                }
            p_glBindTexture(GL_TEXTURE_2D, was);
            p_glActiveTexture(active);
            while (p_glGetError())
                ;
            rt_log("[aniso] now %s (%u textures)\n", n > 1 ? "on" : "off", count);
            if (n > 1)
                rt_log("[aniso] level %g\n", n);
        }
    }
}