/* ns_coll.c -- NSNumber, NSValue, NSNull, NSArray, NSDictionary, NSSet, NSData, enumerators
   and the CFArray / CFDictionary / CFNumber functions.

   Host layouts (mutable and immutable share one concrete class each, as on iOS):
     array       {isa, count, capacity, id *items, mutations}
     dictionary  {isa, count, capacity, id *keys, id *values, u32 *hashes, u32 *slots, nslots,
                  mutations, raw}            (sets: the same, values unused)
     number      {isa, type, low, high}
     data        {isa, bytes, length, capacity, owns}
   `raw` marks CF containers made with NULL callbacks: plain pointers, compared by address and
   not retained. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ns.h"

typedef struct { u32 isa, n, cap; u32 *v; u32 muts; u32 raw; } Arr;
typedef struct { u32 isa, n, cap; u32 *k, *v, *h, *slot; u32 nslot, muts, raw; } Dict;
typedef struct { u32 isa; u32 type; u32 lo, hi; } Num;
typedef struct { u32 isa; u8 *p; u32 n, cap, own; } Data;
typedef struct { u32 isa; u32 arr, i; } Enum;
typedef struct { u32 isa; char type[40]; u32 n; u8 bytes[64]; } Val;

#define ARR(o) ((Arr *)GP(o))
#define DICT(o) ((Dict *)GP(o))
#define NUM(o) ((Num *)GP(o))
#define DATA(o) ((Data *)GP(o))

static u32 cls_arr, cls_marr, cls_arrM, cls_dict, cls_mdict, cls_dictM, cls_set, cls_mset, cls_setM;
static u32 cls_num, cls_cfnum, cls_cfbool, cls_data, cls_mdata, cls_cfdata, cls_enum, cls_null, cls_val, cls_cval;
static id null_obj, yes_obj, no_obj;

/* CF callback structures: their addresses say "retain the objects" (dataimp.c hands them out) */
u32 ns_cf_callbacks[3][8];

static void bad_index(const char *what, u32 i, u32 n)
{
    rt_fatal("%s: index %u beyond bounds [0 .. %d] (NSRangeException)", what, i, (int)n - 1);
}

/* equality, hashing */

int ns_is_number(id o)
{
    if (!o)
        return 0;
    u32 isa = rd32(o);
    return isa == cls_cfnum || isa == cls_cfbool;
}

int ns_equal(id a, id b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    if (ns_is_string(a) && ns_is_string(b))
        return ns_str_eq(a, b);
    if (ns_is_number(a) && ns_is_number(b))
        return ns_num_double(a) == ns_num_double(b) && ns_num_int(a) == ns_num_int(b);
    return msg1(a, "isEqual:", b) & 0xFF;
}

u32 ns_hash(id o)
{
    if (!o)
        return 0;
    if (ns_is_string(o))
        return ns_hash_bytes(ns_utf8(o), ns_utf8_len(o));
    if (ns_is_number(o)) {
        double d = ns_num_double(o);
        int64_t i = (int64_t)d;
        return (double)i == d ? (u32)i * 2654435761u : ns_hash_bytes(&d, 8);
    }
    return msg0(o, "hash");
}

/* NSNumber */

static id num_make(char type, u64 bits)
{
    id o = ns_alloc(cls_cfnum, sizeof(Num));
    NUM(o)->type = (u8)type;
    NUM(o)->lo = (u32)bits;
    NUM(o)->hi = (u32)(bits >> 32);
    return o;
}

static int is_float_type(u32 t) { return t == 'f' || t == 'd'; }

double ns_num_double(id n)
{
    if (!n)
        return 0;
    if (!ns_is_number(n))
        return bits2d(rt_guest_call64(rt_cpu(), objc_lookup(rd32(n), sel_get("doubleValue")), 2,
                                      (u32[]){n, sel_get("doubleValue")}));
    Num *x = NUM(n);
    u64 v = (u64)x->hi << 32 | x->lo;
    switch (x->type) {
    case 'f': return bits2f(x->lo);
    case 'd': return bits2d(v);
    case 'Q': return (double)v;
    case 'q': return (double)(int64_t)v;
    case 'I': case 'L': case 'C': case 'S': return (double)x->lo;
    default: return (double)(int32_t)x->lo;
    }
}

int64_t ns_num_int(id n)
{
    if (!n)
        return 0;
    if (!ns_is_number(n))
        return (int64_t)rt_guest_call64(rt_cpu(), objc_lookup(rd32(n), sel_get("longLongValue")), 2,
                                        (u32[]){n, sel_get("longLongValue")});
    Num *x = NUM(n);
    u64 v = (u64)x->hi << 32 | x->lo;
    switch (x->type) {
    case 'f': case 'd': return (int64_t)ns_num_double(n);
    case 'q': case 'Q': return (int64_t)v;
    case 'I': case 'L': case 'C': case 'S': return (int64_t)x->lo;
    default: return (int64_t)(int32_t)x->lo;
    }
}

id ns_num_ll(int64_t v)
{
    if (v >= INT32_MIN && v <= INT32_MAX)
        return objc_autorelease(num_make('i', (u32)(int32_t)v));
    return objc_autorelease(num_make('q', (u64)v));
}
id ns_num_d(double v) { return objc_autorelease(num_make('d', d2bits(v))); }
id ns_num_bool(int v) { return v ? yes_obj : no_obj; }

static const char *const num_cluster[] = {"NSNumber", "__NSCFNumber", "NSDecimalNumber", NULL};

static void n_alloc(CPU *c) { ret_i(c, ns_cluster_alloc(SELF, num_cluster, cls_cfnum, sizeof(Num))); }

/* numberWithX: / initWithX: share one table: the selector's type letter */
static char sel_type(const char *s)
{
    static const struct { const char *name; char t; } tab[] = {
        {"Bool:", 'B'}, {"UnsignedChar:", 'C'}, {"Char:", 'c'}, {"UnsignedShort:", 'S'}, {"Short:", 's'},
        {"UnsignedInt:", 'I'}, {"UnsignedInteger:", 'I'}, {"Integer:", 'i'}, {"Int:", 'i'},
        {"UnsignedLongLong:", 'Q'}, {"LongLong:", 'q'}, {"UnsignedLong:", 'L'}, {"Long:", 'l'},
        {"Float:", 'f'}, {"Double:", 'd'}, {NULL, 0}};
    for (int i = 0; tab[i].name; i++) {
        size_t n = strlen(tab[i].name), m = strlen(s);
        if (m >= n && !strcmp(s + m - n, tab[i].name))
            return tab[i].t;
    }
    return 'i';
}

static u64 num_arg(CPU *c, char t)
{
    u32 w = ARG(0);
    switch (t) {
    case 'B': return (u8)w != 0;
    case 'c': return (u32)(int32_t)(int8_t)w;
    case 'C': return (u8)w;
    case 's': return (u32)(int32_t)(int16_t)w;
    case 'S': return (u16)w;
    case 'q': case 'Q': case 'd': return arg_l(c, 2);
    default: return w;
    }
}

static void n_numberWith(CPU *c)
{
    char t = sel_type(sel_name(c->r[1]));
    if (t == 'B') {
        ret_i(c, ns_num_bool((u8)ARG(0)));
        return;
    }
    ret_i(c, objc_autorelease(num_make(t, num_arg(c, t))));
}
static void n_initWith(CPU *c)
{
    char t = sel_type(sel_name(c->r[1]));
    id self = SELF;
    if (rd32(self) != cls_cfnum)
        return;
    if (t == 'B') {
        objc_free_instance(self);
        ret_i(c, ns_num_bool((u8)ARG(0)));
        return;
    }
    NUM(self)->type = (u8)t;
    u64 v = num_arg(c, t);
    NUM(self)->lo = (u32)v;
    NUM(self)->hi = (u32)(v >> 32);
}
static void n_decimalWithString(CPU *c)
{
    ret_i(c, objc_autorelease(num_make('d', d2bits(strtod(ns_utf8(ARG(0)), NULL)))));
}

static void n_bool(CPU *c) { ret_i(c, ns_num_int(SELF) != 0 || ns_num_double(SELF) != 0); }
static void n_int(CPU *c) { ret_i(c, (u32)ns_num_int(SELF)); }
static void n_char(CPU *c) { ret_i(c, (u32)(int32_t)(int8_t)ns_num_int(SELF)); }
static void n_uchar(CPU *c) { ret_i(c, (u8)ns_num_int(SELF)); }
static void n_short(CPU *c) { ret_i(c, (u32)(int32_t)(int16_t)ns_num_int(SELF)); }
static void n_ushort(CPU *c) { ret_i(c, (u16)ns_num_int(SELF)); }
static void n_ll(CPU *c) { ret_l(c, (u64)ns_num_int(SELF)); }
static void n_float(CPU *c) { ret_f(c, (float)ns_num_double(SELF)); }
static void n_double(CPU *c) { ret_d(c, ns_num_double(SELF)); }
static void n_stringValue(CPU *c)
{
    Num *x = NUM(SELF);
    if (x->type == 'B' || rd32(SELF) == cls_cfbool)
        ret_i(c, ns_string(x->lo ? "1" : "0"));
    else if (is_float_type(x->type))
        ret_i(c, ns_stringf("%.15g", ns_num_double(SELF)));
    else if (x->type == 'Q')
        ret_i(c, ns_stringf("%llu", (unsigned long long)ns_num_int(SELF)));
    else
        ret_i(c, ns_stringf("%lld", (long long)ns_num_int(SELF)));
}
static void n_compare(CPU *c)
{
    double a = ns_num_double(SELF), b = ns_num_double(ARG(0));
    int64_t x = ns_num_int(SELF), y = ns_num_int(ARG(0));
    ret_i(c, a < b || (a == b && x < y) ? (u32)-1 : a > b || x > y ? 1 : 0);
}
static void n_isEqual(CPU *c) { ret_i(c, ARG(0) && ns_is_number(ARG(0)) && ns_equal(SELF, ARG(0))); }
static void n_hash(CPU *c) { ret_i(c, ns_hash(SELF)); }
static void n_objCType(CPU *c)
{
    static char types[256][2];
    u32 t = rd32(SELF) == cls_cfbool ? 'c' : NUM(SELF)->type;
    if (t == 'B')
        t = 'c';
    types[t & 0xFF][0] = (char)t;
    ret_i(c, GA(types[t & 0xFF]));
}
static void n_retain_self(CPU *c) { ret_i(c, objc_retain(SELF)); }
static void n_immortal(CPU *c) { (void)c; }
static void n_rc(CPU *c) { ret_i(c, 0x7fffffff); }

/* NSValue */

