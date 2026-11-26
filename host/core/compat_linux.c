#include <dirent.h>
#include <malloc.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "compat.h"

void gt_alt_stack(void);

static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }
static BOOL fail_errno(void)
{
    last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : (DWORD)errno;
    return FALSE;
}

void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}

static void abs_deadline(struct timespec *t, DWORD ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += (time_t)(ms / 1000);
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

void InitializeConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_cond_init(&c->c, NULL);
    pthread_mutex_init(&c->m, NULL);
}
void WakeConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_mutex_lock(&c->m);
    pthread_cond_signal(&c->c);
    pthread_mutex_unlock(&c->m);
}
void WakeAllConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_mutex_lock(&c->m);
    pthread_cond_broadcast(&c->c);
    pthread_mutex_unlock(&c->m);
}
static BOOL cond_wait(CONDITION_VARIABLE *c, DWORD ms)
{
    int r = 0;
    if (ms == INFINITE) {
        r = pthread_cond_wait(&c->c, &c->m);
    } else {
        struct timespec t;
        abs_deadline(&t, ms);
        r = pthread_cond_timedwait(&c->c, &c->m, &t);
    }
    pthread_mutex_unlock(&c->m);
    if (r == ETIMEDOUT) {
        last_error = ERROR_TIMEOUT;
        return FALSE;
    }
    return TRUE;
}
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms)
{
    pthread_mutex_lock(&c->m);
    pthread_mutex_unlock(cs);
    BOOL ok = cond_wait(c, ms);
    pthread_mutex_lock(cs);
    return ok;
}
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags)
{
    pthread_mutex_lock(&c->m);
    pthread_rwlock_unlock(l);
    BOOL ok = cond_wait(c, ms);
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED)
        pthread_rwlock_rdlock(l);
    else
        pthread_rwlock_wrlock(l);
    return ok;
}

BOOL InitOnceBeginInitialize(INIT_ONCE *o, DWORD flags, BOOL *pending, void **ctx)
{
    (void)flags, (void)ctx;
    if (__atomic_load_n(&o->done, __ATOMIC_ACQUIRE)) {
        *pending = FALSE;
        return TRUE;
    }
    pthread_mutex_lock(&o->m);
    if (o->done) {
        pthread_mutex_unlock(&o->m);
        *pending = FALSE;
        return TRUE;
    }
    *pending = TRUE;
    return TRUE;
}
BOOL InitOnceComplete(INIT_ONCE *o, DWORD flags, void *ctx)
{
    (void)flags, (void)ctx;
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, void *param, void **ctx)
{
    BOOL pending;
    InitOnceBeginInitialize(o, 0, &pending, NULL);
    if (pending) {
        fn(o, param, ctx);
        InitOnceComplete(o, 0, NULL);
    }
    return TRUE;
}

enum { H_THREAD = 1, H_EVENT, H_TIMER, H_PROCESS, H_FIND };
typedef struct Handle {
    int kind, refs;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled, manual;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    struct timespec due;
    pid_t pid;
    char **names;
    int nnames, next;
    char dir_path[MAX_PATH];
    char name[256];
} Handle;

static Handle *handle_new(int kind)
{
    Handle *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->kind = kind;
    h->refs = 1;
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    return h;
}
static void handle_unref(Handle *h)
{
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL))
        return;
    pthread_mutex_destroy(&h->m);
    pthread_cond_destroy(&h->c);
    free(h);
}
static void handle_signal(Handle *h)
{
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
}

static __thread Handle *self_handle;
static __thread void *alt_stack;

void gt_alt_stack(void)
{
    if (alt_stack)
        return;
    stack_t ss = {.ss_sp = malloc(SIGSTKSZ * 4), .ss_flags = 0, .ss_size = SIGSTKSZ * 4};
    if (ss.ss_sp && !sigaltstack(&ss, NULL))
        alt_stack = ss.ss_sp;
    else
        free(ss.ss_sp);
}

static void *thread_start(void *p)
{
    Handle *h = p;
    self_handle = h;
    gt_alt_stack();
    h->fn(h->arg);
    if (alt_stack) {
        stack_t off = {.ss_sp = NULL, .ss_flags = SS_DISABLE, .ss_size = 0};
        sigaltstack(&off, NULL);
        free(alt_stack);
        alt_stack = NULL;
    }
    self_handle = NULL;
    handle_signal(h);
    handle_unref(h);
    return NULL;
}

HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid)
{
    (void)sec, (void)flags;
    Handle *h = handle_new(H_THREAD);
    if (!h)
        return NULL;
    h->fn = fn;
    h->arg = arg;
    h->refs = 2;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, stack ? (size_t)stack : 16u << 20);
    pthread_t t;
    int r = pthread_create(&t, &a, thread_start, h);
    pthread_attr_destroy(&a);
    if (r) {
        handle_unref(h);
        handle_unref(h);
        errno = r;
        fail_errno();
        return NULL;
    }
    if (tid)
        *tid = 0;
    return h;
}

HANDLE CreateEventA(void *sec, BOOL manual, BOOL initial, const char *name)
{
    (void)sec, (void)name;
    Handle *h = handle_new(H_EVENT);
    if (h) {
        h->manual = manual;
        h->signaled = initial;
    }
    return h;
}
BOOL SetEvent(HANDLE p)
{
    handle_signal(p);
    return TRUE;
}
BOOL ResetEvent(HANDLE p)
{
    Handle *h = p;
    pthread_mutex_lock(&h->m);
    h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return TRUE;
}

HANDLE CreateWaitableTimerExW(void *sec, const void *name, DWORD flags, DWORD access)
{
    (void)sec, (void)name, (void)flags, (void)access;
    return handle_new(H_TIMER);
}
BOOL SetWaitableTimerEx(HANDLE p, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *wake, ULONG delay)
{
    (void)period, (void)fn, (void)arg, (void)wake, (void)delay;
    Handle *h = p;
    LONGLONG ns = due->QuadPart < 0 ? -due->QuadPart * 100 : due->QuadPart * 100;
    clock_gettime(CLOCK_MONOTONIC, &h->due);
    h->due.tv_sec += (time_t)(ns / 1000000000LL);
    h->due.tv_nsec += (long)(ns % 1000000000LL);
    if (h->due.tv_nsec >= 1000000000L) {
        h->due.tv_sec++;
        h->due.tv_nsec -= 1000000000L;
    }
    return TRUE;
}

HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    (void)access, (void)inherit;
    if (!pid || kill((pid_t)pid, 0))
        return NULL;
    Handle *h = handle_new(H_PROCESS);
    if (h)
        h->pid = (pid_t)pid;
    return h;
}

static DWORD wait_signal(Handle *h, DWORD ms)
{
    pthread_mutex_lock(&h->m);
    int r = 0;
    if (ms == INFINITE) {
        while (!h->signaled)
            pthread_cond_wait(&h->c, &h->m);
    } else {
        struct timespec t;
        abs_deadline(&t, ms);
        while (!h->signaled && r != ETIMEDOUT)
            r = pthread_cond_timedwait(&h->c, &h->m, &t);
    }
    int got = h->signaled;
    if (got && h->kind == H_EVENT && !h->manual)
        h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

static int process_alive(pid_t pid)
{
    if (kill(pid, 0) && errno == ESRCH)
        return 0;
    char p[64], st[512];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 1;
    ssize_t n = read(fd, st, sizeof st - 1);
    close(fd);
    st[n > 0 ? n : 0] = 0;
    const char *e = strrchr(st, ')');
    return !(e && e[1] == ' ' && (e[2] == 'Z' || e[2] == 'X'));
}

DWORD WaitForSingleObject(HANDLE p, DWORD ms)
{
    Handle *h = p;
    if (!h)
        return WAIT_FAILED;
    switch (h->kind) {
    case H_THREAD:
    case H_EVENT:
        return wait_signal(h, ms);
    case H_TIMER:
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &h->due, NULL) == EINTR)
            ;
        return WAIT_OBJECT_0;
    case H_PROCESS:
        for (DWORD waited = 0; ms == INFINITE || waited < ms; waited += 20) {
            if (!process_alive(h->pid))
                return WAIT_OBJECT_0;
            usleep(20000);
        }
        return WAIT_TIMEOUT;
    }
    return WAIT_FAILED;
}

BOOL CloseHandle(HANDLE p)
{
    if (!p || p == INVALID_HANDLE_VALUE)
        return FALSE;
    handle_unref(p);
    return TRUE;
}

#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)
#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD opts)
{
    (void)sp, (void)tp, (void)access, (void)inherit, (void)opts;
    if (src != PSEUDO_THREAD) {
        Handle *h = src;
        __atomic_add_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
        *out = h;
        return TRUE;
    }
    Handle *h = self_handle;
    if (!h)
        h = self_handle = handle_new(H_THREAD);
    if (!h)
        return FALSE;
    __atomic_add_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
    *out = h;
    return TRUE;
}

typedef struct Job { struct Job *next; PTP_SIMPLE_CALLBACK fn; void *arg; } Job;
static struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    Job *head, *tail;
    int workers, idle;
} pool = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 0};

