#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "features.h"
#include "game.h"

extern void F_50a654(CPU *c);

static int on = -1, check;
static unsigned n_calls, n_bad;

#define VT_SLOTS 512
static u32 vt_key[VT_SLOTS];
static u8 vt_val[VT_SLOTS];

static int vt_is_list(u32 ch)
{
    u32 vt = rd32(ch);
    unsigned h = (vt >> 3) & (VT_SLOTS - 1);
    for (unsigned n = 0; n < VT_SLOTS; n++, h = (h + 1) & (VT_SLOTS - 1)) {
        if (vt_key[h] == vt)
            return vt_val[h];
        if (!vt_key[h]) {
            int r = rt_call2(rd32(vt + 8), ch, 2) != 0;
            vt_key[h] = vt;
            vt_val[h] = (u8)r;
            if (check)
                rt_log("[swffind] vtable 0x%08x: is() 0x%08x says %d\n", vt, rd32(vt + 8), r);
            return r;
        }
    }
    return rt_call2(rd32(vt + 8), ch, 2) != 0;
}

typedef struct { u32 ch, path; } Pair;
static Pair *q;
static unsigned qcap;
static unsigned visited;

#define ST_SLOTS 1024
typedef struct { char path[80]; u32 hash; unsigned calls, misses, cached; unsigned long long nodes; } FindStat;
static FindStat *st;
static int stats = -1;
u32 gt_swf_tree_site[8];
static unsigned nc_dropped;
static u32 st_frame0;
static unsigned long long st_nodes;
static unsigned st_calls, st_misses, st_cached;

static void stats_add(const char *path, u32 found, int cached)
{
    u32 h = 2166136261u;
    for (const char *p = path; *p; p++)
        h = (h ^ (u8)*p) * 16777619u;
    if (!st)
        st = calloc(ST_SLOTS, sizeof *st);
    if (!st)
        return;
    unsigned i = h & (ST_SLOTS - 1);
    for (unsigned n = 0; n < ST_SLOTS; n++, i = (i + 1) & (ST_SLOTS - 1)) {
        if (st[i].calls && (st[i].hash != h || strncmp(st[i].path, path, sizeof st[i].path - 1)))
            continue;
        if (!st[i].calls) {
            st[i].hash = h;
            strncpy(st[i].path, path, sizeof st[i].path - 1);
        }
        st[i].calls++;
        st[i].misses += !found;
        st[i].cached += cached;
        st[i].nodes += visited;
        break;
    }
    st_calls++;
    st_misses += !found;
    st_cached += cached;
    st_nodes += visited;
}

static int st_cmp(const void *a, const void *b)
{
    const FindStat *x = a, *y = b;
    return x->nodes < y->nodes ? 1 : x->nodes > y->nodes ? -1 : 0;
}

static void stats_report(void)
{
    u32 f = eagl_frames();
    if (!st_frame0)
        st_frame0 = f;
    if (f - st_frame0 < 600 || !st)
        return;
    u32 frames = f - st_frame0;
    rt_log("[swffind] frames %u-%u: %u searches (%.1f a frame), %u found nothing (%u of them from the cache), %.0f characters looked at a frame\n",
           st_frame0, f, st_calls, (double)st_calls / frames, st_misses, st_cached, (double)st_nodes / frames);
    qsort(st, ST_SLOTS, sizeof *st, st_cmp);
    for (int i = 0; i < 15 && st[i].calls; i++)
        rt_log("[swffind]   %5.1f %%  %6.2f a frame  %3u %% nothing  %3u %% cached  %s\n",
               st_nodes ? 100.0 * (double)st[i].nodes / (double)st_nodes : 0.0, (double)st[i].calls / frames,
               100 * st[i].misses / st[i].calls, 100 * st[i].cached / st[i].calls, st[i].path);
    rt_log("[swffind]   characters made %u, named %u %u %u %u %u %u (by place); kept misses dropped for a name %u\n",
           gt_swf_tree_site[0], gt_swf_tree_site[1], gt_swf_tree_site[2], gt_swf_tree_site[3], gt_swf_tree_site[4],
           gt_swf_tree_site[5], gt_swf_tree_site[6], nc_dropped);
    nc_dropped = 0;
    memset(gt_swf_tree_site, 0, sizeof gt_swf_tree_site);
    memset(st, 0, ST_SLOTS * sizeof *st);
    st_frame0 = f, st_calls = st_misses = st_cached = 0, st_nodes = 0;
}