static id val_make(const void *p, u32 n, const char *type)
{
    id o = ns_alloc(cls_cval, sizeof(Val));
    Val *v = GP(o);
    size_t tl = strlen(type);                    /* an @encode string: cut to the field's size */
    if (tl >= sizeof v->type)
        tl = sizeof v->type - 1;
    memcpy(v->type, type, tl);
    v->type[tl] = 0;
    v->n = n > sizeof v->bytes ? sizeof v->bytes : n;
    memcpy(v->bytes, p, v->n);
    return objc_autorelease(o);
}
static void v_withPoint(CPU *c)
{
    u32 w[2] = {ARG(0), ARG(1)};
    ret_i(c, val_make(w, 8, "{CGPoint=ff}"));
}
static void v_withSize(CPU *c)
{
    u32 w[2] = {ARG(0), ARG(1)};
    ret_i(c, val_make(w, 8, "{CGSize=ff}"));
}
static void v_withRect(CPU *c)
{
    u32 w[4] = {ARG(0), ARG(1), ARG(2), ARG(3)};
    ret_i(c, val_make(w, 16, "{CGRect={CGPoint=ff}{CGSize=ff}}"));
}
static void v_withAffine(CPU *c)
{
    u32 w[6];
    for (int i = 0; i < 6; i++)
        w[i] = ARG(i);
    ret_i(c, val_make(w, 24, "{CGAffineTransform=ffffff}"));
}
static void v_withPointer(CPU *c)
{
    u32 w = ARG(0);
    ret_i(c, val_make(&w, 4, "^v"));
}
static u32 type_size(const char *t)
{
    /* sizes of the encodings NSValue is used with here */
    u32 n = 0;
    for (; *t; t++) {
        switch (*t) {
        case 'c': case 'C': case 'B': n += 1; break;
        case 's': case 'S': n += 2; break;
        case 'i': case 'I': case 'l': case 'L': case 'f': case '^': case '@': case '#': case ':': case '*': n += 4;
            if (*t == '^')
                return n;
            break;
        case 'q': case 'Q': case 'd': n += 8; break;
        case '=':
            break;
        case '{':
            while (*t && *t != '=' && *t != '}')
                t++;
            if (!*t)
                return n;
            break;
        default: break;
        }
    }
    return n;
}
static void v_withBytes(CPU *c)
{
    /* value:withObjCType: / valueWithBytes:objCType: */
    const char *t = GP(ARG(1));
    u32 n = type_size(t);
    ret_i(c, val_make(GP(ARG(0)), n ? n : 4, t));
}
static void v_getValue(CPU *c)
{
    if (rd32(SELF) == cls_cval) {
        Val *v = GP(SELF);
        memcpy(GP(ARG(0)), v->bytes, v->n);
    } else if (ns_is_number(SELF)) {
        Num *x = NUM(SELF);
        u32 n = x->type == 'q' || x->type == 'Q' || x->type == 'd' ? 8 : 4;
        memcpy(GP(ARG(0)), &x->lo, n);
    }
}
static void v_objCType(CPU *c) { ret_i(c, GA(((Val *)GP(SELF))->type)); }
static void v_stret_copy(CPU *c)
{
    /* CGPointValue / CGSizeValue / CGRectValue / CGAffineTransformValue: stret */
    Val *v = GP(ST_SELF);
    memcpy(GP(ST_RET), v->bytes, v->n);
}
static void v_point(CPU *c)
{
    /* CGPoint/CGSize are 8 bytes: stret too on iOS ARM */
    v_stret_copy(c);
}
static void v_pointer(CPU *c) { ret_i(c, *(u32 *)((Val *)GP(SELF))->bytes); }
static void v_isEqual(CPU *c)
{
    id o = ARG(0);
    Val *a = GP(SELF), *b = o ? GP(o) : NULL;
    ret_i(c, o == SELF || (o && rd32(o) == cls_cval && a->n == b->n && !memcmp(a->bytes, b->bytes, a->n)));
}

/* NSNull */

id ns_null(void) { return null_obj; }
static void null_null(CPU *c) { ret_i(c, null_obj); }
static void null_description(CPU *c) { ret_i(c, ns_string("<null>")); }

/* arrays */

int ns_is_array(id o) { return o && rd32(o) == cls_arrM; }

static void arr_reserve(Arr *a, u32 n)
{
    if (n <= a->cap)
        return;
    u32 cap = a->cap ? a->cap : 8;
    while (cap < n)
        cap *= 2;
    a->v = realloc(a->v, cap * 4);
    a->cap = cap;
}

id ns_array_new(const u32 *objs, u32 n)
{
    id o = ns_alloc(cls_arrM, sizeof(Arr));
    Arr *a = ARR(o);
    arr_reserve(a, n);
    for (u32 i = 0; i < n; i++)
        a->v[i] = objc_retain(objs[i]);
    a->n = n;
    return o;
}

void ns_array_add(id o, id x)
{
    Arr *a = ARR(o);
    if (!x)
        rt_fatal("-[NSMutableArray addObject:]: object cannot be nil");
    arr_reserve(a, a->n + 1);
    a->v[a->n++] = a->raw ? x : objc_retain(x);
    a->muts++;
}

static void arr_insert(id o, id x, u32 i)
{
    Arr *a = ARR(o);
    if (!x)
        rt_fatal("-[NSMutableArray insertObject:atIndex:]: object cannot be nil");
    if (i > a->n)
        bad_index("-[NSMutableArray insertObject:atIndex:]", i, a->n);
    arr_reserve(a, a->n + 1);
    memmove(a->v + i + 1, a->v + i, (a->n - i) * 4);
    a->v[i] = a->raw ? x : objc_retain(x);
    a->n++;
    a->muts++;
}

static void arr_remove_at(id o, u32 i)
{
    Arr *a = ARR(o);
    if (i >= a->n)
        bad_index("-[NSMutableArray removeObjectAtIndex:]", i, a->n);
    id x = a->v[i];
    memmove(a->v + i, a->v + i + 1, (a->n - i - 1) * 4);
    a->n--;
    a->muts++;
    if (!a->raw)
        objc_release(x);
}

static void arr_clear(id o)
{
    Arr *a = ARR(o);
    u32 n = a->n;
    u32 *v = a->v;
    a->v = NULL;
    a->n = a->cap = 0;
    a->muts++;
    if (!a->raw)
        for (u32 i = 0; i < n; i++)
            objc_release(v[i]);
    free(v);
}

u32 ns_array_count(id o)
{
    if (!o)
        return 0;
    return ns_is_array(o) ? ARR(o)->n : msg0(o, "count");
}

id ns_array_at(id o, u32 i)
{
    if (ns_is_array(o)) {
        if (i >= ARR(o)->n)
            bad_index("-[NSArray objectAtIndex:]", i, ARR(o)->n);
        return ARR(o)->v[i];
    }
    return msg1(o, "objectAtIndex:", i);
}

/* the objects of any array (host or not) into a malloc'd list */
static u32 *items_of(id o, u32 *n)
{
    *n = ns_array_count(o);
    u32 *v = malloc(4 * (size_t)*n + 4);
    for (u32 i = 0; i < *n; i++)
        v[i] = ns_array_at(o, i);
    return v;
}

static const char *const arr_cluster[] = {"NSArray", "NSMutableArray", "__NSArrayM", NULL};
static void a_alloc(CPU *c) { ret_i(c, ns_cluster_alloc(SELF, arr_cluster, cls_arrM, sizeof(Arr))); }
static int host_arr(id o) { return o && rd32(o) == cls_arrM; }

/* class constructors: autoreleased host arrays (game subclasses get alloc + init...) */
static id arr_from(u32 cls, const u32 *v, u32 n)
{
    if (cls != cls_arr && cls != cls_marr && cls != cls_arrM) {
        id tmp = objc_autorelease(ns_array_new(v, n));
        return objc_autorelease(msg1(msg0(cls, "alloc"), "initWithArray:", tmp));
    }
    return objc_autorelease(ns_array_new(v, n));
}

/* nil-terminated variadic objects starting at word `first` */
static u32 *nil_list(CPU *c, int first, u32 *n)
{
    u32 w[VA_WORDS];
    const u32 *va = va_words(c, first, w);
    u32 k = 0;
    while (k < VA_WORDS && va[k])
        k++;
    u32 *v = malloc(4 * (size_t)k + 4);
    memcpy(v, va, 4 * (size_t)k);
    *n = k;
    return v;
}

static void a_array(CPU *c) { ret_i(c, arr_from(SELF, NULL, 0)); }
static void a_withObject(CPU *c)
{
    u32 x = ARG(0);
    ret_i(c, arr_from(SELF, &x, 1));
}
static void a_withObjects(CPU *c)
{
    u32 n, *v = nil_list(c, 2, &n);
    ret_i(c, arr_from(SELF, v, n));
    free(v);
}
static void a_withObjectsCount(CPU *c) { ret_i(c, arr_from(SELF, GP(ARG(0)), ARG(1))); }
static void a_withArray(CPU *c)
{
    u32 n, *v = items_of(ARG(0), &n);
    ret_i(c, arr_from(SELF, v, n));
    free(v);
}
static void a_withContentsOfFile(CPU *c)
{
    id o = plist_read_file(ns_utf8(ARG(0)));
    if (o && !ns_is_array(o)) {
        objc_release(o);
        o = 0;
    }
    ret_i(c, o ? objc_autorelease(o) : 0);
}

static void a_fill(id self, const u32 *v, u32 n)
{
    if (!host_arr(self))
        return;
    arr_clear(self);
    arr_reserve(ARR(self), n);
    for (u32 i = 0; i < n; i++)
        ARR(self)->v[i] = objc_retain(v[i]);
    ARR(self)->n = n;
}
static void a_init(CPU *c) { (void)c; }
static void a_initWithObjects(CPU *c)
{
    u32 n, *v = nil_list(c, 2, &n);
    a_fill(SELF, v, n);
    free(v);
}
static void a_initWithObjectsCount(CPU *c) { a_fill(SELF, GP(ARG(0)), ARG(1)); }
static void a_initWithArray(CPU *c)
{
    u32 n, *v = items_of(ARG(0), &n);
    if (c->r[1] == sel_get("initWithArray:copyItems:") && (u8)ARG(1))
        for (u32 i = 0; i < n; i++)
            v[i] = objc_autorelease(msg1(v[i], "copyWithZone:", 0));
    a_fill(SELF, v, n);
    free(v);
}
static void a_initWithContentsOfFile(CPU *c)
{
    id o = plist_read_file(ns_utf8(ARG(0)));
    if (!o || !ns_is_array(o)) {
        if (o)
            objc_release(o);
        objc_release(SELF);
        ret_i(c, 0);
        return;
    }
    a_fill(SELF, ARR(o)->v, ARR(o)->n);
    objc_release(o);
}
static void a_dealloc(CPU *c)
{
    arr_clear(SELF);
    objc_free_instance(SELF);
}