static void *pool_main(void *p)
{
    (void)p;
    pthread_mutex_lock(&pool.m);
    for (;;) {
        while (!pool.head) {
            pool.idle++;
            pthread_cond_wait(&pool.c, &pool.m);
            pool.idle--;
        }
        Job *j = pool.head;
        pool.head = j->next;
        if (!pool.head)
            pool.tail = NULL;
        pthread_mutex_unlock(&pool.m);
        j->fn(NULL, j->arg);
        free(j);
        pthread_mutex_lock(&pool.m);
    }
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK fn, void *arg, void *env)
{
    (void)env;
    Job *j = malloc(sizeof *j);
    if (!j)
        return FALSE;
    j->next = NULL, j->fn = fn, j->arg = arg;
    pthread_mutex_lock(&pool.m);
    if (pool.tail)
        pool.tail->next = j;
    else
        pool.head = j;
    pool.tail = j;
    if (!pool.idle && pool.workers < 16) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 1u << 20);
        pthread_t t;
        if (!pthread_create(&t, &a, pool_main, NULL))
            pool.workers++;
        pthread_attr_destroy(&a);
    }
    pthread_cond_signal(&pool.c);
    int ok = pool.workers > 0;
    pthread_mutex_unlock(&pool.m);
    return ok;
}

BOOL SwitchToThread(void) { return sched_yield() == 0; }
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
void ExitProcess(UINT code)
{
    fflush(NULL);
    _exit((int)code);
}

void Sleep(DWORD ms)
{
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR)
        ;
}

void GetSystemInfo(SYSTEM_INFO *si)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwPageSize = 4096;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *t)
{
    struct timespec s;
    clock_gettime(CLOCK_MONOTONIC, &s);
    t->QuadPart = (LONGLONG)s.tv_sec * 1000000000LL + s.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}
ULONGLONG GetTickCount64(void)
{
    struct timespec s;
    clock_gettime(CLOCK_MONOTONIC, &s);
    return (ULONGLONG)s.tv_sec * 1000u + (ULONGLONG)s.tv_nsec / 1000000u;
}
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

#define FT_UNIX_EPOCH 116444736000000000ULL
static FILETIME ft_of(ULONGLONG v) { return (FILETIME){(DWORD)v, (DWORD)(v >> 32)}; }
static FILETIME ft_unix(time_t sec, long nsec)
{
    return ft_of((ULONGLONG)((LONGLONG)sec * 10000000LL + nsec / 100) + FT_UNIX_EPOCH);
}
void GetSystemTimePreciseAsFileTime(FILETIME *ft)
{
    struct timespec s;
    clock_gettime(CLOCK_REALTIME, &s);
    *ft = ft_unix(s.tv_sec, s.tv_nsec);
}
void GetLocalTime(SYSTEMTIME *st)
{
    struct timespec s;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &s);
    localtime_r(&s.tv_sec, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(s.tv_nsec / 1000000L);
}
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION *z)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    memset(z, 0, sizeof *z);
    z->Bias = (LONG)(-tm.tm_gmtoff / 60);
    return TIME_ZONE_ID_STANDARD;
}
static BOOL cpu_times(clockid_t id, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user)
{
    struct timespec s;
    if (clock_gettime(id, &s))
        return FALSE;
    *create = *exit_ = *kernel = ft_of(0);
    *user = ft_of((ULONGLONG)s.tv_sec * 10000000u + (ULONGLONG)s.tv_nsec / 100u);
    return TRUE;
}
BOOL GetProcessTimes(HANDLE p, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user)
{
    (void)p;
    return cpu_times(CLOCK_PROCESS_CPUTIME_ID, create, exit_, kernel, user);
}
BOOL GetThreadTimes(HANDLE t, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user)
{
    (void)t;
    return cpu_times(CLOCK_THREAD_CPUTIME_ID, create, exit_, kernel, user);
}

int rand_s(unsigned *out)
{
    if (getrandom(out, sizeof *out, 0) == (ssize_t)sizeof *out)
        return 0;
    struct timespec s;
    clock_gettime(CLOCK_MONOTONIC, &s);
    *out = (unsigned)(s.tv_nsec ^ (s.tv_sec << 20) ^ getpid());
    return 0;
}

typedef struct { uintptr_t lo; size_t n; } Region;
static Region regions[64];
static pthread_mutex_t regions_lock = PTHREAD_MUTEX_INITIALIZER;

static int prot_of(DWORD p)
{
    if (p & PAGE_NOACCESS)
        return PROT_NONE;
    if (p & PAGE_READONLY)
        return PROT_READ;
    return PROT_READ | PROT_WRITE;
}

