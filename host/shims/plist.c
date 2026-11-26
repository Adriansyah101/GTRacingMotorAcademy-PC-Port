#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include "compat.h"
#endif

#include "ns.h"

typedef struct {
    const u8 *p;
    u32 n, offsz, refsz, nobj;
    u32 offtab;
    id *cache;
    int depth;
} BP;

static u64 be(const u8 *p, u32 sz)
{
    u64 v = 0;
    for (u32 i = 0; i < sz; i++)
        v = v << 8 | p[i];
    return v;
}

static id bp_obj(BP *b, u32 ref);

static int bp_count(BP *b, u32 *pos, u32 low, u32 *count)
{
    if (low != 0xF) {
        *count = low;
        return 1;
    }
    if (*pos >= b->n || (b->p[*pos] & 0xF0) != 0x10)
        return 0;
    u32 sz = 1u << (b->p[*pos] & 0xF);
    if (*pos + 1 + sz > b->n)
        return 0;
    *count = (u32)be(b->p + *pos + 1, sz);
    *pos += 1 + sz;
    return 1;
}

static id bp_parse_at(BP *b, u32 pos)
{
    if (pos >= b->n)
        return 0;
    u8 m = b->p[pos++];
    u32 hi = m >> 4, low = m & 0xF, n;
    switch (hi) {
    case 0x0:
        if (low == 8 || low == 9)
            return objc_retain(ns_num_bool(low == 9));
        return objc_retain(ns_null());
    case 0x1: {
        u32 sz = 1u << low;
        if (pos + sz > b->n)
            return 0;
        u64 v = be(b->p + pos, sz > 8 ? 8 : sz);
        if (sz == 16)
            v = be(b->p + pos + 8, 8);
        int64_t s = sz == 1 ? (int64_t)(u8)v : sz == 2 ? (int64_t)(u16)v : sz == 4 ? (int64_t)(u32)v : (int64_t)v;
        return objc_retain(ns_num_ll(s));
    }
    case 0x2: {
        u32 sz = 1u << low;
        if (pos + sz > b->n)
            return 0;
        u64 v = be(b->p + pos, sz);
        double d = sz == 4 ? (double)bits2f((u32)v) : bits2d(v);
        return objc_retain(ns_num_d(d));
    }
    case 0x3:
        if (pos + 8 > b->n)
            return 0;
        return objc_retain(ns_date(bits2d(be(b->p + pos, 8))));
    case 0x4:
        if (!bp_count(b, &pos, low, &n) || pos + n > b->n)
            return 0;
        return ns_data_new(b->p + pos, n);
    case 0x5: {
        if (!bp_count(b, &pos, low, &n) || pos + n > b->n)
            return 0;
        Buf t = {0};
        for (u32 i = 0; i < n; i++) {
            u8 ch = b->p[pos + i];
            if (ch < 0x80) {
                buf_put(&t, &ch, 1);
            } else {
                char o[2] = {(char)(0xC0 | ch >> 6), (char)(0x80 | (ch & 0x3F))};
                buf_put(&t, o, 2);
            }
        }
        id s = ns_string_new(t.p ? t.p : "", t.n);
        buf_free(&t);
        return s;
    }
    case 0x6: {
        if (!bp_count(b, &pos, low, &n) || pos + 2 * n > b->n)
            return 0;
        u16 *w = malloc(2 * (size_t)n + 2);
        for (u32 i = 0; i < n; i++)
            w[i] = (u16)be(b->p + pos + 2 * i, 2);
        id s = msg2(msg0(ns_class("NSString"), "alloc"), "initWithCharacters:length:", GA(w), n);
        free(w);
        return s;
    }
    case 0x8:
        return objc_retain(ns_num_ll((int64_t)be(b->p + pos, low + 1)));
    case 0xA:
    case 0xC: {
        if (!bp_count(b, &pos, low, &n) || pos + n * b->refsz > b->n)
            return 0;
        id a = ns_array_new(NULL, 0);
        for (u32 i = 0; i < n; i++) {
            id o = bp_obj(b, (u32)be(b->p + pos + i * b->refsz, b->refsz));
            if (o)
                ns_array_add(a, o);
        }
        return a;
    }
    case 0xD: {
        if (!bp_count(b, &pos, low, &n) || pos + 2 * n * b->refsz > b->n)
            return 0;
        id d = ns_dict_new();
        for (u32 i = 0; i < n; i++) {
            id k = bp_obj(b, (u32)be(b->p + pos + i * b->refsz, b->refsz));
            id v = bp_obj(b, (u32)be(b->p + pos + (n + i) * b->refsz, b->refsz));
            if (k && v)
                ns_dict_set(d, k, v);
        }
        return d;
    }
    default:
        return 0;
    }
}