static void a_count(CPU *c) { ret_i(c, ARR(SELF)->n); }
static void a_objectAtIndex(CPU *c) { ret_i(c, ns_array_at(SELF, ARG(0))); }
static void a_lastObject(CPU *c)
{
    u32 n = ns_array_count(SELF);
    ret_i(c, n ? ns_array_at(SELF, n - 1) : 0);
}
static void a_firstObject(CPU *c) { ret_i(c, ns_array_count(SELF) ? ns_array_at(SELF, 0) : 0); }
static u32 index_of(id a, id x, int identical)
{
    for (u32 i = 0, n = ns_array_count(a); i < n; i++) {
        id y = ns_array_at(a, i);
        if (y == x || (!identical && ns_equal(y, x)))
            return i;
    }
    return NS_NOT_FOUND;
}
static void a_containsObject(CPU *c) { ret_i(c, index_of(SELF, ARG(0), 0) != NS_NOT_FOUND); }
static void a_indexOfObject(CPU *c) { ret_i(c, index_of(SELF, ARG(0), 0)); }
static void a_indexOfObjectIdenticalTo(CPU *c) { ret_i(c, index_of(SELF, ARG(0), 1)); }
static void a_getObjects(CPU *c)
{
    for (u32 i = 0, n = ns_array_count(SELF); i < n; i++)
        wr32(ARG(0) + 4 * i, ns_array_at(SELF, i));
}

static id enum_make(id arr)
{
    id e = ns_alloc(cls_enum, sizeof(Enum));
    ((Enum *)GP(e))->arr = arr;     /* +1 owned */
    return objc_autorelease(e);
}
static void a_objectEnumerator(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    ret_i(c, enum_make(ns_array_new(v, n)));
    free(v);
}
static void a_reverseObjectEnumerator(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    for (u32 i = 0; i < n / 2; i++) {
        u32 t = v[i];
        v[i] = v[n - 1 - i];
        v[n - 1 - i] = t;
    }
    ret_i(c, enum_make(ns_array_new(v, n)));
    free(v);
}

/* NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]} */
static u32 fast_enum(u32 state, u32 *items, u32 n, u32 *muts)
{
    if (rd32(state))
        return 0;
    wr32(state, 1);
    wr32(state + 4, GA(items));
    wr32(state + 8, GA(muts));
    return n;
}
static void a_countByEnumerating(CPU *c) { ret_i(c, fast_enum(ARG(0), ARR(SELF)->v, ARR(SELF)->n, &ARR(SELF)->muts)); }

static void a_componentsJoined(CPU *c)
{
    Buf b = {0};
    const char *sep = ns_utf8(ARG(0));
    for (u32 i = 0, n = ns_array_count(SELF); i < n; i++) {
        if (i)
            buf_puts(&b, sep);
        buf_puts(&b, ns_describe(ns_array_at(SELF, i)));
    }
    ret_i(c, objc_autorelease(ns_string_new(b.p ? b.p : "", b.n)));
    buf_free(&b);
}
static void a_isEqualToArray(CPU *c)
{
    id o = ARG(0);
    u32 n = ns_array_count(SELF);
    int eq = o && ns_array_count(o) == n;
    for (u32 i = 0; eq && i < n; i++)
        eq = ns_equal(ns_array_at(SELF, i), ns_array_at(o, i));
    ret_i(c, eq);
}
static void a_isEqual(CPU *c)
{
    id o = ARG(0);
    if (o == SELF || (o && objc_is_kind(o, cls_arr)))
        a_isEqualToArray(c);
    else
        ret_i(c, 0);
}
static void a_hash(CPU *c) { ret_i(c, ns_array_count(SELF)); }

static void describe(Buf *b, id o, int indent);
static void a_description(CPU *c)
{
    Buf b = {0};
    describe(&b, SELF, 0);
    ret_i(c, objc_autorelease(ns_string_new(b.p, b.n)));
    buf_free(&b);
}
static void a_copy(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    ret_i(c, ns_array_new(v, n));
    free(v);
}
static void a_byAdding(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    id a = ns_array_new(v, n);
    free(v);
    if (c->r[1] == sel_get("arrayByAddingObject:")) {
        ns_array_add(a, ARG(0));
    } else {
        for (u32 i = 0, m = ns_array_count(ARG(0)); i < m; i++)
            ns_array_add(a, ns_array_at(ARG(0), i));
    }
    ret_i(c, objc_autorelease(a));
}
static void a_subarray(CPU *c)
{
    u32 n, *v = items_of(SELF, &n), loc = ARG(0), len = ARG(1);
    if (loc + len > n)
        bad_index("-[NSArray subarrayWithRange:]", loc + len, n);
    ret_i(c, objc_autorelease(ns_array_new(v + loc, len)));
    free(v);
}

/* stable merge sort with a guest comparison */
typedef struct { SEL sel; u32 fn, ctx; } SortBy;
static int sort_cmp(const SortBy *s, u32 a, u32 b)
{
    u32 r;
    if (s->fn) {
        r = rt_call3(s->fn, a, b, s->ctx);
    } else if (ns_is_string(a) && ns_is_string(b) && s->sel == sel_get("compare:")) {
        int d = strcmp(ns_utf8(a), ns_utf8(b));
        return d < 0 ? -1 : d > 0;
    } else {
        r = objc_msg(a, sel_name(s->sel), 1, &b);
    }
    return (int)(s32)r;
}
static void merge_sort(u32 *v, u32 *tmp, u32 n, const SortBy *s)
{
    if (n < 2)
        return;
    u32 h = n / 2;
    merge_sort(v, tmp, h, s);
    merge_sort(v + h, tmp, n - h, s);
    u32 i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = sort_cmp(s, v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, 4 * (size_t)n);
}
static void sort_items(u32 *v, u32 n, const SortBy *s)
{
    u32 *tmp = malloc(4 * (size_t)n + 4);
    merge_sort(v, tmp, n, s);
    free(tmp);
}
static void a_sortedUsingSelector(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    SortBy s = {ARG(0), 0, 0};
    sort_items(v, n, &s);
    ret_i(c, objc_autorelease(ns_array_new(v, n)));
    free(v);
}
static void a_sortedUsingFunction(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    SortBy s = {0, ARG(0), ARG(1)};
    sort_items(v, n, &s);
    ret_i(c, objc_autorelease(ns_array_new(v, n)));
    free(v);
}
static void a_makeObjectsPerform(CPU *c)
{
    u32 n, *v = items_of(SELF, &n);
    int with = c->r[1] == sel_get("makeObjectsPerformSelector:withObject:");
    u32 arg = ARG(1);
    const char *sel = sel_name(ARG(0));
    for (u32 i = 0; i < n; i++)
        objc_msg(v[i], sel, with, &arg);
    free(v);
}
static void a_writeToFile(CPU *c) { ret_i(c, plist_write_file(ns_utf8(ARG(0)), SELF)); }

static void ma_need(id o, const char *what)
{
    if (!host_arr(o))
        rt_fatal("-[%s %s] on an array the host did not make", objc_class_name(rd32(o)), what);
}
static void ma_add(CPU *c)
{
    ma_need(SELF, "addObject:");
    ns_array_add(SELF, ARG(0));
}
static void ma_addFromArray(CPU *c)
{
    ma_need(SELF, "addObjectsFromArray:");
    u32 n, *v = items_of(ARG(0), &n);
    for (u32 i = 0; i < n; i++)
        ns_array_add(SELF, v[i]);
    free(v);
}
static void ma_insert(CPU *c)
{
    ma_need(SELF, "insertObject:atIndex:");
    arr_insert(SELF, ARG(0), ARG(1));
}
static void ma_removeAt(CPU *c)
{
    ma_need(SELF, "removeObjectAtIndex:");
    arr_remove_at(SELF, ARG(0));
}
static void ma_removeLast(CPU *c)
{
    ma_need(SELF, "removeLastObject");
    if (ARR(SELF)->n)
        arr_remove_at(SELF, ARR(SELF)->n - 1);
}
static void ma_removeObject(CPU *c)
{
    ma_need(SELF, "removeObject:");
    int identical = c->r[1] == sel_get("removeObjectIdenticalTo:");
    id x = ARG(0);
    objc_retain(x);
    for (u32 i = ARR(SELF)->n; i-- > 0;)
        if (ARR(SELF)->v[i] == x || (!identical && ns_equal(ARR(SELF)->v[i], x)))
            arr_remove_at(SELF, i);
    objc_release(x);
}
static void ma_removeAll(CPU *c)
{
    ma_need(SELF, "removeAllObjects");
    arr_clear(SELF);
}
static void ma_removeInArray(CPU *c)
{
    ma_need(SELF, "removeObjectsInArray:");
    u32 n, *v = items_of(ARG(0), &n);
    for (u32 j = 0; j < n; j++)
        for (u32 i = ARR(SELF)->n; i-- > 0;)
            if (ns_equal(ARR(SELF)->v[i], v[j]))
                arr_remove_at(SELF, i);
    free(v);
}
static void ma_removeInRange(CPU *c)
{
    ma_need(SELF, "removeObjectsInRange:");
    for (u32 k = 0; k < ARG(1); k++)
        arr_remove_at(SELF, ARG(0));
}
static void ma_replace(CPU *c)
{
    ma_need(SELF, "replaceObjectAtIndex:withObject:");
    Arr *a = ARR(SELF);
    u32 i = ARG(0);
    if (i >= a->n)
        bad_index("-[NSMutableArray replaceObjectAtIndex:withObject:]", i, a->n);
    id old = a->v[i];
    a->v[i] = objc_retain(ARG(1));
    a->muts++;
    objc_release(old);
}
static void ma_exchange(CPU *c)
{
    ma_need(SELF, "exchangeObjectAtIndex:withObjectAtIndex:");
    Arr *a = ARR(SELF);
    u32 i = ARG(0), j = ARG(1);
    if (i >= a->n || j >= a->n)
        bad_index("-[NSMutableArray exchangeObjectAtIndex:withObjectAtIndex:]", i > j ? i : j, a->n);
    u32 t = a->v[i];
    a->v[i] = a->v[j];
    a->v[j] = t;
    a->muts++;
}
static void ma_setArray(CPU *c)
{
    ma_need(SELF, "setArray:");
    u32 n, *v = items_of(ARG(0), &n);
    for (u32 i = 0; i < n; i++)
        objc_retain(v[i]);
    arr_clear(SELF);
    for (u32 i = 0; i < n; i++) {
        ns_array_add(SELF, v[i]);
        objc_release(v[i]);
    }
    free(v);
}
static void ma_sortUsingSelector(CPU *c)
{
    ma_need(SELF, "sortUsingSelector:");
    SortBy s = {ARG(0), 0, 0};
    sort_items(ARR(SELF)->v, ARR(SELF)->n, &s);
    ARR(SELF)->muts++;
}
static void ma_sortUsingFunction(CPU *c)
{
    ma_need(SELF, "sortUsingFunction:context:");
    SortBy s = {0, ARG(0), ARG(1)};
    sort_items(ARR(SELF)->v, ARR(SELF)->n, &s);
    ARR(SELF)->muts++;
}

/* enumerators */

