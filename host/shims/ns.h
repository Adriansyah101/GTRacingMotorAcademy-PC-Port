/* ns.h -- the host's Foundation (ns_string.c, ns_coll.c, ns_misc.c, plist.c).

   Objects the host makes have small fixed layouts the host reads directly; objects of game
   subclasses of the class clusters (NSString, NSDictionary...) are reached through their
   primitive methods instead.  Methods are raw bridges (objc.h): self in r0, _cmd in r1, then
   the arguments; a method returning a struct bigger than 4 bytes is reached through
   objc_msgSend_stret, so its result pointer is r0, self r1 and its arguments start at word 3. */
#ifndef GT_NS_H
#define GT_NS_H

#include <stdint.h>

#include "abi.h"
#include "objc.h"

#define SELF c->r[0]
#define ARG(i) arg_word(c, 2 + (i))
#define ARGF(i) arg_f(c, 2 + (i))
#define ARGD(i) arg_d(c, 2 + (i))
#define ST_RET c->r[0]
#define ST_SELF c->r[1]
#define ST_ARG(i) arg_word(c, 3 + (i))

typedef struct { u32 loc, len; } NSRange;
#define NS_NOT_FOUND 0x7fffffffu

/* growing byte buffer */
typedef struct { char *p; u32 n, cap; } Buf;
void buf_put(Buf *b, const void *s, u32 n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(Buf *b);

/* classes */
u32 ns_class(const char *name);            /* host class_t address (made on first use) */
id ns_alloc(u32 cls, u32 bytes);           /* zeroed object of at least `bytes` */
/* Registers a table of host methods. */
typedef struct { const char *sel; ObjcHostIMP fn; int cls; } NsMethod;
void ns_methods(const char *cls, const NsMethod *m);
/* +alloc for a class cluster: the concrete host class when the receiver is one of `names`
   (NULL-terminated), else a plain instance of the receiver (a game subclass). */
id ns_cluster_alloc(u32 self, const char *const *names, u32 concrete, u32 bytes);

/* strings (ns_string.c) */
/* Host strings: {isa, flags, char *utf8 (NUL-terminated), bytes, capacity}; the game's constant
   strings have the same first four words (UTF-16 ones are converted at load). */
int ns_is_string(id o);
id ns_string_new(const char *utf8, u32 bytes);     /* +1 */
id ns_string(const char *utf8);                    /* autoreleased */
id ns_stringf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* autoreleased */
const char *ns_utf8(id s);                         /* any NSString; "" for nil */
u32 ns_utf8_len(id s);
int ns_str_eq(id a, id b);
u32 ns_hash_bytes(const void *p, u32 n);
/* NSString format with %@: words are the variadic argument words (APCS, i386 va_list layout) */
void ns_format(Buf *out, const char *fmt, const u32 *words);
const char *ns_describe(id o);                     /* -description as UTF-8 (autoreleased) */
void ns_fix_cfstrings(void);                       /* the image's UTF-16 constant strings -> UTF-8 */

/* collections, numbers, data (ns_coll.c) */
int ns_equal(id a, id b);
u32 ns_hash(id o);
id ns_array_new(const u32 *objs, u32 n);           /* +1, mutable, retains the objects */
void ns_array_add(id a, id o);
u32 ns_array_count(id a);
id ns_array_at(id a, u32 i);
id ns_set_new(const u32 *objs, u32 n);             /* autoreleased, mutable */
id ns_dict_new(void);                              /* +1, mutable */
id ns_dict_get(id d, id key);
void ns_dict_set(id d, id key, id val);
u32 ns_dict_count(id d);
id ns_dict_key_at(id d, u32 i);                    /* host dictionaries: entry i */
id ns_dict_val_at(id d, u32 i);
id ns_num_ll(int64_t v);                           /* autoreleased */
id ns_num_d(double v);
id ns_num_bool(int v);                             /* the shared YES / NO objects */
int ns_is_number(id o);
double ns_num_double(id n);
int64_t ns_num_int(id n);
id ns_data_new(const void *p, u32 n);              /* +1, copies */
int ns_is_data(id o);
const u8 *ns_data_bytes(id d, u32 *len);
id ns_null(void);
int ns_is_array(id o);
int ns_is_dict(id o);

/* property lists (plist.c) */
id plist_parse(const u8 *p, u32 n);                /* XML or binary; +1, mutable containers; 0 on error */
id plist_read_file(const char *guest_path);        /* +1 or 0 */
int plist_write_file(const char *guest_path, id obj);

/* files and paths (ns_misc.c) */
extern const char *gt_guest_home, *gt_guest_bundle;
const char *gt_host_path(const char *guest, char *buf, size_t n);
u8 *gt_read_file(const char *guest_path, u32 *len);   /* malloc'd + NUL, or NULL */
int gt_write_file(const char *guest_path, const void *p, u32 n);
void gt_paths_init(const char *host_bundle, const char *exe_dir);

/* dates, run loop, misc (ns_misc.c) */
double ns_now(void);                               /* seconds since 2001-01-01 (NSDate's reference) */
void ns_wait(double seconds);                      /* PRECISE_WAIT: high-resolution timer sleep (or Sleep) */
int ns_precise_wait_on(void);
id ns_date(double since_2001);                     /* autoreleased */
double ns_date_value(id d);
int ns_is_date(id o);
double ns_run_loop_once(void);                     /* runs due timers/performs of this thread */
id ns_error(const char *domain, int code);         /* autoreleased */
void ns_post_notification(const char *name, id object);
void ns_perform_main(id target, const char *sel, id arg);   /* later, on the main run loop */
void ns_perform_here(id target, const char *sel, id arg, double delay);   /* this thread's run loop */
extern u32 ns_cf_callbacks[3][8];                  /* kCFType{Array,DictionaryKey,DictionaryValue}CallBacks */

void ns_init(void);                                /* all of the above */

#endif