static id bp_obj(BP *b, u32 ref)
{
    if (ref >= b->nobj || b->depth > 64)
        return 0;
    if (b->cache[ref])
        return b->cache[ref];
    u32 off = (u32)be(b->p + b->offtab + ref * b->offsz, b->offsz);
    b->depth++;
    id o = bp_parse_at(b, off);
    b->depth--;
    b->cache[ref] = o;
    return o;
}

static id parse_binary(const u8 *p, u32 n)
{
    if (n < 40)
        return 0;
    const u8 *t = p + n - 32;
    BP b = {p, n, t[6], t[7], (u32)be(t + 8, 8), (u32)be(t + 24, 8), NULL, 0};
    u32 top = (u32)be(t + 16, 8);
    if (!b.offsz || !b.refsz || b.offtab + (u64)b.nobj * b.offsz > n)
        return 0;
    b.cache = calloc(b.nobj + 1, 4);
    id root = bp_obj(&b, top);
    objc_retain(root);
    for (u32 i = 0; i < b.nobj; i++)
        if (b.cache[i])
            objc_release(b.cache[i]);
    free(b.cache);
    return root;
}

typedef struct { const char *p, *e; } XP;

static void xp_skip(XP *x)
{
    for (;;) {
        while (x->p < x->e && isspace((u8)*x->p))
            x->p++;
        if (x->e - x->p >= 4 && !memcmp(x->p, "<!--", 4)) {
            const char *q = strstr(x->p, "-->");
            x->p = q ? q + 3 : x->e;
        } else if (x->e - x->p >= 2 && (!memcmp(x->p, "<?", 2) || !memcmp(x->p, "<!", 2))) {
            const char *q = memchr(x->p, '>', (size_t)(x->e - x->p));
            x->p = q ? q + 1 : x->e;
        } else {
            return;
        }
    }
}

static int xp_tag(XP *x, char *tag, size_t n)
{
    xp_skip(x);
    if (x->p >= x->e || *x->p != '<')
        return 0;
    x->p++;
    int kind = 1;
    if (*x->p == '/') {
        kind = 2;
        x->p++;
    }
    size_t k = 0;
    while (x->p < x->e && !isspace((u8)*x->p) && *x->p != '>' && *x->p != '/') {
        if (k + 1 < n)
            tag[k++] = *x->p;
        x->p++;
    }
    tag[k] = 0;
    while (x->p < x->e && *x->p != '>') {
        if (*x->p == '/')
            kind = 3;
        x->p++;
    }
    if (x->p < x->e)
        x->p++;
    return kind;
}