void *VirtualAlloc(void *addr, SIZE_T n, DWORD type, DWORD prot)
{
    (void)type;
    size_t len = ((size_t)n + 4095) & ~(size_t)4095;
    void *p = mmap(addr, len, prot_of(prot), MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) {
        fail_errno();
        return NULL;
    }
    pthread_mutex_lock(&regions_lock);
    for (int i = 0; i < 64; i++)
        if (!regions[i].lo) {
            regions[i] = (Region){(uintptr_t)p, len};
            break;
        }
    pthread_mutex_unlock(&regions_lock);
    return p;
}
BOOL VirtualFree(void *addr, SIZE_T n, DWORD type)
{
    (void)type;
    size_t len = (size_t)n;
    pthread_mutex_lock(&regions_lock);
    for (int i = 0; i < 64; i++)
        if (regions[i].lo == (uintptr_t)addr) {
            if (!len)
                len = regions[i].n;
            regions[i].lo = 0;
            break;
        }
    pthread_mutex_unlock(&regions_lock);
    return len && !munmap(addr, len) ? TRUE : fail_errno();
}
BOOL VirtualProtect(void *addr, SIZE_T n, DWORD prot, DWORD *old)
{
    uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095, hi = ((uintptr_t)addr + n + 4095) & ~(uintptr_t)4095;
    if (old)
        *old = PAGE_READWRITE;
    return mprotect((void *)lo, hi - lo, prot_of(prot)) ? fail_errno() : TRUE;
}
static SIZE_T query_maps(char *buf, uintptr_t a, MEMORY_BASIC_INFORMATION *m);
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *m, SIZE_T n)
{
    (void)n;
    uintptr_t a = (uintptr_t)addr;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    static char buf[256 * 1024];
    static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&buf_lock);
    size_t got = 0;
    for (ssize_t r; got < sizeof buf - 1 && (r = read(fd, buf + got, sizeof buf - 1 - got)) > 0;)
        got += (size_t)r;
    close(fd);
    buf[got] = 0;
    SIZE_T done = query_maps(buf, a, m);
    pthread_mutex_unlock(&buf_lock);
    return done;
}

static SIZE_T query_maps(char *buf, uintptr_t a, MEMORY_BASIC_INFORMATION *m)
{
    uintptr_t gap = a & ~(uintptr_t)4095;
    for (char *s = buf; *s;) {
        char *e = strchr(s, '\n');
        unsigned long lo, hi;
        char perm[5];
        if (sscanf(s, "%lx-%lx %4s", &lo, &hi, perm) == 3) {
            if (a < lo) {
                *m = (MEMORY_BASIC_INFORMATION){(void *)gap, lo - gap, MEM_FREE, PAGE_NOACCESS};
                return sizeof *m;
            }
            if (a < hi) {
                DWORD p = perm[0] != 'r' ? PAGE_NOACCESS : perm[1] == 'w' ? PAGE_READWRITE : PAGE_READONLY;
                *m = (MEMORY_BASIC_INFORMATION){(void *)(uintptr_t)lo, hi - lo, MEM_COMMIT, p};
                return sizeof *m;
            }
            gap = hi;
        }
        if (!e)
            break;
        s = e + 1;
    }
    *m = (MEMORY_BASIC_INFORMATION){(void *)gap, 4096, MEM_FREE, PAGE_NOACCESS};
    return sizeof *m;
}

typedef struct {
    char *dir;
    time_t sec;
    long nsec;
    char **names;
    int n;
} DirCache;
static DirCache dcache[64];
static int dcache_next;
static pthread_mutex_t dcache_lock = PTHREAD_MUTEX_INITIALIZER;

static int mtime_of(const char *p, time_t *sec, long *nsec)
{
    struct statx sx;
    if (statx(AT_FDCWD, p, 0, STATX_MTIME | STATX_TYPE, &sx))
        return -1;
    *sec = (time_t)sx.stx_mtime.tv_sec;
    *nsec = (long)sx.stx_mtime.tv_nsec;
    return (sx.stx_mode & S_IFMT) == S_IFDIR ? 0 : -1;
}

