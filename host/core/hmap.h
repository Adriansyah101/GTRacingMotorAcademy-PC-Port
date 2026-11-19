/* hmap.h -- small open-addressing hash maps (u32 -> u32, string -> u32) for the runtime.
   Not thread-safe by themselves: callers lock. */
#ifndef GT_HMAP_H
#define GT_HMAP_H

#include <stdlib.h>
#include <string.h>

#include "arm_rt.h"

typedef struct { u32 *k, *v; u32 mask, n; } Map32;     /* key 0 is never stored */

static inline u32 hm_hash32(u32 k)
{
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

static inline void m32_grow(Map32 *m);
static inline u32 *m32_slot(Map32 *m, u32 k)
{
    u32 i = hm_hash32(k) & m->mask;
    while (m->k[i] && m->k[i] != k)
        i = (i + 1) & m->mask;
    return &m->k[i];
}
static inline int m32_get(Map32 *m, u32 k, u32 *v)
{
    if (!m->k)
        return 0;
    u32 *s = m32_slot(m, k);
    if (!*s)
        return 0;
    *v = m->v[s - m->k];
    return 1;
}
static inline void m32_put(Map32 *m, u32 k, u32 v)
{
    if (!m->k || (m->n + 1) * 4 > (m->mask + 1) * 3)
        m32_grow(m);
    u32 *s = m32_slot(m, k);
    if (!*s) {
        *s = k;
        m->n++;
    }
    m->v[s - m->k] = v;
}
static inline void m32_del(Map32 *m, u32 k)
{
    if (!m->k)
        return;
    u32 *s = m32_slot(m, k);
    if (!*s)
        return;
    /* backward-shift deletion */
    u32 i = (u32)(s - m->k);
    m->k[i] = 0;
    m->n--;
    u32 j = i;
    for (;;) {
        j = (j + 1) & m->mask;
        if (!m->k[j])
            break;
        u32 h = hm_hash32(m->k[j]) & m->mask;
        if ((j > i && (h <= i || h > j)) || (j < i && (h <= i && h > j))) {
            m->k[i] = m->k[j];
            m->v[i] = m->v[j];
            m->k[j] = 0;
            i = j;
        }
    }
}
static inline void m32_clear(Map32 *m)
{
    if (m->k) {
        memset(m->k, 0, (m->mask + 1) * 4);
        m->n = 0;
    }
}
static inline void m32_grow(Map32 *m)
{
    u32 cap = m->k ? (m->mask + 1) * 2 : 16;
    Map32 n = {calloc(cap, 4), calloc(cap, 4), cap - 1, 0};
    if (m->k) {
        for (u32 i = 0; i <= m->mask; i++)
            if (m->k[i])
                m32_put(&n, m->k[i], m->v[i]);
        free(m->k);
        free(m->v);
    }
    *m = n;
}

typedef struct { const char **k; u32 *v; u32 mask, n; } MapStr;

static inline u32 hm_hashstr(const char *s)
{
    u32 h = 2166136261u;
    while (*s)
        h = (h ^ (u8)*s++) * 16777619u;
    return h;
}
static inline void ms_grow(MapStr *m);
static inline u32 ms_index(MapStr *m, const char *k)
{
    u32 i = hm_hashstr(k) & m->mask;
    while (m->k[i] && strcmp(m->k[i], k))
        i = (i + 1) & m->mask;
    return i;
}
static inline int ms_get(MapStr *m, const char *k, u32 *v)
{
    if (!m->k)
        return 0;
    u32 i = ms_index(m, k);
    if (!m->k[i])
        return 0;
    *v = m->v[i];
    return 1;
}
/* k must stay alive (the map keeps the pointer) */
static inline void ms_put(MapStr *m, const char *k, u32 v)
{
    if (!m->k || (m->n + 1) * 4 > (m->mask + 1) * 3)
        ms_grow(m);
    u32 i = ms_index(m, k);
    if (!m->k[i]) {
        m->k[i] = k;
        m->n++;
    }
    m->v[i] = v;
}
static inline void ms_grow(MapStr *m)
{
    u32 cap = m->k ? (m->mask + 1) * 2 : 64;
    MapStr n = {calloc(cap, sizeof(char *)), calloc(cap, 4), cap - 1, 0};
    if (m->k) {
        for (u32 i = 0; i <= m->mask; i++)
            if (m->k[i])
                ms_put(&n, m->k[i], m->v[i]);
        free(m->k);
        free(m->v);
    }
    *m = n;
}

#endif