static u32 search(u32 finder)
{
    u32 n0 = rd32(finder + 0x2c), d0 = rd32(finder + 0x28);
    unsigned n = 0;
    for (u32 i = 0; i < n0; i++) {
        if (n == qcap) {
            qcap = qcap ? qcap * 2 : 1024;
            q = realloc(q, qcap * sizeof *q);
        }
        q[n].ch = rd32(d0 + i * 8);
        q[n].path = rd32(d0 + i * 8 + 4);
        n++;
    }
    u32 last = 0;
    const char *stop = NULL;
    for (unsigned i = 0; i < n; i++) {
        u32 path = q[i].path, ch = q[i].ch;
        if (!path) {
            visited = i;
            return 0;
        }
        const char *p = GP(path);
        if (path != last) {
            stop = strchr(p, '.');
            if (!stop)
                stop = p + strlen(p);
            last = path;
        }
        int sz = (signed char)*(u8 *)GP(ch + 0x44);
        const char *name = GP(ch + 0x45);
        if (sz == -1) {
            sz = (int)rd32(ch + 0x48);
            name = GP(rd32(ch + 0x50));
        }
        if (sz - 1 == stop - p && (sz == 1 || *name == *p) && !memcmp(name, p, (size_t)(sz - 1))) {
            if (!*stop) {
                visited = i + 1;
                return ch;
            }
            path = GA(stop + 1);
        }
        if (vt_is_list(ch)) {
            u32 cnt = rd32(ch + 0xbc), kids = rd32(ch + 0xb8);
            if ((int)cnt > 0) {
                if (n + cnt > qcap) {
                    while (n + cnt > qcap)
                        qcap = qcap ? qcap * 2 : 1024;
                    q = realloc(q, qcap * sizeof *q);
                }
                for (u32 k = 0; k < cnt; k++) {
                    q[n].ch = rd32(kids + k * 4);
                    q[n].path = path;
                    n++;
                }
            }
        }
    }
    visited = n;
    return 0;
}

u32 swf_find_under(u32 pl, u32 parent, const char *rest)
{
    return pl && parent ? rt_call3(GT_FN_SWF_CHILD, pl, GA((void *)rest), parent) : 0;
}

volatile u32 gt_swf_tree_gen = 1;

#define NC_SLOTS 32
typedef struct { u32 gen, start, hash; char path[96]; } Miss;
static Miss nc[NC_SLOTS];
static unsigned nc_next, nc_hits, nc_win_hits;
static int nc_on = -1;

static const char *nc_key(u32 finder, u32 *start, u32 *hash)
{
    if (rd32(finder + 0x2c) != 1)
        return NULL;
    u32 d0 = rd32(finder + 0x28), path = rd32(d0 + 4);
    if (!path)
        return NULL;
    const char *p = GP(path);
    u32 h = 2166136261u;
    size_t n = 0;
    for (const char *t = p; *t; t++, n++)
        h = (h ^ (u8)*t) * 16777619u;
    if (n >= sizeof nc[0].path)
        return NULL;
    *start = rd32(d0);
    *hash = h;
    return p;
}

static int nc_has(u32 start, const char *p, u32 h)
{
    u32 g = gt_swf_tree_gen;
    for (unsigned i = 0; i < NC_SLOTS; i++)
        if (nc[i].gen == g && nc[i].start == start && nc[i].hash == h && !strcmp(nc[i].path, p))
            return 1;
    return 0;
}

static void nc_add(u32 start, const char *p, u32 h)
{
    Miss *m = &nc[nc_next++ % NC_SLOTS];
    m->gen = gt_swf_tree_gen;
    m->start = start;
    m->hash = h;
    strcpy(m->path, p);
}