static void e_nextObject(CPU *c)
{
    Enum *e = GP(SELF);
    ret_i(c, e->i < ARR(e->arr)->n ? ARR(e->arr)->v[e->i++] : 0);
}
static void e_allObjects(CPU *c)
{
    Enum *e = GP(SELF);
    Arr *a = ARR(e->arr);
    ret_i(c, objc_autorelease(ns_array_new(a->v + e->i, a->n - e->i)));
    e->i = a->n;
}
static void e_countByEnumerating(CPU *c)
{
    Enum *e = GP(SELF);
    Arr *a = ARR(e->arr);
    u32 n = fast_enum(ARG(0), a->v + e->i, a->n - e->i, &a->muts);
    e->i = a->n;
    ret_i(c, n);
}
static void e_dealloc(CPU *c)
{
    objc_release(((Enum *)GP(SELF))->arr);
    objc_free_instance(SELF);
}

/* dictionaries and sets */

int ns_is_dict(id o) { return o && rd32(o) == cls_dictM; }
static int host_dict(id o) { return o && (rd32(o) == cls_dictM || rd32(o) == cls_setM); }

static u32 key_hash(Dict *d, id k) { return d->raw & 1 ? k * 2654435761u : ns_hash(k); }
static int key_eq(Dict *d, id a, id b) { return d->raw & 1 ? a == b : ns_equal(a, b); }

static void d_reindex(Dict *d)
{
    u32 ns = 16;
    while (ns < d->n * 2 + 2)
        ns *= 2;
    free(d->slot);
    d->slot = calloc(ns, 4);
    d->nslot = ns;
    for (u32 i = 0; i < d->n; i++) {
        u32 j = d->h[i] & (ns - 1);
        while (d->slot[j])
            j = (j + 1) & (ns - 1);
        d->slot[j] = i + 1;
    }
}

static int d_find(Dict *d, id k, u32 h)
{
    if (!d->nslot)
        return -1;
    for (u32 j = h & (d->nslot - 1);; j = (j + 1) & (d->nslot - 1)) {
        u32 s = d->slot[j];
        if (!s)
            return -1;
        if (d->h[s - 1] == h && key_eq(d, d->k[s - 1], k))
            return (int)(s - 1);
    }
}

static id dict_alloc(u32 cls, u32 raw)
{
    id o = ns_alloc(cls, sizeof(Dict));
    DICT(o)->raw = raw;
    return o;
}
id ns_dict_new(void) { return dict_alloc(cls_dictM, 0); }

static void d_set(id o, id k, id v)
{
    Dict *d = DICT(o);
    int set = rd32(o) == cls_setM, rawv = (d->raw & 2) || set;
    if (!k)
        rt_fatal("-[NSMutableDictionary setObject:forKey:]: key cannot be nil");
    if (!v && !rawv)
        rt_fatal("-[NSMutableDictionary setObject:forKey:]: object cannot be nil (key: %s)", ns_describe(k));
    u32 h = key_hash(d, k);
    int i = d_find(d, k, h);
    if (i >= 0) {
        if (set)
            return;
        id old = d->v[i];
        d->v[i] = rawv ? v : objc_retain(v);
        if (!rawv)
            objc_release(old);
        d->muts++;
        return;
    }
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->k = realloc(d->k, d->cap * 4);
        d->v = realloc(d->v, d->cap * 4);
        d->h = realloc(d->h, d->cap * 4);
    }
    /* keys are copied (a mutable string key must not change under the dictionary) */
    id key = k;
    if (!(d->raw & 1))
        key = ns_is_string(k) ? msg1(k, "copyWithZone:", 0) : objc_retain(k);
    d->k[d->n] = key;
    d->v[d->n] = set ? key : rawv ? v : objc_retain(v);
    d->h[d->n] = h;
    d->n++;
    d->muts++;
    if (d->n * 2 + 2 > d->nslot)
        d_reindex(d);
    else {
        u32 j = h & (d->nslot - 1);
        while (d->slot[j])
            j = (j + 1) & (d->nslot - 1);
        d->slot[j] = d->n;
    }
}

static void d_remove(id o, id k)
{
    Dict *d = DICT(o);
    int i = d_find(d, k, key_hash(d, k));
    if (i < 0)
        return;
    id ok = d->k[i], ov = d->v[i];
    memmove(d->k + i, d->k + i + 1, (d->n - (u32)i - 1) * 4);
    memmove(d->v + i, d->v + i + 1, (d->n - (u32)i - 1) * 4);
    memmove(d->h + i, d->h + i + 1, (d->n - (u32)i - 1) * 4);
    d->n--;
    d->muts++;
    d_reindex(d);
    if (!(d->raw & 1))
        objc_release(ok);
    if (!(d->raw & 2) && rd32(o) != cls_setM)
        objc_release(ov);
}

static void d_clear(id o)
{
    Dict *d = DICT(o);
    u32 n = d->n, *k = d->k, *v = d->v;
    int set = rd32(o) == cls_setM;
    d->k = d->v = NULL;
    free(d->h);
    free(d->slot);
    d->h = d->slot = NULL;
    d->n = d->cap = d->nslot = 0;
    d->muts++;
    for (u32 i = 0; i < n; i++) {
        if (!(d->raw & 1))
            objc_release(k[i]);
        if (!(d->raw & 2) && !set)
            objc_release(v[i]);
    }
    free(k);
    free(v);
}

id ns_dict_get(id o, id k)
{
    if (!o || !k)
        return 0;
    if (host_dict(o)) {
        Dict *d = DICT(o);
        int i = d_find(d, k, key_hash(d, k));
        return i < 0 ? 0 : d->v[i];
    }
    return msg1(o, "objectForKey:", k);
}
void ns_dict_set(id d, id k, id v) { d_set(d, k, v); }
u32 ns_dict_count(id o) { return host_dict(o) ? DICT(o)->n : o ? msg0(o, "count") : 0; }
id ns_dict_key_at(id o, u32 i) { return DICT(o)->k[i]; }
id ns_dict_val_at(id o, u32 i) { return DICT(o)->v[i]; }

/* keys and values of any dictionary */
static u32 dict_pairs(id o, u32 **keys, u32 **vals)
{
    if (host_dict(o)) {
        Dict *d = DICT(o);
        *keys = malloc(4 * (size_t)d->n + 4);
        *vals = malloc(4 * (size_t)d->n + 4);
        memcpy(*keys, d->k, 4 * (size_t)d->n);
        memcpy(*vals, d->v, 4 * (size_t)d->n);
        return d->n;
    }
    id ks = msg0(o, "allKeys");
    u32 n, *k = items_of(ks, &n);
    u32 *v = malloc(4 * (size_t)n + 4);
    for (u32 i = 0; i < n; i++)
        v[i] = msg1(o, "objectForKey:", k[i]);
    *keys = k;
    *vals = v;
    return n;
}

static const char *const dict_cluster[] = {"NSDictionary", "NSMutableDictionary", "__NSDictionaryM", NULL};
static void d_alloc(CPU *c) { ret_i(c, ns_cluster_alloc(SELF, dict_cluster, cls_dictM, sizeof(Dict))); }

static id dict_from(u32 cls, const u32 *k, const u32 *v, u32 n)
{
    id d = ns_dict_new();
    for (u32 i = 0; i < n; i++)
        d_set(d, k[i], v[i]);
    if (cls != cls_dict && cls != cls_mdict && cls != cls_dictM) {
        objc_autorelease(d);
        return objc_autorelease(msg1(msg0(cls, "alloc"), "initWithDictionary:", d));
    }
    return objc_autorelease(d);
}

static void d_dictionary(CPU *c) { ret_i(c, dict_from(SELF, NULL, NULL, 0)); }
static void d_withObjectForKey(CPU *c)
{
    u32 v = ARG(0), k = ARG(1);
    ret_i(c, dict_from(SELF, &k, &v, 1));
}
/* objects and keys alternate, nil-terminated, from word `first` */
static u32 obj_key_list(CPU *c, int first, u32 **keys, u32 **vals)
{
    u32 w[VA_WORDS];
    const u32 *va = va_words(c, first, w);
    u32 n = 0;
    while (2 * n + 1 < VA_WORDS && va[2 * n])
        n++;
    *keys = malloc(4 * (size_t)n + 4);
    *vals = malloc(4 * (size_t)n + 4);
    for (u32 i = 0; i < n; i++) {
        (*vals)[i] = va[2 * i];
        (*keys)[i] = va[2 * i + 1];
    }
    return n;
}
static void d_withObjectsAndKeys(CPU *c)
{
    u32 *k, *v, n = obj_key_list(c, 2, &k, &v);
    ret_i(c, dict_from(SELF, k, v, n));
    free(k);
    free(v);
}
static void d_withDictionary(CPU *c)
{
    u32 *k, *v, n = ARG(0) ? dict_pairs(ARG(0), &k, &v) : 0;
    ret_i(c, dict_from(SELF, n ? k : NULL, n ? v : NULL, n));
    if (ARG(0)) {
        free(k);
        free(v);
    }
}
static void d_withObjectsForKeys(CPU *c)
{
    u32 nk, *k = items_of(ARG(1), &nk), nv, *v = items_of(ARG(0), &nv);
    ret_i(c, dict_from(SELF, k, v, nk < nv ? nk : nv));
    free(k);
    free(v);
}
static void d_withObjectsForKeysCount(CPU *c) { ret_i(c, dict_from(SELF, GP(ARG(1)), GP(ARG(0)), ARG(2))); }
static void d_withContentsOfFile(CPU *c)
{
    id o = plist_read_file(ns_utf8(ARG(0)));
    if (o && !ns_is_dict(o)) {
        objc_release(o);
        o = 0;
    }
    ret_i(c, o ? objc_autorelease(o) : 0);
}

static void d_fill(id self, const u32 *k, const u32 *v, u32 n)
{
    if (!host_dict(self))
        return;
    d_clear(self);
    for (u32 i = 0; i < n; i++)
        d_set(self, k[i], v[i]);
}
static void d_init(CPU *c) { (void)c; }
static void d_initWithObjectsAndKeys(CPU *c)
{
    u32 *k, *v, n = obj_key_list(c, 2, &k, &v);
    d_fill(SELF, k, v, n);
    free(k);
    free(v);
}
static void d_initWithDictionary(CPU *c)
{
    if (!ARG(0))
        return;
    u32 *k, *v, n = dict_pairs(ARG(0), &k, &v);
    if (c->r[1] == sel_get("initWithDictionary:copyItems:") && (u8)ARG(1))
        for (u32 i = 0; i < n; i++)
            v[i] = objc_autorelease(msg1(v[i], "copyWithZone:", 0));
    if (host_dict(SELF)) {
        d_fill(SELF, k, v, n);
    } else {
        u32 a[3] = {GA(v), GA(k), n};
        ret_i(c, objc_msg(SELF, "initWithObjects:forKeys:count:", 3, a));
    }
    free(k);
    free(v);
}
static void d_initWithObjectsForKeys(CPU *c)
{
    u32 nk, *k = items_of(ARG(1), &nk), nv, *v = items_of(ARG(0), &nv);
    d_fill(SELF, k, v, nk < nv ? nk : nv);
    free(k);
    free(v);
}
static void d_initWithObjectsForKeysCount(CPU *c) { d_fill(SELF, GP(ARG(1)), GP(ARG(0)), ARG(2)); }
static void d_initWithContentsOfFile(CPU *c)
{
    id o = plist_read_file(ns_utf8(ARG(0)));
    if (!o || !ns_is_dict(o)) {
        if (o)
            objc_release(o);
        objc_release(SELF);
        ret_i(c, 0);
        return;
    }
    d_fill(SELF, DICT(o)->k, DICT(o)->v, DICT(o)->n);
    objc_release(o);
}
static void d_dealloc(CPU *c)
{
    d_clear(SELF);
    objc_free_instance(SELF);
}

