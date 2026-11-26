#ifndef GT_RT_H
#define GT_RT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "arm_rt.h"
#include "layout.h"

#ifdef _WIN32
#define GT_SEP "\\"
#define GT_SEPC '\\'
#define GT_EXE_NAME "gtracing.exe"
#else
#define GT_SEP "/"
#define GT_SEPC '/'
#define GT_EXE_NAME "gtracing"
#endif

typedef struct RtJmp {
    u32 guest_buf;
    void *jb[5];
    u32 r[16];
    u64 d[8];
    RtFrame *top;
    struct RtJmp *next;
} RtJmp;

typedef struct RtThread {
    CPU cpu;
    RtFrame *top;
    u32 stack_lo, stack_hi;
    RtJmp *jmps;
    int id;
    u32 sjlj_head;
    u32 eh_caught;
    u32 eh_uncaught;
    void *host;
    void *objc_pool;
    u32 errno_cell;
} RtThread;

RtThread *rt_thread(void);
RtThread *rt_thread_if(void);
static inline CPU *rt_cpu(void) { return &rt_thread()->cpu; }
RtThread *rt_thread_new(u32 stack_size);
void rt_thread_end(void);

u32 rt_guest_call(CPU *c, u32 fn, int n, const u32 *args);
u64 rt_guest_call64(CPU *c, u32 fn, int n, const u32 *args);
u32 rt_call1(u32 fn, u32 a);
u32 rt_call2(u32 fn, u32 a, u32 b);
u32 rt_call3(u32 fn, u32 a, u32 b, u32 c3);
u32 rt_call4(u32 fn, u32 a, u32 b, u32 c3, u32 d);

void (*rt_lookup(u32 addr))(CPU *);

typedef void (*RtBridge)(CPU *);
typedef struct { const char *name; RtBridge fn; } RtBridgeEntry;
extern const RtBridgeEntry rt_bridges[];
extern const unsigned rt_bridge_count;
RtBridge rt_bridge_by_name(const char *name);
u32 rt_host_addr(RtBridge fn, const char *what);
u32 rt_import_addr(const char *name);
const char *rt_addr_name(u32 addr);
void rt_missing_import(CPU *c, const char *name);

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int rt_vlog(const char *fmt, va_list ap);
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log_open(const char *path);
int rt_log_enabled(void);
void rt_hide_path(const char *path, const char *tag);

void gt_settings_load(const char *exe_dir);
void gt_settings_log(void);
const char *gt_ini_path(void);
void gt_ini_forget_env(void);
int rt_setting_int(const char *key, int def, int lo, int hi);
float rt_setting_float(const char *key, float def, float lo, float hi);
int rt_parse_int(const char *s, int def, int lo, int hi);
float rt_parse_float(const char *s, float def, float lo, float hi);

extern int gt_game_running;
void gt_dump_arm(const char *log);
__attribute__((noreturn)) void gt_crash(int code, const char *why);
void gt_crash_test_tick(u32 frame);
int gt_selftest(void);

#ifndef GT_DEV
#define GT_DEV 0
#endif
#define dev_getenv(k) (GT_DEV ? getenv(k) : (char *)0)
#define dev_log(...) do { if (GT_DEV) rt_log(__VA_ARGS__); } while (0)
#if GT_DEV
extern int rt_trace;
#else
#define rt_trace 0
#endif

int gt_load(const char *exe_path, char *err, size_t errsz);
extern u32 gt_entry, gt_mh_header;

#define GP(a) ((void *)(uintptr_t)(u32)(a))
#define GA(p) ((u32)(uintptr_t)(p))
static inline u32 rd32(u32 a) { return *(ua32 *)GP(a); }
static inline void wr32(u32 a, u32 v) { *(ua32 *)GP(a) = v; }

#endif
