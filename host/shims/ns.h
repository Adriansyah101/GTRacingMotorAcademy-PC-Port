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

typedef struct { char *p; u32 n, cap; } Buf;
void buf_put(Buf *b, const void *s, u32 n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(Buf *b);

u32 ns_class(const char *name);
id ns_alloc(u32 cls, u32 bytes);
typedef struct { const char *sel; ObjcHostIMP fn; int cls; } NsMethod;
void ns_methods(const char *cls, const NsMethod *m);
id ns_cluster_alloc(u32 self, const char *const *names, u32 concrete, u32 bytes);

int ns_is_string(id o);
id ns_string_new(const char *utf8, u32 bytes);
id ns_string(const char *utf8);
id ns_stringf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *ns_utf8(id s);
u32 ns_utf8_len(id s);
int ns_str_eq(id a, id b);
u32 ns_hash_bytes(const void *p, u32 n);
void ns_format(Buf *out, const char *fmt, const u32 *words);
const char *ns_describe(id o);
void ns_fix_cfstrings(void);

int ns_equal(id a, id b);
u32 ns_hash(id o);
id ns_array_new(const u32 *objs, u32 n);
void ns_array_add(id a, id o);
u32 ns_array_count(id a);
id ns_array_at(id a, u32 i);
id ns_set_new(const u32 *objs, u32 n);
id ns_dict_new(void);
id ns_dict_get(id d, id key);
void ns_dict_set(id d, id key, id val);
u32 ns_dict_count(id d);
id ns_dict_key_at(id d, u32 i);
id ns_dict_val_at(id d, u32 i);
id ns_num_ll(int64_t v);
id ns_num_d(double v);
id ns_num_bool(int v);
int ns_is_number(id o);
double ns_num_double(id n);
int64_t ns_num_int(id n);
id ns_data_new(const void *p, u32 n);
int ns_is_data(id o);
const u8 *ns_data_bytes(id d, u32 *len);
id ns_null(void);
int ns_is_array(id o);
int ns_is_dict(id o);

id plist_parse(const u8 *p, u32 n);
id plist_read_file(const char *guest_path);
int plist_write_file(const char *guest_path, id obj);

extern const char *gt_guest_home, *gt_guest_bundle;
const char *gt_host_path(const char *guest, char *buf, size_t n);
u8 *gt_read_file(const char *guest_path, u32 *len);
int gt_write_file(const char *guest_path, const void *p, u32 n);
void gt_paths_init(const char *host_bundle, const char *exe_dir);

double ns_now(void);
void ns_wait(double seconds);
int ns_precise_wait_on(void);
id ns_date(double since_2001);
double ns_date_value(id d);
int ns_is_date(id o);
double ns_run_loop_once(void);
id ns_error(const char *domain, int code);
void ns_post_notification(const char *name, id object);
void ns_perform_main(id target, const char *sel, id arg);
void ns_perform_here(id target, const char *sel, id arg, double delay);
extern u32 ns_cf_callbacks[3][8];

void ns_init(void);

#endif