static void d_count(CPU *c) { ret_i(c, DICT(SELF)->n); }
static void d_objectForKey(CPU *c) { ret_i(c, ns_dict_get(SELF, ARG(0))); }
static void d_allKeys(CPU *c) { ret_i(c, objc_autorelease(ns_array_new(DICT(SELF)->k, DICT(SELF)->n))); }
static void d_allValues(CPU *c) { ret_i(c, objc_autorelease(ns_array_new(DICT(SELF)->v, DICT(SELF)->n))); }
static void d_keyEnumerator(CPU *c) { ret_i(c, enum_make(ns_array_new(DICT(SELF)->k, DICT(SELF)->n))); }
static void d_objectEnumerator(CPU *c) { ret_i(c, enum_make(ns_array_new(DICT(SELF)->v, DICT(SELF)->n))); }
static void d_allKeysForObject(CPU *c)
{
    Dict *d = DICT(SELF);
    id a = ns_array_new(NULL, 0);
    for (u32 i = 0; i < d->n; i++)
        if (ns_equal(d->v[i], ARG(0)))
            ns_array_add(a, d->k[i]);
    ret_i(c, objc_autorelease(a));
}
static void d_keysSortedByValue(CPU *c)
{
    /* sort the keys by their values with the selector */
    Dict *d = DICT(SELF);
    u32 n = d->n, *idx = malloc(4 * (size_t)n + 4);
    u32 *vals = malloc(4 * (size_t)n + 4);
    memcpy(vals, d->v, 4 * (size_t)n);
    SortBy s = {ARG(0), 0, 0};
    sort_items(vals, n, &s);
    u8 *used = calloc(n + 1, 1);
    for (u32 i = 0; i < n; i++)
        for (u32 j = 0; j < n; j++)
            if (!used[j] && d->v[j] == vals[i]) {
                idx[i] = d->k[j];
                used[j] = 1;
                break;
            }
    ret_i(c, objc_autorelease(ns_array_new(idx, n)));
    free(idx);
    free(vals);
    free(used);
}
static void d_isEqualToDictionary(CPU *c)
{
    id o = ARG(0);
    Dict *d = DICT(SELF);
    int eq = o && ns_dict_count(o) == d->n;
    for (u32 i = 0; eq && i < d->n; i++)
        eq = ns_equal(d->v[i], ns_dict_get(o, d->k[i]));
    ret_i(c, eq);
}
static void d_countByEnumerating(CPU *c) { ret_i(c, fast_enum(ARG(0), DICT(SELF)->k, DICT(SELF)->n, &DICT(SELF)->muts)); }
static void d_copy(CPU *c)
{
    Dict *d = DICT(SELF);
    id o = ns_dict_new();
    for (u32 i = 0; i < d->n; i++)
        d_set(o, d->k[i], d->v[i]);
    ret_i(c, o);
}
static void d_writeToFile(CPU *c) { ret_i(c, plist_write_file(ns_utf8(ARG(0)), SELF)); }
static void d_objectsForKeys(CPU *c)
{
    u32 n, *k = items_of(ARG(0), &n);
    id a = ns_array_new(NULL, 0);
    for (u32 i = 0; i < n; i++) {
        id v = ns_dict_get(SELF, k[i]);
        ns_array_add(a, v ? v : ARG(1));
    }
    free(k);
    ret_i(c, objc_autorelease(a));
}
static void d_valuesForKeys(CPU *c)
{
    u32 n, *k = items_of(ARG(0), &n);
    id d = ns_dict_new();
    for (u32 i = 0; i < n; i++) {
        id v = msg1(SELF, "valueForKey:", k[i]);
        d_set(d, k[i], v ? v : null_obj);
    }
    free(k);
    ret_i(c, objc_autorelease(d));
}

static void md_need(id o, const char *what)
{
    if (!host_dict(o))
        rt_fatal("-[%s %s] on a dictionary the host did not make", objc_class_name(rd32(o)), what);
}
static void md_setObjectForKey(CPU *c)
{
    md_need(SELF, "setObject:forKey:");
    d_set(SELF, ARG(1), ARG(0));
}
static void md_setValueForKey(CPU *c)
{
    md_need(SELF, "setValue:forKey:");
    if (ARG(0))
        d_set(SELF, ARG(1), ARG(0));
    else
        d_remove(SELF, ARG(1));
}
static void md_removeObjectForKey(CPU *c)
{
    md_need(SELF, "removeObjectForKey:");
    d_remove(SELF, ARG(0));
}
static void md_removeAll(CPU *c)
{
    md_need(SELF, "removeAllObjects");
    d_clear(SELF);
}
static void md_addEntries(CPU *c)
{
    md_need(SELF, "addEntriesFromDictionary:");
    if (!ARG(0))
        return;
    u32 *k, *v, n = dict_pairs(ARG(0), &k, &v);
    for (u32 i = 0; i < n; i++)
        d_set(SELF, k[i], v[i]);
    free(k);
    free(v);
}
static void md_removeForKeys(CPU *c)
{
    md_need(SELF, "removeObjectsForKeys:");
    u32 n, *k = items_of(ARG(0), &n);
    for (u32 i = 0; i < n; i++)
        d_remove(SELF, k[i]);
    free(k);
}
static void md_setDictionary(CPU *c)
{
    md_need(SELF, "setDictionary:");
    u32 *k, *v, n = dict_pairs(ARG(0), &k, &v);
    for (u32 i = 0; i < n; i++) {
        objc_retain(k[i]);
        objc_retain(v[i]);
    }
    d_clear(SELF);
    for (u32 i = 0; i < n; i++) {
        d_set(SELF, k[i], v[i]);
        objc_release(k[i]);
        objc_release(v[i]);
    }
    free(k);
    free(v);
}

/* sets: dictionaries whose values are the keys */
static const char *const set_cluster[] = {"NSSet", "NSMutableSet", "__NSSetM", NULL};
static void st_alloc(CPU *c) { ret_i(c, ns_cluster_alloc(SELF, set_cluster, cls_setM, sizeof(Dict))); }
static id set_from(const u32 *v, u32 n)
{
    id s = dict_alloc(cls_setM, 0);
    for (u32 i = 0; i < n; i++)
        d_set(s, v[i], v[i]);
    return objc_autorelease(s);
}
id ns_set_new(const u32 *objs, u32 n) { return set_from(objs, n); }
static void st_set(CPU *c) { ret_i(c, set_from(NULL, 0)); }
static void st_withObject(CPU *c)
{
    u32 x = ARG(0);
    ret_i(c, set_from(&x, 1));
}
static void st_withObjects(CPU *c)
{
    u32 n, *v = nil_list(c, 2, &n);
    ret_i(c, set_from(v, n));
    free(v);
}
static void st_withArray(CPU *c)
{
    u32 n, *v = items_of(ARG(0), &n);
    ret_i(c, set_from(v, n));
    free(v);
}
static void st_withSet(CPU *c)
{
    id o = ARG(0);
    ret_i(c, o && host_dict(o) ? set_from(DICT(o)->k, DICT(o)->n) : set_from(NULL, 0));
}
static void st_initWithArray(CPU *c)
{
    u32 n, *v = items_of(ARG(0), &n);
    for (u32 i = 0; i < n; i++)
        d_set(SELF, v[i], v[i]);
    free(v);
}
static void st_initWithObjects(CPU *c)
{
    u32 n, *v = nil_list(c, 2, &n);
    for (u32 i = 0; i < n; i++)
        d_set(SELF, v[i], v[i]);
    free(v);
}
static void st_member(CPU *c)
{
    Dict *d = DICT(SELF);
    int i = ARG(0) ? d_find(d, ARG(0), key_hash(d, ARG(0))) : -1;
    ret_i(c, i < 0 ? 0 : d->k[i]);
}
static void st_contains(CPU *c)
{
    Dict *d = DICT(SELF);
    ret_i(c, ARG(0) && d_find(d, ARG(0), key_hash(d, ARG(0))) >= 0);
}
static void st_anyObject(CPU *c) { ret_i(c, DICT(SELF)->n ? DICT(SELF)->k[0] : 0); }
static void st_add(CPU *c) { d_set(SELF, ARG(0), ARG(0)); }
static void st_remove(CPU *c) { d_remove(SELF, ARG(0)); }
static void st_addFromArray(CPU *c) { st_initWithArray(c); }
static void st_union(CPU *c)
{
    id o = ARG(0);
    if (o && host_dict(o))
        for (u32 i = 0; i < DICT(o)->n; i++)
            d_set(SELF, DICT(o)->k[i], DICT(o)->k[i]);
}
static void st_minus(CPU *c)
{
    id o = ARG(0);
    if (o && host_dict(o))
        for (u32 i = 0; i < DICT(o)->n; i++)
            d_remove(SELF, DICT(o)->k[i]);
}
static void st_intersect(CPU *c)
{
    id o = ARG(0);
    Dict *d = DICT(SELF);
    for (u32 i = d->n; i-- > 0;)
        if (!o || !host_dict(o) || d_find(DICT(o), d->k[i], d->h[i]) < 0)
            d_remove(SELF, d->k[i]);
}
static void st_copy(CPU *c)
{
    id s = dict_alloc(cls_setM, 0);
    for (u32 i = 0; i < DICT(SELF)->n; i++)
        d_set(s, DICT(SELF)->k[i], DICT(SELF)->k[i]);
    ret_i(c, s);
}
static void st_isEqualToSet(CPU *c)
{
    id o = ARG(0);
    Dict *d = DICT(SELF);
    int eq = o && host_dict(o) && DICT(o)->n == d->n;
    for (u32 i = 0; eq && i < d->n; i++)
        eq = d_find(DICT(o), d->k[i], d->h[i]) >= 0;
    ret_i(c, eq);
}

/* NSData */

int ns_is_data(id o) { return o && rd32(o) == cls_cfdata; }

static void data_reserve(Data *d, u32 n)
{
    if (n <= d->cap && d->own)
        return;
    u32 cap = d->cap > 16 ? d->cap : 16;
    while (cap < n)
        cap *= 2;
    u8 *p = malloc(cap);
    if (d->n)
        memcpy(p, d->p, d->n);
    if (d->own == 1)
        free(d->p);
    d->p = p;
    d->cap = cap;
    d->own = 1;
}

