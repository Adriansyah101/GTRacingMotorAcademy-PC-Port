#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#endif

#include "gl.h"
#include "liveblocks.h"
#include "loader.h"
#include "ns.h"
#include "rt.h"
#include "ui.h"

static char app_dir[MAX_PATH], exe_dir[MAX_PATH];

void gt_objc_init(void);
void gt_objc_image(void);
void gt_objc_load(void);
extern char gt_reserve_start[];

static int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int has_game(const char *dir)
{
    char p[MAX_PATH * 3];
    if (snprintf(p, sizeof p, "%s" GT_SEP GT_GAME_NAME, dir) >= (int)sizeof p)
        return 0;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int take_app(const char *path)
{
    char tmp[MAX_PATH * 2];
    GetFullPathNameA(path, sizeof app_dir, app_dir, NULL);
    if (!is_dir(app_dir))
        return 0;
    if (has_game(app_dir))
        return 1;
    snprintf(tmp, sizeof tmp, "%s" GT_SEP GT_GAME_NAME ".app", app_dir);
    if (is_dir(tmp) && has_game(tmp)) {
        GetFullPathNameA(tmp, sizeof app_dir, app_dir, NULL);
        return 1;
    }
    return -1;
}

static int find_app(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--app"))
            return take_app(argv[i + 1]);
    static const char *const rel[] = {"", GT_SEP "..", GT_SEP ".." GT_SEP "..", GT_SEP ".." GT_SEP ".." GT_SEP ".."};
    int empty = 0;
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char tmp[MAX_PATH * 2];
        snprintf(tmp, sizeof tmp, "%s%s" GT_SEP GT_GAME_NAME ".app", exe_dir, rel[i]);
        int r = take_app(tmp);
        if (r > 0)
            return 1;
        if (r < 0)
            empty = 1;
    }
    return empty ? -1 : 0;
}

static __thread struct { int reg, active; u32 delta, eip; } low_fix;

#ifdef _WIN32
#define GT_LD "gt.ld"
#define GT_MAP_PATH "build\\host\\gtracing.map"
static DWORD *ctx_reg(CONTEXT *x, int r)
{
    switch (r) {
    case 0: return &x->Eax;
    case 1: return &x->Ecx;
    case 2: return &x->Edx;
    case 3: return &x->Ebx;
    case 4: return &x->Esp;
    case 5: return &x->Ebp;
    case 6: return &x->Esi;
    default: return &x->Edi;
    }
}
#else
#define GT_LD "gt_linux.ld"
#define GT_MAP_PATH "build/host/linux/gtracing.map"
static greg_t *ctx_reg(ucontext_t *x, int r)
{
    static const int map[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};
    return &x->uc_mcontext.gregs[map[r & 7]];
}
#endif

static int decode_mem(const u8 *p, int *base, int *index, int *scale, int *dest, int *dest_is_input)
{
    int two = 0;
    while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36)
        p++;
    u8 op = *p++;
    if (op == 0x0F) {
        two = 1;
        op = *p++;
    }
    *dest = -1;
    *dest_is_input = 0;
    if (!two) {
        if (op == 0x8B || op == 0x8A)
            *dest = 1;
        else if ((op & 0xC7) <= 0x03 && (op & 0xC7) >= 0x02 && op < 0x40) {
            *dest = 1;
            *dest_is_input = (op & 0xF8) != 0x38;
            if ((op & 0xF8) == 0x38)
                *dest = -1;
        } else if (op == 0x69 || op == 0x6B)
            *dest = 1;
        else if (!((op >= 0x80 && op <= 0x85) || op == 0xF6 || op == 0xF7 || op == 0xFF || op == 0x38 ||
                   op == 0x39 || op == 0x3A || op == 0x3B))
            return 0;
    } else {
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF)
            *dest = 1;
        else if (op == 0xAF || (op >= 0x40 && op <= 0x4F)) {
            *dest = 1;
            *dest_is_input = 1;
        } else if (!(op == 0x10 || op == 0x12 || op == 0x16 || op == 0x28 || op == 0x2E || op == 0x2F ||
                     op == 0x6E || op == 0x6F || op == 0x7E || (op >= 0x51 && op <= 0x5F) || op == 0x5A))
            return 0;
    }
    u8 modrm = *p++;
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (mod == 3)
        return 0;
    if (*dest == 1)
        *dest = reg;
    *base = rm;
    *index = -1;
    *scale = 1;
    if (rm == 4) {
        u8 sib = *p++;
        *scale = 1 << (sib >> 6);
        *index = (sib >> 3) & 7;
        if (*index == 4)
            *index = -1;
        *base = sib & 7;
        if (*base == 5 && mod == 0)
            *base = -1;
    } else if (rm == 5 && mod == 0) {
        *base = -1;
    }
    return *base >= 0 || *index >= 0;
}

