#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "ns.h"

enum {
    BLOCK_REFCOUNT_MASK = 0xfffe, BLOCK_NEEDS_FREE = 1 << 24, BLOCK_HAS_COPY_DISPOSE = 1 << 25,
    BLOCK_IS_GLOBAL = 1 << 28,
    FIELD_IS_OBJECT = 3, FIELD_IS_BLOCK = 7, FIELD_IS_BYREF = 8, FIELD_IS_WEAK = 16, BYREF_CALLER = 128
};

typedef struct { u32 isa; volatile LONG flags; u32 reserved, invoke, desc; } Block;
typedef struct { u32 isa, forwarding; volatile LONG flags; u32 size, keep, destroy; } Byref;

static u32 cls_malloc_block;

u32 gt_block_copy(u32 b)
{
    if (!b)
        return 0;
    Block *s = GP(b);
    if (s->flags & BLOCK_NEEDS_FREE) {
        InterlockedAdd(&s->flags, 2);
        return b;
    }
    if (s->flags & BLOCK_IS_GLOBAL)
        return b;
    u32 size = rd32(s->desc + 4);
    Block *d = malloc(size);
    memcpy(d, s, size);
    d->isa = cls_malloc_block;
    d->flags = (s->flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 2;
    if (s->flags & BLOCK_HAS_COPY_DISPOSE)
        rt_call2(rd32(s->desc + 8), GA(d), b);
    return GA(d);
}

static void gt_block_release(u32 b)
{
    if (!b)
        return;
    Block *s = GP(b);
    if (!(s->flags & BLOCK_NEEDS_FREE))
        return;
    if ((InterlockedAdd(&s->flags, -2) & BLOCK_REFCOUNT_MASK) != 0)
        return;
    if (s->flags & BLOCK_HAS_COPY_DISPOSE)
        rt_call1(rd32(s->desc + 12), b);
    free(s);
}

static u32 byref_copy(u32 p)
{
    Byref *src = GP(p);
    Byref *fw = GP(src->forwarding);
    if (fw->flags & BLOCK_NEEDS_FREE) {
        InterlockedAdd(&fw->flags, 2);
        return GA(fw);
    }
    if ((fw->flags & BLOCK_REFCOUNT_MASK) == 0) {
        Byref *copy = malloc(src->size);
        copy->isa = 0;
        copy->flags = src->flags | BLOCK_NEEDS_FREE | 4;
        copy->forwarding = GA(copy);
        src->forwarding = GA(copy);
        copy->size = src->size;
        if (src->flags & BLOCK_HAS_COPY_DISPOSE) {
            copy->keep = src->keep;
            copy->destroy = src->destroy;
            if (src->size > sizeof(Byref))
                memcpy(copy + 1, src + 1, src->size - sizeof(Byref));
            rt_call2(src->keep, GA(copy), p);
        } else if (src->size > 16) {
            memcpy((u8 *)copy + 16, (u8 *)src + 16, src->size - 16);
        }
        return GA(copy);
    }
    return src->forwarding;
}
static void byref_release(u32 p)
{
    Byref *b = GP(rd32(p + 4));
    if (!(b->flags & BLOCK_NEEDS_FREE))
        return;
    if ((InterlockedAdd(&b->flags, -2) & BLOCK_REFCOUNT_MASK) != 0)
        return;
    if (b->flags & BLOCK_HAS_COPY_DISPOSE)
        rt_call1(b->destroy, GA(b));
    free(b);
}

void h_Block_object_assign(u32 dest, u32 obj, u32 flags)
{
    switch (flags & 0xff) {
    case FIELD_IS_OBJECT:
        wr32(dest, objc_retain(obj));
        break;
    case FIELD_IS_BLOCK:
        wr32(dest, gt_block_copy(obj));
        break;
    case FIELD_IS_BYREF:
    case FIELD_IS_BYREF | FIELD_IS_WEAK:
        wr32(dest, byref_copy(obj));
        break;
    default:
        if ((flags & BYREF_CALLER) && (flags & 0x7f) == FIELD_IS_BLOCK)
            wr32(dest, gt_block_copy(obj));
        else
            wr32(dest, obj);
        break;
    }
}
void h_Block_object_dispose(u32 obj, u32 flags)
{
    switch (flags & 0xff) {
    case FIELD_IS_OBJECT:
        objc_release(obj);
        break;
    case FIELD_IS_BLOCK:
        gt_block_release(obj);
        break;
    case FIELD_IS_BYREF:
    case FIELD_IS_BYREF | FIELD_IS_WEAK:
        byref_release(obj);
        break;
    default:
        if ((flags & BYREF_CALLER) && (flags & 0x7f) == FIELD_IS_BLOCK)
            gt_block_release(obj);
        break;
    }
}

static void b_copy(CPU *c) { ret_i(c, gt_block_copy(SELF)); }
static void b_retain(CPU *c)
{
    Block *b = GP(SELF);
    if (b->flags & BLOCK_NEEDS_FREE)
        InterlockedAdd(&b->flags, 2);
}
static void b_release(CPU *c) { gt_block_release(SELF); }
static void b_autorelease(CPU *c)
{
    if (((Block *)GP(SELF))->flags & BLOCK_NEEDS_FREE)
        objc_autorelease(SELF);
}
static void b_retainCount(CPU *c) { ret_i(c, (((Block *)GP(SELF))->flags & BLOCK_REFCOUNT_MASK) / 2); }
static void b_invoke(CPU *c) { rt_call1(((Block *)GP(SELF))->invoke, SELF); }

void h_dispatch_async(u32 queue, u32 block)
{
    (void)queue;
    u32 b = gt_block_copy(block);
    ns_perform_main(b, "gtInvoke", 0);
    objc_release(b);
}
static void b_dealloc(CPU *c) { gt_block_release(SELF); }

void gt_blocks_init(void)
{
    cls_malloc_block = ns_class("__NSMallocBlock__");
    static const NsMethod m[] = {
        {"copy", b_copy, 0}, {"copyWithZone:", b_copy, 0}, {"retain", b_retain, 0}, {"release", b_release, 0},
        {"autorelease", b_autorelease, 0}, {"retainCount", b_retainCount, 0}, {"gtInvoke", b_invoke, 0},
        {"invoke", b_invoke, 0}, {"dealloc", b_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSBlock", m);
}