id ns_data_new(const void *p, u32 n)
{
    id o = ns_alloc(cls_cfdata, sizeof(Data));
    Data *d = DATA(o);
    data_reserve(d, n + 1);
    if (n)
        memcpy(d->p, p, n);
    d->n = n;
    return o;
}

const u8 *ns_data_bytes(id o, u32 *len)
{
    if (ns_is_data(o)) {
        if (len)
            *len = DATA(o)->n;
        return DATA(o)->p;
    }
    if (len)
        *len = o ? msg0(o, "length") : 0;
    return o ? GP(msg0(o, "bytes")) : NULL;
}

static const char *const data_cluster[] = {"NSData", "NSMutableData", "__NSCFData", NULL};
static void dt_alloc(CPU *c) { ret_i(c, ns_cluster_alloc(SELF, data_cluster, cls_cfdata, sizeof(Data))); }
static void dt_data(CPU *c) { ret_i(c, objc_autorelease(ns_data_new(NULL, 0))); }
static void dt_withBytes(CPU *c) { ret_i(c, objc_autorelease(ns_data_new(GP(ARG(0)), ARG(1)))); }
static void dt_withBytesNoCopy(CPU *c)
{
    /* dataWithBytesNoCopy:length:[freeWhenDone:] -- the bytes are taken over */
    id o = ns_alloc(cls_cfdata, sizeof(Data));
    Data *d = DATA(o);
    d->p = GP(ARG(0));
    d->n = d->cap = ARG(1);
    d->own = c->r[1] == sel_get("dataWithBytesNoCopy:length:freeWhenDone:") && !(u8)ARG(2) ? 0 : 2;
    ret_i(c, objc_autorelease(o));
}
static void dt_withContentsOfFile(CPU *c)
{
    u32 n;
    u8 *p = gt_read_file(ns_utf8(ARG(0)), &n);
    if (!p) {
        if (c->r[1] == sel_get("dataWithContentsOfFile:options:error:") && ARG(2))
            wr32(ARG(2), 0);
        ret_i(c, 0);
        return;
    }
    id o = ns_data_new(p, n);
    free(p);
    ret_i(c, objc_autorelease(o));
}
static void dt_withContentsOfURL(CPU *c)
{
    id url = ARG(0);
    const char *path = url ? ns_utf8(msg0(url, "path")) : "";
    u32 n;
    u8 *p = *path ? gt_read_file(path, &n) : NULL;
    if (!p) {
        rt_log("[ns] dataWithContentsOfURL: %s: not read (no network)\n", url ? ns_describe(url) : "nil");
        ret_i(c, 0);
        return;
    }
    id o = ns_data_new(p, n);
    free(p);
    ret_i(c, objc_autorelease(o));
}
static void dt_withData(CPU *c)
{
    u32 n;
    const u8 *p = ns_data_bytes(ARG(0), &n);
    ret_i(c, objc_autorelease(ns_data_new(p, n)));
}
static void dt_withLength(CPU *c)
{
    id o = ns_data_new(NULL, 0);
    data_reserve(DATA(o), ARG(0) + 1);
    memset(DATA(o)->p, 0, ARG(0));
    DATA(o)->n = ARG(0);
    ret_i(c, objc_autorelease(o));
}
static void dt_set_bytes(id self, const u8 *p, u32 n)
{
    if (!ns_is_data(self))
        return;
    Data *d = DATA(self);
    d->n = 0;
    data_reserve(d, n + 1);
    if (n)
        memcpy(d->p, p, n);
    d->n = n;
}
static void dt_initWithBytes(CPU *c) { dt_set_bytes(SELF, GP(ARG(0)), ARG(1)); }
static void dt_initWithBytesNoCopy(CPU *c)
{
    if (!ns_is_data(SELF))
        return;
    Data *d = DATA(SELF);
    d->p = GP(ARG(0));
    d->n = d->cap = ARG(1);
    d->own = c->r[1] == sel_get("initWithBytesNoCopy:length:freeWhenDone:") && !(u8)ARG(2) ? 0 : 2;
}
static void dt_initWithContentsOfFile(CPU *c)
{
    u32 n;
    u8 *p = gt_read_file(ns_utf8(ARG(0)), &n);
    if (!p) {
        objc_release(SELF);
        ret_i(c, 0);
        return;
    }
    dt_set_bytes(SELF, p, n);
    free(p);
}
static void dt_initWithContentsOfURL(CPU *c)
{
    id url = ARG(0);
    const char *path = url ? ns_utf8(msg0(url, "path")) : "";
    u32 n;
    u8 *p = *path ? gt_read_file(path, &n) : NULL;
    if (!p) {
        objc_release(SELF);
        ret_i(c, 0);
        return;
    }
    dt_set_bytes(SELF, p, n);
    free(p);
}
static void dt_initWithData(CPU *c)
{
    u32 n;
    const u8 *p = ns_data_bytes(ARG(0), &n);
    dt_set_bytes(SELF, p, n);
}
static void dt_initWithLength(CPU *c)
{
    if (!ns_is_data(SELF))
        return;
    data_reserve(DATA(SELF), ARG(0) + 1);
    memset(DATA(SELF)->p, 0, ARG(0));
    DATA(SELF)->n = ARG(0);
}
static void dt_dealloc(CPU *c)
{
    Data *d = DATA(SELF);
    if (d->own == 1)
        free(d->p);
    else if (d->own == 2)
        free(d->p);         /* NoCopy data came from the game's malloc, which is the host's */
    objc_free_instance(SELF);
}
static void dt_bytes(CPU *c) { ret_i(c, GA(DATA(SELF)->p)); }
static void dt_length(CPU *c) { ret_i(c, DATA(SELF)->n); }
static void dt_getBytes(CPU *c)
{
    /* getBytes: / getBytes:length: / getBytes:range: */
    Data *d = DATA(SELF);
    SEL s = c->r[1];
    u32 loc = 0, len = d->n;
    if (s == sel_get("getBytes:length:"))
        len = ARG(1) < d->n ? ARG(1) : d->n;
    else if (s == sel_get("getBytes:range:")) {
        loc = ARG(1);
        len = ARG(2);
        if (loc + len > d->n)
            bad_index("-[NSData getBytes:range:]", loc + len, d->n);
    }
    memcpy(GP(ARG(0)), d->p + loc, len);
}
static void dt_subdata(CPU *c)
{
    Data *d = DATA(SELF);
    if (ARG(0) + ARG(1) > d->n)
        bad_index("-[NSData subdataWithRange:]", ARG(0) + ARG(1), d->n);
    ret_i(c, objc_autorelease(ns_data_new(d->p + ARG(0), ARG(1))));
}
static void dt_isEqual(CPU *c)
{
    id o = ARG(0);
    u32 n;
    const u8 *p = o && (ns_is_data(o) || objc_is_kind(o, cls_data)) ? ns_data_bytes(o, &n) : NULL;
    ret_i(c, o == SELF || (p && n == DATA(SELF)->n && !memcmp(p, DATA(SELF)->p, n)));
}
static void dt_writeToFile(CPU *c)
{
    if (c->r[1] == sel_get("writeToFile:options:error:") && ARG(2))
        wr32(ARG(2), 0);
    ret_i(c, gt_write_file(ns_utf8(ARG(0)), DATA(SELF)->p, DATA(SELF)->n));
}
static void dt_copy(CPU *c) { ret_i(c, ns_data_new(DATA(SELF)->p, DATA(SELF)->n)); }
static void dt_description(CPU *c)
{
    Data *d = DATA(SELF);
    Buf b = {0};
    buf_puts(&b, "<");
    for (u32 i = 0; i < d->n; i++) {
        if (i && i % 4 == 0)
            buf_puts(&b, " ");
        buf_printf(&b, "%02x", d->p[i]);
    }
    buf_puts(&b, ">");
    ret_i(c, objc_autorelease(ns_string_new(b.p, b.n)));
    buf_free(&b);
}
static void data_set_len(Data *d, u32 n)
{
    data_reserve(d, n + 1);
    if (n > d->n)
        memset(d->p + d->n, 0, n - d->n);
    d->n = n;
}
static void dt_setLength(CPU *c) { data_set_len(DATA(SELF), ARG(0)); }
static void dt_increaseLength(CPU *c)
{
    Data *d = DATA(SELF);
    u32 n = d->n + ARG(0);
    data_reserve(d, n + 1);
    memset(d->p + d->n, 0, ARG(0));
    d->n = n;
}
static void dt_append(CPU *c)
{
    /* appendBytes:length: / appendData: */
    Data *d = DATA(SELF);
    u32 n;
    const u8 *p;
    if (c->r[1] == sel_get("appendData:")) {
        p = ns_data_bytes(ARG(0), &n);
    } else {
        p = GP(ARG(0));
        n = ARG(1);
    }
    u8 *tmp = malloc(n + 1);
    memcpy(tmp, p, n);
    data_reserve(d, d->n + n + 1);
    memcpy(d->p + d->n, tmp, n);
    d->n += n;
    free(tmp);
}
static void dt_replaceBytes(CPU *c)
{
    Data *d = DATA(SELF);
    u32 loc = ARG(0), len = ARG(1);
    if (loc + len > d->n)
        data_set_len(d, loc + len);
    memcpy(d->p + loc, GP(ARG(2)), len);
}
static void dt_resetBytes(CPU *c)
{
    Data *d = DATA(SELF);
    if (ARG(0) + ARG(1) <= d->n)
        memset(d->p + ARG(0), 0, ARG(1));
}
static void dt_setData(CPU *c)
{
    u32 n;
    const u8 *p = ns_data_bytes(ARG(0), &n);
    u8 *tmp = malloc(n + 1);
    memcpy(tmp, p, n);
    dt_set_bytes(SELF, tmp, n);
    free(tmp);
}
static void dt_base64(CPU *c)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Data *d = DATA(SELF);
    u32 line = ARG(0), col = 0;
    Buf b = {0};
    for (u32 i = 0; i < d->n; i += 3) {
        u32 v = (u32)d->p[i] << 16 | (i + 1 < d->n ? (u32)d->p[i + 1] << 8 : 0) | (i + 2 < d->n ? d->p[i + 2] : 0);
        char o[4] = {tab[v >> 18 & 63], tab[v >> 12 & 63], i + 1 < d->n ? tab[v >> 6 & 63] : '=',
                     i + 2 < d->n ? tab[v & 63] : '='};
        if (line && col + 4 > line) {
            buf_puts(&b, "\n");
            col = 0;
        }
        buf_put(&b, o, 4);
        col += 4;
    }
    ret_i(c, objc_autorelease(ns_string_new(b.p ? b.p : "", b.n)));
    buf_free(&b);
}

/* descriptions */

