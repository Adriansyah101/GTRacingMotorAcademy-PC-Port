#include <ctype.h>
#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "gl.h"
#include "game_hooks.h"
#include "gles_internal.h"
#include "liveblocks.h"
#include "ui.h"

#define GLP(name, params, args) T_##name p_##name;
#define GLR(ret, name, params, args) T_##name p_##name;
#define GLX(ret, name, params) T_##name p_##name;
#include "gl_list.h"

#define GLP(name, params, args) void h_##name params { p_##name args; }
#define GLR(ret, name, params, args) ret h_##name params { return p_##name args; }
#include "gl_list.h"

static int loaded;
static char gpu_vendor[128], gpu_renderer[128];

#define GL_DEBUG_LOG_MAX 200
static void GT_GLAPI gl_debug_cb(u32 source, u32 type, u32 id, u32 severity, int len, const char *msg, const void *user)
{
    (void)source, (void)id, (void)len, (void)user;
    static volatile LONG n;
    if (type != GL_DEBUG_TYPE_ERROR && severity != GL_DEBUG_SEVERITY_HIGH)
        return;
    if (InterlockedIncrement(&n) <= GL_DEBUG_LOG_MAX) {
        u32 fbo = 0, prog = 0, status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, GA(&fbo));
        p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
        rt_log("[gl debug] %s (thread %lu, frame %u, fbo %u status 0x%x, program %u)\n", msg, GetCurrentThreadId(),
               eagl_frames(), fbo, status, prog);
    }
}
static void gl_debug_on(void)
{
    typedef void(GT_GLAPI * T_cb)(u32, u32, u32, u32, int, const char *, const void *);
    typedef void(GT_GLAPI * T_set)(T_cb, const void *);
    T_set set = (T_set)SDL_GL_GetProcAddress("glDebugMessageCallback");
    if (!set) {
        rt_log("[gl] no glDebugMessageCallback\n");
        return;
    }
    p_glEnable(GL_DEBUG_OUTPUT);
    p_glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    set(gl_debug_cb, NULL);
    rt_log("[gl] debug output on\n");
}

void gles_load(void)
{
    if (loaded)
        return;
    int missing = 0;
#define LOAD(name)                                                                    \
    p_##name = (T_##name)SDL_GL_GetProcAddress(#name);                                \
    if (!p_##name) {                                                                  \
        rt_log("[gl] %s is missing from this OpenGL\n", #name);                       \
        missing++;                                                                    \
    }
#define GLP(name, params, args) LOAD(name)
#define GLR(ret, name, params, args) LOAD(name)
#define GLX(ret, name, params) LOAD(name)
#include "gl_list.h"
#undef LOAD
    loaded = 1;
    rt_log("[gl] %s / %s / OpenGL %s%s\n", (const char *)p_glGetString(GL_VENDOR),
           (const char *)p_glGetString(GL_RENDERER), (const char *)p_glGetString(GL_VERSION),
           missing ? " (entry points missing, see above)" : "");
    const char *v = (const char *)p_glGetString(GL_VENDOR), *r = (const char *)p_glGetString(GL_RENDERER);
    snprintf(gpu_vendor, sizeof gpu_vendor, "%s", v ? v : "");
    snprintf(gpu_renderer, sizeof gpu_renderer, "%s", r ? r : "");
    gpu_profile_start();
}

const char *gles_gpu_vendor(void) { return gpu_vendor; }
const char *gles_gpu_renderer(void) { return gpu_renderer; }

void gles_context_created(void)
{
    if (dev_getenv("GT_GLDEBUG"))
        gl_debug_on();
}

static void once(const char *what)
{
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what)
            return;
        if (!seen[i]) {
            seen[i] = what;
            rt_log("[gl] %s\n", what);
            return;
        }
    }
}

int gles_api = 2;

u32 h_glGetString(u32 name)
{
    static const char ext[] =
        "GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_mapbuffer GL_OES_element_index_uint "
        "GL_OES_framebuffer_object GL_OES_blend_func_separate GL_OES_texture_mirrored_repeat "
        "GL_IMG_texture_compression_pvrtc GL_EXT_texture_filter_anisotropic GL_EXT_read_format_bgra "
        "GL_APPLE_texture_2D_limited_npot GL_IMG_read_format GL_EXT_blend_minmax ";
    switch (name) {
    case GL_VENDOR:
        return GA("Imagination Technologies");
    case GL_RENDERER:
        return GA("PowerVR SGX 543");
    case GL_VERSION:
        return GA(gles_api == 1 ? "OpenGL ES-CM 1.1 IMGSGX543-63.14.2" : "OpenGL ES 2.0 IMGSGX543-63.14.2");
    case GL_SHADING_LANGUAGE_VERSION:
        return GA("OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS:
        return GA(ext);
    }
    return p_glGetString(name);
}

void h_glGetIntegerv(u32 pname, u32 out)
{
    int *v = GP(out);
    switch (pname) {
    case GL_MAX_VERTEX_UNIFORM_VECTORS:
        p_glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, out);
        *v /= 4;
        return;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
        p_glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, out);
        *v /= 4;
        return;
    case GL_MAX_VARYING_VECTORS:
        p_glGetIntegerv(GL_MAX_VARYING_FLOATS, out);
        *v /= 4;
        return;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE:
        *v = GL_UNSIGNED_BYTE;
        return;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        *v = GL_RGBA;
        return;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
        *v = 4;
        return;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        v[0] = GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, v[1] = GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, v[2] = GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, v[3] = GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG;
        return;
    case GL_SHADER_COMPILER:
        *v = 1;
        return;
    case GL_NUM_SHADER_BINARY_FORMATS:
        *v = 0;
        return;
    case GL_MAX_TEXTURE_SIZE:
        p_glGetIntegerv(pname, out);
        if (*v > 4096)
            *v = 4096;
        return;
    case GL_FRAMEBUFFER_BINDING: {
        p_glGetIntegerv(pname, out);
        *v = (int)glx_logical_fbo((u32)*v);
        return;
    }
    }
    p_glGetIntegerv(pname, out);
}

void h_glClearDepthf(float d) { p_glClearDepth(d); }

u32 h_glMapBufferOES(u32 target, u32 access) { return p_glMapBuffer(target, access); }
u32 h_glUnmapBufferOES(u32 target) { return p_glUnmapBuffer(target) & 0xff; }

void h_glCurrentPaletteMatrixOES(u32 m) { (void)m; once("OES_matrix_palette (glCurrentPaletteMatrixOES) is not supported"); }
void h_glMatrixIndexPointerOES(u32 a, u32 b, u32 c_, u32 d) { (void)a, (void)b, (void)c_, (void)d; once("OES_matrix_palette (glMatrixIndexPointerOES) is not supported"); }
void h_glWeightPointerOES(u32 a, u32 b, u32 c_, u32 d) { (void)a, (void)b, (void)c_, (void)d; once("OES_matrix_palette (glWeightPointerOES) is not supported"); }

#define MAX_ATTR 16
typedef struct {
    u32 on, size, type, norm, stride, ptr, vbo;
    float *conv;
    u32 conv_n;
} Attr;
typedef struct {
    Attr a[MAX_ATTR];
    u32 array_buf, elem_buf;
    u32 draw_mode, draw_count, draw_type, draw_ptr, draws;
    struct { u32 lo, hi; } ok[16];
    u32 ok_next, ok_frame;
    u32 del_seen;
    u32 client;
    u32 bufarr;
    u32 sc_unit, sc_tex[32], sc_prog, sc_bsrc, sc_bdst;
    u32 sc_tex_gen, sc_prog_gen;
    u32 sc_caps_known, sc_caps_on;
    u32 sc_dmask, sc_cmask;
    u32 sc_abuf_gen, sc_ebuf_gen;
    u32 sc_arr_known, sc_arr_on;
    u32 tk_known, tk_prog, tk_unit, tk_tex0, tk_rfb, tk_dfb, tk_rb, tk_scissor_on, tk_box[4];
    float tk_clear[4];
    u32 sv_ring, sv_iring, sv_off, sv_ioff, sv_moved;
    void *sv_ctx;
    unsigned char *sv_map[2];
    u32 sv_seg[2];
    void *sv_fence[2][4];
} VState;
enum { TK_PROG = 1, TK_UNIT = 2, TK_TEX0 = 4, TK_RFB = 8, TK_DFB = 16, TK_RB = 32, TK_SCISSOR_ON = 64, TK_BOX = 128,
       TK_CLEAR = 256 };
