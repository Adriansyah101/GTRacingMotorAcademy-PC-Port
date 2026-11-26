#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include "compat.h"

#include "rt.h"
#include "ui.h"

int gt_game_running;
static char dump_path[MAX_PATH * 2];
static int crash_sig;
static siginfo_t crash_si;
static ucontext_t crash_uc;
static volatile int have_ctx;
static volatile LONG crash_owner;

size_t rt_scrub(char *s, size_t n);

void gt_dump_arm(const char *log)
{
    const char *e = getenv("GT_CRASH_DUMP");
    if (e && *e == '0') {
        rt_log("[crash] no crash report (GT_CRASH_DUMP=0)\n");
        return;
    }
    snprintf(dump_path, sizeof dump_path, "%s", log);
    char *dot = strrchr(dump_path, '.'), *sep = strrchr(dump_path, '/');
    if (dot && (!sep || dot > sep))
        *dot = 0;
    strncat(dump_path, ".crash", sizeof dump_path - strlen(dump_path) - 1);
}

void gt_dump_note(int sig, const siginfo_t *si, const ucontext_t *uc)
{
    if (!__sync_bool_compare_and_swap(&have_ctx, 0, 1))
        return;
    crash_sig = sig;
    crash_si = *si;
    crash_uc = *uc;
}

static size_t put(char *b, size_t n, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t put(char *b, size_t n, size_t cap, const char *fmt, ...)
{
    if (n >= cap)
        return n;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + n, cap - n, fmt, ap);
    va_end(ap);
    return k < 0 ? n : n + (size_t)k < cap ? n + (size_t)k : cap - 1;
}

static int dump_write(void)
{
    if (!dump_path[0] || !gt_game_running)
        return 0;
    static char b[96 * 1024];
    size_t cap = sizeof b, n = 0;
    n = put(b, n, cap, GT_GAME_TITLE " crash report\n\n");
    if (have_ctx) {
        const greg_t *g = crash_uc.uc_mcontext.gregs;
        n = put(b, n, cap, "signal %d (%s), code %d, address 0x%08x\n", crash_sig, strsignal(crash_sig),
                crash_si.si_code, (u32)(uintptr_t)crash_si.si_addr);
        n = put(b, n, cap, "eip %08x  efl %08x  err %08x  trap %u\n", (u32)g[REG_EIP], (u32)g[REG_EFL],
                (u32)g[REG_ERR], (u32)g[REG_TRAPNO]);
        n = put(b, n, cap, "eax %08x  ebx %08x  ecx %08x  edx %08x\nesi %08x  edi %08x  ebp %08x  esp %08x\n",
                (u32)g[REG_EAX], (u32)g[REG_EBX], (u32)g[REG_ECX], (u32)g[REG_EDX], (u32)g[REG_ESI],
                (u32)g[REG_EDI], (u32)g[REG_EBP], (u32)g[REG_ESP]);
    } else {
        n = put(b, n, cap, "no fault: the game stopped itself\n");
    }
    RtThread *t = rt_thread_if();
    if (t) {
        const CPU *c = &t->cpu;
        n = put(b, n, cap, "\nguest thread %d\n", t->id);
        for (int i = 0; i < 16; i += 4)
            n = put(b, n, cap, "r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1],
                    i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    }
    void *frames[64];
    int nf = backtrace(frames, 64);
    n = put(b, n, cap, "\nhost stack (names in gtracing.map):\n");
    for (int i = 0; i < nf; i++)
        n = put(b, n, cap, "  %08x\n", (u32)(uintptr_t)frames[i]);
    n = put(b, n, cap, "\nmodules:\n");
    static char maps[64 * 1024];
    int mf = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    for (ssize_t r; mf >= 0 && got < sizeof maps - 1 && (r = read(mf, maps + got, sizeof maps - 1 - got)) > 0;)
        got += (size_t)r;
    if (mf >= 0)
        close(mf);
    maps[got] = 0;
    for (char *line = maps, *e; *line; line = e ? e + 1 : line + strlen(line)) {
        e = strchr(line, '\n');
        if (e)
            *e = 0;
        unsigned long lo, hi;
        char perm[5];
        int at = 0;
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &lo, &hi, perm, &at) == 3 && perm[2] == 'x' && at &&
            line[at] == '/')
            n = put(b, n, cap, "  %08lx-%08lx %s\n", lo, hi, line + at);
        if (!e)
            break;
    }
    n = rt_scrub(b, n);
    errno = 0;
    int fd = open(dump_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    size_t done = 0;
    while (fd >= 0 && done < n) {
        ssize_t w = write(fd, b + done, n - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    int ok = fd >= 0 && done == n;
    if (fd >= 0 && close(fd))
        ok = 0;
    if (!ok) {
        int e = errno;
        unlink(dump_path);
        rt_log("[crash] no crash report: cannot write the file (%s)\n", e ? strerror(e) : "short write");
        return 0;
    }
    const char *base = strrchr(dump_path, '/');
    rt_log("[crash] wrote %s, next to the log\n", base ? base + 1 : dump_path);
    return 1;
}

__attribute__((noreturn)) void gt_crash(int code, const char *why)
{
    LONG me = (LONG)GetCurrentThreadId();
    if (InterlockedCompareExchange(&crash_owner, me, 0)) {
        if (crash_owner == me)
            ExitProcess((UINT)code);
        for (;;)
            Sleep(INFINITE);
    }
    rt_log("\n[FATAL] %s\n", why);
    int dumped = dump_write();
    const char *e = dev_getenv("GT_QUIT_AT");
    if (e && *e) {
        rt_log("[crash] a test run (GT_QUIT_AT): no box\n");
        ExitProcess((UINT)code);
    }
    static char text[2048];
    int logged = rt_log_enabled();
    snprintf(text, sizeof text,
             GT_GAME_TITLE " has to stop.\n\n%s\n\n%s", why,
             dumped && logged ? "The details are in gtracing.log and gtracing.crash, next to the game's program. "
                                "Please send those two files with your bug report."
             : dumped         ? "The details are in gtracing.crash, next to the game's program. "
                                "Please send that file with your bug report."
             : logged         ? "The details are in gtracing.log, next to the game's program. "
                                "Please send that file with your bug report."
                              : "The log is off (GT_LOG=0): with GT_LOG unset the next crash leaves gtracing.log.");
    al_stop_all();
    if (ui_window) {
        SDL_SetWindowRelativeMouseMode(ui_window, false);
        SDL_SetWindowMouseGrab(ui_window, false);
        if (SDL_GetWindowFlags(ui_window) & SDL_WINDOW_FULLSCREEN)
            SDL_MinimizeWindow(ui_window);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GT_GAME_TITLE, text, NULL);
    ExitProcess((UINT)code);
}

void gt_crash_test_tick(u32 frame)
{
    static int at = -1;
    if (at < 0) {
        const char *e = dev_getenv("GT_CRASH_TEST");
        at = e && *e ? atoi(e) : 0;
    }
    if (at > 0 && frame >= (u32)at) {
        dev_log("[crash] GT_CRASH_TEST: a write to 0xfffffff0 on frame %u\n", frame);
        *(volatile u32 *)(uintptr_t)0xfffffff0u = 1;
    }
}