static void indent_to(Buf *b, int n)
{
    for (int i = 0; i < n; i++)
        buf_puts(b, "    ");
}
static void describe(Buf *b, id o, int ind)
{
    if (ns_is_array(o)) {
        buf_puts(b, "(\n");
        for (u32 i = 0; i < ARR(o)->n; i++) {
            indent_to(b, ind + 1);
            describe(b, ARR(o)->v[i], ind + 1);
            buf_puts(b, i + 1 < ARR(o)->n ? ",\n" : "\n");
        }
        indent_to(b, ind);
        buf_puts(b, ")");
    } else if (ns_is_dict(o)) {
        buf_puts(b, "{\n");
        for (u32 i = 0; i < DICT(o)->n; i++) {
            indent_to(b, ind + 1);
            describe(b, DICT(o)->k[i], ind + 1);
            buf_puts(b, " = ");
            describe(b, DICT(o)->v[i], ind + 1);
            buf_puts(b, ";\n");
        }
        indent_to(b, ind);
        buf_puts(b, "}");
    } else {
        buf_puts(b, ns_describe(o));
    }
}
static void d_description(CPU *c) { a_description(c); }

/* CF functions */

void h_CFRelease(u32 o)
{
    if (!o)
        rt_fatal("CFRelease(NULL)");
    objc_release(o);
}
u32 h_CFAllocatorGetDefault(void) { return 0; }

u32 h_CFArrayCreate(u32 alloc, u32 values, u32 n, u32 cb)
{
    (void)alloc;
    id a = ns_alloc(cls_arrM, sizeof(Arr));
    ARR(a)->raw = cb != GA(ns_cf_callbacks[0]);
    for (u32 i = 0; i < n; i++)
        ns_array_add(a, rd32(values + 4 * i));
    return a;
}
u32 h_CFArrayCreateMutable(u32 alloc, u32 cap, u32 cb)
{
    (void)alloc;
    (void)cap;
    id a = ns_alloc(cls_arrM, sizeof(Arr));
    ARR(a)->raw = cb != GA(ns_cf_callbacks[0]);
    return a;
}
void h_CFArrayAppendValue(u32 a, u32 v) { ns_array_add(a, v); }
u32 h_CFArrayGetCount(u32 a) { return ns_array_count(a); }
u32 h_CFArrayGetValueAtIndex(u32 a, u32 i) { return ns_array_at(a, i); }
void h_CFArrayRemoveAllValues(u32 a) { arr_clear(a); }

u32 h_CFDictionaryCreateMutable(u32 alloc, u32 cap, u32 kcb, u32 vcb)
{
    (void)alloc;
    (void)cap;
    return dict_alloc(cls_dictM, (kcb != GA(ns_cf_callbacks[1]) ? 1u : 0u) | (vcb != GA(ns_cf_callbacks[2]) ? 2u : 0u));
}
u32 h_CFDictionaryGetValue(u32 d, u32 k) { return ns_dict_get(d, k); }
u32 h_CFDictionaryGetValueIfPresent(u32 d, u32 k, u32 out)
{
    if (!host_dict(d)) {
        id v = ns_dict_get(d, k);
        if (v && out)
            wr32(out, v);
        return v != 0;
    }
    int i = d_find(DICT(d), k, key_hash(DICT(d), k));
    if (i >= 0 && out)
        wr32(out, DICT(d)->v[i]);
    return i >= 0;
}
void h_CFDictionarySetValue(u32 d, u32 k, u32 v) { d_set(d, k, v); }
void h_CFDictionaryRemoveValue(u32 d, u32 k) { d_remove(d, k); }

/* CFNumberType: 1 SInt8, 2 SInt16, 3 SInt32, 4 SInt64, 5 Float32, 6 Float64, 7 char, 8 short,
   9 int, 10 long, 11 long long, 12 float, 13 double, 14 CFIndex, 15 NSInteger, 16 CGFloat */
u32 h_CFNumberGetValue(u32 n, u32 type, u32 out)
{
    switch (type) {
    case 1: case 7: *(u8 *)GP(out) = (u8)ns_num_int(n); break;
    case 2: case 8: *(u16 *)GP(out) = (u16)ns_num_int(n); break;
    case 4: case 11: *(ua64 *)GP(out) = (u64)ns_num_int(n); break;
    case 5: case 12: case 16: *(float *)GP(out) = (float)ns_num_double(n); break;
    case 6: case 13: *(double *)GP(out) = ns_num_double(n); break;
    default: wr32(out, (u32)ns_num_int(n)); break;
    }
    return 1;
}

/* registration */

static void make_bool(id *o, int v)
{
    *o = ns_alloc(cls_cfbool, sizeof(Num));
    NUM(*o)->type = 'c';
    NUM(*o)->lo = (u32)v;
}