#ifdef _WIN32
#define CTX_EIP(x) ((x)->Eip)
#define CTX_EFL(x) ((x)->EFlags)
static int fix_low_read(EXCEPTION_POINTERS *e, u32 addr)
{
    CONTEXT *x = e->ContextRecord;
#else
#define CTX_EIP(x) ((x)->uc_mcontext.gregs[REG_EIP])
#define CTX_EFL(x) ((x)->uc_mcontext.gregs[REG_EFL])
static int fix_low_read(ucontext_t *x, u32 addr)
{
#endif
    int base, index, scale, dest, dest_in;
    if (!decode_mem((const u8 *)(uintptr_t)CTX_EIP(x), &base, &index, &scale, &dest, &dest_in))
        return 0;
    int r = base >= 0 ? base : index;
    u32 delta = base >= 0 ? GT_LOW_DELTA : GT_LOW_DELTA / (u32)scale;
    if (r == dest && dest_in)
        return 0;
    static u32 seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == (u32)CTX_EIP(x))
            break;
        if (!seen[i]) {
            seen[i] = (u32)CTX_EIP(x);
            CPU *c = rt_cpu();
            rt_log("[low] the game read guest 0x%05x (a NULL pointer's field, readable on iOS) at host 0x%08lx, "
                   "lr 0x%08x\n", addr, (unsigned long)CTX_EIP(x), c->r[14]);
            break;
        }
    }
    *ctx_reg(x, r) += delta;
    low_fix.reg = r == dest ? -1 : r;
    low_fix.delta = delta;
    low_fix.active = 1;
    low_fix.eip = (u32)CTX_EIP(x);
    CTX_EFL(x) |= 0x100;
    return 1;
}

