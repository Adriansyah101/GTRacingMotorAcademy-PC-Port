#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "liveblocks.h"
#include "rt.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

#define LIVE_MIN 65536u
#define SMALL_MIN 4096u
typedef struct {
    volatile unsigned lo[2], hi[2];
} Gran;
typedef struct {
    unsigned min, shift, ngran;
    Gran *volatile tab;
} Tier;
static Tier tier[2] = {{LIVE_MIN, 16, 1u << 16, NULL}, {SMALL_MIN, 12, 1u << 20, NULL}};
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG nlive, nstale, nfull;

static void drop_locked(Tier *t, unsigned lo)
{
    Gran *g0 = &t->tab[lo >> t->shift];
    unsigned hi = g0->lo[0] == lo ? g0->hi[0] : g0->lo[1] == lo ? g0->hi[1] : 0;
    if (!hi)
        return;
    for (unsigned g = lo >> t->shift; g <= (hi - 1) >> t->shift; g++)
        for (int k = 0; k < 2; k++)
            if (t->tab[g].lo[k] == lo)
                t->tab[g].lo[k] = 0, t->tab[g].hi[k] = 0;
    nlive--;
}

static void add(unsigned p, size_t n)
{
    Tier *t = n >= LIVE_MIN ? &tier[0] : n >= SMALL_MIN ? &tier[1] : NULL;
    if (!t || !t->tab || !p || n > 0xffffffffu - p)
        return;
    unsigned hi = p + (unsigned)n;
    AcquireSRWLockExclusive(&lock);
    for (int u = 0; u < 2; u++) {
        Tier *T = &tier[u];
        if (!T->tab)
            continue;
        for (unsigned g = p >> T->shift; g <= (hi - 1) >> T->shift; g++)
            for (int k = 0; k < 2; k++) {
                unsigned lo = T->tab[g].lo[k];
                if (lo && lo < hi && p < T->tab[g].hi[k]) {
                    drop_locked(T, lo);
                    nstale++;
                }
            }
    }
    unsigned g0 = p >> t->shift, g1 = (hi - 1) >> t->shift;
    for (unsigned g = g0; g <= g1; g++) {
        Gran *G = &t->tab[g];
        int k = !G->lo[0] ? 0 : !G->lo[1] ? 1 : -1;
        if (k < 0) {
            for (unsigned u = g0; u < g; u++)
                for (int j = 0; j < 2; j++)
                    if (t->tab[u].lo[j] == p)
                        t->tab[u].lo[j] = 0, t->tab[u].hi[j] = 0;
            nfull++;
            ReleaseSRWLockExclusive(&lock);
            return;
        }
        G->hi[k] = hi;
        G->lo[k] = p;
    }
    nlive++;
    ReleaseSRWLockExclusive(&lock);
}

static void del(unsigned p)
{
    if (!p)
        return;
    for (int u = 0; u < 2; u++) {
        Tier *t = &tier[u];
        if (!t->tab)
            continue;
        Gran *g = &t->tab[p >> t->shift];
        if (g->lo[0] != p && g->lo[1] != p)
            continue;
        AcquireSRWLockExclusive(&lock);
        if (g->lo[0] == p || g->lo[1] == p)
            drop_locked(t, p);
        ReleaseSRWLockExclusive(&lock);
        return;
    }
}

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (p && n >= SMALL_MIN)
        add((unsigned)(uintptr_t)p, n);
    return p;
}
void *__wrap_calloc(size_t n, size_t m)
{
    void *p = __real_calloc(n, m);
    if (p && m && n <= SIZE_MAX / m && n * m >= SMALL_MIN)
        add((unsigned)(uintptr_t)p, n * m);
    return p;
}
void *__wrap_realloc(void *p, size_t n)
{
#ifndef _WIN32
    gt_free_unhold(p);
#endif
    del((unsigned)(uintptr_t)p);
    void *q = __real_realloc(p, n);
    if (q && n >= SMALL_MIN)
        add((unsigned)(uintptr_t)q, n);
    return q;
}
void __wrap_free(void *p)
{
#ifndef _WIN32
    int held = gt_free_hold(p);
    if (held == 2)
        return;
#endif
    del((unsigned)(uintptr_t)p);
#ifndef _WIN32
    if (held)
        return;
#endif
    __real_free(p);
}

int gt_live_has(unsigned a, unsigned n)
{
    if (!tier[0].tab)
        return 0;
    int r = 0;
    AcquireSRWLockShared(&lock);
    for (int u = 0; u < 2 && !r; u++) {
        const Tier *t = &tier[u];
        if (!t->tab)
            continue;
        const Gran *g = &t->tab[a >> t->shift];
        for (int k = 0; k < 2; k++) {
            unsigned lo = g->lo[k], hi = g->hi[k];
            if (lo && a >= lo && a < hi && n <= hi - a)
                r = 1;
        }
    }
    ReleaseSRWLockShared(&lock);
    return r;
}

