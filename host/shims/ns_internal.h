/* ns_internal.h -- what the Foundation shims' files (ns_misc.c, ns_runloop.c) share. */
#ifndef GT_NS_INTERNAL_H
#define GT_NS_INTERNAL_H

#include "ns.h"

extern u32 ns_main_thread_id;            /* the thread UIApplicationMain runs on (ns_misc.c, at start) */
id ns_singleton(id *slot, const char *cls, u32 bytes); /* a shared instance (ns_misc.c) */

#define SINCE_1970 978307200.0          /* seconds from 1970 to 2001-01-01, NSDate's reference date */


/* methods that only answer */
static inline void ret_self(CPU *c) { (void)c; }
static inline void ret_zero(CPU *c) { ret_i(c, 0); }
static inline void ret_yes(CPU *c) { ret_i(c, 1); }

/* an object's word i, and an object field set with the retain (set_obj) or copy (set_copy) the property asks */
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