#define SC_UNKNOWN 0xffffffffu
static void sc_forget(VState *s)
{
    s->sc_unit = s->sc_prog = s->sc_bsrc = s->sc_bdst = SC_UNKNOWN;
    memset(s->sc_tex, 0xff, sizeof s->sc_tex);
    s->sc_caps_known = 0;
    s->sc_dmask = s->sc_cmask = SC_UNKNOWN;
    s->sc_abuf_gen = s->sc_ebuf_gen = SC_UNKNOWN;
    s->sc_arr_known = 0;
}
static VState *vstate_new(void)
{
    VState *s = calloc(1, sizeof *s);
    if (s)
        sc_forget(s);
    return s;
}
static volatile LONG del_gen;
static SRWLOCK del_lock = SRWLOCK_INIT;
static void gles_draw_lock(void) { AcquireSRWLockShared(&del_lock); }
static void gles_draw_unlock(void) { ReleaseSRWLockShared(&del_lock); }
static __thread VState *vs_;
static struct { void *gl; VState *s; } ctx_vs[16];
static SRWLOCK ctx_lock = SRWLOCK_INIT;
void gles_context_current(void *gl)
{
    if (!gl)
        return;
    VState *s = NULL;
    AcquireSRWLockExclusive(&ctx_lock);
    int i;
    for (i = 0; i < 16 && ctx_vs[i].gl; i++)
        if (ctx_vs[i].gl == gl) {
            s = ctx_vs[i].s;
            break;
        }
    if (!s && i < 16 && (s = vstate_new()) != NULL)
        ctx_vs[i].gl = gl, ctx_vs[i].s = s;
    ReleaseSRWLockExclusive(&ctx_lock);
    if (s && vs_ && vs_ != s) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 8)
            rt_log("[gl] thread %lu moves to another context's vertex state (context %p)\n", GetCurrentThreadId(),
                   gl);
    }
    if (s)
        vs_ = s;
}
void gles_context_destroyed(void *gl)
{
    AcquireSRWLockExclusive(&ctx_lock);
    for (int i = 0; i < 16; i++)
        if (ctx_vs[i].gl == gl)
            ctx_vs[i].gl = (void *)1;
    ReleaseSRWLockExclusive(&ctx_lock);
}
static VState *vs(void)
{
    if (!vs_)
        gles_context_current(SDL_GL_GetCurrentContext());
    if (!vs_)
        vs_ = vstate_new();
    return vs_;
}

static inline void client_set(VState *s, u32 k)
{
    u32 b = 1u << k;
    if (s->a[k].on && !s->a[k].vbo)
        s->client |= b;
    else
        s->client &= ~b;
    if (s->a[k].on && s->a[k].vbo)
        s->bufarr |= b;
    else
        s->bufarr &= ~b;
}

u32 gles_draws(void) { return vs()->draws; }

static u32 type_bytes(u32 t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return 2;
    default: return 4;
    }
}

static u32 img_lo, img_hi;
static unsigned rd_query, rd_image, rd_stack, rd_live;
static void image_range(void)
{
#ifdef _WIN32
    const u8 *base = (const u8 *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    img_lo = (u32)(uintptr_t)base;
    img_hi = img_lo + nt->OptionalHeader.SizeOfImage;
#else
    img_lo = GT_RESERVE_START;
    img_hi = GT_IMPORT_BASE;
#endif
}
static int stable(u32 a, u32 n)
{
    if (!img_hi)
        image_range();
    if (a >= img_lo && a < img_hi && n <= img_hi - a) {
        rd_image++;
        return 1;
    }
    RtThread *t = rt_thread_if();
    if (t && a >= t->stack_lo && a < t->stack_hi && n <= t->stack_hi - a) {
        rd_stack++;
        return 1;
    }
    return 0;
}

static int readable(VState *s, u32 a, u32 n)
{
    if (stable(a, n))
        return 1;
    if (gt_live_has(a, n)) {
        rd_live++;
        return 1;
    }
    u32 f = eagl_frames();
    if (s->ok_frame != f) {
        memset(s->ok, 0, sizeof s->ok);
        s->ok_frame = f;
    }
    u32 end = a + n;
    while (a < end) {
        int hit = 0;
        for (int i = 0; i < 16; i++)
            if (a >= s->ok[i].lo && a < s->ok[i].hi) {
                a = s->ok[i].hi;
                hit = 1;
                break;
            }
        if (hit)
            continue;
        MEMORY_BASIC_INFORMATION m;
        rd_query++;
        if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect)
            return 0;
        u32 lo = (u32)(uintptr_t)m.BaseAddress, hi = lo + (u32)m.RegionSize;
        u32 slot = s->ok_next++ & 15;
        s->ok[slot].lo = lo;
        s->ok[slot].hi = hi;
        a = hi;
    }
    return 1;
}

enum { SC_TEX, SC_PROG, SC_BLEND, SC_CAP, SC_MASK, SC_BUF, SC_UNI, SC_ARR, SC_KINDS };
static int sc_enabled(void);
static u32 sc_calls[SC_KINDS], sc_skipped[SC_KINDS];
static volatile LONG buf_gen;

void h_glBindBuffer(u32 target, u32 id)
{
    VState *s = vs();
    u32 g = (u32)buf_gen;
    if (target == GL_ARRAY_BUFFER) {
        sc_calls[SC_BUF]++;
        if (s->array_buf == id && s->sc_abuf_gen == g && sc_enabled()) {
            sc_skipped[SC_BUF]++;
            return;
        }
        s->array_buf = id, s->sc_abuf_gen = g;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        sc_calls[SC_BUF]++;
        if (s->elem_buf == id && s->sc_ebuf_gen == g && sc_enabled()) {
            sc_skipped[SC_BUF]++;
            return;
        }
        s->elem_buf = id, s->sc_ebuf_gen = g;
    }
    p_glBindBuffer(target, id);
}
void h_glGenBuffers(u32 n, u32 ids)
{
    p_glGenBuffers(n, ids);
    InterlockedIncrement(&buf_gen);
}
#define DD_MAX 256
static u32 dd_names[DD_MAX];
static int dd_n, dd_on = -1;
static unsigned dd_kept, dd_freed;
static int dd_enabled(void)
{
    if (dd_on < 0) {
        const char *e = getenv("GT_GL_DEFER_DELETE");
        dd_on = !(e && *e == '0');
        rt_log("[gl] DEFER_DELETE: %s\n", dd_on ? "on" : "off (GT_GL_DEFER_DELETE=0)");
    }
    return dd_on;
}
static int dd_in_use(const VState *skip, u32 id)
{
    int used = 0;
    AcquireSRWLockShared(&ctx_lock);
    for (int i = 0; i < 16 && ctx_vs[i].gl && !used; i++) {
        const VState *o = ctx_vs[i].s;
        if (ctx_vs[i].gl == (void *)1 || !o || o == skip)
            continue;
        if (o->array_buf == id || o->elem_buf == id)
            used = 1;
        for (u32 k = 0; k < MAX_ATTR && !used; k++)
            if (o->a[k].on && o->a[k].vbo == id)
                used = 1;
    }
    ReleaseSRWLockShared(&ctx_lock);
    return used;
}
static void dd_sweep_locked(void)
{
    int w = 0, freed = 0;
    for (int r = 0; r < dd_n; r++) {
        u32 id = dd_names[r];
        if (dd_in_use(NULL, id))
            dd_names[w++] = id;
        else {
            p_glDeleteBuffers(1, GA(&id));
            freed++;
        }
    }
    dd_n = w;
    if (freed) {
        dd_freed += (unsigned)freed;
        InterlockedIncrement(&del_gen);
        InterlockedIncrement(&buf_gen);
    }
}
static void dd_sweep(void)
{
    if (!dd_n || !SDL_GL_GetCurrentContext())
        return;
    AcquireSRWLockExclusive(&del_lock);
    dd_sweep_locked();
    ReleaseSRWLockExclusive(&del_lock);
}