void gt_swf_named(u32 name, int site)
{
    gt_swf_tree_site[site & 7]++;
    if (!name)
        return;
    int sz = *(u8 *)GP(name);
    const char *t = GP(name + 1);
    if (sz == 0xff) {
        sz = (int)rd32(name + 4);
        t = GP(rd32(name + 0xc));
    }
    size_t n = sz > 0 ? (size_t)sz - 1 : 0;
    if (!n)
        return;
    u32 g = gt_swf_tree_gen;
    for (unsigned i = 0; i < NC_SLOTS; i++) {
        if (nc[i].gen != g)
            continue;
        for (const char *p = nc[i].path; *p;) {
            const char *e = strchr(p, '.');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len == n && !memcmp(p, t, n)) {
                nc[i].gen = 0;
                nc_dropped++;
                break;
            }
            if (!e)
                break;
            p = e + 1;
        }
    }
}

static u32 find(u32 finder)
{
    u32 start = 0, h = 0;
    const char *p = nc_on > 0 ? nc_key(finder, &start, &h) : NULL;
    if (p && nc_has(start, p, h)) {
        nc_win_hits++;
        if (++nc_hits & 127) {
            visited = 0;
            return 0;
        }
        u32 r = search(finder);
        if (r) {
            rt_log("[swffind] the cache of misses was wrong: \"%s\" from 0x%08x is 0x%08x; cache off\n", p, start, r);
            nc_on = 0;
        }
        return r;
    }
    u32 r = search(finder);
    if (p && !r)
        nc_add(start, p, h);
    return r;
}

void gt_swf_search(CPU *c)
{
    if (on < 0) {
        const char *e = getenv("GT_FAST_SWF_FIND");
        on = !(e && *e == '0');
        e = dev_getenv("GT_SWF_FIND_CHECK");
        check = on && e && *e == '1';
        e = getenv("GT_SWF_MISS_CACHE");
        nc_on = on && !(e && *e == '0');
        rt_log("[swffind] gameswf find-by-name: %s%s%s\n", on ? "native (FAST_SWF_FIND)" : "original",
               nc_on ? ", misses kept (GT_SWF_MISS_CACHE)" : "", check ? ", checked against the original" : "");
    }
    if (!on) {
        F_50a654(c);
        return;
    }
    if (check) {
        u32 finder = c->r[0], mine = find(finder);
        c->r[1] = 0;
        F_50a654(c);
        n_calls++;
        if (c->r[0] != mine && n_bad++ < 20)
            rt_log("[swffind] MISMATCH: path \"%s\": original 0x%08x, native 0x%08x\n",
                   (const char *)GP(rd32(rd32(finder + 0x28) + 4)), c->r[0], mine);
        if (!(n_calls & 0x3fff))
            dev_log("[swffind] %u searches checked, %u different, %u answered by the cache of misses\n", n_calls, n_bad, nc_hits);
        return;
    }
    if (stats < 0) {
        const char *e = dev_getenv("GT_SWF_FIND_STATS");
        stats = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    }
    if (!GT_DEV || !stats) {
        c->r[0] = find(c->r[0]);
        return;
    }
    u32 finder = c->r[0], d0 = rd32(finder + 0x28), path = rd32(finder + 0x2c) ? rd32(d0 + 4) : 0;
    if (stats == 2 && path) {
        static u32 seen[256][5];
        static unsigned nseen;
        u32 ch[4] = {0}, sp = c->r[13];
        for (int k = 0, w = 0; k < 4 && w < 512; w++) {
            u32 v = rd32(sp + (u32)w * 4);
            if (v < GT_RESERVE_START + 4 || v >= GT_TEXT_END || (v & 3))
                continue;
            u32 in = rd32(v - 4);
            if ((in & 0x0f000000) == 0x0b000000 || (in & 0x0ffffff0) == 0x012fff30)
                ch[k++] = v;
        }
        u32 h = 0;
        for (const char *p = GP(path); *p; p++)
            h = h * 31 + (u8)*p;
        unsigned k;
        for (k = 0; k < nseen; k++)
            if (seen[k][0] == h && !memcmp(seen[k] + 1, ch, sizeof ch))
                break;
        if (k == nseen && nseen < 256) {
            seen[nseen][0] = h;
            memcpy(seen[nseen++] + 1, ch, sizeof ch);
            rt_log("[swffind] caller of \"%s\": 0x%x < 0x%x < 0x%x < 0x%x\n", (const char *)GP(path), ch[0], ch[1], ch[2], ch[3]);
        }
    }
    unsigned h0 = nc_win_hits;
    c->r[0] = find(finder);
    stats_add(path ? (const char *)GP(path) : "(none)", c->r[0], nc_win_hits != h0);
    stats_report();
}