void ns_init_collections(void)
{
    cls_arr = ns_class("NSArray");
    cls_marr = ns_class("NSMutableArray");
    cls_arrM = ns_class("__NSArrayM");
    cls_dict = ns_class("NSDictionary");
    cls_mdict = ns_class("NSMutableDictionary");
    cls_dictM = ns_class("__NSDictionaryM");
    cls_set = ns_class("NSSet");
    cls_mset = ns_class("NSMutableSet");
    cls_setM = ns_class("__NSSetM");
    cls_num = ns_class("NSNumber");
    cls_cfnum = ns_class("__NSCFNumber");
    cls_cfbool = ns_class("__NSCFBoolean");
    cls_data = ns_class("NSData");
    cls_mdata = ns_class("NSMutableData");
    cls_cfdata = ns_class("__NSCFData");
    cls_enum = ns_class("__NSFastEnumerationEnumerator");
    cls_null = ns_class("NSNull");
    cls_val = ns_class("NSValue");
    cls_cval = ns_class("NSConcreteValue");

    if (!yes_obj) {
        make_bool(&yes_obj, 1);
        make_bool(&no_obj, 0);
        null_obj = ns_alloc(cls_null, 4);
    }

    static const NsMethod num[] = {
        {"alloc", n_alloc, 1}, {"allocWithZone:", n_alloc, 1},
        {"numberWithBool:", n_numberWith, 1}, {"numberWithChar:", n_numberWith, 1},
        {"numberWithUnsignedChar:", n_numberWith, 1}, {"numberWithShort:", n_numberWith, 1},
        {"numberWithUnsignedShort:", n_numberWith, 1}, {"numberWithInt:", n_numberWith, 1},
        {"numberWithUnsignedInt:", n_numberWith, 1}, {"numberWithLong:", n_numberWith, 1},
        {"numberWithUnsignedLong:", n_numberWith, 1}, {"numberWithLongLong:", n_numberWith, 1},
        {"numberWithUnsignedLongLong:", n_numberWith, 1}, {"numberWithFloat:", n_numberWith, 1},
        {"numberWithDouble:", n_numberWith, 1}, {"numberWithInteger:", n_numberWith, 1},
        {"numberWithUnsignedInteger:", n_numberWith, 1},
        {"initWithBool:", n_initWith, 0}, {"initWithChar:", n_initWith, 0}, {"initWithUnsignedChar:", n_initWith, 0},
        {"initWithShort:", n_initWith, 0}, {"initWithUnsignedShort:", n_initWith, 0}, {"initWithInt:", n_initWith, 0},
        {"initWithUnsignedInt:", n_initWith, 0}, {"initWithLong:", n_initWith, 0},
        {"initWithUnsignedLong:", n_initWith, 0}, {"initWithLongLong:", n_initWith, 0},
        {"initWithUnsignedLongLong:", n_initWith, 0}, {"initWithFloat:", n_initWith, 0},
        {"initWithDouble:", n_initWith, 0}, {"initWithInteger:", n_initWith, 0},
        {"initWithUnsignedInteger:", n_initWith, 0},
        {"boolValue", n_bool, 0}, {"charValue", n_char, 0}, {"unsignedCharValue", n_uchar, 0},
        {"shortValue", n_short, 0}, {"unsignedShortValue", n_ushort, 0}, {"intValue", n_int, 0},
        {"unsignedIntValue", n_int, 0}, {"longValue", n_int, 0}, {"unsignedLongValue", n_int, 0},
        {"integerValue", n_int, 0}, {"unsignedIntegerValue", n_int, 0}, {"longLongValue", n_ll, 0},
        {"unsignedLongLongValue", n_ll, 0}, {"floatValue", n_float, 0}, {"doubleValue", n_double, 0},
        {"stringValue", n_stringValue, 0}, {"description", n_stringValue, 0},
        {"descriptionWithLocale:", n_stringValue, 0}, {"compare:", n_compare, 0},
        {"isEqualToNumber:", n_isEqual, 0}, {"isEqual:", n_isEqual, 0}, {"hash", n_hash, 0},
        {"objCType", n_objCType, 0}, {"copyWithZone:", n_retain_self, 0}, {"copy", n_retain_self, 0},
        {NULL, NULL, 0}};
    ns_methods("NSNumber", num);
    static const NsMethod dec[] = {{"decimalNumberWithString:", n_decimalWithString, 1}, {NULL, NULL, 0}};
    ns_methods("NSDecimalNumber", dec);
    static const NsMethod immortal[] = {
        {"retain", n_immortal, 0}, {"release", n_immortal, 0}, {"autorelease", n_immortal, 0},
        {"dealloc", n_immortal, 0}, {"retainCount", n_rc, 0}, {NULL, NULL, 0}};
    ns_methods("__NSCFBoolean", immortal);
    ns_methods("NSNull", immortal);

    static const NsMethod val[] = {
        {"valueWithCGPoint:", v_withPoint, 1}, {"valueWithCGSize:", v_withSize, 1},
        {"valueWithCGRect:", v_withRect, 1}, {"valueWithCGAffineTransform:", v_withAffine, 1},
        {"valueWithPointer:", v_withPointer, 1}, {"valueWithNonretainedObject:", v_withPointer, 1},
        {"value:withObjCType:", v_withBytes, 1}, {"valueWithBytes:objCType:", v_withBytes, 1},
        {"getValue:", v_getValue, 0}, {"objCType", v_objCType, 0}, {"CGPointValue", v_point, 0},
        {"CGSizeValue", v_point, 0}, {"CGRectValue", v_stret_copy, 0},
        {"CGAffineTransformValue", v_stret_copy, 0}, {"pointerValue", v_pointer, 0},
        {"nonretainedObjectValue", v_pointer, 0}, {"isEqual:", v_isEqual, 0}, {"isEqualToValue:", v_isEqual, 0},
        {NULL, NULL, 0}};
    ns_methods("NSValue", val);
    static const NsMethod nul[] = {{"null", null_null, 1}, {"description", null_description, 0}, {NULL, NULL, 0}};
    ns_methods("NSNull", nul);

    static const NsMethod arr[] = {
        {"alloc", a_alloc, 1}, {"allocWithZone:", a_alloc, 1}, {"array", a_array, 1},
        {"arrayWithObject:", a_withObject, 1}, {"arrayWithObjects:", a_withObjects, 1},
        {"arrayWithObjects:count:", a_withObjectsCount, 1}, {"arrayWithArray:", a_withArray, 1},
        {"arrayWithCapacity:", a_array, 1}, {"arrayWithContentsOfFile:", a_withContentsOfFile, 1},
        {"init", a_init, 0}, {"initWithCapacity:", a_init, 0}, {"initWithObjects:", a_initWithObjects, 0},
        {"initWithObjects:count:", a_initWithObjectsCount, 0}, {"initWithArray:", a_initWithArray, 0},
        {"initWithArray:copyItems:", a_initWithArray, 0}, {"initWithContentsOfFile:", a_initWithContentsOfFile, 0},
        {"objectAtIndex:", a_objectAtIndex, 0}, {"lastObject", a_lastObject, 0}, {"firstObject", a_firstObject, 0},
        {"containsObject:", a_containsObject, 0}, {"indexOfObject:", a_indexOfObject, 0},
        {"indexOfObjectIdenticalTo:", a_indexOfObjectIdenticalTo, 0}, {"getObjects:", a_getObjects, 0},
        {"objectEnumerator", a_objectEnumerator, 0}, {"reverseObjectEnumerator", a_reverseObjectEnumerator, 0},
        {"componentsJoinedByString:", a_componentsJoined, 0}, {"isEqualToArray:", a_isEqualToArray, 0},
        {"isEqual:", a_isEqual, 0}, {"hash", a_hash, 0}, {"description", a_description, 0},
        {"copyWithZone:", a_copy, 0}, {"mutableCopyWithZone:", a_copy, 0},
        {"arrayByAddingObject:", a_byAdding, 0}, {"arrayByAddingObjectsFromArray:", a_byAdding, 0},
        {"subarrayWithRange:", a_subarray, 0}, {"sortedArrayUsingSelector:", a_sortedUsingSelector, 0},
        {"sortedArrayUsingFunction:context:", a_sortedUsingFunction, 0},
        {"makeObjectsPerformSelector:", a_makeObjectsPerform, 0},
        {"makeObjectsPerformSelector:withObject:", a_makeObjectsPerform, 0},
        {"writeToFile:atomically:", a_writeToFile, 0}, {NULL, NULL, 0}};
    ns_methods("NSArray", arr);
    static const NsMethod marr[] = {
        {"addObject:", ma_add, 0}, {"addObjectsFromArray:", ma_addFromArray, 0},
        {"insertObject:atIndex:", ma_insert, 0}, {"removeObjectAtIndex:", ma_removeAt, 0},
        {"removeLastObject", ma_removeLast, 0}, {"removeObject:", ma_removeObject, 0},
        {"removeObjectIdenticalTo:", ma_removeObject, 0}, {"removeAllObjects", ma_removeAll, 0},
        {"removeObjectsInArray:", ma_removeInArray, 0}, {"removeObjectsInRange:", ma_removeInRange, 0},
        {"replaceObjectAtIndex:withObject:", ma_replace, 0},
        {"exchangeObjectAtIndex:withObjectAtIndex:", ma_exchange, 0}, {"setArray:", ma_setArray, 0},
        {"sortUsingSelector:", ma_sortUsingSelector, 0}, {"sortUsingFunction:context:", ma_sortUsingFunction, 0},
        {NULL, NULL, 0}};
    ns_methods("NSMutableArray", marr);
    static const NsMethod arrM[] = {
        {"count", a_count, 0}, {"countByEnumeratingWithState:objects:count:", a_countByEnumerating, 0},
        {"dealloc", a_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("__NSArrayM", arrM);

    static const NsMethod en[] = {
        {"nextObject", e_nextObject, 0}, {"allObjects", e_allObjects, 0},
        {"countByEnumeratingWithState:objects:count:", e_countByEnumerating, 0}, {"dealloc", e_dealloc, 0},
        {NULL, NULL, 0}};
    ns_methods("__NSFastEnumerationEnumerator", en);

    static const NsMethod dict[] = {
        {"alloc", d_alloc, 1}, {"allocWithZone:", d_alloc, 1}, {"dictionary", d_dictionary, 1},
        {"dictionaryWithCapacity:", d_dictionary, 1}, {"dictionaryWithObject:forKey:", d_withObjectForKey, 1},
        {"dictionaryWithObjectsAndKeys:", d_withObjectsAndKeys, 1}, {"dictionaryWithDictionary:", d_withDictionary, 1},
        {"dictionaryWithObjects:forKeys:", d_withObjectsForKeys, 1},
        {"dictionaryWithObjects:forKeys:count:", d_withObjectsForKeysCount, 1},
        {"dictionaryWithContentsOfFile:", d_withContentsOfFile, 1},
        {"init", d_init, 0}, {"initWithCapacity:", d_init, 0}, {"initWithObjectsAndKeys:", d_initWithObjectsAndKeys, 0},
        {"initWithDictionary:", d_initWithDictionary, 0}, {"initWithDictionary:copyItems:", d_initWithDictionary, 0},
        {"initWithObjects:forKeys:", d_initWithObjectsForKeys, 0},
        {"initWithObjects:forKeys:count:", d_initWithObjectsForKeysCount, 0},
        {"initWithContentsOfFile:", d_initWithContentsOfFile, 0},
        {"objectsForKeys:notFoundMarker:", d_objectsForKeys, 0}, {"dictionaryWithValuesForKeys:", d_valuesForKeys, 0},
        {NULL, NULL, 0}};
    ns_methods("NSDictionary", dict);
    static const NsMethod dictM[] = {
        {"count", d_count, 0}, {"objectForKey:", d_objectForKey, 0}, {"valueForKey:", d_objectForKey, 0},
        {"allKeys", d_allKeys, 0}, {"allValues", d_allValues, 0}, {"keyEnumerator", d_keyEnumerator, 0},
        {"objectEnumerator", d_objectEnumerator, 0}, {"allKeysForObject:", d_allKeysForObject, 0},
        {"keysSortedByValueUsingSelector:", d_keysSortedByValue, 0},
        {"isEqualToDictionary:", d_isEqualToDictionary, 0}, {"isEqual:", d_isEqualToDictionary, 0},
        {"countByEnumeratingWithState:objects:count:", d_countByEnumerating, 0},
        {"copyWithZone:", d_copy, 0}, {"mutableCopyWithZone:", d_copy, 0}, {"description", d_description, 0},
        {"writeToFile:atomically:", d_writeToFile, 0}, {"dealloc", d_dealloc, 0},
        {"setObject:forKey:", md_setObjectForKey, 0}, {"setValue:forKey:", md_setValueForKey, 0},
        {"removeObjectForKey:", md_removeObjectForKey, 0}, {"removeAllObjects", md_removeAll, 0},
        {"addEntriesFromDictionary:", md_addEntries, 0}, {"removeObjectsForKeys:", md_removeForKeys, 0},
        {"setDictionary:", md_setDictionary, 0}, {NULL, NULL, 0}};
    ns_methods("__NSDictionaryM", dictM);

    static const NsMethod set[] = {
        {"alloc", st_alloc, 1}, {"allocWithZone:", st_alloc, 1}, {"set", st_set, 1},
        {"setWithCapacity:", st_set, 1}, {"setWithObject:", st_withObject, 1},
        {"setWithObjects:", st_withObjects, 1}, {"setWithArray:", st_withArray, 1}, {"setWithSet:", st_withSet, 1},
        {NULL, NULL, 0}};
    ns_methods("NSSet", set);
    static const NsMethod setM[] = {
        {"init", d_init, 0}, {"initWithCapacity:", d_init, 0}, {"initWithArray:", st_initWithArray, 0},
        {"initWithObjects:", st_initWithObjects, 0}, {"count", d_count, 0}, {"member:", st_member, 0},
        {"containsObject:", st_contains, 0}, {"anyObject", st_anyObject, 0}, {"allObjects", d_allKeys, 0},
        {"objectEnumerator", d_keyEnumerator, 0},
        {"countByEnumeratingWithState:objects:count:", d_countByEnumerating, 0},
        {"addObject:", st_add, 0}, {"removeObject:", st_remove, 0}, {"removeAllObjects", md_removeAll, 0},
        {"addObjectsFromArray:", st_addFromArray, 0}, {"unionSet:", st_union, 0}, {"minusSet:", st_minus, 0},
        {"intersectSet:", st_intersect, 0}, {"isEqualToSet:", st_isEqualToSet, 0}, {"copyWithZone:", st_copy, 0},
        {"mutableCopyWithZone:", st_copy, 0}, {"dealloc", d_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("__NSSetM", setM);

    static const NsMethod data[] = {
        {"alloc", dt_alloc, 1}, {"allocWithZone:", dt_alloc, 1}, {"data", dt_data, 1},
        {"dataWithBytes:length:", dt_withBytes, 1}, {"dataWithBytesNoCopy:length:", dt_withBytesNoCopy, 1},
        {"dataWithBytesNoCopy:length:freeWhenDone:", dt_withBytesNoCopy, 1},
        {"dataWithContentsOfFile:", dt_withContentsOfFile, 1},
        {"dataWithContentsOfFile:options:error:", dt_withContentsOfFile, 1},
        {"dataWithContentsOfURL:", dt_withContentsOfURL, 1}, {"dataWithData:", dt_withData, 1},
        {"dataWithCapacity:", dt_data, 1}, {"dataWithLength:", dt_withLength, 1},
        {"init", a_init, 0}, {"initWithBytes:length:", dt_initWithBytes, 0},
        {"initWithBytesNoCopy:length:", dt_initWithBytesNoCopy, 0},
        {"initWithBytesNoCopy:length:freeWhenDone:", dt_initWithBytesNoCopy, 0},
        {"initWithContentsOfFile:", dt_initWithContentsOfFile, 0},
        {"initWithContentsOfURL:", dt_initWithContentsOfURL, 0}, {"initWithData:", dt_initWithData, 0},
        {"initWithCapacity:", a_init, 0}, {"initWithLength:", dt_initWithLength, 0}, {NULL, NULL, 0}};
    ns_methods("NSData", data);
    static const NsMethod cfdata[] = {
        {"bytes", dt_bytes, 0}, {"mutableBytes", dt_bytes, 0}, {"length", dt_length, 0},
        {"getBytes:", dt_getBytes, 0}, {"getBytes:length:", dt_getBytes, 0}, {"getBytes:range:", dt_getBytes, 0},
        {"subdataWithRange:", dt_subdata, 0}, {"isEqual:", dt_isEqual, 0}, {"isEqualToData:", dt_isEqual, 0},
        {"writeToFile:atomically:", dt_writeToFile, 0}, {"writeToFile:options:error:", dt_writeToFile, 0},
        {"copyWithZone:", dt_copy, 0}, {"mutableCopyWithZone:", dt_copy, 0}, {"description", dt_description, 0},
        {"setLength:", dt_setLength, 0}, {"increaseLengthBy:", dt_increaseLength, 0},
        {"appendBytes:length:", dt_append, 0}, {"appendData:", dt_append, 0},
        {"replaceBytesInRange:withBytes:", dt_replaceBytes, 0}, {"resetBytesInRange:", dt_resetBytes, 0},
        {"setData:", dt_setData, 0}, {"base64EncodingWithLineLength:", dt_base64, 0},
        {"base64Encoding", dt_base64, 0}, {"dealloc", dt_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("__NSCFData", cfdata);
}