void h_glDeleteBuffers(u32 n, u32 ids)
{
    VState *s = vs();
    const u32 *v = GP(ids);
    int defer = v && dd_enabled();
    u32 now[64], keep[64];
    u32 nn = 0, nk = 0;
    for (u32 i = 0; v && i < n; i++) {
        if (v[i] && v[i] == s->array_buf) {
            s->array_buf = 0;
            if (defer)
                p_glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
        if (v[i] && v[i] == s->elem_buf) {
            s->elem_buf = 0;
            if (defer)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        for (u32 k = 0; v[i] && k < MAX_ATTR; k++)
            if (s->a[k].vbo == v[i]) {
                s->a[k].vbo = 0;
                client_set(s, k);
                if (s->a[k].on)
                    once("glDeleteBuffers on the buffer of an enabled attribute array: left out of draws until set again");
            }
    }
    AcquireSRWLockExclusive(&del_lock);
    if (!defer || n > 64) {
        p_glDeleteBuffers(n, ids);
        InterlockedIncrement(&del_gen);
        InterlockedIncrement(&buf_gen);
    } else {
        for (u32 i = 0; i < n; i++) {
            if (!v[i])
                continue;
            if (dd_n < DD_MAX && dd_in_use(s, v[i]))
                keep[nk++] = v[i];
            else
                now[nn++] = v[i];
        }
        for (u32 i = 0; i < nk; i++)
            dd_names[dd_n++] = keep[i];
        if (nk) {
            dd_kept += nk;
            static volatile LONG logged;
            if (InterlockedIncrement(&logged) <= 4)
                rt_log("[gl] DEFER_DELETE: buffer %u deleted on thread %lu while another context still has it bound: "
                       "kept until none does (%d kept now)\n", keep[0], GetCurrentThreadId(), dd_n);
        }
        if (nn) {
            p_glDeleteBuffers(nn, GA(now));
            InterlockedIncrement(&del_gen);
            InterlockedIncrement(&buf_gen);
        }
        dd_sweep_locked();
    }
    ReleaseSRWLockExclusive(&del_lock);
}

static u32 gl_int(u32 pname)
{
    u32 v = 0;
    p_glGetIntegerv(pname, GA(&v));
    return v;
}
static void check_deleted(VState *s)
{
    u32 g = (u32)del_gen;
    if (s->del_seen == g)
        return;
    s->del_seen = g;
    for (u32 k = 0; k < MAX_ATTR; k++) {
        if (!s->a[k].vbo)
            continue;
        u32 b = 0;
        p_glGetVertexAttribiv(k, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, GA(&b));
        if (!b || b != s->a[k].vbo || !(p_glIsBuffer(b) & 0xff)) {
            if (s->a[k].on)
                once("a buffer deleted on another thread was still an enabled attribute array here: left out of draws until set again");
            s->a[k].vbo = 0;
            client_set(s, k);
        }
    }
    s->array_buf = gl_int(GL_ARRAY_BUFFER_BINDING);
    s->elem_buf = gl_int(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    s->sc_abuf_gen = s->sc_ebuf_gen = SC_UNKNOWN;
}
static int ac_on = -1;
static int ac_enabled(void)
{
    if (ac_on < 0) {
        const char *e = getenv("GT_GL_ARRAY_CACHE");
        int want = !(e && *e == '0');
        ac_on = want && sc_enabled();
        rt_log("[gl] ARRAY_CACHE: %s\n", ac_on ? "on" : want ? "off (GT_GL_STATE_CACHE=0)" : "off (GT_GL_ARRAY_CACHE=0)");
    }
    return ac_on;
}
static void array_set(u32 i, int on)
{
    if (i < MAX_ATTR) {
        VState *s = vs();
        s->a[i].on = on != 0;
        client_set(s, i);
        u32 b = 1u << i;
        sc_calls[SC_ARR]++;
        if ((s->sc_arr_known & b) && !(s->sc_arr_on & b) == !on && ac_enabled()) {
            sc_skipped[SC_ARR]++;
            return;
        }
        s->sc_arr_known |= b;
        s->sc_arr_on = on ? s->sc_arr_on | b : s->sc_arr_on & ~b;
    }
    if (on)
        p_glEnableVertexAttribArray(i);
    else
        p_glDisableVertexAttribArray(i);
}
void h_glEnableVertexAttribArray(u32 i) { array_set(i, 1); }
void h_glDisableVertexAttribArray(u32 i) { array_set(i, 0); }
void h_glEnableClientState(u32 a)
{
    vs()->sc_arr_known = 0;
    p_glEnableClientState(a);
}
void h_glDisableClientState(u32 a)
{
    vs()->sc_arr_known = 0;
    p_glDisableClientState(a);
}
void h_glVertexAttribPointer(u32 index, u32 size, u32 type, u32 norm, u32 stride, u32 ptr)
{
    VState *s = vs();
    norm &= 0xff;
    check_deleted(s);
    if (index < MAX_ATTR) {
        Attr *a = &s->a[index];
        a->size = size, a->type = type, a->norm = norm, a->stride = stride, a->ptr = ptr, a->vbo = s->array_buf;
        client_set(s, index);
        s->sv_moved &= ~(1u << index);
        if (type == GL_FIXED && !a->vbo) {
            once("GL_FIXED vertex attributes from client memory: turned into floats at each draw");
            return;
        }
    }
    p_glVertexAttribPointer(index, size, type, norm, stride, ptr);
}

static void unreadable_note(u32 i, const Attr *a, u32 first, u32 last, u32 bufs)
{
    enum { KINDS = 64 };
    static struct { u32 attr, ptr, size, type, stride, n; } kind[KINDS];
    static int nkinds;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    int k = 0;
    while (k < nkinds && !(kind[k].attr == i && kind[k].ptr == a->ptr && kind[k].size == a->size &&
                           kind[k].type == a->type && kind[k].stride == a->stride))
        k++;
    u32 n = 0;
    if (k < nkinds)
        n = ++kind[k].n;
    else if (nkinds < KINDS) {
        kind[k].attr = i, kind[k].ptr = a->ptr, kind[k].size = a->size, kind[k].type = a->type;
        kind[k].stride = a->stride, kind[k].n = n = 1;
        nkinds++;
    }
    ReleaseSRWLockExclusive(&lock);
    if (n == 1)
        rt_log("[gl] draw: attribute %u's array at 0x%08x (%u x %u, type 0x%x, stride %u) is not readable for vertices "
               "%u..%u: left out of this draw (frame %u; repeats counted; arrays from buffers in it: 0x%x)\n", i,
               a->ptr, a->size, type_bytes(a->type), a->type, a->stride, first, last, eagl_frames(), bufs);
    else if (n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        rt_log("[gl] draw: attribute %u's array at 0x%08x left out %u times so far (frame %u)\n", i, a->ptr, n,
               eagl_frames());
    else if (!n)
        once("more kinds of unreadable attribute arrays than are counted: left out of draws, not logged");
}

static int fx_on = -1, ix_on = -1;
static int fx_enabled(void)
{
    if (fx_on < 0) {
        const char *e = getenv("GT_FIXED_FAST");
        fx_on = !(e && *e == '0');
        rt_log("[gl] FIXED_FAST: %s\n", fx_on ? "on" : "off (GT_FIXED_FAST=0)");
    }
    return fx_on;
}
static int ix_enabled(void)
{
    if (ix_on < 0) {
        const char *e = getenv("GT_INDEX_FAST");
        ix_on = !(e && *e == '0');
        rt_log("[gl] INDEX_FAST: %s\n", ix_on ? "on" : "off (GT_INDEX_FAST=0)");
    }
    return ix_on;
}
static u32 fx_verts, fx_direct;

static void fixed_plain(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    for (u32 v = 0; v < n; v++, src += stride, d += size)
        for (u32 k = 0; k < size; k++)
            d[k] = ((const int *)src)[k] * (1.0f / 65536.0f);
}
static void fixed_sse2(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    const __m128 k = _mm_set1_ps(1.0f / 65536.0f);
    u32 v = 0;
    switch (size) {
    case 4:
        for (; v < n; v++, src += stride, d += 4)
            _mm_storeu_ps(d, _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)src)), k));
        return;
    case 3:
        for (; v < n; v++, src += stride, d += 3) {
            __m128i x = _mm_loadl_epi64((const __m128i *)src);
            x = _mm_unpacklo_epi64(x, _mm_cvtsi32_si128(((const int *)src)[2]));
            __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(x), k);
            _mm_storel_pi((__m64 *)d, f);
            _mm_store_ss(d + 2, _mm_movehl_ps(f, f));
        }
        return;
    case 2:
        for (; v < n; v++, src += stride, d += 2)
            _mm_storel_pi((__m64 *)d, _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadl_epi64((const __m128i *)src)), k));
        return;
    }
    fixed_plain(d, src, n, size, stride);
}
static void fixed_convert(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    fx_verts += n;
    if (fx_enabled())
        fixed_sse2(d, src, n, size, stride);
    else
        fixed_plain(d, src, n, size, stride);
}

static void index_plain(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_)
{
    u32 mn = 0xffffffff, mx = 0;
    for (u32 i = 0; i < count; i++) {
        u32 v = n == 1 ? p[i] : n == 2 ? ((const unsigned short *)p)[i] : ((const u32 *)p)[i];
        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }
    *mn_ = mn, *mx_ = mx;
}
static void index_sse2(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_)
{
    u32 mn = 0xffffffff, mx = 0, i = 0;
    if (n == 2 && count >= 16) {
        const __m128i flip = _mm_set1_epi16((short)0x8000);
        __m128i lo = _mm_set1_epi16(0x7fff), hi = flip;
        for (; i + 8 <= count; i += 8) {
            __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(p + i * 2)), flip);
            lo = _mm_min_epi16(lo, x);
            hi = _mm_max_epi16(hi, x);
        }
        unsigned short a[8], b[8];
        _mm_storeu_si128((__m128i *)a, _mm_xor_si128(lo, flip));
        _mm_storeu_si128((__m128i *)b, _mm_xor_si128(hi, flip));
        for (int k = 0; k < 8; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    } else if (n == 1 && count >= 32) {
        __m128i lo = _mm_set1_epi8((char)0xff), hi = _mm_setzero_si128();
        for (; i + 16 <= count; i += 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
            lo = _mm_min_epu8(lo, x);
            hi = _mm_max_epu8(hi, x);
        }
        u8 a[16], b[16];
        _mm_storeu_si128((__m128i *)a, lo);
        _mm_storeu_si128((__m128i *)b, hi);
        for (int k = 0; k < 16; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    }
    u32 tmn, tmx;
    index_plain(p + i * n, count - i, n, &tmn, &tmx);
    *mn_ = tmn < mn ? tmn : mn, *mx_ = tmx > mx ? tmx : mx;
}

