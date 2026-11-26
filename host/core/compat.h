#ifndef GT_COMPAT_H
#define GT_COMPAT_H

#ifdef _WIN32
#include <windows.h>
#else

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD, ULONG;
typedef unsigned int UINT, UINT32;
typedef long LONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR, SIZE_T;
typedef void *HANDLE, *HMODULE, *LPVOID, *PTP_CALLBACK_INSTANCE;
typedef const char *LPCSTR;
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; } u; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { LONG Bias, StandardBias, DaylightBias; } TIME_ZONE_INFORMATION;
typedef struct { DWORD dwNumberOfProcessors; DWORD dwPageSize; } SYSTEM_INFO;
typedef struct { void *BaseAddress; SIZE_T RegionSize; DWORD State, Protect; } MEMORY_BASIC_INFORMATION;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[256];
} WIN32_FIND_DATAA;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 1024
#define INFINITE 0xFFFFFFFFul
#define WINAPI
#define CALLBACK
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFul
#define FILE_ATTRIBUTE_DIRECTORY 0x10ul
#define FILE_ATTRIBUTE_NORMAL 0x80ul
#define WAIT_OBJECT_0 0ul
#define WAIT_TIMEOUT 258ul
#define WAIT_FAILED 0xFFFFFFFFul
#define ERROR_ALREADY_EXISTS 183ul
#define ERROR_TIMEOUT 1460ul
#define MEM_COMMIT 0x1000ul
#define MEM_RESERVE 0x2000ul
#define MEM_RELEASE 0x8000ul
#define MEM_FREE 0x10000ul
#define PAGE_NOACCESS 0x01ul
#define PAGE_READONLY 0x02ul
#define PAGE_READWRITE 0x04ul
#define PAGE_GUARD 0x100ul
#define MOVEFILE_REPLACE_EXISTING 0x1ul
#define MOVEFILE_WRITE_THROUGH 0x8ul
#define GetFileExInfoStandard 0
#define TIME_ZONE_ID_STANDARD 1ul
#define TIME_ZONE_ID_DAYLIGHT 2ul
#define SYNCHRONIZE 0x00100000ul
#define TIMER_ALL_ACCESS 0x1F0003ul
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2ul
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000ul
#define DUPLICATE_SAME_ACCESS 0x2ul

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _fileno fileno
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _mkgmtime timegm
#define strtok_s strtok_r
#define SecureZeroMemory(p, n) explicit_bzero((p), (n))
#define localtime_s(tm, t) localtime_r((t), (tm))
#define gmtime_s(tm, t) gmtime_r((t), (tm))
#define _aligned_malloc(n, a) aligned_alloc((a), ((n) + (a) - 1) / (a) * (a))
#define _aligned_free free
#define MemoryBarrier() __sync_synchronize()
#define timeBeginPeriod(ms) ((void)(ms))
#define timeEndPeriod(ms) ((void)(ms))

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchangePointer(p, x, c) __sync_val_compare_and_swap((p), (c), (x))

typedef pthread_mutex_t CRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
#define EnterCriticalSection(cs) pthread_mutex_lock(cs)
#define LeaveCriticalSection(cs) pthread_mutex_unlock(cs)
#define TryEnterCriticalSection(cs) (pthread_mutex_trylock(cs) == 0)
#define DeleteCriticalSection(cs) pthread_mutex_destroy(cs)

typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define InitializeSRWLock(l) pthread_rwlock_init((l), NULL)
#define AcquireSRWLockExclusive(l) pthread_rwlock_wrlock(l)
#define AcquireSRWLockShared(l) pthread_rwlock_rdlock(l)
#define ReleaseSRWLockExclusive(l) pthread_rwlock_unlock(l)
#define ReleaseSRWLockShared(l) pthread_rwlock_unlock(l)
#define TryAcquireSRWLockExclusive(l) (pthread_rwlock_trywrlock(l) == 0)
#define TryAcquireSRWLockShared(l) (pthread_rwlock_tryrdlock(l) == 0)
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1ul

typedef struct { pthread_cond_t c; pthread_mutex_t m; } CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}
void InitializeConditionVariable(CONDITION_VARIABLE *c);
void WakeConditionVariable(CONDITION_VARIABLE *c);
void WakeAllConditionVariable(CONDITION_VARIABLE *c);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags);

typedef struct { pthread_mutex_t m; volatile int done; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
typedef BOOL (*PINIT_ONCE_FN)(INIT_ONCE *, void *, void **);
BOOL InitOnceBeginInitialize(INIT_ONCE *o, DWORD flags, BOOL *pending, void **ctx);
BOOL InitOnceComplete(INIT_ONCE *o, DWORD flags, void *ctx);
BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, void *param, void **ctx);

typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
typedef void (*PTP_SIMPLE_CALLBACK)(PTP_CALLBACK_INSTANCE, void *);
HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid);
HANDLE CreateEventA(void *sec, BOOL manual, BOOL initial, const char *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateWaitableTimerExW(void *sec, const void *name, DWORD flags, DWORD access);
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *wake, ULONG delay);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD opts);
BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK fn, void *arg, void *env);
BOOL SwitchToThread(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
void Sleep(DWORD ms);
__attribute__((noreturn)) void ExitProcess(UINT code);
DWORD GetLastError(void);
void SetLastError(DWORD e);
void GetSystemInfo(SYSTEM_INFO *si);

BOOL QueryPerformanceCounter(LARGE_INTEGER *t);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void GetSystemTimePreciseAsFileTime(FILETIME *ft);
void GetLocalTime(SYSTEMTIME *t);
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION *z);
BOOL GetProcessTimes(HANDLE p, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user);
BOOL GetThreadTimes(HANDLE t, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user);
int rand_s(unsigned *out);

void *VirtualAlloc(void *addr, SIZE_T n, DWORD type, DWORD prot);
BOOL VirtualFree(void *addr, SIZE_T n, DWORD type);
BOOL VirtualProtect(void *addr, SIZE_T n, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *m, SIZE_T n);

const char *gt_ci_path(const char *in, char *out, size_t n);
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n);
DWORD GetFullPathNameA(const char *p, DWORD n, char *out, char **file);
DWORD GetCurrentDirectoryA(DWORD n, char *out);
DWORD GetFileAttributesA(const char *p);
BOOL GetFileAttributesExA(const char *p, int level, void *out);
BOOL CreateDirectoryA(const char *p, void *sec);
BOOL RemoveDirectoryA(const char *p);
BOOL DeleteFileA(const char *p);
BOOL MoveFileA(const char *from, const char *to);
BOOL MoveFileExA(const char *from, const char *to, DWORD flags);
BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists);
BOOL GetDiskFreeSpaceExA(const char *p, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_);
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *d);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d);
BOOL FindClose(HANDLE h);
int _putenv_s(const char *name, const char *value);
int gt_free_hold(void *p);
void gt_free_unhold(void *p);

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define _O_BINARY 0
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_APPEND O_APPEND
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_EXCL O_EXCL
#define _S_IREAD 0444
#define _S_IWRITE 0222
#define _S_IFDIR S_IFDIR
#define _read read
#define _write write
#define _close close
#define _lseeki64 lseek
int _open(const char *p, int flags, int mode);
struct _stati64 {
    DWORD st_dev;
    unsigned st_mode;
    LONGLONG st_size;
    struct timespec st_atim, st_mtim, st_ctim;
};
int _stati64(const char *p, struct _stati64 *st);
int _fstati64(int fd, struct _stati64 *st);

typedef struct { WORD dmSize; DWORD dmPelsWidth, dmPelsHeight; } DEVMODEA;
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *dm);

#endif

#endif
