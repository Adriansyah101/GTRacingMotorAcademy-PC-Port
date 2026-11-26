#ifndef GT_GL_H
#define GT_GL_H

#include <SDL3/SDL_opengl.h>

#include "arm_rt.h"

#ifdef _WIN32
#define GT_GLAPI __attribute__((stdcall))
#else
#define GT_GLAPI
#endif
#define GLP(name, params, args) typedef void(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#define GLR(ret, name, params, args) typedef ret(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#define GLX(ret, name, params) typedef ret(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#include "gl_list.h"

#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG  0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG  0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03
#endif

void gles_bind_fb(u32 target, u32 fb);
void gles_bind_rb(u32 rb);
void gles_state(u32 pname, void *out);

int gles_add_shader_patch(char *(*patch)(const char *src));
int gles_add_host_uniform(const char *name, float (*value)(void));
u32 gles_draws(void);
const char *gles_gpu_vendor(void);
const char *gles_gpu_renderer(void);
void gles_fault_dump(void);
int gles_selftest_fixed(void), gles_selftest_index(void), gles_selftest_pvrtc(void);

#endif
