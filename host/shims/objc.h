#ifndef GT_OBJC_H
#define GT_OBJC_H

#include <stdio.h>

#include "rt.h"

typedef u32 id;
typedef u32 SEL;

typedef struct ObjcClass ObjcClass;

SEL sel_get(const char *name);
const char *sel_name(SEL s);

u32 objc_class_addr(ObjcClass *k);
u32 objc_getClass_addr(const char *name);
const char *objc_class_name(u32 cls);
int objc_is_kind(id obj, u32 cls);
u32 objc_instance_size(u32 cls);

ObjcClass *objc_host_class(const char *name);
void objc_host_size(const char *name, u32 bytes);
typedef void (*ObjcHostIMP)(CPU *c);
void objc_add_method(const char *cls, const char *sel, ObjcHostIMP fn, int class_method);

u32 objc_lookup(u32 cls, SEL sel);
u32 objc_msg(id self, const char *sel, int n, const u32 *args);
u32 msg0(id self, const char *sel);
u32 msg1(id self, const char *sel, u32 a);
u32 msg2(id self, const char *sel, u32 a, u32 b);
int objc_responds(id self, const char *sel);

id objc_alloc_instance(u32 cls, u32 extra);
void objc_free_instance(id obj);
id objc_retain(id obj);
void objc_release(id obj);
id objc_autorelease(id obj);
u32 objc_pool_push(void);
void objc_pool_pop(u32 mark);

int objc_ehtype_matches(u32 ehtype, id exception);

void gt_objc_dump(FILE *f);
void gt_blocks_init(void);
u32 gt_block_copy(u32 b);

#endif
