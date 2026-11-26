#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include "compat.h"
#endif

#include "abi.h"
#include "ns.h"

#define IOS 16
#define IOS_SIZE 136
#define I_PREC 4
#define I_WIDTH 8
#define I_FLAGS 12
#define I_STATE 20
#define I_FILL 116

enum { F_BOOLALPHA = 1, F_DEC = 2, F_FIXED = 4, F_HEX = 8, F_INTERNAL = 16, F_LEFT = 32, F_OCT = 64, F_RIGHT = 128,
       F_SCIENTIFIC = 256, F_SHOWBASE = 512, F_SHOWPOINT = 1024, F_SHOWPOS = 2048, F_SKIPWS = 4096,
       F_UPPERCASE = 16384 };
enum { S_BAD = 1, S_EOF = 2, S_FAIL = 4 };
enum { M_APP = 1, M_ATE = 2, M_IN = 8, M_OUT = 16, M_TRUNC = 32 };

typedef struct {
    FILE *f;
    char *buf;
    u32 len, cap, gpos, ppos;
    u32 mode;
    int is_file;
} Stream;

void gt_string_make(u32 out, const char *s, u32 n);
const char *gt_string_cstr(u32 str);
u32 h_ZNSs6assignEPKcm(u32 self, u32 s, u32 n);

static void virtual_call(CPU *c)
{
    rt_fatal("a virtual function of a host iostream was called (return address 0x%08x)", c->r[14]);
}
static u32 vt_primary[3 + 16], vt_second[3 + 16], vt_ios[3 + 16];
static void vtables(void)
{
    if (vt_primary[3])
        return;
    u32 fn = rt_host_addr(virtual_call, "iostream virtual");
    for (int i = 3; i < 19; i++)
        vt_primary[i] = vt_second[i] = vt_ios[i] = fn;
    vt_primary[0] = IOS, vt_primary[1] = 0;
    vt_second[0] = IOS - 8, vt_second[1] = (u32)-8;
}

static u32 top_of(u32 p)
{
    u32 vp = rd32(p);
    return vp == GA(&vt_second[3]) ? p - 8 : p;
}
static Stream *st_of(u32 p) { return GP(rd32(top_of(p) + 12)); }
static u32 ios_of(u32 p) { return top_of(p) + IOS; }
static u32 state(u32 p) { return rd32(ios_of(p) + I_STATE); }
static void set_state(u32 p, u32 bits) { wr32(ios_of(p) + I_STATE, state(p) | bits); }

static void construct(u32 obj, u32 mode, int is_file)
{
    vtables();
    memset(GP(obj), 0, IOS + IOS_SIZE);
    wr32(obj, GA(&vt_primary[3]));
    wr32(obj + 8, GA(&vt_second[3]));
    Stream *s = calloc(1, sizeof *s);
    s->mode = mode;
    s->is_file = is_file;
    wr32(obj + 12, GA(s));
    u32 ios = obj + IOS;
    wr32(ios, GA(&vt_ios[3]));
    wr32(ios + I_PREC, 6);
    wr32(ios + I_FLAGS, F_SKIPWS | F_DEC);
    ((u8 *)GP(ios))[I_FILL] = ' ';
    ((u8 *)GP(ios))[I_FILL + 1] = 1;
}
static void destruct(u32 obj)
{
    Stream *s = st_of(obj);
    if (!s)
        return;
    if (s->f)
        fclose(s->f);
    free(s->buf);
    free(s);
    wr32(top_of(obj) + 12, 0);
}