int gles_selftest_fixed(void)
{
    enum { NV = 67, MAXS = 24 };
    static int src[NV * MAXS / 4 + 8];
    static float a[NV * 4 + 4], b[NV * 4 + 4];
    u32 x = 0x12345678u;
    int bad = 0;
    for (u32 i = 0; i < sizeof src / sizeof src[0]; i++) {
        u32 r = st_rand(&x);
        src[i] = i % 11 == 0 ? (int)0x80000000u : i % 11 == 1 ? 0x7fffffff : i % 11 == 2 ? (int)(r & 0xff) - 128 : (int)r;
    }
    for (u32 size = 1; size <= 4; size++)
        for (u32 stride = size * 4; stride <= MAXS; stride += 4)
            for (u32 n = 0; n <= NV; n += n < 5 ? 1 : 31) {
                memset(a, 0x55, sizeof a);
                memset(b, 0x55, sizeof b);
                fixed_plain(a, (const u8 *)src, n, size, stride);
                fixed_sse2(b, (const u8 *)src, n, size, stride);
                bad += memcmp(a, b, sizeof a) != 0;
            }
    return bad;
}
int gles_selftest_index(void)
{
    static u8 buf[4 * 5000 + 16];
    u32 x = 0x9e3779b9u;
    int bad = 0;
    static const u32 counts[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 100, 4999};
    for (int round = 0; round < 6; round++)
        for (u32 n = 1; n <= 4; n *= 2)
            for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
                for (u32 i = 0; i < sizeof buf; i++)
                    buf[i] = (u8)(round < 2 ? st_rand(&x) : round == 2 ? 0xff : round == 3 ? 0 : 0x80 + (st_rand(&x) & 1));
                if (round == 5 && counts[c])
                    memset(buf + (counts[c] - 1) * n, 0xff, n);
                for (u32 off = 0; off < 3; off++) {
                    u32 a0, a1, b0, b1;
                    index_plain(buf + off * n, counts[c], n, &a0, &a1);
                    index_sse2(buf + off * n, counts[c], n, &b0, &b1);
                    bad += a0 != b0 || a1 != b1;
                }
            }
    return bad;
}

#define SV_RING  (4u << 20)
#define SV_IRING (1u << 20)
typedef void(GT_GLAPI *T_sv_subdata)(u32 target, u32 off, u32 n, const void *p);
typedef void(GT_GLAPI *T_sv_basevertex)(u32 mode, u32 count, u32 type, u32 ptr, int base);
typedef void(GT_GLAPI *T_sv_storage)(u32 target, u32 size, const void *p, u32 flags);
typedef void *(GT_GLAPI *T_sv_map)(u32 target, u32 off, u32 n, u32 access);
typedef void *(GT_GLAPI *T_sv_fence)(u32 cond, u32 flags);
typedef u32(GT_GLAPI *T_sv_wait)(void *sync, u32 flags, unsigned long long ns);
typedef void(GT_GLAPI *T_sv_delsync)(void *sync);
static T_sv_subdata sv_subdata;
static T_sv_basevertex sv_basevertex;
static T_sv_storage sv_storage;
static T_sv_map sv_mapr;
static T_sv_fence sv_fencesync;
static T_sv_wait sv_clientwait;
static T_sv_delsync sv_delsync;
static int sv_on = -1;
static int sv_mode;
#define SV_SEGS 4
static u32 sv_size(int w) { return (w ? SV_IRING : SV_RING) * (sv_mode == 2 ? SV_SEGS : 1); }
static struct { u32 draws, client, streamed, bytes, readback, nofit, mixed, kept, hit; } sv_st;

static int mk_on = -1;
static int mk_enabled(void)
{
    if (mk_on < 0) {
        const char *e = getenv("GT_STREAM_MIXED_KEEP");
        mk_on = !(e && *e == '0');
        rt_log("[gl] MIXED_KEEP: %s\n", mk_on ? "on" : "off (GT_STREAM_MIXED_KEEP=0)");
    }
    return mk_on;
}
static int st_on = -1;
static u32 prog_reads(u32 prog)
{
    static struct { u32 prog, mask; } c[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (c[i].prog == prog)
            return c[i].mask;
    u32 cnt = 0, mask = 0;
    p_glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, GA(&cnt));
    for (u32 i = 0; i < cnt && i < 32; i++) {
        char name[64] = {0};
        u32 len = 0, size = 0, type = 0;
        p_glGetActiveAttrib(prog, i, sizeof name, GA(&len), GA(&size), GA(&type), GA(name));
        int loc = (int)p_glGetAttribLocation(prog, GA(name));
        if (loc >= 0 && loc < 32)
            mask |= 1u << loc;
    }
    if (n < 256)
        c[n].prog = prog, c[n].mask = mask, n++;
    return mask;
}
static void mixed_note(VState *s, u32 bufs, u32 lo, int keep)
{
    if (st_on < 0)
        st_on = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (!GT_DEV || !st_on || !lo || !bufs)
        return;
    u32 prog = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
    u32 r = prog_reads(prog);
    if (!(r & bufs))
        return;
    sv_st.hit++;
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 6)
        rt_log("[gl] MIXED_KEEP: program %u reads attributes 0x%x, from buffers 0x%x, client 0x%x; lowest vertex %u: %s\n",
               prog, r, bufs, s->client, lo, keep ? "vertex numbers kept" : "REBASED (buffer arrays read from the wrong vertices)");
}

static int sv_enabled(void)
{
    if (sv_on < 0) {
        const char *e = getenv("GT_STREAM_VBO");
        int want = !(e && *e == '0');
        sv_subdata = (T_sv_subdata)SDL_GL_GetProcAddress("glBufferSubData");
        sv_basevertex = (T_sv_basevertex)SDL_GL_GetProcAddress("glDrawElementsBaseVertex");
        sv_storage = (T_sv_storage)SDL_GL_GetProcAddress("glBufferStorage");
        sv_mapr = (T_sv_map)SDL_GL_GetProcAddress("glMapBufferRange");
        sv_fencesync = (T_sv_fence)SDL_GL_GetProcAddress("glFenceSync");
        sv_clientwait = (T_sv_wait)SDL_GL_GetProcAddress("glClientWaitSync");
        sv_delsync = (T_sv_delsync)SDL_GL_GetProcAddress("glDeleteSync");
        int can_map = sv_storage && sv_mapr && sv_fencesync && sv_clientwait && sv_delsync;
        sv_mode = e && !_stricmp(e, "subdata") ? 1 : 2;
        if (sv_mode == 2 && !can_map)
            sv_mode = 1;
        sv_on = want && sv_subdata;
        rt_log("[gl] STREAM_VBO: %s%s\n",
               sv_on ? (sv_mode == 2 ? "on (persistent mapped ring)" : "on (glBufferSubData ring)")
                     : want ? "off (no glBufferSubData)" : "off (GT_STREAM_VBO=0)",
               sv_on && !sv_basevertex ? " (no glDrawElementsBaseVertex: indexed draws keep their offsets)" : "");
    }
    return sv_on;
}

