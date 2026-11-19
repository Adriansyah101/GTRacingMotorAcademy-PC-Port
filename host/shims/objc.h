/* objc.h -- the Objective-C 2 runtime of the host (objc.c).

   Classes, metaclasses, selectors and objects are the game's own structures in guest memory
   (class_t / class_ro_t / method lists as clang emits them for armv7) or host-made ones laid
   out the same way, so the game can read isa, superclass and ivars directly.  The runtime keeps
   its own index (ObjcClass) beside them.  A method's IMP is a guest code address or, for
   methods the host implements, an address in the host-function range (rt_host_addr). */
#ifndef GT_OBJC_H
#define GT_OBJC_H

#include <stdio.h>

#include "rt.h"

typedef u32 id;        /* guest object address */
typedef u32 SEL;       /* guest address of the selector's canonical name */

typedef struct ObjcClass ObjcClass;

/* Selectors */
SEL sel_get(const char *name);             /* registers on first use */
const char *sel_name(SEL s);

/* Classes */
u32 objc_class_addr(ObjcClass *k);
u32 objc_getClass_addr(const char *name);  /* class_t address or 0 */
const char *objc_class_name(u32 cls);
int objc_is_kind(id obj, u32 cls);
u32 objc_instance_size(u32 cls);

/* A class the host provides: made on first use (its superclass from the host's table). */
ObjcClass *objc_host_class(const char *name);
/* Host state inside a host class's objects: at least `bytes` (before gt_objc_image). */
void objc_host_size(const char *name, u32 bytes);
/* Host methods, written as raw bridges: self in r0, _cmd in r1, arguments in r2... */
typedef void (*ObjcHostIMP)(CPU *c);
void objc_add_method(const char *cls, const char *sel, ObjcHostIMP fn, int class_method);

/* Messaging from host code (up to 4 argument words after self and _cmd). */
u32 objc_lookup(u32 cls, SEL sel);         /* IMP or 0 */
u32 objc_msg(id self, const char *sel, int n, const u32 *args);
u32 msg0(id self, const char *sel);
u32 msg1(id self, const char *sel, u32 a);
u32 msg2(id self, const char *sel, u32 a, u32 b);
int objc_responds(id self, const char *sel);

/* Objects */
id objc_alloc_instance(u32 cls, u32 extra);
void objc_free_instance(id obj);
id objc_retain(id obj);
void objc_release(id obj);
id objc_autorelease(id obj);
u32 objc_pool_push(void);
void objc_pool_pop(u32 mark);

/* Exception type info (OBJC_EHTYPE_$_...) for the unwinder: the class it matches, 0 = id. */
int objc_ehtype_matches(u32 ehtype, id exception);

void gt_objc_dump(FILE *f);                /* --objc-dump: every class and method */
/* blocks (blocks.c) */
void gt_blocks_init(void);
u32 gt_block_copy(u32 b);                  /* _Block_copy */

#endif
