#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#ifdef _WIN32
#include <dxgi.h>
#include <d3d11.h>
#endif

#include "gl.h"
#include "rt.h"
#include "features.h"

static int integrated = -1;
static SRWLOCK lock = SRWLOCK_INIT;

static unsigned vendor_id(const char *v)
{
    if (strstr(v, "NVIDIA"))
        return 0x10de;
    if (strstr(v, "ATI") || strstr(v, "AMD"))
        return 0x1002;
    if (strstr(v, "Intel"))
        return 0x8086;
    return 0;
}

static int by_name(unsigned vid, const char *r)
{
    if (vid == 0x8086)
        return !(strstr(r, "Arc(TM) A") || strstr(r, "Arc(TM) B") || strstr(r, "Arc A") || strstr(r, "Arc B") ||
                 strstr(r, "Iris(R) Xe MAX"));
    if (vid == 0x1002) {
        if (strstr(r, " RX ") || strstr(r, "Pro") || strstr(r, "Series") || strstr(r, "FirePro"))
            return 0;
        if (strstr(r, "Radeon(TM) Graphics") || strstr(r, "Radeon Graphics") || strstr(r, "Vega"))
            return 1;
        for (const char *p = r; (p = strstr(p, "Radeon")) != NULL; p++) {
            const char *d = strchr(p, ' ');
            if (d && d[1] >= '6' && d[1] <= '8' && d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9' &&
                d[4] == 'M')
                return 1;
        }
        return 0;
    }
    return 0;
}

#ifdef _WIN32
static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

static int by_d3d(unsigned vid, const char *renderer, char *name, size_t nn)
{
    typedef HRESULT(WINAPI * T_factory)(REFIID, void **);
    typedef HRESULT(WINAPI * T_device)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                       UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    HMODULE dx = LoadLibraryA("dxgi.dll"), d3 = LoadLibraryA("d3d11.dll");
    T_factory mk = dx ? (T_factory)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    T_device dev_mk = d3 ? (T_device)(void *)GetProcAddress(d3, "D3D11CreateDevice") : NULL;
    IDXGIFactory1 *f = NULL;
    if (!mk || !dev_mk || FAILED(mk(&iid_factory1, (void **)&f)) || !f)
        return -1;
    IDXGIAdapter1 *best = NULL;
    int best_score = -1;
    IDXGIAdapter1 *a;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        int score = -1;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!vid || d.VendorId == vid)) {
            char n[128];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n, NULL, NULL);
            score = 1 + (*n && !strncmp(renderer, n, strlen(n)));
            if (score > best_score) {
                snprintf(name, nn, "%s", n);
            }
        }
        if (score > best_score) {
            if (best)
                IDXGIAdapter1_Release(best);
            best = a, best_score = score;
        } else
            IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (!best)
        return -1;
    ID3D11Device *dev = NULL;
    HRESULT hr = dev_mk((IDXGIAdapter *)best, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL,
                        NULL);
    IDXGIAdapter1_Release(best);
    if (FAILED(hr) || !dev)
        return -1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
    memset(&o2, 0, sizeof o2);
    hr = ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
    ID3D11Device_Release(dev);
    return FAILED(hr) ? -1 : o2.UnifiedMemoryArchitecture ? 1 : 0;
}
#else
static int by_d3d(unsigned vid, const char *renderer, char *name, size_t nn)
{
    (void)vid, (void)renderer, (void)nn;
    name[0] = 0;
    return -1;
}
#endif

static void detect(void)
{
    {
        const char *e = getenv("GT_GPU_PROFILE"), *v = gles_gpu_vendor(), *r = gles_gpu_renderer();
        char name[128] = "";
        static char tv[64], tr[128];
        const char *ta = dev_getenv("GT_GPU_TEST_AS"), *bar = ta ? strchr(ta, '|') : NULL;
        if (bar) {
            snprintf(tv, sizeof tv, "%.*s", (int)(bar - ta), ta);
            snprintf(tr, sizeof tr, "%s", bar + 1);
            v = tv, r = tr;
            rt_log("[gpu] test: asked as %s / %s (GT_GPU_TEST_AS)\n", v, r);
        }
        if (e && !_stricmp(e, "igpu")) {
            integrated = 1;
            rt_log("[gpu] %s: built-in GPU profile (GT_GPU_PROFILE=igpu)\n", *r ? r : "(no GL yet)");
        } else if (e && !_stricmp(e, "full")) {
            integrated = 0;
            rt_log("[gpu] %s: graphics card profile (GT_GPU_PROFILE=full)\n", *r ? r : "(no GL yet)");
        } else if (vendor_id(v) == 0x10de) {
            integrated = 0;
            rt_log("[gpu] %s: graphics card (NVIDIA); settings on auto: anti-aliasing 4x, the full motion blur\n", r);
        } else {
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceCounter(&t0);
            unsigned vid = vendor_id(v);
            int k = by_d3d(vid, r, name, sizeof name);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            integrated = k >= 0 ? k : by_name(vid, r);
            rt_log("[gpu] %s: %s (%s, %.0f ms); settings on auto: anti-aliasing 4x, %s motion blur\n",
                   *r ? r : "(no GL yet)", integrated ? "built-in GPU" : "graphics card",
                   k >= 0 ? (k ? "its memory is the CPU's (Direct3D 11)" : "memory of its own (Direct3D 11)")
                          : "by its name",
                   (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart,
                   integrated ? "the iPad 3's lighter" : "the full");
            if (*name && strncmp(r, name, strlen(name)))
                rt_log("[gpu] (Direct3D's adapter: %s)\n", name);
        }
    }
}

static HANDLE worker;
static DWORD WINAPI worker_main(void *p)
{
    (void)p;
    AcquireSRWLockExclusive(&lock);
    if (integrated < 0)
        detect();
    ReleaseSRWLockExclusive(&lock);
    return 0;
}

int gpu_setting_auto(const char *key);
void gpu_profile_start(void)
{
    if (!worker && integrated < 0 && gpu_setting_auto("GT_FULL_BLUR"))
        worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
}

static int gpu_integrated(void)
{
    if (integrated >= 0)
        return integrated;
    if (worker)
        WaitForSingleObject(worker, 5000);
    AcquireSRWLockExclusive(&lock);
    if (integrated < 0)
        detect();
    ReleaseSRWLockExclusive(&lock);
    return integrated;
}

int gpu_setting_auto(const char *key)
{
    const char *e = getenv(key);
    return !e || !*e || !_stricmp(e, "auto");
}

int gpu_auto_msaa(void) { return 4; }
int gpu_auto_full_blur(void) { return !gpu_integrated(); }