static int sv_ready(VState *s)
{
    void *ctx = SDL_GL_GetCurrentContext();
    if (s->sv_ring && s->sv_ctx == ctx)
        return 1;
    if (!ctx)
        return 0;
    u32 b[2] = {0, 0};
    p_glGenBuffers(2, GA(b));
    if (!b[0] || !b[1])
        return 0;
    unsigned char *mp[2] = {NULL, NULL};
    for (int w = 0; w < 2; w++) {
        u32 target = w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, size = sv_size(w);
        p_glBindBuffer(target, b[w]);
        if (sv_mode == 2) {
            sv_storage(target, size, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
            mp[w] = sv_mapr(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        } else
            p_glBufferData(target, size, 0, GL_STREAM_DRAW);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->elem_buf);
    if (sv_mode == 2 && (!mp[0] || !mp[1])) {
        rt_log("[gl] STREAM_VBO: the persistent rings could not be mapped; this context draws as before\n");
        return 0;
    }
    memset(s->sv_fence, 0, sizeof s->sv_fence);
    s->sv_map[0] = mp[0], s->sv_map[1] = mp[1], s->sv_seg[0] = s->sv_seg[1] = 0;
    s->sv_ring = b[0], s->sv_iring = b[1], s->sv_off = s->sv_ioff = 0, s->sv_moved = 0, s->sv_ctx = ctx;
    rt_log("[gl] STREAM_VBO: rings %u / %u for context %p (thread %lu)\n", b[0], b[1], ctx, GetCurrentThreadId());
    return 1;
}

static void sv_room(VState *s, int w, u32 *off, u32 total)
{
    u32 size = sv_size(w);
    if (!total)
        return;
    if (sv_mode != 2) {
        if (*off + total > size) {
            p_glBufferData(w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, size, 0, GL_STREAM_DRAW);
            *off = 0;
        }
        return;
    }
    if (*off + total > size)
        *off = 0;
    u32 seg = size / SV_SEGS, last = (*off + total - 1) / seg, cur = s->sv_seg[w];
    while (cur != last) {
        s->sv_fence[w][cur] = sv_fencesync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        cur = (cur + 1) % SV_SEGS;
        void **f = &s->sv_fence[w][cur];
        if (*f) {
            for (int i = 0; i < 50; i++) {
                u32 r = sv_clientwait(*f, i ? 0 : 1, 100000000ull);
                if (r != GL_TIMEOUT_EXPIRED)
                    break;
            }
            sv_delsync(*f);
            *f = NULL;
        }
    }
    s->sv_seg[w] = cur;
}

static void sv_write(VState *s, int w, u32 off, u32 bytes, u32 src)
{
    if (sv_mode == 2)
        memcpy(s->sv_map[w] + off, (const void *)(uintptr_t)src, bytes);
    else
        sv_subdata(w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, off, bytes, (const void *)(uintptr_t)src);
}

static void sv_put_back(VState *s, u32 m)
{
    m &= s->sv_moved;
    if (!m)
        return;
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (u32 k = m; k; k &= k - 1) {
        u32 i = (u32)__builtin_ctz(k);
        Attr *a = &s->a[i];
        if (a->type != GL_FIXED)
            p_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, a->ptr);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    s->sv_moved &= ~m;
}

static int sv_move(VState *s, u32 m, u32 first, u32 last, int rebase, u32 pend)
{
    struct { u32 lo, hi, base, stride; u32 i; } a[MAX_ATTR];
    int na = 0;
    for (u32 k = m; k; k &= k - 1) {
        u32 i = (u32)__builtin_ctz(k);
        const Attr *at = &s->a[i];
        u32 elem, stride, base;
        if (at->type == GL_FIXED) {
            elem = stride = at->size * 4;
            base = (u32)(uintptr_t)at->conv;
        } else {
            elem = at->size * type_bytes(at->type);
            stride = at->stride ? at->stride : elem;
            base = at->ptr;
        }
        a[na].lo = base + first * stride;
        a[na].hi = base + last * stride + elem;
        a[na].base = base, a[na].stride = stride, a[na].i = i;
        na++;
    }
    for (int x = 1; x < na; x++)
        for (int y = x; y > 0 && a[y].lo < a[y - 1].lo; y--) {
            __typeof__(a[0]) t = a[y];
            a[y] = a[y - 1];
            a[y - 1] = t;
        }
    struct { u32 glo, ghi, need; int x, y; } g[MAX_ATTR];
    int ng = 0;
    u32 total = 0;
    for (int x = 0; x < na;) {
        u32 glo = a[x].lo, ghi = a[x].hi;
        int y = x + 1;
        while (y < na && a[y].lo < ghi) {
            if (a[y].hi > ghi)
                ghi = a[y].hi;
            y++;
        }
        u32 need = 0;
        for (int k = x; k < y && !rebase; k++)
            if (glo - a[k].base > need)
                need = glo - a[k].base;
        if (need > SV_RING || ghi - glo > SV_RING - need) {
            sv_st.nofit++;
            return 0;
        }
        total += ((need + 15) & ~15u) + ghi - glo + 16;
        g[ng].glo = glo, g[ng].ghi = ghi, g[ng].need = need, g[ng].x = x, g[ng].y = y;
        ng++;
        x = y;
    }
    if (total > SV_RING) {
        sv_st.nofit++;
        return 0;
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->sv_ring);
    sv_room(s, 0, &s->sv_off, total);
    for (int j = 0; j < ng; j++) {
        u32 off = (s->sv_off + 15) & ~15u;
        if (off < g[j].need)
            off = (g[j].need + 15) & ~15u;
        u32 bytes = g[j].ghi - g[j].glo;
        int direct = 0;
        for (int k = g[j].x; k < g[j].y; k++) {
            u32 i = a[k].i;
            if (!(pend & 1u << i))
                continue;
            Attr *at = &s->a[i];
            u32 st = at->stride ? at->stride : at->size * 4;
            const u8 *src = (const u8 *)(uintptr_t)(at->ptr + first * st);
            if (g[j].y - g[j].x == 1 && sv_mode == 2) {
                fixed_convert((float *)(s->sv_map[0] + off), src, last - first + 1, at->size, st);
                fx_direct += last - first + 1;
                direct = 1;
            } else
                fixed_convert(at->conv + first * at->size, src, last - first + 1, at->size, st);
        }
        if (!direct)
            sv_write(s, 0, off, bytes, g[j].glo);
        s->sv_off = off + bytes;
        sv_st.bytes += bytes;
        for (int k = g[j].x; k < g[j].y; k++) {
            u32 i = a[k].i;
            const Attr *at = &s->a[i];
            u32 at_off = rebase ? off + (a[k].lo - g[j].glo) : off + a[k].base - g[j].glo;
            if (at->type == GL_FIXED)
                p_glVertexAttribPointer(i, at->size, GL_FLOAT, 0, 0, at_off);
            else
                p_glVertexAttribPointer(i, at->size, at->type, at->norm, at->stride, at_off);
            s->sv_moved |= 1u << i;
        }
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    return 1;
}

static u32 sv_indices(VState *s, u32 ptr, u32 bytes)
{
    if (bytes > SV_IRING)
        return ~0u;
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->sv_iring);
    u32 off = (s->sv_ioff + 3) & ~3u;
    sv_room(s, 1, &off, bytes);
    sv_write(s, 1, off, bytes, ptr);
    s->sv_ioff = off + bytes;
    sv_st.bytes += bytes;
    return off;
}

static int index_range(VState *s, u32 count, u32 type, u32 ptr, u32 *lo, u32 *hi)
{
    u32 n = type_bytes(type);
    const unsigned char *p;
    unsigned char *tmp = NULL;
    if (s->elem_buf) {
        sv_st.readback++;
        tmp = malloc(count * n);
        if (!tmp)
            return 0;
        p_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, ptr, count * n, GA(tmp));
        p = tmp;
    } else {
        if (!readable(s, ptr, count * n))
            return 0;
        p = (const unsigned char *)(uintptr_t)ptr;
    }
    u32 mn, mx;
    if (ix_enabled())
        index_sse2(p, count, n, &mn, &mx);
    else
        index_plain(p, count, n, &mn, &mx);
    free(tmp);
    *lo = count ? mn : 0, *hi = count ? mx : 0;
    return 1;
}

static void unreadable_used(u32 i)
{
    if (st_on < 0)
        st_on = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (!GT_DEV || !st_on)
        return;
    u32 prog = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
    u32 r = prog_reads(prog);
    static volatile LONG used, unused;
    if (r & 1u << i) {
        LONG n = InterlockedIncrement(&used);
        if (n <= 4 || n == 100 || n == 10000)
            rt_log("[gl] left out: attribute %u IS read by program %u (reads 0x%x; %ld such draws so far)\n", i, prog, r,
                   n);
    } else if (InterlockedIncrement(&unused) <= 4)
        rt_log("[gl] left out: attribute %u is not read by program %u (reads 0x%x)\n", i, prog, r);
}

static u32 draw_prepare(VState *s, int have_range, u32 lo, u32 hi, u32 *pend)
{
    u32 off = 0;
    for (u32 m = s->client; m; m &= m - 1) {
        u32 i = (u32)__builtin_ctz(m);
        Attr *a = &s->a[i];
        u32 elem = a->size * type_bytes(a->type);
        u32 stride = a->stride ? a->stride : elem;
        u32 first = have_range ? lo : 0, last = have_range ? hi : 0;
        u32 start = a->ptr + first * stride, len = (last - first) * stride + elem;
        if (!a->ptr || !readable(s, start, len)) {
            unreadable_note(i, a, first, last, s->bufarr);
            unreadable_used(i);
            p_glDisableVertexAttribArray(i);
            off |= 1u << i;
            continue;
        }
        if (a->type == GL_FIXED) {
            u32 need = (last + 1) * a->size;
            if (a->conv_n < need) {
                free(a->conv);
                a->conv = malloc(need * sizeof(float));
                a->conv_n = a->conv ? need : 0;
                if (!a->conv) {
                    p_glDisableVertexAttribArray(i);
                    off |= 1u << i;
                    continue;
                }
            }
            if (pend && fx_enabled() && sv_enabled() && sv_mode == 2) {
                *pend |= 1u << i;
                continue;
            }
            fixed_convert(a->conv + first * a->size, (const u8 *)(uintptr_t)(a->ptr + first * stride),
                          last - first + 1, a->size, stride);
            p_glVertexAttribPointer(i, a->size, GL_FLOAT, 0, 0, GA(a->conv));
        }
    }
    return off;
}
static void fixed_finish(VState *s, u32 pend, int have_range, u32 lo, u32 hi)
{
    for (; pend; pend &= pend - 1) {
        u32 i = (u32)__builtin_ctz(pend);
        Attr *a = &s->a[i];
        u32 stride = a->stride ? a->stride : a->size * 4;
        u32 first = have_range ? lo : 0, last = have_range ? hi : 0;
        fixed_convert(a->conv + first * a->size, (const u8 *)(uintptr_t)(a->ptr + first * stride), last - first + 1,
                      a->size, stride);
        p_glVertexAttribPointer(i, a->size, GL_FLOAT, 0, 0, GA(a->conv));
    }
}
static void draw_done(u32 off)
{
    for (u32 i = 0; off; i++, off >>= 1)
        if (off & 1)
            p_glEnableVertexAttribArray(i);
}

void h_glDrawArrays(u32 mode, u32 first, u32 count)
{
    softrefl_draw();
    VState *s = vs();
    s->draw_mode = mode, s->draw_count = count, s->draw_type = 0, s->draw_ptr = first, s->draws++;
    if (!count)
        return;
    gles_draw_lock();
    check_deleted(s);
    sv_st.draws++;
    u32 off = 0;
    if (s->client) {
        sv_st.client++;
        u32 pend = 0;
        off = draw_prepare(s, 1, first, first + count - 1, &pend);
        u32 m = s->client & ~off;
        int keep = first && (s->bufarr & ~off) && mk_enabled();
        if (m && sv_enabled() && sv_ready(s) && sv_move(s, m, first, first + count - 1, !keep, pend)) {
            sv_st.streamed++;
            if (s->bufarr & ~off)
                sv_st.mixed++, sv_st.kept += keep != 0, mixed_note(s, s->bufarr & ~off, first, keep);
            p_glDrawArrays(mode, keep ? first : 0, count);
            draw_done(off);
            gles_draw_unlock();
            return;
        }
        sv_put_back(s, m);
        fixed_finish(s, pend, 1, first, first + count - 1);
    }
    p_glDrawArrays(mode, first, count);
    draw_done(off);
    gles_draw_unlock();
}
void h_glDrawElements(u32 mode, u32 count, u32 type, u32 ptr)
{
    softrefl_draw();
    VState *s = vs();
    s->draw_mode = mode, s->draw_count = count, s->draw_type = type, s->draw_ptr = ptr, s->draws++;
    if (!count)
        return;
    gles_draw_lock();
    check_deleted(s);
    if (!s->elem_buf && !readable(s, ptr, count * type_bytes(type))) {
        once("glDrawElements with unreadable client indices: skipped");
        gles_draw_unlock();
        return;
    }
    u32 lo = 0, hi = 0, off = 0;
    sv_st.draws++;
    if (s->client) {
        sv_st.client++;
        int have = index_range(s, count, type, ptr, &lo, &hi);
        u32 pend = 0;
        off = draw_prepare(s, have, lo, hi, &pend);
        u32 m = s->client & ~off;
        int keep = lo && (s->bufarr & ~off) && mk_enabled();
        int rebase = lo == 0 || (!keep && sv_basevertex && lo <= 0x7fffffffu);
        if (have && m && sv_enabled() && sv_ready(s) && sv_move(s, m, lo, hi, rebase, pend)) {
            sv_st.streamed++;
            if (s->bufarr & ~off)
                sv_st.mixed++, sv_st.kept += keep != 0, mixed_note(s, s->bufarr & ~off, lo, keep);
            u32 ip = s->elem_buf ? ptr : sv_indices(s, ptr, count * type_bytes(type));
            if (ip == ~0u)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0), ip = ptr;
            if (rebase && lo)
                sv_basevertex(mode, count, type, ip, -(int)lo);
            else
                p_glDrawElements(mode, count, type, ip);
            if (!s->elem_buf)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            draw_done(off);
            gles_draw_unlock();
            return;
        }
        sv_put_back(s, m);
        fixed_finish(s, pend, have, lo, hi);
    }
    p_glDrawElements(mode, count, type, ptr);
    draw_done(off);
    gles_draw_unlock();
}

