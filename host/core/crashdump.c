#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>

#include "rt.h"
#include "ui.h"

typedef BOOL(WINAPI *minidump_fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

enum { DUMP_WAIT_MS = 20000 };

int gt_game_running;
static char dump_path[MAX_PATH * 2];
static EXCEPTION_POINTERS *volatile crash_ep;
static volatile DWORD crash_tid, writer_tid;
static HANDLE dump_file = INVALID_HANDLE_VALUE;
static const char *volatile dump_err;

void rt_scrub_bytes(unsigned char *d, size_t n);
void rt_scrub_names(unsigned char *d, size_t n);

void gt_dump_arm(const char *log)
{
    const char *e = getenv("GT_CRASH_DUMP");
    if (e && *e == '0') {
        rt_log("[crash] no crash dump (GT_CRASH_DUMP=0)\n");
        return;
    }
    snprintf(dump_path, sizeof dump_path, "%s", log);
    char *dot = strrchr(dump_path, '.'), *sep = strrchr(dump_path, '\\');
    if (dot && (!sep || dot > sep))
        *dot = 0;
    strncat(dump_path, ".dmp", sizeof dump_path - strlen(dump_path) - 1);
}

void gt_dump_note(EXCEPTION_POINTERS *ep)
{
    if (!InterlockedCompareExchangePointer((void *volatile *)&crash_ep, ep, NULL))
        crash_tid = GetCurrentThreadId();
}

int gt_dump_is_writer(void)
{
    return writer_tid && writer_tid == GetCurrentThreadId();
}

static void scrub_memory(unsigned char *d, DWORD n)
{
    const MINIDUMP_HEADER *h = (const MINIDUMP_HEADER *)d;
    if (n < sizeof *h || h->Signature != 0x504d444du || h->StreamDirectoryRva > n ||
        h->NumberOfStreams > (n - h->StreamDirectoryRva) / sizeof(MINIDUMP_DIRECTORY))
        return;
    const MINIDUMP_DIRECTORY *dir = (const MINIDUMP_DIRECTORY *)(d + h->StreamDirectoryRva);
    for (ULONG i = 0; i < h->NumberOfStreams; i++) {
        DWORD rva = dir[i].Location.Rva;
        if (dir[i].StreamType != MemoryListStream || rva > n - 4)
            continue;
        ULONG cnt = *(const ULONG *)(d + rva);
        if (cnt > (n - rva - 4) / sizeof(MINIDUMP_MEMORY_DESCRIPTOR))
            continue;
        const MINIDUMP_MEMORY_DESCRIPTOR *m = (const MINIDUMP_MEMORY_DESCRIPTOR *)(d + rva + 4);
        for (ULONG j = 0; j < cnt; j++)
            if (m[j].Memory.Rva <= n && m[j].Memory.DataSize <= n - m[j].Memory.Rva)
                rt_scrub_names(d + m[j].Memory.Rva, m[j].Memory.DataSize);
    }
}

static int scrub_file(HANDLE f)
{
    int ok = 0;
    DWORD n = GetFileSize(f, NULL);
    HANDLE map = n && n != INVALID_FILE_SIZE ? CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, 0, NULL) : NULL;
    unsigned char *d = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (d) {
        rt_scrub_bytes(d, n);
        scrub_memory(d, n);
        ok = UnmapViewOfFile(d);
    }
    if (map)
        CloseHandle(map);
    return ok;
}

static DWORD WINAPI dump_thread(void *unused)
{
    (void)unused;
    writer_tid = GetCurrentThreadId();
    HMODULE lib = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    minidump_fn write = lib ? (minidump_fn)(void *)GetProcAddress(lib, "MiniDumpWriteDump") : NULL;
    if (!write) {
        dump_err = "no dbghelp";
        return 0;
    }
    dump_file = CreateFileA(dump_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump_file == INVALID_HANDLE_VALUE) {
        dump_err = "cannot create the file";
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION ex = {crash_tid, crash_ep, FALSE};
    MINIDUMP_TYPE type = MiniDumpNormal | MiniDumpFilterModulePaths;
    if (!write(GetCurrentProcess(), GetCurrentProcessId(), dump_file, type, crash_ep ? &ex : NULL, NULL, NULL))
        dump_err = "MiniDumpWriteDump failed";
    else if (!scrub_file(dump_file))
        dump_err = "cannot clean the file";
    return 0;
}

static int dump_write(void)
{
    if (!dump_path[0] || !gt_game_running)
        return 0;
    dump_err = NULL;
    HANDLE t = CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
    if (!t)
        dump_err = "no thread";
    else {
        if (WaitForSingleObject(t, DUMP_WAIT_MS) != WAIT_OBJECT_0) {
            TerminateThread(t, 0);
            dump_err = "timed out";
        }
        CloseHandle(t);
    }
    if (dump_file != INVALID_HANDLE_VALUE) {
        CloseHandle(dump_file);
        dump_file = INVALID_HANDLE_VALUE;
    }
    if (dump_err) {
        DeleteFileA(dump_path);
        rt_log("[crash] no dump: %s\n", dump_err);
        return 0;
    }
    const char *base = strrchr(dump_path, '\\');
    rt_log("[crash] wrote %s, next to the log\n", base ? base + 1 : dump_path);
    return 1;
}

static wchar_t box_text[2048];
static volatile LONG crash_owner;
static volatile DWORD box_tid;

static BOOL CALLBACK minimise_full_screen(HWND w, LPARAM unused)
{
    (void)unused;
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w))
        return TRUE;
    MONITORINFO mi = {.cbSize = sizeof mi};
    RECT r;
    if (GetWindowRect(w, &r) && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) &&
        r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
        r.bottom >= mi.rcMonitor.bottom)
        ShowWindowAsync(w, SW_MINIMIZE);
    return TRUE;
}

static DWORD WINAPI box_thread(void *unused)
{
    (void)unused;
    box_tid = GetCurrentThreadId();
    MessageBoxW(NULL, box_text, L"" GT_GAME_TITLE, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

__attribute__((noreturn)) void gt_crash(int code, const char *why)
{
    DWORD me = GetCurrentThreadId();
    if (InterlockedCompareExchange(&crash_owner, (LONG)me, 0)) {
        if ((DWORD)crash_owner == me || box_tid == me)
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
             dumped && logged ? "The details are in gtracing.log and gtracing.dmp, next to the game's program. "
                                "Please send those two files with your bug report."
             : dumped         ? "The details are in gtracing.dmp, next to the game's program. "
                                "Please send that file with your bug report."
             : logged         ? "The details are in gtracing.log, next to the game's program. "
                                "Please send that file with your bug report."
                              : "The log is off (GT_LOG=0): with GT_LOG unset the next crash leaves gtracing.log.");
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, box_text, 2048))
        MultiByteToWideChar(CP_ACP, 0, text, -1, box_text, 2048);
    al_stop_all();
    ClipCursor(NULL);
    EnumWindows(minimise_full_screen, 0);
    HANDLE t = CreateThread(NULL, 0, box_thread, NULL, 0, NULL);
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    } else
        box_thread(NULL);
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