static int dir_match(const char *dir, const char *name, char *out, size_t n)
{
    time_t sec;
    long nsec;
    if (mtime_of(dir, &sec, &nsec))
        return 0;
    pthread_mutex_lock(&dcache_lock);
    DirCache *c = NULL;
    for (int i = 0; i < 64; i++)
        if (dcache[i].dir && !strcmp(dcache[i].dir, dir)) {
            c = &dcache[i];
            break;
        }
    if (c && (c->sec != sec || c->nsec != nsec)) {
        for (int i = 0; i < c->n; i++)
            free(c->names[i]);
        free(c->names);
        c->names = NULL;
        c->n = 0;
        c->sec = sec;
        c->nsec = nsec;
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            char **nn = realloc(c->names, sizeof *nn * (size_t)(c->n + 1));
            if (!nn)
                break;
            c->names = nn;
            c->names[c->n++] = strdup(e->d_name);
        }
        if (d)
            closedir(d);
    } else if (!c) {
        c = &dcache[dcache_next++ & 63];
        for (int i = 0; i < c->n; i++)
            free(c->names[i]);
        free(c->names);
        free(c->dir);
        *c = (DirCache){strdup(dir), 0, -1, NULL, 0};
        pthread_mutex_unlock(&dcache_lock);
        return dir_match(dir, name, out, n);
    }
    int found = 0;
    for (int i = 0; i < c->n; i++)
        if (!strcasecmp(c->names[i], name) && strlen(c->names[i]) < n) {
            strcpy(out, c->names[i]);
            found = 1;
            break;
        }
    pthread_mutex_unlock(&dcache_lock);
    return found;
}

const char *gt_ci_path(const char *in, char *out, size_t n)
{
    size_t len = 0;
    for (; in[len] && len + 1 < n; len++)
        out[len] = in[len] == '\\' ? '/' : in[len];
    out[len] = 0;
    if (!out[0] || !access(out, F_OK))
        return out;
    char res[PATH_MAX], part[NAME_MAX + 1], real[NAME_MAX + 1];
    size_t r = 0;
    const char *s = out;
    if (*s == '/') {
        res[r++] = '/';
        while (*s == '/')
            s++;
    }
    res[r] = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t pl = e ? (size_t)(e - s) : strlen(s);
        if (pl > NAME_MAX || r + pl + 2 >= sizeof res)
            return out;
        memcpy(part, s, pl);
        part[pl] = 0;
        size_t at = r;
        memcpy(res + r, part, pl + 1);
        r += pl;
        if (access(res, F_OK)) {
            res[at] = 0;
            if (dir_match(at ? res : ".", part, real, sizeof real)) {
                size_t rl = strlen(real);
                if (at + rl + 2 >= sizeof res)
                    return out;
                memcpy(res + at, real, rl + 1);
                r = at + rl;
            } else {
                snprintf(res + at, sizeof res - at, "%s", s);
                r = strlen(res);
                break;
            }
        }
        if (!e)
            break;
        s = e;
        while (*s == '/')
            s++;
        if (*s && r + 1 < sizeof res) {
            res[r++] = '/';
            res[r] = 0;
        }
    }
    snprintf(out, n, "%s", res);
    return out;
}

static int stat_of(const char *p, struct statx *sx)
{
    char buf[PATH_MAX];
    return statx(AT_FDCWD, gt_ci_path(p, buf, sizeof buf), 0, STATX_BASIC_STATS | STATX_BTIME, sx);
}

int __real_remove(const char *p);
int __wrap_remove(const char *p)
{
    char buf[PATH_MAX];
    return __real_remove(p ? gt_ci_path(p, buf, sizeof buf) : p);
}
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    return __real_rename(gt_ci_path(from, a, sizeof a), gt_ci_path(to, b, sizeof b));
}

int _open(const char *p, int flags, int mode)
{
    char buf[PATH_MAX];
    return open(gt_ci_path(p, buf, sizeof buf), flags | O_CLOEXEC, mode);
}

static void put_stati64(const struct statx *sx, struct _stati64 *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = (DWORD)(sx->stx_dev_major << 8 | sx->stx_dev_minor);
    st->st_mode = sx->stx_mode;
    st->st_size = (LONGLONG)sx->stx_size;
    st->st_atim = (struct timespec){(time_t)sx->stx_atime.tv_sec, (long)sx->stx_atime.tv_nsec};
    st->st_mtim = (struct timespec){(time_t)sx->stx_mtime.tv_sec, (long)sx->stx_mtime.tv_nsec};
    st->st_ctim = (struct timespec){(time_t)sx->stx_ctime.tv_sec, (long)sx->stx_ctime.tv_nsec};
}
int _stati64(const char *p, struct _stati64 *st)
{
    struct statx sx;
    if (stat_of(p, &sx))
        return -1;
    put_stati64(&sx, st);
    return 0;
}
int _fstati64(int fd, struct _stati64 *st)
{
    struct statx sx;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx))
        return -1;
    put_stati64(&sx, st);
    return 0;
}