void gt_live_init(void)
{
    const char *e = getenv("GT_LIVE_BLOCKS");
    if (e && *e == '0') {
        rt_log("[mem] LIVE_BLOCKS: off (GT_LIVE_BLOCKS=0)\n");
        return;
    }
    Gran *t0 = VirtualAlloc(NULL, tier[0].ngran * sizeof(Gran), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Gran *t1 = t0 ? VirtualAlloc(NULL, tier[1].ngran * sizeof(Gran), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : NULL;
    if (!t0 || !t1) {
        if (t0)
            VirtualFree(t0, 0, MEM_RELEASE);
        rt_log("[mem] LIVE_BLOCKS: off (no memory for the tables)\n");
        return;
    }
    tier[1].tab = t1;
    tier[0].tab = t0;
    rt_log("[mem] LIVE_BLOCKS: on (heap blocks of %u KB or more)\n", SMALL_MIN >> 10);
}

typedef struct {
    unsigned seed, bad;
    unsigned blocks[64];
    int nblocks;
} StressArg;
static unsigned st_rnd(unsigned *x)
{
    *x ^= *x << 13, *x ^= *x >> 17, *x ^= *x << 5;
    return *x;
}
static DWORD WINAPI stress(void *arg)
{
    StressArg *s = arg;
    struct { unsigned char *p; size_t n; } live[8];
    memset(live, 0, sizeof live);
    for (int i = 0; i < 3000; i++) {
        int k = (int)(st_rnd(&s->seed) % 8);
        size_t n = st_rnd(&s->seed) % 300000;
        switch (st_rnd(&s->seed) % 4) {
        case 0:
            free(live[k].p);
            live[k].p = malloc(n), live[k].n = n;
            break;
        case 1:
            free(live[k].p);
            live[k].p = calloc(1, n), live[k].n = n;
            break;
        case 2: {
            unsigned char *q = realloc(live[k].p, n ? n : 1);
            if (q)
                live[k].p = q, live[k].n = n ? n : 1;
            break;
        }
        default:
            free(live[k].p);
            live[k].p = NULL, live[k].n = 0;
        }
        for (int j = 0; j < 8; j++)
            if (live[j].p) {
                unsigned a = (unsigned)(uintptr_t)live[j].p;
                int want = live[j].n >= SMALL_MIN;
                if (gt_live_has(a, (unsigned)live[j].n) != want || gt_live_has(a, (unsigned)live[j].n + 1))
                    s->bad++;
            }
    }
    for (int j = 0; j < 8; j++)
        if (live[j].p) {
            if (s->nblocks < 64)
                s->blocks[s->nblocks++] = (unsigned)(uintptr_t)live[j].p;
            free(live[j].p);
        }
    return 0;
}

int gt_live_selftest(char *why, size_t len)
{
    if (!tier[0].tab) {
        rt_log("[selftest] LIVE_BLOCKS is off (GT_LIVE_BLOCKS=0): its checks are skipped\n");
        return 0;
    }
#define FAIL(...) do { snprintf(why, len, __VA_ARGS__); return 1; } while (0)
    LONG base = nlive;
    unsigned char *p = malloc(256 << 10);
    unsigned a = (unsigned)(uintptr_t)p;
    if (!p || !gt_live_has(a, 256 << 10) || !gt_live_has(a + 100, (256 << 10) - 100))
        FAIL("a 256 KB block is not in the table (are malloc / free wrapped?)");
    if (gt_live_has(a + 100, 256 << 10) || gt_live_has(a - 16, 32))
        FAIL("a range past a block's end or from before it was taken as inside");
    free(p);
    if (gt_live_has(a, 16))
        FAIL("a freed 256 KB block is still in the table");
    p = malloc(1000);
    if (gt_live_has((unsigned)(uintptr_t)p, 1000))
        FAIL("a 1000-byte block is in the table");
    free(p);
    p = malloc(5000);
    a = (unsigned)(uintptr_t)p;
    if (!p || !gt_live_has(a + 12, 4000) || gt_live_has(a + 12, 4990))
        FAIL("a 5000-byte block is not in the table, or a range past its end is");
    free(p);
    if (gt_live_has(a, 16))
        FAIL("a freed 5000-byte block is still in the table");
    p = calloc(4, 100000);
    if (!p || !gt_live_has((unsigned)(uintptr_t)p, 400000))
        FAIL("a calloc'd 400 KB block is not in the table");
    free(p);
    unsigned char *r = malloc(100000);
    unsigned ra = (unsigned)(uintptr_t)r;
    unsigned char *r2 = realloc(r, 3u << 20);
    if (!r2 || !gt_live_has((unsigned)(uintptr_t)r2, 3u << 20))
        FAIL("a block grown by realloc is not in the table");
    if ((unsigned)(uintptr_t)r2 != ra && gt_live_has(ra, 16))
        FAIL("the old place of a block moved by realloc is still in the table");
    unsigned char *r3 = realloc(r2, 100);
    if (r3 && gt_live_has((unsigned)(uintptr_t)r3, 16))
        FAIL("a block shrunk under 4 KB by realloc is still in the table");
    free(r3);
    p = malloc(2u << 20);
    a = (unsigned)(uintptr_t)p;
    if (!p || !gt_live_has(a, 2u << 20))
        FAIL("a 2 MB block is not in the table");
    free(p);
    if (gt_live_has(a, 16))
        FAIL("a freed 2 MB block is still in the table");
    MEMORY_BASIC_INFORMATION m;
#ifdef _WIN32
    rt_log("[selftest] LIVE_BLOCKS: a freed 2 MB block's memory is now %s for Windows\n",
           VirtualQuery((void *)(uintptr_t)a, &m, sizeof m) && m.State == MEM_COMMIT ? "still committed" : "given back");
#else
    volatile uintptr_t freed = a;
    rt_log("[selftest] LIVE_BLOCKS: a freed 2 MB block's memory is now %s for Linux\n",
           VirtualQuery((void *)freed, &m, sizeof m) && m.State == MEM_COMMIT ? "still mapped" : "given back");
#endif
    static unsigned char *many[256];
    int missing = 0;
    static const unsigned sizes[2] = {LIVE_MIN, SMALL_MIN}, counts[2] = {64, 256};
    for (int t = 0; t < 2; t++) {
        for (unsigned i = 0; i < counts[t]; i++)
            many[i] = malloc(sizes[t]);
        for (unsigned i = 0; i < counts[t]; i++)
            missing += !many[i] || !gt_live_has((unsigned)(uintptr_t)many[i], sizes[t]);
        for (unsigned i = 0; i < counts[t]; i++)
            free(many[i]);
    }
    if (missing || nfull)
        FAIL("%d of 320 blocks of 64 KB / 4 KB not in the tables (%ld refused)", missing, (long)nfull);
    LONG stale0 = nstale;
    unsigned fake = (unsigned)(uintptr_t)tier[1].tab;
    add(fake, 128u << 10);
    add(fake + 0x10000u, 128u << 10);
    int old_in = gt_live_has(fake, 16), new_in = gt_live_has(fake + 0x10000u, 128u << 10);
    del(fake + 0x10000u);
    if (old_in || !new_in || nstale != stale0 + 1 || gt_live_has(fake + 0x10000u, 16))
        FAIL("a stale entry was not dropped by a block over it (old %d, new %d)", old_in, new_in);
    add(fake + 0x41000u, 8192);
    int small_in = gt_live_has(fake + 0x41000u, 8192);
    add(fake + 0x30000u, 128u << 10);
    new_in = gt_live_has(fake + 0x30000u, 128u << 10);
    del(fake + 0x30000u);
    if (!small_in || !new_in || nstale != stale0 + 2 || gt_live_has(fake + 0x41000u, 16) ||
        gt_live_has(fake + 0x30000u, 16))
        FAIL("a stale small-table entry was not dropped by a large block over it");
    StressArg sa[4];
    HANDLE th[4];
    for (int i = 0; i < 4; i++) {
        memset(&sa[i], 0, sizeof sa[i]);
        sa[i].seed = 0x1234567u * (unsigned)(i + 1);
        th[i] = CreateThread(NULL, 0, stress, &sa[i], 0, NULL);
    }
    for (int i = 0; i < 4; i++)
        if (th[i]) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
    unsigned bad = 0, gone = 0;
    for (int i = 0; i < 4; i++) {
        bad += sa[i].bad;
        for (int j = 0; j < sa[i].nblocks; j++)
            gone += gt_live_has(sa[i].blocks[j], 1);
    }
    if (bad || gone || nlive != base)
        FAIL("four threads: %u wrong answers, %u freed blocks still in, %ld blocks in the table (%ld before)", bad,
             gone, (long)nlive, (long)base);
    rt_log("[selftest] LIVE_BLOCKS: %ld blocks in the table, %ld dropped as stale (the made-up ones)\n", (long)nlive,
           (long)nstale);
    return 0;
#undef FAIL
}