static void put(u32 p, const char *t, u32 n)
{
    Stream *s = st_of(p);
    if (!s)
        return;
    if (s->is_file) {
        if (!s->f || fwrite(t, 1, n, s->f) != n)
            set_state(p, S_BAD);
        return;
    }
    if (s->mode & M_APP)
        s->ppos = s->len;
    u32 end = s->ppos + n;
    if (end + 1 > s->cap) {
        s->cap = (end + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->ppos, t, n);
    s->ppos = end;
    if (end > s->len)
        s->len = end;
    s->buf[s->len] = 0;
}
static int getch(u32 p)
{
    Stream *s = st_of(p);
    if (!s)
        return -1;
    if (s->is_file)
        return s->f ? fgetc(s->f) : -1;
    return s->gpos < s->len ? (u8)s->buf[s->gpos++] : -1;
}
static void ungetch(u32 p, int ch)
{
    Stream *s = st_of(p);
    if (ch < 0 || !s)
        return;
    if (s->is_file)
        ungetc(ch, s->f);
    else if (s->gpos)
        s->gpos--;
}

static void put_field(u32 p, const char *t, u32 n)
{
    u32 ios = ios_of(p), w = rd32(ios + I_WIDTH), fl = rd32(ios + I_FLAGS);
    char fill = ((char *)GP(ios))[I_FILL];
    wr32(ios + I_WIDTH, 0);
    if (w > n && !(fl & F_LEFT))
        for (u32 i = n; i < w; i++)
            put(p, &fill, 1);
    put(p, t, n);
    if (w > n && (fl & F_LEFT))
        for (u32 i = n; i < w; i++)
            put(p, &fill, 1);
}
static void put_int(u32 p, int64_t v, int is_signed)
{
    u32 fl = rd32(ios_of(p) + I_FLAGS);
    char t[40];
    if (fl & F_HEX)
        snprintf(t, sizeof t, fl & F_UPPERCASE ? "%s%llX" : "%s%llx", fl & F_SHOWBASE ? "0x" : "",
                 is_signed ? (unsigned long long)v : (unsigned long long)(u32)v);
    else if (fl & F_OCT)
        snprintf(t, sizeof t, "%s%llo", fl & F_SHOWBASE ? "0" : "", (unsigned long long)(is_signed ? v : (u32)v));
    else if (is_signed)
        snprintf(t, sizeof t, fl & F_SHOWPOS ? "%+lld" : "%lld", (long long)v);
    else
        snprintf(t, sizeof t, "%llu", (unsigned long long)(u32)v);
    put_field(p, t, (u32)strlen(t));
}

static FILE *open_file(const char *guest, u32 mode)
{
    char host[1024];
    const char *m = (mode & M_APP) ? "ab" : (mode & M_IN) && (mode & M_OUT) ? ((mode & M_TRUNC) ? "w+b" : "r+b")
                  : (mode & M_OUT) ? "wb" : "rb";
    FILE *f = fopen(gt_host_path(guest, host, sizeof host), m);
    if (rt_trace)
        rt_log("[stream] open %s (%s): %s\n", guest, m, f ? "ok" : "failed");
    return f;
}

u32 h_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode(u32 self, u32 path, u32 mode)
{
    construct(self, mode | M_IN, 1);
    Stream *s = st_of(self);
    s->f = open_file(GP(path), mode | M_IN);
    if (!s->f)
        set_state(self, S_FAIL);
    return self;
}
u32 h_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev(u32 self) { destruct(self); return self; }
void h_ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv(u32 self)
{
    Stream *s = st_of(self);
    if (s && s->f) {
        fclose(s->f);
        s->f = NULL;
    } else {
        set_state(self, S_FAIL);
    }
}
u32 h_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev(u32 self) { construct(self, M_IN | M_OUT, 1); return self; }
u32 h_ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev(u32 self) { destruct(self); return self; }
void h_ZNSt13basic_fstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode(u32 self, u32 path, u32 mode)
{
    Stream *s = st_of(self);
    if (s->f) {
        set_state(self, S_FAIL);
        return;
    }
    s->mode = mode;
    s->f = open_file(GP(path), mode);
    if (s->f) {
        wr32(ios_of(self) + I_STATE, 0);
        if (mode & M_ATE)
            fseek(s->f, 0, SEEK_END);
    } else {
        set_state(self, S_FAIL);
    }
}
void h_ZNSt13basic_fstreamIcSt11char_traitsIcEE5closeEv(u32 self) { h_ZNSt14basic_ifstreamIcSt11char_traitsIcEE5closeEv(self); }
u32 h_ZNSt13basic_fstreamIcSt11char_traitsIcEE7is_openEv(u32 self)
{
    Stream *s = st_of(self);
    return s && s->f;
}

static void set_str(u32 self, const char *t, u32 n)
{
    Stream *s = st_of(self);
    free(s->buf);
    s->cap = n + 16;
    s->buf = malloc(s->cap);
    memcpy(s->buf, t, n);
    s->buf[n] = 0;
    s->len = n;
    s->gpos = 0;
    s->ppos = (s->mode & (M_APP | M_ATE)) ? n : 0;
}
static u32 str_len(u32 str) { return rd32(rd32(str) - 12); }

u32 h_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(u32 self, u32 mode) { construct(self, mode, 0); return self; }
u32 h_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev(u32 self) { destruct(self); return self; }
u32 h_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode(u32 self, u32 str, u32 mode)
{
    construct(self, mode | M_IN, 0);
    set_str(self, gt_string_cstr(str), str_len(str));
    return self;
}
u32 h_ZNSt19basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev(u32 self) { destruct(self); return self; }
u32 h_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode(u32 self, u32 mode) { construct(self, mode | M_OUT, 0); return self; }
u32 h_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev(u32 self) { destruct(self); return self; }
void h_ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv(u32 out, u32 self)
{
    Stream *s = st_of(self);
    gt_string_make(out, s && s->buf ? s->buf : "", s ? s->len : 0);
}
void h_ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv(u32 out, u32 self)
{
    h_ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv(out, self);
}
void h_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strERKSs(u32 self, u32 str)
{
    set_str(self, gt_string_cstr(str), str_len(str));
}

u32 h_ZNSolsEi(u32 os, u32 v) { put_int(os, (s32)v, 1); return os; }
u32 h_ZNSolsEj(u32 os, u32 v) { put_int(os, v, 0); return os; }
u32 h_ZNSolsEm(u32 os, u32 v) { put_int(os, v, 0); return os; }
u32 h_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(u32 os, u32 s)
{
    if (!s)
        set_state(os, S_BAD);
    else
        put_field(os, GP(s), (u32)strlen(GP(s)));
    return os;
}
u32 h_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c(u32 os, u32 ch)
{
    char c = (char)ch;
    put_field(os, &c, 1);
    return os;
}
u32 h_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E(u32 os, u32 str)
{
    put_field(os, gt_string_cstr(str), str_len(str));
    return os;
}
u32 h_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw(u32 os, u32 w)
{
    wr32(ios_of(os) + I_WIDTH, w);
    return os;
}
u32 h_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E(u32 os, u32 fill)
{
    ((u8 *)GP(ios_of(os)))[I_FILL] = (u8)fill;
    return os;
}
u32 h_ZNSolsEPFRSt8ios_baseS0_E(u32 os, u32 fn)
{
    rt_call1(fn, ios_of(os));
    return os;
}

static int skip_ws(u32 is)
{
    int ch;
    do
        ch = getch(is);
    while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f');
    return ch;
}
static int token(u32 is, char *t, int max, const char *set)
{
    if (state(is)) {
        set_state(is, S_FAIL);
        return 0;
    }
    int ch = skip_ws(is), n = 0;
    while (ch >= 0 && strchr(set, ch) && n < max - 1) {
        t[n++] = (char)ch;
        ch = getch(is);
    }
    if (ch < 0)
        set_state(is, S_EOF);
    else
        ungetch(is, ch);
    t[n] = 0;
    return n;
}
u32 h_ZNSirsERi(u32 is, u32 out)
{
    char t[64], *e;
    if (!token(is, t, sizeof t, "+-0123456789")) {
        set_state(is, S_FAIL);
        return is;
    }
    long v = strtol(t, &e, 10);
    if (*e)
        set_state(is, S_FAIL);
    else
        wr32(out, (u32)v);
    return is;
}
u32 h_ZNSirsERf(u32 is, u32 out)
{
    char t[96], *e;
    if (!token(is, t, sizeof t, "+-0123456789.eEinfINFaA")) {
        set_state(is, S_FAIL);
        return is;
    }
    float v = strtof(t, &e);
    if (*e)
        set_state(is, S_FAIL);
    else
        memcpy(GP(out), &v, 4);
    return is;
}
u32 h_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_E(u32 is, u32 str)
{
    if (state(is)) {
        set_state(is, S_FAIL);
        return is;
    }
    int ch = skip_ws(is);
    Buf b = {0};
    while (ch >= 0 && !(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f')) {
        char c = (char)ch;
        buf_put(&b, &c, 1);
        ch = getch(is);
    }
    if (ch < 0)
        set_state(is, S_EOF);
    else
        ungetch(is, ch);
    if (!b.n)
        set_state(is, S_FAIL);
    h_ZNSs6assignEPKcm(str, GA(b.p ? b.p : ""), b.n);
    buf_free(&b);
    return is;
}
u32 h_ZNSi4readEPci(u32 is, u32 dst, u32 n)
{
    Stream *s = st_of(is);
    u32 got = 0;
    if (!state(is) && s) {
        if (s->is_file) {
            got = s->f ? (u32)fread(GP(dst), 1, n, s->f) : 0;
        } else {
            got = s->len - s->gpos < n ? s->len - s->gpos : n;
            memcpy(GP(dst), s->buf + s->gpos, got);
            s->gpos += got;
        }
    }
    wr32(top_of(is) + 4, got);
    if (got < n)
        set_state(is, S_EOF | S_FAIL);
    return is;
}
u32 h_ZNSi5seekgExSt12_Ios_Seekdir(u32 is, u64 off, u32 dir)
{
    Stream *s = st_of(is);
    wr32(ios_of(is) + I_STATE, state(is) & ~(u32)S_EOF);
    if (state(is) || !s)
        return is;
    if (s->is_file) {
        if (!s->f || _fseeki64(s->f, (long long)off, dir == 0 ? SEEK_SET : dir == 1 ? SEEK_CUR : SEEK_END))
            set_state(is, S_FAIL);
    } else {
        long long base = dir == 0 ? 0 : dir == 1 ? s->gpos : s->len, at = base + (long long)off;
        if (at < 0 || at > s->len)
            set_state(is, S_FAIL);
        else
            s->gpos = (u32)at;
    }
    return is;
}
void h_ZNSi5tellgEv(u32 out, u32 is)
{
    Stream *s = st_of(is);
    long long at = -1;
    if (!state(is) && s)
        at = s->is_file ? (s->f ? _ftelli64(s->f) : -1) : s->gpos;
    memset(GP(out), 0, 136);
    memcpy(GP(out), &at, 8);
}
void h_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate(u32 ios, u32 st) { wr32(ios + I_STATE, st); }