static volatile LONG tex_del_gen, prog_del_gen;
static int sc_on = -1;
static int sc_enabled(void)
{
    if (sc_on < 0) {
        const char *e = getenv("GT_GL_STATE_CACHE");
        sc_on = !(e && *e == '0');
        rt_log("[gl] state cache: %s\n", sc_on ? "on" : "off (GT_GL_STATE_CACHE=0)");
    }
    return sc_on;
}
void gles_state_forget(void)
{
    if (vs_)
        sc_forget(vs_);
    dd_sweep();
    static int stats = -1;
    if (stats < 0)
        stats = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (GT_DEV && stats && eagl_frames() % 600 == 0) {
        static const char *const what[SC_KINDS] = {"texture", "program", "blend", "enable", "mask", "buffer",
                                                   "uniform", "array"};
        char line[512];
        int o = 0;
        for (int k = 0; k < SC_KINDS && o < (int)sizeof line - 64; k++) {
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %u/%u", k ? ", " : "", what[k], sc_skipped[k],
                          sc_calls[k]);
            sc_skipped[k] = sc_calls[k] = 0;
        }
        rt_log("[gl] state cache, calls left out / made in 600 frames: %s\n", line);
        rt_log("[gl] client arrays in 600 frames: %u memory queries; %u in the exe's image, %u on the guest stack and "
               "%u in live heap blocks (LIVE_BLOCKS) without one\n", rd_query, rd_image, rd_stack, rd_live);
        rd_query = rd_image = rd_stack = rd_live = 0;
        rt_log("[gl] GL_FIXED vertices in 600 frames: %u turned into floats, %u of them straight into the ring "
               "(FIXED_FAST)\n", fx_verts, fx_direct);
        fx_verts = fx_direct = 0;
        gles_pvrtc_stats();
        rt_log("[gl] draws in 600 frames: %u, %u with client arrays, %u of them streamed (STREAM_VBO, %.0f KB a frame); "
               "%u index ranges read back from an element buffer, %u did not fit; %u streamed with arrays in buffers "
               "too (MIXED_KEEP: %u kept their vertex numbers; %u from a vertex past 0 whose program reads an array from a "
               "buffer)\n", sv_st.draws, sv_st.client, sv_st.streamed, sv_st.bytes / 600.0 / 1024, sv_st.readback,
               sv_st.nofit, sv_st.mixed, sv_st.kept, sv_st.hit);
        memset(&sv_st, 0, sizeof sv_st);
        rt_log("[gl] DEFER_DELETE in 600 frames: %u deletes held back, %u held ones deleted, %d held now\n", dd_kept,
               dd_freed, dd_n);
        dd_kept = dd_freed = 0;
    }
}
void h_glActiveTexture(u32 unit)
{
    VState *s = vs();
    s->tk_unit = unit, s->tk_known |= TK_UNIT;
    sc_calls[SC_TEX]++;
    if (sc_enabled() && s->sc_unit == unit) {
        sc_skipped[SC_TEX]++;
        return;
    }
    s->sc_unit = unit;
    p_glActiveTexture(unit);
}
void h_glBindTexture(u32 target, u32 id)
{
    VState *s = vs();
    if (target == GL_TEXTURE_2D && (s->tk_known & TK_UNIT)) {
        if (s->tk_unit == GL_TEXTURE0)
            s->tk_tex0 = id, s->tk_known |= TK_TEX0;
    } else if (target == GL_TEXTURE_2D)
        s->tk_known &= ~(u32)TK_TEX0;
    u32 u = s->sc_unit - GL_TEXTURE0;
    if (target != GL_TEXTURE_2D || u >= 32 || !sc_enabled()) {
        if (u < 32 && target == GL_TEXTURE_2D)
            s->sc_tex[u] = id;
        p_glBindTexture(target, id);
        return;
    }
    u32 g = (u32)tex_del_gen;
    if (s->sc_tex_gen != g) {
        s->sc_tex_gen = g;
        memset(s->sc_tex, 0xff, sizeof s->sc_tex);
    }
    sc_calls[SC_TEX]++;
    if (s->sc_tex[u] == id) {
        sc_skipped[SC_TEX]++;
        return;
    }
    s->sc_tex[u] = id;
    p_glBindTexture(target, id);
}
void h_glDeleteTextures(u32 n, u32 ids)
{
    VState *s = vs();
    const u32 *v = GP(ids);
    for (u32 i = 0; v && i < n; i++) {
        for (u32 u = 0; u < 32; u++)
            if (v[i] && s->sc_tex[u] == v[i])
                s->sc_tex[u] = 0;
        if (v[i] && s->tk_tex0 == v[i])
            s->tk_tex0 = 0;
    }
    p_glDeleteTextures(n, ids);
    InterlockedIncrement(&tex_del_gen);
}
#define HOST_UNI_MAX 32
#define HOST_UNI_KINDS 4
static struct { const char *name; float (*value)(void); } host_uni_kind[HOST_UNI_KINDS];
static int nhost_uni_kinds;
static struct { u32 prog; int loc[HOST_UNI_KINDS]; float val[HOST_UNI_KINDS]; } host_uni[HOST_UNI_MAX];
static int nhost_uni;
static SRWLOCK host_uni_lock = SRWLOCK_INIT;

int gles_add_host_uniform(const char *name, float (*value)(void))
{
    if (nhost_uni_kinds >= HOST_UNI_KINDS)
        return 0;
    host_uni_kind[nhost_uni_kinds].name = name;
    host_uni_kind[nhost_uni_kinds++].value = value;
    return 1;
}

static float host_uni_value(int k) { return host_uni_kind[k].value(); }

static void host_uni_linked(u32 prog)
{
    int loc[HOST_UNI_KINDS], any = 0;
    for (int k = 0; k < HOST_UNI_KINDS; k++) {
        loc[k] = k < nhost_uni_kinds ? (int)p_glGetUniformLocation(prog, GA(host_uni_kind[k].name)) : -1;
        any |= loc[k] >= 0;
    }
    AcquireSRWLockExclusive(&host_uni_lock);
    int i = 0;
    while (i < nhost_uni && host_uni[i].prog != prog)
        i++;
    if (any && i == nhost_uni && nhost_uni < HOST_UNI_MAX)
        nhost_uni++;
    if (i < nhost_uni) {
        host_uni[i].prog = any ? prog : 0;
        for (int k = 0; k < HOST_UNI_KINDS; k++) {
            host_uni[i].loc[k] = loc[k];
            host_uni[i].val[k] = -1.0f;
        }
    }
    ReleaseSRWLockExclusive(&host_uni_lock);
}