#ifdef _WIN32
void gt_dump_note(EXCEPTION_POINTERS *ep);
int gt_dump_is_writer(void);
__attribute__((noreturn)) void gt_crash(int code, const char *why);

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (gt_dump_is_writer())
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_SINGLE_STEP && low_fix.active) {
        low_fix.active = 0;
        if (low_fix.reg >= 0)
            *ctx_reg(e->ContextRecord, low_fix.reg) -= low_fix.delta;
        e->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->ExceptionInformation[0] == 0) {
        u32 a = (u32)e->ExceptionRecord->ExceptionInformation[1];
        if (a >= GT_LOW_START && a < GT_LOW_END && fix_low_read(e, a))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    u32 eip = (u32)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    static char why[512];
    gt_dump_note(e);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        u32 addr = (u32)e->ExceptionRecord->ExceptionInformation[1];
        const char *g = gt_guard_name(addr);
        const char *imp = rt_addr_name(addr);
        const char *what = e->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" :
                           e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read";
        rt_log("\n[FAULT] %s of 0x%08x at host 0x%08x%s%s%s%s\n", what, addr, eip,
               g ? " -- missing data import " : "", g ? g : "", imp ? " -- import address of " : "",
               imp ? imp : "");
        snprintf(why, sizeof why, "The game crashed: a bad memory %s at 0x%08x (host 0x%08x).", what, addr, eip);
    } else {
        rt_log("\n[FAULT] exception 0x%08lx at host 0x%08x\n", code, eip);
        snprintf(why, sizeof why, "The game crashed: %s (exception 0x%08lx at host 0x%08x).",
                 code == EXCEPTION_STACK_OVERFLOW ? "a stack overflow" :
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "a division by zero" : "an illegal instruction",
                 code, eip);
    }
    HMODULE mod = NULL;
    char modname[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)eip, &mod) &&
        mod != GetModuleHandleA(NULL) && GetModuleFileNameA(mod, modname, sizeof modname))
        rt_log("host address is in %s+0x%x\n", modname, eip - (u32)(uintptr_t)mod);
    CPU *c = rt_cpu();
    rt_log("guest r0-r3 %08x %08x %08x %08x  r4-r7 %08x %08x %08x %08x\n"
           "      r8-r12 %08x %08x %08x %08x %08x  sp %08x lr %08x\n", c->r[0], c->r[1], c->r[2], c->r[3],
           c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14]);
    gles_fault_dump();
    rt_log("(the host address names the function: " GT_MAP_PATH ")\n");
    gt_crash(4, why);
}
#else
void gt_dump_note(int sig, const siginfo_t *si, const ucontext_t *uc);
__attribute__((noreturn)) void gt_crash(int code, const char *why);

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *x = ctx;
    if (sig == SIGTRAP && low_fix.active) {
        low_fix.active = 0;
        if (low_fix.reg >= 0)
            *ctx_reg(x, low_fix.reg) -= low_fix.delta;
        CTX_EFL(x) &= ~0x100;
        return;
    }
    long err = x->uc_mcontext.gregs[REG_ERR];
    u32 addr = (u32)(uintptr_t)si->si_addr;
    if (sig == SIGSEGV && !(err & 2) && !(err & 0x10) && addr >= GT_LOW_START && addr < GT_LOW_END &&
        fix_low_read(x, addr))
        return;
    u32 eip = (u32)CTX_EIP(x);
    static char why[512];
    gt_dump_note(sig, si, x);
    if (sig == SIGSEGV || sig == SIGBUS) {
        const char *g = gt_guard_name(addr);
        const char *imp = rt_addr_name(addr);
        const char *what = err & 0x10 ? "execute" : err & 2 ? "write" : "read";
        rt_log("\n[FAULT] %s of 0x%08x at host 0x%08x%s%s%s%s\n", what, addr, eip,
               g ? " -- missing data import " : "", g ? g : "", imp ? " -- import address of " : "",
               imp ? imp : "");
        snprintf(why, sizeof why, "The game crashed: a bad memory %s at 0x%08x (host 0x%08x).", what, addr, eip);
    } else {
        rt_log("\n[FAULT] signal %d at host 0x%08x\n", sig, eip);
        snprintf(why, sizeof why, "The game crashed: %s (signal %d at host 0x%08x).",
                 sig == SIGFPE ? "a division by zero" : sig == SIGABRT ? "an abort" :
                 sig == SIGTRAP ? "a breakpoint" : "an illegal instruction", sig, eip);
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)eip, &di) && di.dli_fname && di.dli_fbase &&
        (uintptr_t)di.dli_fbase != GT_EXE_BASE)
        rt_log("host address is in %s+0x%x\n", di.dli_fname, eip - (u32)(uintptr_t)di.dli_fbase);
    CPU *c = rt_cpu();
    rt_log("guest r0-r3 %08x %08x %08x %08x  r4-r7 %08x %08x %08x %08x\n"
           "      r8-r12 %08x %08x %08x %08x %08x  sp %08x lr %08x\n", c->r[0], c->r[1], c->r[2], c->r[3],
           c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14]);
    gles_fault_dump();
    rt_log("(the host address names the function: " GT_MAP_PATH ")\n");
    gt_crash(4, why);
}

