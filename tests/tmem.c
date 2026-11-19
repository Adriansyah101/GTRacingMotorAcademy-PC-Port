/* Sparse guest memory for the difference tests (ARM_TEST_MEM).
 * Pages are filled from the game image or, elsewhere, from a pattern that
 * the Python side reproduces for Unicorn; written bytes are marked per page. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define EXPORT __declspec(dllexport)
#define MAXPAGES 4096
#define HASH 8192

typedef struct Page {
    u32 page;
    u8 data[4096];
    u8 wmask[4096];
} Page;

static const u8 *img;
static u32 img_base, img_size;
static Page *pages[MAXPAGES];
static int npages;
static int hash[HASH];          /* index + 1 into pages, 0 = empty */
static u32 last_page = 0xFFFFFFFFu;
static Page *last;

void *tm_jb[5];
int tm_code;
u32 tm_site, tm_steps, tm_limit = 300000;

void tm_bail(int code)
{
    tm_code = code;
    __builtin_longjmp(tm_jb, 1);
}

static u8 pat(u32 a)
{
    u32 w = (a >> 2) * 0x9E3779B1u;
    return (u8)(w >> (8 * (a & 3)));
}

static Page *page_of(u32 a)
{
    u32 p = a >> 12;
    if (p == last_page) return last;
    u32 h = (p * 2654435761u) & (HASH - 1);
    while (hash[h]) {
        Page *g = pages[hash[h] - 1];
        if (g->page == p) { last_page = p; last = g; return g; }
        h = (h + 1) & (HASH - 1);
    }
    if (npages == MAXPAGES) tm_bail(5);
    Page *g = malloc(sizeof(Page));
    g->page = p;
    memset(g->wmask, 0, 4096);
    for (u32 k = 0; k < 4096; k++) {
        u32 x = (p << 12) + k;
        g->data[k] = (x >= img_base && x - img_base < img_size) ? img[x - img_base] : pat(x);
    }
    pages[npages++] = g;
    hash[h] = npages;
    last_page = p;
    last = g;
    return g;
}

u8 tm_ld8(u32 a) { return page_of(a)->data[a & 0xFFF]; }
u16 tm_ld16(u32 a) { return (u16)(tm_ld8(a) | tm_ld8(a + 1) << 8); }
u32 tm_ld32(u32 a) { return tm_ld16(a) | (u32)tm_ld16(a + 2) << 16; }
u64 tm_ld64(u32 a) { return tm_ld32(a) | (u64)tm_ld32(a + 4) << 32; }
void tm_st8(u32 a, u8 v)
{
    Page *g = page_of(a);
    g->data[a & 0xFFF] = v;
    g->wmask[a & 0xFFF] = 1;
}
void tm_st16(u32 a, u16 v) { tm_st8(a, (u8)v); tm_st8(a + 1, (u8)(v >> 8)); }
void tm_st32(u32 a, u32 v) { tm_st16(a, (u16)v); tm_st16(a + 2, (u16)(v >> 16)); }
void tm_st64(u32 a, u64 v) { tm_st32(a, (u32)v); tm_st32(a + 4, (u32)(v >> 32)); }

EXPORT void tm_init(const u8 *image, u32 base, u32 size)
{
    img = image;
    img_base = base;
    img_size = size;
}

EXPORT void tm_reset(void)
{
    for (int i = 0; i < npages; i++) free(pages[i]);
    npages = 0;
    memset(hash, 0, sizeof hash);
    last_page = 0xFFFFFFFFu;
    last = NULL;
}

/* Written byte addresses (up to max). */
EXPORT int tm_written(u32 *out, int max)
{
    int n = 0;
    for (int i = 0; i < npages; i++)
        for (u32 k = 0; k < 4096; k++)
            if (pages[i]->wmask[k]) {
                if (n == max) return n;
                out[n++] = (pages[i]->page << 12) + k;
            }
    return n;
}

EXPORT int tm_peek(u32 a) { return tm_ld8(a); }
EXPORT u32 tm_last_site(void) { return tm_site; }

/* Stage 3 runtime entry points: in the tests they end the run. */
typedef struct CPU CPU;
void rt_trap(CPU *c, u32 site, int kind) { (void)c; tm_site = site; tm_bail(100 + kind); }
void rt_call_indirect(CPU *c, u32 t) { (void)c; tm_site = t; tm_bail(3); }
void rt_svc(CPU *c, u32 site, u32 imm) { (void)c; (void)imm; tm_site = site; tm_bail(4); }