static void host_uni_used(u32 prog)
{
    if (!nhost_uni || !prog)
        return;
    AcquireSRWLockExclusive(&host_uni_lock);
    for (int i = 0; i < nhost_uni; i++)
        if (host_uni[i].prog == prog) {
            for (int k = 0; k < HOST_UNI_KINDS; k++) {
                if (host_uni[i].loc[k] < 0)
                    continue;
                float v = host_uni_value(k);
                if (host_uni[i].val[k] != v) {
                    host_uni[i].val[k] = v;
                    p_glUniform1f((u32)host_uni[i].loc[k], v);
                }
            }
            break;
        }
    ReleaseSRWLockExclusive(&host_uni_lock);
}

void h_glUseProgram(u32 prog)
{
    VState *s = vs();
    s->tk_prog = prog, s->tk_known |= TK_PROG;
    if (!sc_enabled()) {
        p_glUseProgram(prog);
        host_uni_used(prog);
        return;
    }
    u32 g = (u32)prog_del_gen;
    if (s->sc_prog_gen != g) {
        s->sc_prog_gen = g;
        s->sc_prog = SC_UNKNOWN;
    }
    sc_calls[SC_PROG]++;
    if (s->sc_prog == prog) {
        sc_skipped[SC_PROG]++;
        host_uni_used(prog);
        return;
    }
    s->sc_prog = prog;
    p_glUseProgram(prog);
    host_uni_used(prog);
}
static volatile LONG uni_gen;
void h_glDeleteProgram(u32 prog)
{
    p_glDeleteProgram(prog);
    InterlockedIncrement(&prog_del_gen);
    InterlockedIncrement(&uni_gen);
}
void h_glBlendFunc(u32 src, u32 dst)
{
    VState *s = vs();
    sc_calls[SC_BLEND]++;
    if (sc_enabled() && s->sc_bsrc == src && s->sc_bdst == dst) {
        sc_skipped[SC_BLEND]++;
        return;
    }
    s->sc_bsrc = src, s->sc_bdst = dst;
    p_glBlendFunc(src, dst);
}
void h_glBlendFuncSeparate(u32 a, u32 b, u32 c_, u32 d)
{
    VState *s = vs();
    s->sc_bsrc = s->sc_bdst = SC_UNKNOWN;
    p_glBlendFuncSeparate(a, b, c_, d);
}

static int cap_bit(u32 cap)
{
    switch (cap) {
    case GL_BLEND: return 0;
    case GL_DEPTH_TEST: return 1;
    case GL_CULL_FACE: return 2;
    case GL_SCISSOR_TEST: return 3;
    case GL_STENCIL_TEST: return 4;
    case GL_ALPHA_TEST: return 5;
    case GL_POLYGON_OFFSET_FILL: return 6;
    case GL_DITHER: return 7;
    }
    return -1;
}
static void cap_set(u32 cap, int on)
{
    if (cap == GL_SCISSOR_TEST) {
        VState *s = vs();
        s->tk_scissor_on = on != 0, s->tk_known |= TK_SCISSOR_ON;
    }
    int i = cap_bit(cap);
    if (i >= 0) {
        VState *s = vs();
        u32 b = 1u << i;
        sc_calls[SC_CAP]++;
        if ((s->sc_caps_known & b) && !(s->sc_caps_on & b) == !on && sc_enabled()) {
            sc_skipped[SC_CAP]++;
            return;
        }
        s->sc_caps_known |= b;
        s->sc_caps_on = on ? s->sc_caps_on | b : s->sc_caps_on & ~b;
    }
    if (on)
        p_glEnable(cap);
    else
        p_glDisable(cap);
}
void h_glEnable(u32 cap) { cap_set(cap, 1); }
void h_glDisable(u32 cap) { cap_set(cap, 0); }
void h_glDepthMask(u32 flag)
{
    VState *s = vs();
    u32 v = (flag & 0xff) != 0;
    sc_calls[SC_MASK]++;
    if (s->sc_dmask == v && sc_enabled()) {
        sc_skipped[SC_MASK]++;
        return;
    }
    s->sc_dmask = v;
    p_glDepthMask(v);
}
void h_glColorMask(u32 r, u32 g, u32 b, u32 a)
{
    VState *s = vs();
    u32 v = ((r & 0xff) != 0) | ((g & 0xff) != 0) << 1 | ((b & 0xff) != 0) << 2 | ((a & 0xff) != 0) << 3;
    sc_calls[SC_MASK]++;
    if (s->sc_cmask == v && sc_enabled()) {
        sc_skipped[SC_MASK]++;
        return;
    }
    s->sc_cmask = v;
    p_glColorMask(v & 1, v >> 1 & 1, v >> 2 & 1, v >> 3 & 1);
}

#define UNI_SLOTS 2048
typedef struct { u32 prog, loc, kind, n, v[16]; } Uni;
static Uni *uni;
static volatile LONG uni_owner;
static __thread int uni_mine = -1;
static u32 uni_seen = SC_UNKNOWN;
enum { U_1F = 1, U_2F, U_3F, U_4F, U_1I, U_2I, U_3I, U_4I, U_1FV, U_2FV, U_3FV, U_4FV, U_1IV, U_2IV, U_3IV, U_4IV,
       U_M2, U_M3, U_M4 };

static int uni_same(u32 kind, u32 loc, const void *vals, u32 words)
{
    if ((s32)loc < 0 || !sc_enabled())
        return 0;
    VState *s = vs();
    u32 prog = s->sc_prog;
    if (prog == SC_UNKNOWN || !prog)
        return 0;
    if (uni_mine < 0) {
        LONG me = (LONG)GetCurrentThreadId();
        InterlockedCompareExchange(&uni_owner, me, 0);
        uni_mine = uni_owner == me;
    }
    if (!uni_mine) {
        InterlockedIncrement(&uni_gen);
        return 0;
    }
    if (!uni && !(uni = calloc(UNI_SLOTS, sizeof *uni)))
        return 0;
    u32 g = (u32)uni_gen;
    if (g != uni_seen) {
        memset(uni, 0, UNI_SLOTS * sizeof *uni);
        uni_seen = g;
    }
    Uni *u = &uni[(prog * 0x9E3779B1u ^ loc * 0x85EBCA77u) >> 21 & (UNI_SLOTS - 1)];
    sc_calls[SC_UNI]++;
    if (words > 16 || !vals) {
        if (u->prog == prog && u->loc == loc)
            u->prog = 0;
        return 0;
    }
    if (u->prog == prog && u->loc == loc && u->kind == kind && u->n == words && !memcmp(u->v, vals, words * 4)) {
        sc_skipped[SC_UNI]++;
        return 1;
    }
    u->prog = prog, u->loc = loc, u->kind = kind, u->n = words;
    memcpy(u->v, vals, words * 4);
    return 0;
}