enum { LIVE_MAX = 1024 };
static FILE *live_files[LIVE_MAX];
static int n_live, untracked;
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *open_stream(FILE *(*real)(const char *, const char *), const char *p, const char *mode)
{
    char buf[PATH_MAX];
    FILE *f = real(p ? gt_ci_path(p, buf, sizeof buf) : p, mode);
    if (f) {
        pthread_mutex_lock(&files_lock);
        if (n_live < LIVE_MAX)
            live_files[n_live++] = f;
        else
            untracked = 1;
        pthread_mutex_unlock(&files_lock);
    }
    return f;
}

FILE *__real_fopen(const char *p, const char *mode);
FILE *__real_fopen64(const char *p, const char *mode);
FILE *__wrap_fopen(const char *p, const char *mode) { return open_stream(__real_fopen, p, mode); }
FILE *__wrap_fopen64(const char *p, const char *mode) { return open_stream(__real_fopen64, p, mode); }

#undef _fileno
int rt_log(const char *fmt, ...);
int __real_fclose(FILE *f);
int __wrap_fclose(FILE *f)
{
    if (f == stdin || f == stdout || f == stderr)
        return __real_fclose(f);
    pthread_mutex_lock(&files_lock);
    int i = 0;
    while (i < n_live && live_files[i] != f)
        i++;
    int live = i < n_live;
    if (live)
        live_files[i] = live_files[--n_live];
    pthread_mutex_unlock(&files_lock);
    if (live) {
        int rc = fflush(f);
        int fd = fileno(f);
        if (fd >= 0 && close(fd))
            rc = EOF;
        f->_fileno = -1;
        setvbuf(f, NULL, _IONBF, 0);
        clearerr(f);
        return rc ? EOF : 0;
    }
    if (untracked)
        return __real_fclose(f);
    static int said;
    if (said++ < 3)
        rt_log("[file] fclose of a stream that is not open (%p): ignored, as on iOS and Windows\n", (void *)f);
    errno = EBADF;
    return EOF;
}

DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n)
{
    (void)m;
    ssize_t r = readlink("/proc/self/exe", out, n ? n - 1 : 0);
    if (r < 0) {
        if (n)
            out[0] = 0;
        return 0;
    }
    out[r] = 0;
    return (DWORD)r;
}

DWORD GetCurrentDirectoryA(DWORD n, char *out)
{
    if (!getcwd(out, n)) {
        if (n)
            out[0] = 0;
        return 0;
    }
    return (DWORD)strlen(out);
}

DWORD GetFullPathNameA(const char *p, DWORD n, char *out, char **file)
{
    char tmp[PATH_MAX * 2], res[PATH_MAX];
    size_t len = 0;
    if (p[0] != '/' && p[0] != '\\') {
        if (!getcwd(tmp, PATH_MAX))
            tmp[0] = 0;
        len = strlen(tmp);
        tmp[len++] = '/';
    }
    snprintf(tmp + len, sizeof tmp - len, "%s", p);
    size_t o = 0;
    for (char *s = tmp; *s;) {
        while (*s == '/' || *s == '\\')
            s++;
        char *e = s;
        while (*e && *e != '/' && *e != '\\')
            e++;
        size_t pl = (size_t)(e - s);
        if (!pl)
            break;
        if (pl == 1 && s[0] == '.') {
        } else if (pl == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && res[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
        } else if (o + pl + 2 < sizeof res) {
            res[o++] = '/';
            memcpy(res + o, s, pl);
            o += pl;
        }
        s = e;
    }
    if (!o)
        res[o++] = '/';
    res[o] = 0;
    if (o + 1 > n)
        return (DWORD)(o + 1);
    memcpy(out, res, o + 1);
    if (file) {
        char *b = strrchr(out, '/');
        *file = b ? b + 1 : out;
    }
    return (DWORD)o;
}


DWORD GetFileAttributesA(const char *p)
{
    struct statx sx;
    if (stat_of(p, &sx)) {
        fail_errno();
        return INVALID_FILE_ATTRIBUTES;
    }
    return (sx.stx_mode & S_IFMT) == S_IFDIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

static void fill_attr(const struct statx *sx, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    d->dwFileAttributes = (sx->stx_mode & S_IFMT) == S_IFDIR ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->ftLastWriteTime = ft_unix((time_t)sx->stx_mtime.tv_sec, (long)sx->stx_mtime.tv_nsec);
    d->ftLastAccessTime = ft_unix((time_t)sx->stx_atime.tv_sec, (long)sx->stx_atime.tv_nsec);
    d->ftCreationTime = sx->stx_mask & STATX_BTIME ? ft_unix((time_t)sx->stx_btime.tv_sec, (long)sx->stx_btime.tv_nsec)
                                                    : d->ftLastWriteTime;
    d->nFileSizeHigh = (DWORD)(sx->stx_size >> 32);
    d->nFileSizeLow = (DWORD)sx->stx_size;
}

BOOL GetFileAttributesExA(const char *p, int level, void *out)
{
    (void)level;
    struct statx sx;
    if (stat_of(p, &sx))
        return fail_errno();
    fill_attr(&sx, out);
    return TRUE;
}

BOOL CreateDirectoryA(const char *p, void *sec)
{
    (void)sec;
    char buf[PATH_MAX];
    return mkdir(gt_ci_path(p, buf, sizeof buf), 0755) ? fail_errno() : TRUE;
}
BOOL RemoveDirectoryA(const char *p)
{
    char buf[PATH_MAX];
    return rmdir(gt_ci_path(p, buf, sizeof buf)) ? fail_errno() : TRUE;
}
BOOL DeleteFileA(const char *p)
{
    char buf[PATH_MAX];
    return unlink(gt_ci_path(p, buf, sizeof buf)) ? fail_errno() : TRUE;
}
BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    char a[PATH_MAX], b[PATH_MAX];
    gt_ci_path(from, a, sizeof a);
    gt_ci_path(to, b, sizeof b);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !access(b, F_OK)) {
        last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    if (rename(a, b))
        return fail_errno();
    if (flags & MOVEFILE_WRITE_THROUGH) {
        char *s = strrchr(b, '/');
        if (s) {
            *s = 0;
            int fd = open(*b ? b : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd >= 0) {
                fsync(fd);
                close(fd);
            }
        }
    }
    return TRUE;
}
BOOL MoveFileA(const char *from, const char *to) { return MoveFileExA(from, to, 0); }

BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists)
{
    char a[PATH_MAX], b[PATH_MAX];
    int in = open(gt_ci_path(from, a, sizeof a), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return fail_errno();
    int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (fail_if_exists ? O_EXCL : 0);
    int out = open(gt_ci_path(to, b, sizeof b), flags, 0644);
    if (out < 0) {
        close(in);
        return fail_errno();
    }
    char buf[65536];
    ssize_t r;
    int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) {
            ok = 0;
            break;
        }
    if (r < 0)
        ok = 0;
    close(in);
    if (close(out))
        ok = 0;
    return ok ? TRUE : fail_errno();
}