void gt_alt_stack(void);
static void faults_on(void)
{
    gt_alt_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

typedef struct { int argc; char **argv; int rc; } MainArgs;
int game_main(int argc, char **argv);
static void *main_thread(void *p)
{
    MainArgs *a = p;
    a->rc = game_main(a->argc, a->argv);
    return NULL;
}
int main(int argc, char **argv)
{
    MainArgs a = {argc, argv, 0};
    pthread_attr_t at;
    pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    int bad = pthread_create(&t, &at, main_thread, &a);
    pthread_attr_destroy(&at);
    if (bad)
        return game_main(argc, argv);
    pthread_join(t, NULL);
    return a.rc;
}
#define main game_main
#endif

int main(int argc, char **argv)
{
    char path[MAX_PATH * 2], err[512] = "";
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    char *s = strrchr(exe_dir, GT_SEPC);
    if (s)
        *s = 0;
    const char *wait_pid = getenv("GT_WAIT_PID");
    if (wait_pid && *wait_pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(wait_pid, NULL, 10));
        if (h) {
            WaitForSingleObject(h, 20000);
            CloseHandle(h);
        }
        _putenv_s("GT_WAIT_PID", "");
    }
    gt_settings_load(exe_dir);
    const char *log_env = getenv("GT_LOG");
    int log_off = log_env && !strcmp(log_env, "0");
    if (log_env && *log_env)
        snprintf(path, sizeof path, "%s", log_env);
    else
        snprintf(path, sizeof path, "%s" GT_SEP "gtracing.log", exe_dir);
    rt_hide_path(exe_dir, "%EXE%");
    if (getenv("GT_HOME") && *getenv("GT_HOME"))
        rt_hide_path(getenv("GT_HOME"), "%HOME%");
    rt_log_open(path);
    gt_settings_log();
    gt_live_init();
    if (log_off)
        snprintf(path, sizeof path, "%s" GT_SEP "gtracing.log", exe_dir);
    gt_dump_arm(path);
#if GT_DEV
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--trace"))
            rt_trace = 1;
    if (dev_getenv("GT_TRACE"))
        rt_trace = 1;
#endif
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);
#else
    faults_on();
#endif

    if ((u32)(uintptr_t)gt_reserve_start != GT_RESERVE_START)
        rt_fatal("the exe's reserved range starts at %p, not 0x%x (link with " GT_LD ")", gt_reserve_start,
                 GT_RESERVE_START);
    int found = find_app(argc, argv);
    if (found < 0)
        rt_fatal("The " GT_GAME_NAME ".app folder next to " GT_EXE_NAME " has no game in it.  Copy the files of "
                 "your own iPad game's " GT_GAME_NAME ".app into it (see the note in that folder).");
    if (!found)
        rt_fatal("no " GT_GAME_NAME ".app next to the program (or give --app <folder>)");
    rt_hide_path(app_dir, "%APP%");
    snprintf(path, sizeof path, "%s" GT_SEP GT_GAME_NAME, app_dir);

    RtThread *t = rt_thread_new(8u << 20);
    CPU *c = &t->cpu;
    gt_paths_init(app_dir, exe_dir);
    gt_objc_init();
    ns_init();
    ui_init();
    if (gt_load(path, err, sizeof err))
        rt_fatal("loading the game: %s", err);
    ns_fix_cfstrings();
    gt_objc_image();
    gt_objc_load();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--selftest")) {
            return gt_selftest();
        } else if (!strcmp(argv[i], "--objc-dump") && i + 1 < argc) {
            FILE *f = fopen(argv[i + 1], "w");
            if (!f)
                rt_fatal("cannot write %s", argv[i + 1]);
            gt_objc_dump(f);
            fclose(f);
            return 0;
        }

    static char exe_path[MAX_PATH * 2];
    snprintf(exe_path, sizeof exe_path, "%s/%s", gt_guest_bundle, GT_GAME_NAME);
    static char *gargv[2], *genvp[1], *gapple[2];
    gargv[0] = exe_path;
    gapple[0] = exe_path;
    static u32 vars_argc;
    vars_argc = 1;
    (void)vars_argc;

    gt_game_running = 1;

    for (u32 i = 0; i < gt_ninit; i++) {
        u32 fn = rd32(gt_init_funcs + 4 * i);
        if (rt_trace)
            rt_log("[init] %u/%u 0x%08x\n", i + 1, gt_ninit, fn);
        u32 args[4] = {1, GA(gargv), GA(genvp), GA(gapple)};
        rt_guest_call(c, fn, 4, args);
    }
    rt_log("[main] %u initializers done; starting the game at 0x%x\n", gt_ninit, gt_entry);

    u32 sp = (c->r[13] - 64) & ~7u;
    u32 w[] = {1, GA(exe_path), 0, 0, GA(exe_path), 0};
    for (unsigned i = 0; i < sizeof w / sizeof *w; i++)
        wr32(sp + 4 * i, w[i]);
    c->r[13] = sp;
    c->r[14] = 0x0FFF0000u;
    rt_call_indirect(c, gt_entry);
    rt_log("[main] start returned\n");
    return 0;
}