#define UNI_F(N, ...)                                                                   \
    {                                                                                   \
        float v[] = {__VA_ARGS__};                                                      \
        if (!uni_same(U_##N##F, loc, v, sizeof v / 4))                                  \
            p_glUniform##N##f(loc, __VA_ARGS__);                                        \
    }
#define UNI_I(N, ...)                                                                   \
    {                                                                                   \
        u32 v[] = {__VA_ARGS__};                                                        \
        if (!uni_same(U_##N##I, loc, v, sizeof v / 4))                                  \
            p_glUniform##N##i(loc, __VA_ARGS__);                                        \
    }
void h_glUniform1f(u32 loc, float a) UNI_F(1, a)
void h_glUniform2f(u32 loc, float a, float b) UNI_F(2, a, b)
void h_glUniform3f(u32 loc, float a, float b, float c_) UNI_F(3, a, b, c_)
void h_glUniform4f(u32 loc, float a, float b, float c_, float d) UNI_F(4, a, b, c_, d)
void h_glUniform1i(u32 loc, u32 a) UNI_I(1, a)
void h_glUniform2i(u32 loc, u32 a, u32 b) UNI_I(2, a, b)
void h_glUniform3i(u32 loc, u32 a, u32 b, u32 c_) UNI_I(3, a, b, c_)
void h_glUniform4i(u32 loc, u32 a, u32 b, u32 c_, u32 d) UNI_I(4, a, b, c_, d)
#define UNI_V(name, kind, N)                                                            \
    void h_##name(u32 loc, u32 count, u32 ptr)                                          \
    {                                                                                   \
        if (!uni_same(kind, loc, ptr ? GP(ptr) : NULL, count > 16 ? 17 : count * (N)))  \
            p_##name(loc, count, ptr);                                                  \
    }
UNI_V(glUniform1fv, U_1FV, 1)
UNI_V(glUniform2fv, U_2FV, 2)
UNI_V(glUniform3fv, U_3FV, 3)
UNI_V(glUniform4fv, U_4FV, 4)
UNI_V(glUniform1iv, U_1IV, 1)
UNI_V(glUniform2iv, U_2IV, 2)
UNI_V(glUniform3iv, U_3IV, 3)
UNI_V(glUniform4iv, U_4IV, 4)
#define UNI_M(name, kind, N)                                                            \
    void h_##name(u32 loc, u32 count, u32 transpose, u32 ptr)                           \
    {                                                                                   \
        if (!uni_same(kind | (transpose & 0xff ? 0x100 : 0), loc, ptr ? GP(ptr) : NULL,                 \
                      count > 16 ? 17 : count * (N)))                                                    \
            p_##name(loc, count, transpose, ptr);                                       \
    }
UNI_M(glUniformMatrix2fv, U_M2, 4)
UNI_M(glUniformMatrix3fv, U_M3, 9)
UNI_M(glUniformMatrix4fv, U_M4, 16)

void gles_fault_dump(void)
{
    VState *s = vs_;
    if (!s)
        return;
    rt_log("[gl] this thread's last draw: mode %u, count %u, index type 0x%x, indices/first 0x%08x (draw #%u); "
           "array buffer %u, element buffer %u\n", s->draw_mode, s->draw_count, s->draw_type, s->draw_ptr, s->draws,
           s->array_buf, s->elem_buf);
    for (u32 i = 0; i < MAX_ATTR; i++) {
        Attr *a = &s->a[i];
        if (a->on)
            rt_log("[gl]   attribute %u: %u x type 0x%x%s stride %u, %s 0x%08x\n", i, a->size, a->type,
                   a->norm ? " normalized" : "", a->stride, a->vbo ? "buffer offset" : "memory", a->ptr);
    }
}

void h_glVertexPointer(u32 size, u32 type, u32 stride, u32 ptr)
{
    if (type == GL_FIXED)
        once("GL_FIXED vertex arrays are not supported");
    p_glVertexPointer(size, type, stride, ptr);
}

#define SHADER_PATCHES_MAX 4
static char *(*shader_patches[SHADER_PATCHES_MAX])(const char *src);
static int nshader_patches;

int gles_add_shader_patch(char *(*patch)(const char *src))
{
    if (nshader_patches >= SHADER_PATCHES_MAX)
        return 0;
    shader_patches[nshader_patches++] = patch;
    return 1;
}

static char *translate_shader(const char *src)
{
    static const char head[] = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    size_t n = strlen(src);
    char *out = malloc(n + sizeof head + 1), *o = out;
    memcpy(o, head, sizeof head - 1);
    o += sizeof head - 1;
    const char *p = src;
    int line_start = 1;
    while (*p) {
        if (line_start) {
            const char *q = p;
            while (*q == ' ' || *q == '\t')
                q++;
            if (!strncmp(q, "#version", 8) || !strncmp(q, "#extension", 10)) {
                while (*p && *p != '\n')
                    p++;
                continue;
            }
        }
        if (!strncmp(p, "precision", 9) && (p == src || !(p[-1] == '_' || isalnum((u8)p[-1]))) &&
            (p[9] == ' ' || p[9] == '\t')) {
            while (*p && *p != ';')
                p++;
            if (*p)
                p++;
            continue;
        }
        line_start = *p == '\n';
        *o++ = *p++;
    }
    *o = 0;
    return out;
}

void h_glShaderSource(u32 shader, u32 count, u32 strings, u32 lengths)
{
    size_t total = 1;
    for (u32 i = 0; i < count; i++) {
        const char *s = GP(rd32(strings + 4 * i));
        int len = lengths && (int)rd32(lengths + 4 * i) >= 0 ? (int)rd32(lengths + 4 * i) : (int)strlen(s);
        total += (size_t)len;
    }
    char *all = malloc(total), *o = all;
    for (u32 i = 0; i < count; i++) {
        const char *s = GP(rd32(strings + 4 * i));
        int len = lengths && (int)rd32(lengths + 4 * i) >= 0 ? (int)rd32(lengths + 4 * i) : (int)strlen(s);
        memcpy(o, s, (size_t)len);
        o += len;
    }
    *o = 0;
    for (int k = 0; k < nshader_patches; k++) {
        char *patched = shader_patches[k](all);
        if (patched) {
            free(all);
            all = patched;
        }
    }
    char *t = translate_shader(all);
    const char *one = t;
    p_glShaderSource(shader, 1, GA(&one), 0);
    free(t);
    free(all);
}

void h_glCompileShader(u32 shader)
{
    p_glCompileShader(shader);
    int ok = 0;
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, GA(&ok));
    if (!ok) {
        static char log[4096], src[32768];
        int n = 0;
        p_glGetShaderInfoLog(shader, sizeof log, GA(&n), GA(log));
        p_glGetShaderSource(shader, sizeof src, GA(&n), GA(src));
        rt_log("[gl] shader %u did not compile:\n%s\n---- source ----\n%s\n----\n", shader, log, src);
    }
}

void h_glLinkProgram(u32 prog)
{
    p_glBindAttribLocation(prog, 0, GA("position"));
    p_glLinkProgram(prog);
    InterlockedIncrement(&uni_gen);
    host_uni_linked(prog);
    int ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, GA(&ok));
    if (!ok) {
        static char log[4096];
        int n = 0;
        int att = -1;
        p_glGetProgramInfoLog(prog, sizeof log, GA(&n), GA(log));
        p_glGetProgramiv(prog, GL_ATTACHED_SHADERS, GA(&att));
        rt_log("[gl] program %u did not link (%d shaders attached, thread %lu, context %p):\n%s\n", prog, att,
               GetCurrentThreadId(), (void *)SDL_GL_GetCurrentContext(), log);
    }
}

static int qf_on = -1, qf_check;
static int qf_enabled(void)
{
    if (qf_on < 0) {
        const char *e = getenv("GT_GL_QUERY_FREE");
        qf_on = !(e && *e == '0');
        e = dev_getenv("GT_GL_QUERY_CHECK");
        qf_check = qf_on && e && *e == '1';
    }
    return qf_on;
}
void gles_bind_fb(u32 target, u32 fb)
{
    VState *s = vs();
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
        s->tk_rfb = fb, s->tk_known |= TK_RFB;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        s->tk_dfb = fb, s->tk_known |= TK_DFB;
    p_glBindFramebuffer(target, fb);
}
void gles_bind_rb(u32 rb)
{
    VState *s = vs();
    s->tk_rb = rb, s->tk_known |= TK_RB;
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
}
void h_glBindRenderbuffer(u32 target, u32 rb)
{
    if (target == GL_RENDERBUFFER) {
        gles_bind_rb(rb);
        return;
    }
    p_glBindRenderbuffer(target, rb);
}
void h_glScissor(u32 x, u32 y, u32 w, u32 h)
{
    VState *s = vs();
    s->tk_box[0] = x, s->tk_box[1] = y, s->tk_box[2] = w, s->tk_box[3] = h, s->tk_known |= TK_BOX;
    p_glScissor(x, y, w, h);
}
void h_glClearColor(float r, float g, float b, float a)
{
    VState *s = vs();
    s->tk_clear[0] = r, s->tk_clear[1] = g, s->tk_clear[2] = b, s->tk_clear[3] = a, s->tk_known |= TK_CLEAR;
    p_glClearColor(r, g, b, a);
}

void gles_state(u32 pname, void *out)
{
    VState *s = vs();
    u32 *o = out;
    u32 bit, n = 1, *k;
    switch (pname) {
    case GL_CURRENT_PROGRAM: bit = TK_PROG, k = &s->tk_prog; break;
    case GL_ACTIVE_TEXTURE: bit = TK_UNIT, k = &s->tk_unit; break;
    case GL_READ_FRAMEBUFFER_BINDING: bit = TK_RFB, k = &s->tk_rfb; break;
    case GL_FRAMEBUFFER_BINDING: bit = TK_DFB, k = &s->tk_dfb; break;
    case GL_RENDERBUFFER_BINDING: bit = TK_RB, k = &s->tk_rb; break;
    case GL_SCISSOR_TEST: bit = TK_SCISSOR_ON, k = &s->tk_scissor_on; break;
    case GL_SCISSOR_BOX: bit = TK_BOX, k = s->tk_box, n = 4; break;
    case GL_COLOR_CLEAR_VALUE: bit = TK_CLEAR, k = (u32 *)s->tk_clear, n = 4; break;
    case GL_TEXTURE_BINDING_2D: bit = TK_TEX0, k = &s->tk_tex0; break;
    case GL_ARRAY_BUFFER_BINDING: *o = s->array_buf; return;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *o = s->elem_buf; return;
    default:
        p_glGetIntegerv(pname, GA(o));
        return;
    }
    if (qf_enabled() && (s->tk_known & bit) && !qf_check) {
        memcpy(o, k, n * 4);
        return;
    }
    u32 v[4] = {0};
    if (pname == GL_COLOR_CLEAR_VALUE)
        p_glGetFloatv(pname, GA(v));
    else if (pname == GL_TEXTURE_BINDING_2D) {
        u32 was = 0;
        p_glGetIntegerv(GL_ACTIVE_TEXTURE, GA(&was));
        p_glActiveTexture(GL_TEXTURE0);
        p_glGetIntegerv(pname, GA(v));
        p_glActiveTexture(was);
    } else
        p_glGetIntegerv(pname, GA(v));
    if (qf_check && (s->tk_known & bit) && memcmp(v, k, n * 4)) {
        static volatile LONG bad;
        if (InterlockedIncrement(&bad) <= 20)
            dev_log("[gl] QUERY_FREE check: 0x%04x kept %u, the driver says %u (frame %u)\n", pname, k[0], v[0],
                   eagl_frames());
    }
    memcpy(o, v, n * 4);
    memcpy(k, v, n * 4);
    s->tk_known |= bit;
}