static void xp_text(XP *x, Buf *b)
{
    while (x->p < x->e && *x->p != '<') {
        if (*x->p == '&') {
            const char *q = memchr(x->p, ';', (size_t)(x->e - x->p));
            if (q) {
                size_t len = (size_t)(q - x->p - 1);
                const char *ent = x->p + 1;
                char out[4];
                u32 n = 0;
                if (len == 2 && !memcmp(ent, "lt", 2)) out[n++] = '<';
                else if (len == 2 && !memcmp(ent, "gt", 2)) out[n++] = '>';
                else if (len == 3 && !memcmp(ent, "amp", 3)) out[n++] = '&';
                else if (len == 4 && !memcmp(ent, "quot", 4)) out[n++] = '"';
                else if (len == 4 && !memcmp(ent, "apos", 4)) out[n++] = '\'';
                else if (len > 1 && ent[0] == '#') {
                    u32 cp = ent[1] == 'x' ? (u32)strtoul(ent + 2, NULL, 16) : (u32)strtoul(ent + 1, NULL, 10);
                    if (cp < 0x80) out[n++] = (char)cp;
                    else if (cp < 0x800) { out[n++] = (char)(0xC0 | cp >> 6); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                    else { out[n++] = (char)(0xE0 | cp >> 12); out[n++] = (char)(0x80 | (cp >> 6 & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                }
                if (n) {
                    buf_put(b, out, n);
                    x->p = q + 1;
                    continue;
                }
            }
        }
        buf_put(b, x->p, 1);
        x->p++;
    }
}

static int b64v(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static id xp_value(XP *x, const char *tag, int kind);

static id xp_next(XP *x)
{
    char tag[32];
    int kind = xp_tag(x, tag, sizeof tag);
    if (kind != 1 && kind != 3)
        return 0;
    return xp_value(x, tag, kind);
}

static int xp_close(XP *x)
{
    char tag[32];
    return xp_tag(x, tag, sizeof tag) == 2;
}

static double iso_date(const char *s)
{
    struct tm tm = {0};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (double)_mkgmtime(&tm) - 978307200.0;
}

static id xp_value(XP *x, const char *tag, int kind)
{
    if (!strcmp(tag, "true") || !strcmp(tag, "false")) {
        if (kind == 1)
            xp_close(x);
        return objc_retain(ns_num_bool(tag[0] == 't'));
    }
    if (!strcmp(tag, "dict")) {
        id d = ns_dict_new();
        if (kind == 3)
            return d;
        for (;;) {
            char t[32];
            int k = xp_tag(x, t, sizeof t);
            if (k == 2 || k == 0)
                break;
            if (strcmp(t, "key"))
                break;
            Buf kb = {0};
            if (k == 1) {
                xp_text(x, &kb);
                xp_close(x);
            }
            id key = ns_string_new(kb.p ? kb.p : "", kb.n);
            buf_free(&kb);
            id v = xp_next(x);
            if (v) {
                ns_dict_set(d, key, v);
                objc_release(v);
            }
            objc_release(key);
        }
        return d;
    }
    if (!strcmp(tag, "array")) {
        id a = ns_array_new(NULL, 0);
        if (kind == 3)
            return a;
        for (;;) {
            const char *save = x->p;
            char t[32];
            int k = xp_tag(x, t, sizeof t);
            if (k == 2 || k == 0)
                break;
            (void)save;
            id v = xp_value(x, t, k);
            if (v) {
                ns_array_add(a, v);
                objc_release(v);
            }
        }
        return a;
    }
    Buf b = {0};
    if (kind == 1) {
        xp_text(x, &b);
        xp_close(x);
    }
    const char *s = b.p ? b.p : "";
    id o = 0;
    if (!strcmp(tag, "string") || !strcmp(tag, "key")) {
        o = ns_string_new(s, b.n);
    } else if (!strcmp(tag, "integer")) {
        o = objc_retain(ns_num_ll(strtoll(s, NULL, 0)));
    } else if (!strcmp(tag, "real")) {
        o = objc_retain(ns_num_d(strtod(s, NULL)));
    } else if (!strcmp(tag, "date")) {
        o = objc_retain(ns_date(iso_date(s)));
    } else if (!strcmp(tag, "data")) {
        Buf d = {0};
        u32 acc = 0, bits = 0;
        for (u32 i = 0; i < b.n; i++) {
            int v = b64v((u8)s[i]);
            if (v < 0)
                continue;
            acc = acc << 6 | (u32)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                u8 ch = (u8)(acc >> bits);
                buf_put(&d, &ch, 1);
            }
        }
        o = ns_data_new(d.p ? d.p : "", d.n);
        buf_free(&d);
    }
    buf_free(&b);
    return o;
}

static id parse_xml(const u8 *p, u32 n)
{
    XP x = {(const char *)p, (const char *)p + n};
    char tag[32];
    int kind = xp_tag(&x, tag, sizeof tag);
    if (kind == 1 && !strcmp(tag, "plist"))
        return xp_next(&x);
    if (kind == 1 || kind == 3)
        return xp_value(&x, tag, kind);
    return 0;
}

id plist_parse(const u8 *p, u32 n)
{
    if (n >= 8 && !memcmp(p, "bplist00", 8))
        return parse_binary(p, n);
    return parse_xml(p, n);
}

id plist_read_file(const char *path)
{
    u32 n;
    u8 *p = gt_read_file(path, &n);
    if (!p)
        return 0;
    id o = plist_parse(p, n);
    free(p);
    if (!o)
        rt_log("[plist] %s: not a property list\n", path);
    return o;
}

static void xml_text(Buf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '<') buf_puts(b, "&lt;");
        else if (*s == '>') buf_puts(b, "&gt;");
        else if (*s == '&') buf_puts(b, "&amp;");
        else buf_put(b, s, 1);
    }
}

static void tabs(Buf *b, int n)
{
    for (int i = 0; i < n; i++)
        buf_puts(b, "\t");
}

static void xml_obj(Buf *b, id o, int ind)
{
    tabs(b, ind);
    if (ns_is_string(o) || objc_is_kind(o, ns_class("NSString"))) {
        buf_puts(b, "<string>");
        xml_text(b, ns_utf8(o));
        buf_puts(b, "</string>\n");
    } else if (ns_is_number(o)) {
        const char *t = GP(msg0(o, "objCType"));
        if (rd32(o) == ns_class("__NSCFBoolean"))
            buf_puts(b, ns_num_int(o) ? "<true/>\n" : "<false/>\n");
        else if (t[0] == 'f' || t[0] == 'd')
            buf_printf(b, "<real>%.17g</real>\n", ns_num_double(o));
        else
            buf_printf(b, "<integer>%lld</integer>\n", (long long)ns_num_int(o));
    } else if (ns_is_data(o)) {
        static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        u32 n;
        const u8 *p = ns_data_bytes(o, &n);
        buf_puts(b, "<data>");
        for (u32 i = 0; i < n; i += 3) {
            u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
            char q[4] = {tab[v >> 18 & 63], tab[v >> 12 & 63], i + 1 < n ? tab[v >> 6 & 63] : '=', i + 2 < n ? tab[v & 63] : '='};
            buf_put(b, q, 4);
        }
        buf_puts(b, "</data>\n");
    } else if (ns_is_date(o)) {
        time_t t = (time_t)(ns_date_value(o) + 978307200.0);
        struct tm *g = gmtime(&t);
        char s[32] = "1970-01-01T00:00:00Z";
        if (g)
            strftime(s, sizeof s, "%Y-%m-%dT%H:%M:%SZ", g);
        buf_printf(b, "<date>%s</date>\n", s);
    } else if (ns_is_array(o) || objc_is_kind(o, ns_class("NSArray"))) {
        u32 n = ns_array_count(o);
        if (!n) {
            buf_puts(b, "<array/>\n");
            return;
        }
        buf_puts(b, "<array>\n");
        for (u32 i = 0; i < n; i++)
            xml_obj(b, ns_array_at(o, i), ind + 1);
        tabs(b, ind);
        buf_puts(b, "</array>\n");
    } else if (ns_is_dict(o)) {
        u32 n = ns_dict_count(o);
        if (!n) {
            buf_puts(b, "<dict/>\n");
            return;
        }
        buf_puts(b, "<dict>\n");
        for (u32 i = 0; i < n; i++) {
            tabs(b, ind + 1);
            buf_puts(b, "<key>");
            xml_text(b, ns_utf8(ns_dict_key_at(o, i)));
            buf_puts(b, "</key>\n");
            xml_obj(b, ns_dict_val_at(o, i), ind + 1);
        }
        tabs(b, ind);
        buf_puts(b, "</dict>\n");
    } else {
        rt_log("[plist] %s cannot go in a property list\n", ns_describe(o));
        buf_puts(b, "<string></string>\n");
    }
}

static void plist_xml(Buf *b, id o)
{
    buf_puts(b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n");
    xml_obj(b, o, 0);
    buf_puts(b, "</plist>\n");
}

int plist_write_file(const char *path, id o)
{
    Buf b = {0};
    plist_xml(&b, o);
    int ok = gt_write_file(path, b.p, b.n);
    buf_free(&b);
    return ok;
}