BOOL GetDiskFreeSpaceExA(const char *p, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_)
{
    char buf[PATH_MAX];
    struct statvfs v;
    if (statvfs(gt_ci_path(p, buf, sizeof buf), &v))
        return fail_errno();
    if (avail)
        avail->QuadPart = (ULONGLONG)v.f_bavail * v.f_frsize;
    if (total)
        total->QuadPart = (ULONGLONG)v.f_blocks * v.f_frsize;
    if (free_)
        free_->QuadPart = (ULONGLONG)v.f_bfree * v.f_frsize;
    return TRUE;
}

static int name_order(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int k = strcasecmp(x, y);
    return k ? k : strcmp(x, y);
}

static void find_free(Handle *h)
{
    for (int i = 0; i < h->nnames; i++)
        free(h->names[i]);
    free(h->names);
    h->names = NULL;
    h->nnames = 0;
}

static int find_fill(Handle *h, WIN32_FIND_DATAA *d)
{
    while (h->next < h->nnames) {
        const char *e = h->names[h->next++];
        if (h->name[0] && strcasecmp(e, h->name))
            continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", h->dir_path, e);
        struct statx sx;
        WIN32_FILE_ATTRIBUTE_DATA a = {0};
        if (!statx(AT_FDCWD, p, 0, STATX_BASIC_STATS | STATX_BTIME, &sx))
            fill_attr(&sx, &a);
        else
            a.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        memset(d, 0, sizeof *d);
        d->dwFileAttributes = a.dwFileAttributes;
        d->ftCreationTime = a.ftCreationTime;
        d->ftLastAccessTime = a.ftLastAccessTime;
        d->ftLastWriteTime = a.ftLastWriteTime;
        d->nFileSizeHigh = a.nFileSizeHigh;
        d->nFileSizeLow = a.nFileSizeLow;
        snprintf(d->cFileName, sizeof d->cFileName, "%.255s", e);
        return 1;
    }
    return 0;
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *d)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", pattern);
    for (char *s = p; *s; s++)
        if (*s == '\\')
            *s = '/';
    char *slash = strrchr(p, '/');
    const char *name = slash ? slash + 1 : p;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - p) : 1, slash ? p : ".");
    if (!dir[0])
        snprintf(dir, sizeof dir, "/");
    Handle *h = handle_new(H_FIND);
    if (!h)
        return INVALID_HANDLE_VALUE;
    gt_ci_path(dir, h->dir_path, sizeof h->dir_path);
    if (strcmp(name, "*") && strcmp(name, "*.*"))
        snprintf(h->name, sizeof h->name, "%.255s", name);
    DIR *dp = opendir(h->dir_path);
    for (struct dirent *e; dp && (e = readdir(dp));) {
        char **nn = realloc(h->names, sizeof *nn * (size_t)(h->nnames + 1));
        if (!nn)
            break;
        h->names = nn;
        if ((h->names[h->nnames] = strdup(e->d_name)))
            h->nnames++;
    }
    if (dp)
        closedir(dp);
    if (h->nnames > 1)
        qsort(h->names, (size_t)h->nnames, sizeof *h->names, name_order);
    if (!dp || !find_fill(h, d)) {
        find_free(h);
        handle_unref(h);
        last_error = 2;
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
BOOL FindNextFileA(HANDLE p, WIN32_FIND_DATAA *d)
{
    Handle *h = p;
    return find_fill(h, d) ? TRUE : FALSE;
}
BOOL FindClose(HANDLE p)
{
    Handle *h = p;
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    find_free(h);
    handle_unref(h);
    return TRUE;
}

enum { Q_SLOTS = 65536, Q_BUCKETS = 65536, Q_BYTES = 32u << 20, Q_BIG = 256u << 10 };
static struct { void *p; size_t n; int next; } q_ring[Q_SLOTS];
static int q_bucket[Q_BUCKETS];
static unsigned q_head, q_count;
static size_t q_bytes;
static int q_ready;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
void __real_free(void *p);
int rt_log(const char *fmt, ...);

static unsigned q_hash(const void *p) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) >> 16; }

