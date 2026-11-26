#ifndef GT_NS_INTERNAL_H
#define GT_NS_INTERNAL_H

#include "ns.h"

extern u32 ns_main_thread_id;
id ns_singleton(id *slot, const char *cls, u32 bytes);

#define SINCE_1970 978307200.0

static inline void ret_self(CPU *c) { (void)c; }
static inline void ret_zero(CPU *c) { ret_i(c, 0); }
static inline void ret_yes(CPU *c) { ret_i(c, 1); }

#define W(o, i) (((u32 *)GP(o))[i])
static inline void set_obj(id o, int i, id v)
{
    id old = W(o, i);
    W(o, i) = objc_retain(v);
    objc_release(old);
}
static inline void set_copy(id o, int i, id v)
{
    id old = W(o, i);
    W(o, i) = v ? msg1(v, "copyWithZone:", 0) : 0;
    objc_release(old);
}

#endif