static int q_find(const void *p, int **link)
{
    int *l = &q_bucket[q_hash(p)];
    while (*l >= 0 && q_ring[*l].p != p)
        l = &q_ring[*l].next;
    if (link)
        *link = l;
    return *l;
}

static void q_drop(unsigned slot)
{
    int *l;
    if (q_ring[slot].p && q_find(q_ring[slot].p, &l) == (int)slot)
        *l = q_ring[slot].next;
    q_bytes -= q_ring[slot].n;
    q_ring[slot].p = NULL, q_ring[slot].n = 0;
}

static void q_init(void)
{
    if (q_ready)
        return;
    for (int i = 0; i < Q_BUCKETS; i++)
        q_bucket[i] = -1;
    q_ready = 1;
}

int gt_free_hold(void *p)
{
    if (!p)
        return 0;
    size_t n = malloc_usable_size(p);
    pthread_mutex_lock(&q_lock);
    q_init();
    if (q_find(p, NULL) >= 0) {
        pthread_mutex_unlock(&q_lock);
        static volatile int said;
        if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 3)
            rt_log("[mem] a block freed a second time (%p): ignored, as on iOS and Windows\n", p);
        return 2;
    }
    if (n > Q_BIG) {
        pthread_mutex_unlock(&q_lock);
        return 0;
    }
    void *out[8];
    int nout = 0;
    while (q_count && (q_count == Q_SLOTS || q_bytes + n > Q_BYTES) && nout < 8) {
        unsigned old = (q_head + Q_SLOTS - q_count) % Q_SLOTS;
        out[nout++] = q_ring[old].p;
        q_drop(old);
        q_count--;
    }
    if (q_count == Q_SLOTS || q_bytes + n > Q_BYTES) {
        pthread_mutex_unlock(&q_lock);
        for (int i = 0; i < nout; i++)
            __real_free(out[i]);
        return 0;
    }
    unsigned slot = q_head;
    q_head = (q_head + 1) % Q_SLOTS;
    q_count++;
    int *b = &q_bucket[q_hash(p)];
    q_ring[slot].p = p, q_ring[slot].n = n, q_ring[slot].next = *b;
    *b = (int)slot;
    q_bytes += n;
    pthread_mutex_unlock(&q_lock);
    for (int i = 0; i < nout; i++)
        __real_free(out[i]);
    return 1;
}

void gt_free_unhold(void *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&q_lock);
    q_init();
    int slot = q_find(p, NULL);
    if (slot >= 0)
        q_drop((unsigned)slot);
    pthread_mutex_unlock(&q_lock);
}

int _putenv_s(const char *name, const char *value)
{
    return value && *value ? setenv(name, value, 1) : unsetenv(name);
}

BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *dm)
{
    (void)device, (void)mode;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return FALSE;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!m || m->w <= 0 || m->h <= 0)
        return FALSE;
    float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
    dm->dmPelsWidth = (DWORD)(m->w * d + 0.5f);
    dm->dmPelsHeight = (DWORD)(m->h * d + 0.5f);
    return TRUE;
}
