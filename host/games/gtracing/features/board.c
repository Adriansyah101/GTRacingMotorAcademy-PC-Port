#define _CRT_RAND_S
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include "compat_net.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "features.h"
#include "game.h"
#include "net.h"
#include "ns.h"

#define RB_VTBL 0x7862f8u
#define RB_UPDATE_SLOT (RB_VTBL + 0x1c)
#define RB_UPDATE_FN 0x1bf5d4u
#define RB_ENTER_SLOT (RB_VTBL + 0x0c)
#define RB_ENTER_FN 0x1bf2b8u
#define RB_ERROR_SLOT (RB_VTBL + 0x78)
#define RB_ERROR_FN 0x1bd79cu
#define RB_STATE 0x630
#define RB_TAB 0x634
#define RB_FLAGS 0x638
#define RB_TIMEOUT 0x644
#define RB_ONLINE 0x5c
#define NET_ERROR 0x7a2c50u
#define MENU "RedBullLeaderboardsMenu"
#define BOARD_NO 15
#define RECORD_SIZE 0x164
#define FN_RECORD_ADD 0x2666b4u
#define ENTRY_PATTERN 0x6de510u
#define RB_CAR 411
#define FN_CAR_NAME 0x2b6c28u

#define LB_PORT 28197
#define MAX_ENTRIES 200
#define SEND_ENTRIES 24
#define NAME_LEN 24
#define MAX_PEERS 16

#define ALIAS_LEN 10
#define SHOW_LEN 16
#define TAG_LEN 4

typedef struct { char id[12]; int ms, car; char name[NAME_LEN]; char alias[12]; } Entry;
typedef struct { int used, tun; u32 ip; DWORD seen, next; int sent; } Peer;

static int on = -1;
static CRITICAL_SECTION lock;
static volatile LONG lock_ready;
static Entry board[MAX_ENTRIES];
static int n_board, loaded, no_save;
static char my_id[12], my_alias[12];
static volatile LONG generation;
static Peer peers[MAX_PEERS];
static SOCKET side = INVALID_SOCKET;
static int side_tried;
static DWORD redirect_until;
static void (*rb_update_fn)(CPU *);

static void lock_init(void)
{
    static volatile LONG once;
    if (!InterlockedExchange(&once, 1)) {
        InitializeCriticalSection(&lock);
        InterlockedExchange(&lock_ready, 1);
    }
    while (!lock_ready)
        Sleep(0);
}

static void alias_of(const char *id, char out[12])
{
    if (strlen(id) == ALIAS_LEN) {
        snprintf(out, 12, "%s", id);
        return;
    }
    char s[32];
    u8 d[32];
    int n = snprintf(s, sizeof s, "gtlb1:%s", id);
    crypto_sha256(s, (size_t)n, d);
    for (int i = 0; i < ALIAS_LEN / 2; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static const char *board_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/redbull_board.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static const char *cloud_state_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/redbull_cloud.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static FILE *open_new(const char *path, char *tmp, size_t cap)
{
    snprintf(tmp, cap, "%s.new", path);
    return fopen(tmp, "w");
}

static void close_new(FILE *f, const char *path, const char *tmp)
{
    int ok = !ferror(f);
    if (fclose(f) || !ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(tmp);
}

static void clean_name(char *d, const char *s, size_t cap)
{
    size_t n = 0;
    for (; *s && n < cap - 1 && n < 20; s++)
        if ((u8)*s >= 0x20 && (u8)*s < 0x7f && *s != '<' && *s != '>' && *s != '&')
            d[n++] = *s;
    d[n] = 0;
}

static int valid_id(const char *s)
{
    size_t n = strlen(s);
    if (n < 4 || n > 10)
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
            return 0;
    return 1;
}

static void board_save(void)
{
    char p[1024], t[1040];
    if (no_save)
        return;
    FILE *f = open_new(board_path(p, sizeof p), t, sizeof t);
    if (!f)
        return;
    fputs("# GT Racing: the Red Bull Time Trial leaderboard of this PC's player and the friends met in multiplayer\n"
          "# (RED_BULL_BOARD): id, best lap in ms, car, name\n", f);
    fprintf(f, "me %s\n", my_id);
    for (int i = 0; i < n_board; i++)
        fprintf(f, "%s\t%d\t%d\t%s\n", board[i].id, board[i].ms, board[i].car, board[i].name);
    close_new(f, p, t);
}

static Entry *find(const char *id)
{
    for (int i = 0; i < n_board; i++)
        if (!strcmp(board[i].id, id))
            return &board[i];
    return NULL;
}

static Entry *find_player(const char *id)
{
    Entry *e = find(id);
    if (e)
        return e;
    char a[12];
    alias_of(id, a);
    for (int i = 0; i < n_board; i++)
        if (!strcmp(board[i].alias, a)) {
            if (strlen(id) != ALIAS_LEN && strlen(board[i].id) == ALIAS_LEN)
                snprintf(board[i].id, sizeof board[i].id, "%s", id);
            return &board[i];
        }
    return NULL;
}

static int is_me(const char *id)
{
    return !strcmp(id, my_id) || (my_alias[0] && !strcmp(id, my_alias));
}

static int put(const char *id, int ms, int car, const char *name)
{
    if (ms <= 0 || ms > 3600000 || !valid_id(id))
        return 0;
    Entry *e = find_player(id);
    int changed = 0;
    if (!e) {
        if (n_board < MAX_ENTRIES)
            e = &board[n_board++];
        else {
            int worst = -1;
            for (int i = 0; i < n_board; i++)
                if (strcmp(board[i].id, my_id) && (worst < 0 || board[i].ms > board[worst].ms))
                    worst = i;
            if (worst < 0 || board[worst].ms <= ms)
                return 0;
            e = &board[worst];
        }
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
        alias_of(e->id, e->alias);
        e->ms = ms, e->car = car;
        clean_name(e->name, name && *name ? name : "?", sizeof e->name);
        return 1;
    }
    if (ms < e->ms) {
        e->ms = ms, e->car = car;
        changed = 1;
    }
    if (name && *name) {
        char c[NAME_LEN];
        clean_name(c, name, sizeof c);
        if (c[0] && strcmp(c, e->name)) {
            snprintf(e->name, sizeof e->name, "%s", c);
            changed = 1;
        }
    }
    return changed;
}

static void board_load(void)
{
    if (loaded)
        return;
    loaded = 1;
    char p[1024], line[256];
    FILE *f = fopen(board_path(p, sizeof p), "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#' || !line[0])
                continue;
            if (!strncmp(line, "me ", 3)) {
                if (valid_id(line + 3))
                    snprintf(my_id, sizeof my_id, "%s", line + 3);
                continue;
            }
            char *id = strtok(line, "\t"), *ms = strtok(NULL, "\t"), *car = strtok(NULL, "\t"), *name = strtok(NULL, "");
            if (id && ms && car)
                put(id, atoi(ms), atoi(car), name ? name : "?");
        }
        fclose(f);
    }
    if (!my_id[0]) {
        unsigned r = 0;
        rand_s(&r);
        snprintf(my_id, sizeof my_id, "%08x", (unsigned)(r ^ GetTickCount()));
        board_save();
    }
    alias_of(my_id, my_alias);
    rt_log("[board] Red Bull leaderboard: %d player(s), this PC's player is %s\n", n_board, my_id);
}

static int game_record(int *ms, int *car)
{
    u32 g = rd32(GT_PROFILE_SLOT), owner = g ? rd32(g) : 0, o = owner ? rd32(owner + 4) : 0;
    if (!o)
        return 0;
    u32 cont = o + 0xc0, n = rd32(cont), recs = rd32(cont + 4);
    if (!recs || n > 1000)
        return 0;
    for (u32 k = 0; k < n; k++) {
        u32 r = recs + k * RECORD_SIZE;
        if (rd32(r) != BOARD_NO)
            continue;
        int t = (int)rd32(r + 8);
        if (t <= 0 || t >= 0x7fffffff)
            return 0;
        *ms = t, *car = (int)rd32(r + 0xc);
        return 1;
    }
    return 0;
}

static void own_sync(void)
{
    int ms, car;
    char name[32];
    p2p_name_now(name, sizeof name);
    EnterCriticalSection(&lock);
    board_load();
    int changed = 0;
    if (game_record(&ms, &car))
        changed = put(my_id, ms, car, name);
    else if (find(my_id))
        changed = put(my_id, find(my_id)->ms, find(my_id)->car, name);
    if (changed) {
        board_save();
        InterlockedIncrement(&generation);
        rt_log("[board] own best %d ms\n", find(my_id) ? find(my_id)->ms : 0);
    }
    LeaveCriticalSection(&lock);
}

static int cmp_ms(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return x->ms != y->ms ? (x->ms < y->ms ? -1 : 1) : strcmp(x->id, y->id);
}

static int payload(char *out, int cap)
{
    int n = snprintf(out, cap, "GTLB1 %s\n", my_id);
    Entry *me = find(my_id);
    if (me)
        n += snprintf(out + n, cap - n, "%s\t%d\t%d\t%s\n", me->id, me->ms, me->car, me->name);
    static Entry tmp[MAX_ENTRIES];
    int m = n_board;
    memcpy(tmp, board, sizeof tmp[0] * m);
    qsort(tmp, m, sizeof tmp[0], cmp_ms);
    for (int i = 0, k = 0; i < m && k < SEND_ENTRIES - 1 && n < cap - 64; i++) {
        if (!strcmp(tmp[i].id, my_id))
            continue;
        n += snprintf(out + n, cap - n, "%s\t%d\t%d\t%s\n", tmp[i].id, tmp[i].ms, tmp[i].car, tmp[i].name);
        k++;
    }
    return n;
}

static void merge(const char *p, u32 n, const char *from)
{
    char buf[2048];
    if (n < 7 || n >= sizeof buf || memcmp(p, "GTLB1 ", 6))
        return;
    memcpy(buf, p, n);
    buf[n] = 0;
    char *ctx = NULL, *line = strtok_s(buf, "\n", &ctx);
    char sender[12];
    snprintf(sender, sizeof sender, "%s", line + 6);
    int got = 0, changed = 0;
    EnterCriticalSection(&lock);
    board_load();
    if (!valid_id(sender) || is_me(sender)) {
        LeaveCriticalSection(&lock);
        return;
    }
    while ((line = strtok_s(NULL, "\n", &ctx))) {
        char *c2 = NULL, *id = strtok_s(line, "\t", &c2), *ms = strtok_s(NULL, "\t", &c2),
             *car = strtok_s(NULL, "\t", &c2), *name = strtok_s(NULL, "", &c2);
        if (!id || !ms || !car || is_me(id))
            continue;
        got++;
        int own = !strcmp(id, sender);
        Entry *e = find_player(id);
        changed += put(id, atoi(ms), atoi(car), own || !e ? (name ? name : "?") : NULL);
    }
    if (changed) {
        board_save();
        InterlockedIncrement(&generation);
    }
    LeaveCriticalSection(&lock);
    rt_log("[board] leaderboard from %s (%s): %d line(s), %d new or better\n", from, sender, got, changed);
}

static void side_open(void)
{
    if (side != INVALID_SOCKET || side_tried || !net_lan_on())
        return;
    side_tried = 1;
    side = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (side == INVALID_SOCKET)
        return;
    BOOL no = FALSE;
    DWORD got;
    WSAIoctl(side, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &got, NULL, NULL);
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = net_test_ip();
    w.sin_port = htons(LB_PORT);
    if (bind(side, (struct sockaddr *)&w, sizeof w)) {
        rt_log("[board] UDP %d is taken (another game on this PC?): leaderboards are sent, not received\n", LB_PORT);
        w.sin_port = 0;
        bind(side, (struct sockaddr *)&w, sizeof w);
    }
    u_long nb = 1;
    ioctlsocket(side, FIONBIO, &nb);
}

static void peer_seen(int tun, u32 ip)
{
    if (on <= 0)
        return;
    DWORD now = GetTickCount();
    EnterCriticalSection(&lock);
    int free_i = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *q = &peers[i];
        if (!q->used) {
            if (free_i < 0)
                free_i = i;
            continue;
        }
        if (q->tun == tun && q->ip == ip) {
            if (now - q->seen > 60000)
                q->sent = 0, q->next = now;
            q->seen = now;
            LeaveCriticalSection(&lock);
            return;
        }
    }
    if (free_i >= 0) {
        peers[free_i] = (Peer){1, tun, ip, now, now, 0};
        if (tun)
            rt_log("[board] online player %u: our leaderboard goes to them\n", ip + 1);
        else {
            char a[16];
            inet_ntop(AF_INET, &ip, a, sizeof a);
            rt_log("[board] player at %s: our leaderboard goes to them\n", a);
        }
    }
    LeaveCriticalSection(&lock);
}

void board_peer_lan(u32 ip_be) { peer_seen(0, ip_be); }
void board_peer_tun(int i) { peer_seen(1, (u32)i); }

void board_rx_tun(int i, const u8 *p, u32 n)
{
    if (on <= 0)
        return;
    char who[32];
    snprintf(who, sizeof who, "online player %d", i + 1);
    merge((const char *)p, n, who);
    peer_seen(1, (u32)i);
}

void board_poll(void)
{
    if (on <= 0 || !net_lan_on())
        return;
    static volatile LONG busy;
    if (InterlockedExchange(&busy, 1))
        return;
    side_open();
    if (side != INVALID_SOCKET) {
        char b[2048];
        for (;;) {
            struct sockaddr_in w;
            int wl = sizeof w;
            int n = recvfrom(side, b, sizeof b, 0, (struct sockaddr *)&w, &wl);
            if (n <= 0)
                break;
            if (net_own_addr(w.sin_addr.s_addr) || !net_addr_allowed(w.sin_addr.s_addr))
                continue;
            char a[16];
            inet_ntop(AF_INET, &w.sin_addr, a, sizeof a);
            merge(b, (u32)n, a);
            peer_seen(0, w.sin_addr.s_addr);
        }
    }
    DWORD now = GetTickCount();
    static DWORD synced;
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && peers[i].sent < 3 && (int)(now - peers[i].next) >= 0 && now - synced > 1000) {
            synced = now;
            own_sync();
            break;
        }
    for (int i = 0; i < MAX_PEERS; i++) {
        EnterCriticalSection(&lock);
        Peer q = peers[i];
        int due = q.used && q.sent < 3 && (int)(now - q.next) >= 0;
        char out[1500];
        int n = 0;
        if (due) {
            board_load();
            n = payload(out, sizeof out);
            peers[i].sent++;
            peers[i].next = now + (peers[i].sent == 1 ? 2000 : 5000);
        }
        if (q.used && now - q.seen > 600000)
            peers[i].used = 0;
        LeaveCriticalSection(&lock);
        if (!due)
            continue;
        if (q.tun)
            net_p2p_board((int)q.ip, out, (u32)n);
        else if (side != INVALID_SOCKET) {
            struct sockaddr_in w = {0};
            w.sin_family = AF_INET;
            w.sin_addr.s_addr = q.ip;
            w.sin_port = htons(LB_PORT);
            sendto(side, out, n, 0, (struct sockaddr *)&w, sizeof w);
        }
    }
    InterlockedExchange(&busy, 0);
}

#define CLOUD_MIN_MS 50000
#define CLOUD_MAX_MS 600000
#define CLOUD_GET_EVERY 60000
#define CLOUD_RETRY 60000
#define CLOUD_QUOTA_WAIT 600000
#define CLOUD_TIMEOUT 4000
#define CLOUD_REPLY_MAX (256 * 1024)

#ifdef _WIN32
#define HTTP_LIB "WinHTTP"
typedef struct { wchar_t host[256], base[512]; INTERNET_PORT port; int secure; } CloudUrl;
#else
#define HTTP_LIB "curl"
typedef void *HINTERNET;
typedef struct { char base[512]; int secure; } CloudUrl;
#endif
typedef struct { int get, post_ms; char body[256]; } CloudJob;

static int cl_on;
static CloudUrl cl_url;
static volatile LONG cl_busy;
static volatile LONG cl_waiting;
static volatile DWORD cl_wait_until;
static DWORD cl_last_get;
static int cl_got_once;
static int cl_sent_ms, cl_refused_ms, cl_state_loaded;
static volatile LONG cl_said;
static DWORD cl_err;

#ifdef _WIN32
static int cloud_crack(const char *u, CloudUrl *o)
{
    wchar_t w[768];
    memset(o, 0, sizeof *o);
    if (!u || !*u || !MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 768))
        return 0;
    URL_COMPONENTS c;
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
    c.dwUserNameLength = c.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w, 0, 0, &c) || !c.dwHostNameLength || c.dwHostNameLength >= 256 || c.dwUserNameLength ||
        c.dwPasswordLength || c.dwExtraInfoLength || c.dwUrlPathLength >= 500)
        return 0;
    wcsncpy(o->host, c.lpszHostName, c.dwHostNameLength);
    o->host[c.dwHostNameLength] = 0;
    int here = !wcscmp(o->host, L"127.0.0.1") || !_wcsicmp(o->host, L"localhost") || !wcscmp(o->host, L"::1") ||
               !wcscmp(o->host, L"[::1]");
    if (c.nScheme == INTERNET_SCHEME_HTTPS)
        o->secure = 1;
    else if (c.nScheme != INTERNET_SCHEME_HTTP || !here)
        return 0;
    o->port = c.nPort;
    size_t n = c.dwUrlPathLength;
    if (n)
        wcsncpy(o->base, c.lpszUrlPath, n);
    o->base[n] = 0;
    while (n && o->base[n - 1] == L'/')
        o->base[--n] = 0;
    return 1;
}
#else
static int cloud_crack(const char *u, CloudUrl *o)
{
    memset(o, 0, sizeof *o);
    if (!u || !*u || strlen(u) >= 500)
        return 0;
    const char *h;
    if (!strncasecmp(u, "https://", 8))
        o->secure = 1, h = u + 8;
    else if (!strncasecmp(u, "http://", 7))
        h = u + 7;
    else
        return 0;
    size_t hl = strcspn(h, "/?#");
    if (!hl || hl >= 256 || memchr(h, '@', hl) || strpbrk(h + hl, "?#"))
        return 0;
    char host[256];
    memcpy(host, h, hl);
    host[hl] = 0;
    char *end = host[0] == '[' ? strchr(host, ']') : strchr(host, ':');
    if (host[0] == '[' && end)
        end[1] = 0;
    else if (end)
        *end = 0;
    int here = !strcmp(host, "127.0.0.1") || !strcasecmp(host, "localhost") || !strcmp(host, "[::1]");
    if (!o->secure && !here)
        return 0;
    size_t n = strlen(u);
    memcpy(o->base, u, n + 1);
    while (n && o->base[n - 1] == '/')
        o->base[--n] = 0;
    return 1;
}

#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_USERAGENT 10018
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_SSLVERSION 32
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURL_SSLVERSION_TLSv1_2 6
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURL_GLOBAL_DEFAULT 3
static struct {
    int (*global_init)(long);
    void *(*easy_init)(void);
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void (*easy_cleanup)(void *);
    void (*easy_reset)(void *);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
} curl;

static int curl_load(void)
{
    void *so = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!so)
        so = dlopen("libcurl-gnutls.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!so)
        return 0;
    *(void **)&curl.global_init = dlsym(so, "curl_global_init");
    *(void **)&curl.easy_init = dlsym(so, "curl_easy_init");
    *(void **)&curl.easy_setopt = dlsym(so, "curl_easy_setopt");
    *(void **)&curl.easy_perform = dlsym(so, "curl_easy_perform");
    *(void **)&curl.easy_getinfo = dlsym(so, "curl_easy_getinfo");
    *(void **)&curl.easy_cleanup = dlsym(so, "curl_easy_cleanup");
    *(void **)&curl.easy_reset = dlsym(so, "curl_easy_reset");
    *(void **)&curl.slist_append = dlsym(so, "curl_slist_append");
    *(void **)&curl.slist_free_all = dlsym(so, "curl_slist_free_all");
    if (!curl.global_init || !curl.easy_init || !curl.easy_setopt || !curl.easy_perform || !curl.easy_getinfo ||
        !curl.easy_cleanup || !curl.easy_reset || !curl.slist_append || !curl.slist_free_all ||
        curl.global_init(CURL_GLOBAL_DEFAULT)) {
        dlclose(so);
        return 0;
    }
    return 1;
}

typedef struct { char *buf; size_t n; } CurlReply;
static size_t curl_take(char *p, size_t size, size_t n, void *arg)
{
    CurlReply *r = arg;
    size_t k = size * n, room = CLOUD_REPLY_MAX - r->n;
    memcpy(r->buf + r->n, p, k < room ? k : room);
    r->n += k < room ? k : room;
    return size * n;
}
#endif

static const char *js_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static int js_hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static const char *js_str(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    if (*p++ != '"')
        return NULL;
    for (;;) {
        unsigned c = (u8)*p++;
        if (c < 0x20)
            return NULL;
        if (c == '"')
            break;
        if (c == '\\') {
            switch (*p++) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': case 'f': case 'n': case 'r': case 't': c = 0; break;
            case 'u':
                c = 0;
                for (int i = 0; i < 4; i++, p++) {
                    int h = js_hex(*p);
                    if (h < 0)
                        return NULL;
                    c = c << 4 | (unsigned)h;
                }
                if (c < 0x20 || c >= 0x7f)
                    c = 0;
                break;
            default:
                return NULL;
            }
        }
        if (c && out && n + 1 < cap)
            out[n++] = (char)c;
    }
    if (out && cap)
        out[n] = 0;
    return p;
}

static const char *js_skip(const char *p, int depth)
{
    p = js_ws(p);
    if (depth > 16)
        return NULL;
    if (*p == '"')
        return js_str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int obj = *p == '{';
        char close = obj ? '}' : ']';
        p = js_ws(p + 1);
        if (*p == close)
            return p + 1;
        for (;;) {
            if (obj) {
                if (!(p = js_str(js_ws(p), NULL, 0)))
                    return NULL;
                p = js_ws(p);
                if (*p++ != ':')
                    return NULL;
            }
            if (!(p = js_skip(p, depth + 1)))
                return NULL;
            p = js_ws(p);
            if (*p == ',') {
                p++;
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    const char *s = p;
    while (*p && strchr("+-0123456789.eEtrufalsn", *p))
        p++;
    return p > s ? p : NULL;
}

static const char *js_int(const char *p, long long *v, int *ok)
{
    const char *e = js_skip(p, 0);
    char t[24];
    size_t n = e ? (size_t)(e - p) : 0;
    *ok = 0;
    if (!e || !n || n >= sizeof t)
        return e;
    memcpy(t, p, n);
    t[n] = 0;
    char *end;
    *v = strtoll(t, &end, 10);
    *ok = !*end && (t[0] == '-' || (t[0] >= '0' && t[0] <= '9'));
    return e;
}

static const char *cloud_row(const char *p, Entry *r, int *good)
{
    char name[64] = "", id[16] = "";
    int have = 0, bad = 0, iok;
    long long v;
    memset(r, 0, sizeof *r);
    *good = 0;
    if (*p != '{')
        return js_skip(p, 1);
    p = js_ws(p + 1);
    if (*p == '}')
        return p + 1;
    for (;;) {
        char key[16];
        if (!(p = js_str(p, key, sizeof key)))
            return NULL;
        p = js_ws(p);
        if (*p++ != ':')
            return NULL;
        p = js_ws(p);
        int text = *p == '"';
        if (!strcmp(key, "id") && text) {
            if (!(p = js_str(p, id, sizeof id)))
                return NULL;
            have |= 1;
        } else if (!strcmp(key, "name") && text) {
            if (!(p = js_str(p, name, sizeof name)))
                return NULL;
            have |= 2;
        } else if (!strcmp(key, "car") || !strcmp(key, "time_ms")) {
            if (!(p = js_int(p, &v, &iok)))
                return NULL;
            if (key[0] == 'c') {
                bad |= !iok || v != RB_CAR;
                r->car = RB_CAR;
                have |= 4;
            } else {
                bad |= !iok || v < CLOUD_MIN_MS || v > CLOUD_MAX_MS;
                r->ms = bad ? 0 : (int)v;
                have |= 8;
            }
        } else {
            bad |= !strcmp(key, "id") || !strcmp(key, "name");
            if (!(p = js_skip(p, 1)))
                return NULL;
        }
        p = js_ws(p);
        if (*p == ',') {
            p = js_ws(p + 1);
            continue;
        }
        if (*p != '}')
            return NULL;
        p++;
        break;
    }
    clean_name(r->name, name, sizeof r->name);
    if (strlen(id) == ALIAS_LEN && valid_id(id) && have == 15 && !bad && r->name[0]) {
        snprintf(r->id, sizeof r->id, "%s", id);
        snprintf(r->alias, sizeof r->alias, "%s", id);
        *good = 1;
    }
    return p;
}

static int cloud_parse(const char *s, Entry *rows, int max)
{
    const char *p = js_ws(s);
    int n = 0;
    if (*p++ != '{')
        return -1;
    p = js_ws(p);
    if (*p == '}')
        return 0;
    for (;;) {
        char key[16];
        if (!(p = js_str(js_ws(p), key, sizeof key)))
            return -1;
        p = js_ws(p);
        if (*p++ != ':')
            return -1;
        p = js_ws(p);
        if (!strcmp(key, "rows") && *p == '[') {
            p = js_ws(p + 1);
            if (*p == ']')
                p++;
            else
                for (;;) {
                    Entry r;
                    int good;
                    if (!(p = cloud_row(p, &r, &good)))
                        return -1;
                    if (good && n < max)
                        rows[n++] = r;
                    p = js_ws(p);
                    if (*p == ',') {
                        p = js_ws(p + 1);
                        continue;
                    }
                    if (*p++ != ']')
                        return -1;
                    break;
                }
        } else if (!(p = js_skip(p, 0)))
            return -1;
        p = js_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        return *p == '}' ? n : -1;
    }
}

static int cloud_body(char *out, size_t cap, const char *id, const char *name, int ms)
{
    size_t n = (size_t)snprintf(out, cap, "{\"player_id\":\"%s\",\"name\":\"", id);
    for (const char *s = name; *s && n + 40 < cap; s++) {
        if (*s == '"' || *s == '\\')
            out[n++] = '\\';
        out[n++] = *s;
    }
    n += (size_t)snprintf(out + n, cap - n, "\",\"car\":%d,\"time_ms\":%d}", RB_CAR, ms);
    return (int)n;
}

static int cloud_merge(const Entry *rows, int n)
{
    int changed = 0;
    EnterCriticalSection(&lock);
    board_load();
    for (int i = 0; i < n; i++) {
        if (is_me(rows[i].id))
            continue;
        Entry *e = find_player(rows[i].id);
        int name_ok = !e || strlen(e->id) == ALIAS_LEN;
        changed += put(rows[i].id, rows[i].ms, rows[i].car, name_ok ? rows[i].name : NULL);
    }
    if (changed) {
        board_save();
        InterlockedIncrement(&generation);
    }
    LeaveCriticalSection(&lock);
    return changed;
}

static void cloud_state_load(void)
{
    if (cl_state_loaded)
        return;
    cl_state_loaded = 1;
    char p[1024], line[96], id[16];
    int ms;
    FILE *f = no_save ? NULL : fopen(cloud_state_path(p, sizeof p), "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "sent %d %15s", &ms, id) == 2 && !strcmp(id, my_id) && ms > 0)
            cl_sent_ms = ms;
    fclose(f);
}

static void cloud_state_save(void)
{
    char p[1024], t[1040];
    if (no_save)
        return;
    FILE *f = open_new(cloud_state_path(p, sizeof p), t, sizeof t);
    if (!f)
        return;
    fprintf(f, "# GT Racing (CLOUD_BOARD): the own best Red Bull lap the leaderboard server has taken (ms, id)\n"
               "sent %d %s\n", cl_sent_ms, my_id);
    close_new(f, p, t);
}

static int cloud_failed(int status)
{
    int quota = status == 429 || status == 503;
    if (InterlockedExchange(&cl_said, status) != status) {
        if (quota)
            rt_log("[board] cloud: the server is out of its quota (HTTP %d): the board stays this PC's and the "
                   "friends', next try in 10 min\n", status);
        else if (status == -1)
            rt_log("[board] cloud: no answer (offline, or the server is down; " HTTP_LIB " %lu): the board stays this "
                   "PC's and the friends', next try in a minute\n", cl_err);
        else if (status == -2)
            rt_log("[board] cloud: the server's answer is not a leaderboard: the board stays local\n");
        else
            rt_log("[board] cloud: HTTP %d: the board stays this PC's and the friends', next try in a minute\n",
                   status);
    }
    return quota ? CLOUD_QUOTA_WAIT : CLOUD_RETRY;
}

#ifdef _WIN32
static int cloud_http(HINTERNET ses, const wchar_t *verb, const char *path, const char *body, char **reply)
{
    wchar_t wp[600];
    int status = -1;
    size_t bl = wcslen(cl_url.base);
    *reply = NULL;
    wcscpy(wp, cl_url.base);
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp + bl, (int)(600 - bl)))
        return -1;
    HINTERNET con = WinHttpConnect(ses, cl_url.host, cl_url.port, 0), req = NULL;
    if (con)
        req = WinHttpOpenRequest(con, verb, wp, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 cl_url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        DWORD n = body ? (DWORD)strlen(body) : 0;
        if (WinHttpSendRequest(req, body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                               body ? (DWORD)-1 : 0, body ? (LPVOID)(ULONG_PTR)body : WINHTTP_NO_REQUEST_DATA, n, n,
                               0) &&
            WinHttpReceiveResponse(req, NULL)) {
            DWORD code = 0, sz = sizeof code, got = 0, r;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX))
                status = (int)code;
            char *buf = malloc(CLOUD_REPLY_MAX + 1);
            if (buf) {
                while (got < CLOUD_REPLY_MAX && WinHttpReadData(req, buf + got, CLOUD_REPLY_MAX - got, &r) && r)
                    got += r;
                buf[got] = 0;
                *reply = buf;
            }
        }
    }
    if (status < 0)
        cl_err = GetLastError();
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    return status;
}
#else
static int cloud_http(HINTERNET ses, const wchar_t *verb, const char *path, const char *body, char **reply)
{
    char url[1024];
    *reply = NULL;
    if (snprintf(url, sizeof url, "%s%s", cl_url.base, path) >= (int)sizeof url)
        return -1;
    CurlReply r = {malloc(CLOUD_REPLY_MAX + 1), 0};
    if (!r.buf)
        return -1;
    curl.easy_reset(ses);
    curl.easy_setopt(ses, CURLOPT_URL, url);
    curl.easy_setopt(ses, CURLOPT_USERAGENT, "GTRacingPC-board/1");
    curl.easy_setopt(ses, CURLOPT_NOSIGNAL, 1L);
    curl.easy_setopt(ses, CURLOPT_FOLLOWLOCATION, 0L);
    curl.easy_setopt(ses, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl.easy_setopt(ses, CURLOPT_CONNECTTIMEOUT_MS, (long)CLOUD_TIMEOUT);
    curl.easy_setopt(ses, CURLOPT_TIMEOUT_MS, (long)CLOUD_TIMEOUT * 2);
    curl.easy_setopt(ses, CURLOPT_WRITEFUNCTION, curl_take);
    curl.easy_setopt(ses, CURLOPT_WRITEDATA, &r);
    void *hdr = NULL;
    if (verb[0] == L'P') {
        hdr = curl.slist_append(NULL, "Content-Type: application/json");
        curl.easy_setopt(ses, CURLOPT_HTTPHEADER, hdr);
        curl.easy_setopt(ses, CURLOPT_POSTFIELDS, body ? body : "");
        curl.easy_setopt(ses, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    }
    int status = -1, rc = curl.easy_perform(ses);
    long code = 0;
    if (!rc && !curl.easy_getinfo(ses, CURLINFO_RESPONSE_CODE, &code) && code > 0)
        status = (int)code;
    else
        cl_err = (DWORD)rc;
    if (hdr)
        curl.slist_free_all(hdr);
    r.buf[r.n] = 0;
    *reply = r.buf;
    return status;
}
#endif

static DWORD WINAPI cloud_thread(void *arg)
{
    CloudJob *j = arg;
    static Entry rows[MAX_ENTRIES];
#ifdef _WIN32
    const wchar_t *agent = L"GTRacingPC-board/1";
    HINTERNET ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, CLOUD_TIMEOUT, CLOUD_TIMEOUT, CLOUD_TIMEOUT, CLOUD_TIMEOUT);
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {
            tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
        }
    }
#else
    HINTERNET ses = curl.easy_init();
#endif
    int wait = 0;
    char *reply = NULL;
    if (j->post_ms) {
        int st = ses ? cloud_http(ses, L"POST", "/submit", j->body, &reply) : -1;
        if (st == 200) {
            EnterCriticalSection(&lock);
            cloud_state_load();
            if (!cl_sent_ms || j->post_ms < cl_sent_ms)
                cl_sent_ms = j->post_ms;
            cloud_state_save();
            LeaveCriticalSection(&lock);
            InterlockedExchange(&cl_said, 0);
            rt_log("[board] cloud: own best %d ms sent (%s)\n", j->post_ms,
                   reply && strstr(reply, "\"improved\":true") ? "now the server's best for us" :
                                                                 "the server had it, or better");
        } else if (st >= 400 && st < 500 && st != 408 && st != 429) {
            EnterCriticalSection(&lock);
            cl_refused_ms = j->post_ms;
            LeaveCriticalSection(&lock);
            rt_log("[board] cloud: own best %d ms refused by the server (HTTP %d): not sent again\n", j->post_ms, st);
        } else
            wait = cloud_failed(st);
        free(reply);
        reply = NULL;
    }
    if (j->get && !wait) {
        int st = ses ? cloud_http(ses, L"GET", "/board", NULL, &reply) : -1;
        int n = st == 200 && reply ? cloud_parse(reply, rows, MAX_ENTRIES) : -1;
        if (n >= 0) {
            int changed = cloud_merge(rows, n);
            InterlockedExchange(&cl_said, 0);
            rt_log("[board] cloud: %d player(s) on the server's board, %d new or better here\n", n, changed);
        } else
            wait = cloud_failed(st == 200 ? -2 : st);
        free(reply);
    }
#ifdef _WIN32
    if (ses)
        WinHttpCloseHandle(ses);
#else
    if (ses)
        curl.easy_cleanup(ses);
#endif
    if (wait) {
        cl_wait_until = GetTickCount() + (DWORD)wait;
        InterlockedExchange(&cl_waiting, 1);
    } else
        InterlockedExchange(&cl_waiting, 0);
    free(j);
    InterlockedExchange(&cl_busy, 0);
    return 0;
}

static void cloud_kick(int menu)
{
    if (cl_on <= 0 || InterlockedCompareExchange(&cl_busy, 1, 0))
        return;
    DWORD now = GetTickCount();
    if (cl_waiting && (int)(cl_wait_until - now) > 0) {
        InterlockedExchange(&cl_busy, 0);
        return;
    }
    CloudJob job = {0};
    static int said_range;
    EnterCriticalSection(&lock);
    board_load();
    cloud_state_load();
    Entry *me = find(my_id);
    if (me && strlen(my_id) == 8 && (!cl_sent_ms || me->ms < cl_sent_ms) && me->ms != cl_refused_ms) {
        if (me->ms >= CLOUD_MIN_MS && me->ms <= CLOUD_MAX_MS) {
            job.post_ms = me->ms;
            cloud_body(job.body, sizeof job.body, my_id, me->name, me->ms);
        } else if (!said_range++)
            rt_log("[board] cloud: own best %d ms is outside %d .. %d ms: not sent\n", me->ms, CLOUD_MIN_MS,
                   CLOUD_MAX_MS);
    }
    LeaveCriticalSection(&lock);
    job.get = menu && (!cl_got_once || now - cl_last_get >= CLOUD_GET_EVERY);
    CloudJob *j = job.get || job.post_ms ? malloc(sizeof *j) : NULL;
    HANDLE t = NULL;
    if (j) {
        *j = job;
        if (job.get)
            cl_got_once = 1, cl_last_get = now;
        t = CreateThread(NULL, 0, cloud_thread, j, 0, NULL);
    }
    if (t)
        CloseHandle(t);
    else {
        free(j);
        InterlockedExchange(&cl_busy, 0);
    }
}

static void cloud_init(void)
{
    const char *e = getenv("GT_CLOUD_BOARD"), *u = getenv("GT_CLOUD_BOARD_URL");
    cl_on = 0;
    if (e && *e == '0')
        rt_log("[board] cloud: off (GT_CLOUD_BOARD=0): this PC's and the friends' board only\n");
    else if (!u || !*u)
        rt_log("[board] cloud: no server (GT_CLOUD_BOARD_URL is empty): this PC's and the friends' board only\n");
    else if (!cloud_crack(u, &cl_url))
        rt_log("[board] cloud: GT_CLOUD_BOARD_URL is not an https:// address (plain http only to this PC): off\n");
#ifndef _WIN32
    else if (!curl_load())
        rt_log("[board] cloud: no 32-bit libcurl (libcurl.so.4): this PC's and the friends' board only\n");
#endif
    else {
        cl_on = 1;
        rt_log("[board] cloud: on (%s; the address is not logged)\n", cl_url.secure ? "https" : "http to this PC");
    }
}

void board_nav_event(const char *name, int kind)
{
    if (on > 0 && kind == 6 && !strcmp(name, "btnLeaderboard"))
        redirect_until = GetTickCount() + 3000;
}

#define G_STATES 0x1ba8
#define G_STATE_AT 0x1bd0
#define SCENE_OPEN 0x34
#define SC_MOVING 0x105
#define FN_RB_RACE_OBJ 0x3770c0u
#define FN_RB_RACE_END 0x376e68u
static DWORD open_after_until;
static int open_after_frames;
static char open_over[64];
static int tab_after;

void __real_F_376e68(CPU *c);
void __wrap_F_376e68(CPU *c)
{
    u32 obj = c->r[0], g = gt_game();
    char was[64] = "";
    if (on > 0 && g)
        snprintf(was, sizeof was, "%s", (const char *)GP(g + G_NEXT_MENU));
    __real_F_376e68(c);
    if (on <= 0 || !g || !was[0] || strlen(was) >= sizeof was - 1)
        return;
    strcpy((char *)GP(g + G_NEXT_MENU), was);
    *(volatile u8 *)GP(obj + 8) = 0;
    strcpy(open_over, was);
    open_after_until = GetTickCount() + 60000;
    open_after_frames = 0;
    rt_log("[board] after the Red Bull race: %s, then the leaderboard over it (no Gameloft LIVE login)\n", was);
}

#define FN_AS_NUMBER 0x507754u
#define FN_AS_FREE 0x506e0cu
#define FN_AS_CALL 0x50ac2cu
static void nav_tab(int tab)
{
    static u32 val[8] __attribute__((aligned(16)));
    u32 g = gt_game(), pl = g ? rd32(g + G_MENUS) : 0;
    u32 obj = pl ? rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"_root.NavigationMenu")) : 0;
    if (!obj)
        return;
    double d = tab;
    u32 w[2];
    memcpy(w, &d, 8);
    memset(val, 0, sizeof val);
    rt_call3(FN_AS_NUMBER, GA(val), w[0], w[1]);
    u32 args[5] = {pl, obj, GA((void *)"ChangeToTab"), GA(val), 1};
    rt_guest_call(rt_cpu(), FN_AS_CALL, 5, args);
    rt_call1(FN_AS_FREE, GA(val));
    rt_log("[board] NavigationMenu.ChangeToTab(%d)\n", tab);
}

static u32 top_menu(void)
{
    u32 g = gt_game(), pl = g ? rd32(g + G_MENUS) : 0;
    int n = pl ? (int)rd32(pl + 0x118) : 0;
    u32 top = n > 0 && rd32(pl + 0x114) ? rd32(rd32(pl + 0x114) + 4 * (n - 1)) : 0;
    return top && rd32(top + 0x58) == 1 ? top : 0;
}

#define NAV_TAB_RED_BULL 6
static void tab_after_step(void)
{
    static int frames;
    u32 top = top_menu();
    if (!top || strcmp((const char *)GP(top + 8), "NavigationMenu")) {
        frames = 0;
        return;
    }
    if (++frames < 5)
        return;
    frames = 0;
    tab_after = 0;
    nav_tab(NAV_TAB_RED_BULL);
}

static void open_after(void)
{
    if ((int)(open_after_until - GetTickCount()) <= 0) {
        open_after_until = 0;
        return;
    }
    u32 g = gt_game();
    int at = g ? (int)rd32(g + G_STATE_AT) : -1;
    u32 scene = at >= 0 ? rd32(g + G_STATES + 4 * at) : 0, pl = g ? rd32(g + G_MENUS) : 0;
    int n = pl ? (int)rd32(pl + 0x118) : 0;
    u32 top = n > 0 && rd32(pl + 0x114) ? rd32(rd32(pl + 0x114) + 4 * (n - 1)) : 0;
    if (!scene || rd32(scene) != GT_VT_SCENE || *(volatile u8 *)GP(scene + SC_MOVING) || !top ||
        rd32(top + 0x58) != 1 || strcmp((const char *)GP(top + 8), open_over)) {
        open_after_frames = 0;
        return;
    }
    if (++open_after_frames < 10)
        return;
    open_after_until = 0;
    rt_log("[board] the leaderboard over %s\n", open_over);
    rt_call4(rd32(GT_VT_SCENE + SCENE_OPEN), scene, GA((void *)MENU), 1, 0);
    tab_after = 1;
}

void board_tick(void)
{
    static int armed = -1, at;
    if (open_after_until)
        open_after();
    if (tab_after == 2)
        tab_after_step();
    if (armed < 0) {
        const char *e = dev_getenv("GT_RED_BULL_BOARD_TEST_END");
        at = e ? atoi(e) : 0;
        armed = at > 0;
    }
    if (!armed || (int)eagl_frames() < at)
        return;
    armed = 0;
    u32 obj = rt_call1(FN_RB_RACE_OBJ, 0);
    rt_call1(FN_RB_RACE_END, obj);
}

u32 board_menu_redirect(u32 name)
{
    static const char menu[] = MENU;
    if (on > 0 && redirect_until && (int)(redirect_until - GetTickCount()) > 0 && name &&
        !strcmp((const char *)GP(name), "GLLiveLoginMenu")) {
        redirect_until = 0;
        rt_log("[board] GLLiveLoginMenu -> " MENU " (offline leaderboard, no login)\n");
        return GA((void *)menu);
    }
    if (tab_after == 1 && name)
        tab_after = !strcmp((const char *)GP(name), "NavigationMenu") ? 2 : 0;
    return name;
}

static void car_name(int car, char *out, size_t cap)
{
    out[0] = 0;
    u32 g = rd32(GT_CAR_TABLE_SLOT), cars = g ? rd32(g) : 0;
    if (!cars)
        return;
    if (car <= 0)
        car = RB_CAR;
    int idx = (int)rt_call2(GT_FN_CAR_INDEX, cars, (u32)car);
    if (idx < 0 || idx >= (int)rd32(cars + 8))
        return;
    static u16 buf[256];
    memset(buf, 0, sizeof buf);
    rt_call4(FN_CAR_NAME, cars, (u32)idx, GA(buf), 0);
    size_t n = 0;
    for (int i = 0; i < 255 && buf[i] && n + 4 < cap; i++) {
        unsigned ch = buf[i];
        if (ch < 0x80)
            out[n++] = (char)ch;
        else if (ch < 0x800)
            out[n++] = (char)(0xc0 | ch >> 6), out[n++] = (char)(0x80 | (ch & 0x3f));
        else
            out[n++] = (char)(0xe0 | ch >> 12), out[n++] = (char)(0x80 | (ch >> 6 & 0x3f)),
            out[n++] = (char)(0x80 | (ch & 0x3f));
    }
    out[n] = 0;
}

static void set_cell(u32 pl, const char *field, int row, const char *text, int mine)
{
    char path[128], html[256];
    snprintf(path, sizeof path, MENU ".TimeBox.InfoBox.%s%d", field, row);
    if (mine && text[0])
        snprintf(html, sizeof html, "<font color=\"#FF9900\">%s</font>", text);
    else
        snprintf(html, sizeof html, "%s", text);
    rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(path), GA(html), 1);
}

static void show_name(const char *name, const char *alias, int twin, int mine, char *out, size_t cap)
{
    int tag = twin || !_stricmp(name, "iPad");
    size_t keep = tag ? SHOW_LEN - 1 - TAG_LEN : SHOW_LEN, n = 0;
    char base[SHOW_LEN + 1], t[TAG_LEN + 1];
    for (; name[n] && n < keep; n++)
        base[n] = name[n];
    base[n] = 0;
    for (int i = 0; i < TAG_LEN; i++)
        t[i] = alias[i] >= 'a' && alias[i] <= 'f' ? (char)(alias[i] - 32) : alias[i] ? alias[i] : '0';
    t[TAG_LEN] = 0;
    if (tag)
        snprintf(out, cap, "%s#%s", base, t);
    else
        snprintf(out, cap, "%s", base);
    if (mine && strlen(out) + 6 <= SHOW_LEN)
        snprintf(out + strlen(out), cap - strlen(out), " (You)");
}

static int name_fit(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == 'W' || c == 'w')
            w += 230;
        else if (c == 'M' || c == 'm')
            w += 195;
        else if (c && strchr("il.,:;'!|j", c))
            w += 60;
        else if (c && strchr("rtfI() ", c))
            w += 85;
        else if (c >= 'A' && c <= 'Z')
            w += 155;
        else if (c >= 'a' && c <= 'z')
            w += 130;
        else
            w += 125;
    }
    int pct = w > 2500 ? 2500 * 100 / w : 100;
    return pct < 55 ? 55 : pct;
}

static int twin_of(const Entry *rows, int n, int i)
{
    for (int k = 0; k < n; k++)
        if (k != i && !_strnicmp(rows[k].name, rows[i].name, SHOW_LEN))
            return 1;
    return 0;
}

static void draw(u32 pl, int tab)
{
    static Entry rows[MAX_ENTRIES];
    EnterCriticalSection(&lock);
    int n = n_board;
    memcpy(rows, board, sizeof rows[0] * n);
    LeaveCriticalSection(&lock);
    qsort(rows, n, sizeof rows[0], cmp_ms);
    int me = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].id, my_id))
            me = i;
    int first = 0;
    if (tab == 1 && me >= 0) {
        first = me - 4;
        if (first > n - 10)
            first = n - 10;
        if (first < 0)
            first = 0;
    }
    for (int r = 1; r <= 10; r++) {
        int i = first + r - 1;
        char num[16] = "", car[96] = "", tm[96] = "", shown[SHOW_LEN + 8], fit[SHOW_LEN + 48];
        const char *name = "";
        int mine = 0;
        if (i < n) {
            Entry *e = &rows[i];
            snprintf(num, sizeof num, "%d", i + 1);
            mine = i == me;
            show_name(e->name, e->alias, twin_of(rows, n, i), mine, shown, sizeof shown);
            int pct = name_fit(shown);
            if (pct < 100)
                snprintf(fit, sizeof fit, "<font size='%d%%'>%s</font>", pct, shown);
            name = pct < 100 ? fit : shown;
            car_name(e->car, car, sizeof car);
            snprintf(tm, sizeof tm, "%02d:%02d.<font size='80%%'>%03d</font>", e->ms / 60000, e->ms / 1000 % 60,
                     e->ms % 1000);
        } else if (r == 1 && !n) {
            name = "No times yet";
            snprintf(car, sizeof car, "Race the Time Trial");
        } else if (r == 1 && tab == 1 && me < 0) {
            name = "No time of yours yet";
        }
        set_cell(pl, "txtNumber", r, num, mine);
        set_cell(pl, "txtPlayer", r, name, mine);
        set_cell(pl, "txtCar", r, car, mine);
        set_cell(pl, "txtTimeNbr", r, tm, mine);
    }
}

static void fake_record(void)
{
    const char *e = dev_getenv("GT_RED_BULL_BOARD_FAKE");
    static int done;
    if (!e || done)
        return;
    done = 1;
    u32 g = rd32(GT_PROFILE_SLOT), owner = g ? rd32(g) : 0;
    if (!owner)
        return;
    static u32 entry[11];
    entry[0] = 0;
    entry[1] = (u32)atoi(e);
    const char *c = strchr(e, ':');
    entry[2] = c ? (u32)atoi(c + 1) : 411;
    memcpy(&entry[3], GP(ENTRY_PATTERN), 0x20);
    u32 args[5] = {owner, BOARD_NO, 0, 0, GA(entry)};
    rt_guest_call(rt_cpu(), FN_RECORD_ADD, 5, args);
    rt_log("[board] test: the game's record 15 given %u ms, car %u\n", entry[1], entry[2]);
}

static void rb_update_hook(CPU *c)
{
    u32 self = c->r[0];
    if (on <= 0) {
        rb_update_fn(c);
        return;
    }
    wr32(self + RB_STATE, 1);
    wr32(self + RB_TIMEOUT, 0);
    wr32(self + RB_FLAGS, 0);
    u8 online = *(u8 *)GP(self + RB_ONLINE);
    u32 err = rd32(NET_ERROR), g = gt_game(), req = g ? rd32(g + G_ONLINE_REQ) : 0;
    *(u8 *)GP(self + RB_ONLINE) = 0;
    if (g)
        wr32(g + G_ONLINE_REQ, 0);
    rb_update_fn(c);
    *(u8 *)GP(self + RB_ONLINE) = online;
    wr32(NET_ERROR, err);
    if (g)
        wr32(g + G_ONLINE_REQ, req);
    u32 pl = rd32(self + 4);
    if (!pl)
        return;
    static u32 frames;
    static DWORD last_tick;
    static int last_tab = -1;
    static LONG last_gen = -1;
    int tab = (int)rd32(self + RB_TAB) == 1;
    DWORD now = GetTickCount();
    int entered = now - last_tick > 300;
    last_tick = now;
    frames++;
    if (entered || tab != last_tab || generation != last_gen || frames % 30 == 0) {
        if (entered || frames % 120 == 0) {
            fake_record();
            own_sync();
            cloud_kick(entered);
        }
        last_tab = tab, last_gen = generation;
        draw(pl, tab);
    }
}

static void (*rb_enter_fn)(CPU *);
static void rb_enter_hook(CPU *c)
{
    u32 g = gt_game(), req = g ? rd32(g + G_ONLINE_REQ) : 0;
    if (on <= 0 || !g) {
        rb_enter_fn(c);
        return;
    }
    wr32(g + G_ONLINE_REQ, 0);
    rb_enter_fn(c);
    wr32(g + G_ONLINE_REQ, req);
    rt_log("[board] " MENU " opened%s\n", req ? " (the online request set aside)" : "");
}

static void rb_error_hook(CPU *c)
{
    static int said;
    if (!said++)
        rt_log("[board] network error %u for " MENU ": not shown (no server)\n", c->r[1]);
}

void __real_F_212170(CPU *c);
void __wrap_F_212170(CPU *c)
{
    if (on > 0 && c->r[0] && rd32(c->r[0]) == RB_VTBL) {
        static int said;
        if (!said++)
            rt_log("[board] " MENU ": its \"Connection Lost\" (text %u) is not shown (no server)\n", c->r[1]);
        return;
    }
    __real_F_212170(c);
}

void board_init(void)
{
    const char *e = getenv("GT_RED_BULL_BOARD");
    on = !(e && *e == '0');
    lock_init();
    if (!on) {
        rt_log("[board] off (GT_RED_BULL_BOARD=0): the Red Bull leaderboard asks Gameloft LIVE\n");
        return;
    }
    if (!gt_hook_slot(RB_UPDATE_SLOT, RB_UPDATE_FN, &rb_update_fn, rb_update_hook, "board rb_update_hook")) {
        rt_log("[board] " MENU "'s update is not the expected one: off\n");
        on = 0;
        return;
    }
    gt_hook_slot(RB_ENTER_SLOT, RB_ENTER_FN, &rb_enter_fn, rb_enter_hook, "board rb_enter_hook");
    gt_hook_slot(RB_ERROR_SLOT, RB_ERROR_FN, NULL, rb_error_hook, "board rb_error_hook");
    rt_log("[board] the Red Bull leaderboard is this PC's and the friends' (no Gameloft LIVE)\n");
    cloud_init();
}

int board_selftest(char *why, size_t len)
{
    static Entry saved[MAX_ENTRIES];
    char s_id[12], s_alias[12], buf[512], a[12];
    int s_n, s_loaded, s_sent, s_state, fails = 0;
#define BT(cond, ...) do { if (!(cond) && !fails++) snprintf(why, len, __VA_ARGS__); } while (0)
    lock_init();
    u8 d[32];
    crypto_sha256("abc", 3, d);
    BT(d[0] == 0xba && d[1] == 0x78 && d[30] == 0x15 && d[31] == 0xad, "SHA-256(abc) wrong");
    char m[200];
    memset(m, 'a', sizeof m);
    crypto_sha256(m, 200, d);
    BT(d[0] == 0xc2 && d[1] == 0xa9 && d[31] == 0xe5, "SHA-256(200 x a) wrong: %02x%02x..%02x", d[0], d[1], d[31]);
    alias_of("0123abcd", a);
    BT(!strcmp(a, "edd6d2700d"), "alias(0123abcd) = %s, worker.js says edd6d2700d", a);

    static Entry rows[16];
    int n = cloud_parse(" {\"v\":1,\"rows\":[{\"id\":\"edd6d2700d\",\"name\":\"Me \\\"x\\\"\",\"car\":411,\"time_ms\":77000},"
                        "{\"id\":\"aaaaaaaaaa\",\"name\":\"Fr\\u0069end\\n\",\"car\":411,\"time_ms\":76000,\"x\":[1,{\"a\":[]}]},"
                        "{\"id\":\"BAD\",\"name\":\"x\",\"car\":411,\"time_ms\":80000},"
                        "{\"id\":\"bbbbbbbbbb\",\"name\":\"Slow\",\"car\":411,\"time_ms\":700000},"
                        "{\"id\":\"cccccccccc\",\"name\":\"Frac\",\"car\":411,\"time_ms\":80000.5},"
                        "{\"id\":\"dddddddddd\",\"name\":\"Car\",\"car\":412,\"time_ms\":80000},"
                        "{\"id\":\"ffffffffff\",\"name\":7,\"car\":411,\"time_ms\":80000},"
                        "{\"id\":\"eeeeeeeeee\",\"name\":\"<b>Ok</b> \\u00e9\",\"car\":411,\"time_ms\":81000},"
                        "[1],\"junk\"]} ", rows, 16);
    BT(n == 3, "the server's sample gave %d rows, not 3", n);
    BT(n == 3 && !strcmp(rows[0].name, "Me \"x\"") && !strcmp(rows[1].name, "Friend") && !strcmp(rows[2].name, "bOk/b ") &&
       rows[2].ms == 81000 && !strcmp(rows[2].alias, "eeeeeeeeee"), "the server's rows read wrong");
    BT(cloud_parse("{\"rows\":[{\"id\":\"aaaaaaaaaa\"", rows, 16) < 0 && cloud_parse("<html>", rows, 16) < 0 &&
       cloud_parse("", rows, 16) < 0 && cloud_parse("{\"rows\":[]}", rows, 16) == 0, "a broken answer read as a board");

    cloud_body(buf, sizeof buf, "0123abcd", "A\"b\\c", 77736);
    BT(!strcmp(buf, "{\"player_id\":\"0123abcd\",\"name\":\"A\\\"b\\\\c\",\"car\":411,\"time_ms\":77736}"), "body %s", buf);

    CloudUrl u;
#ifdef _WIN32
    BT(cloud_crack("https://board.example.dev/", &u) && u.secure && u.port == 443 && !wcscmp(u.host, L"board.example.dev") &&
       !u.base[0], "https URL");
    BT(cloud_crack("https://x.example.dev:8443/api/", &u) && u.port == 8443 && !wcscmp(u.base, L"/api"), "URL with a path");
    BT(cloud_crack("http://127.0.0.1:8787", &u) && !u.secure && u.port == 8787, "http to this PC");
#else
    BT(cloud_crack("https://board.example.dev/", &u) && u.secure && !strcmp(u.base, "https://board.example.dev"),
       "https URL");
    BT(cloud_crack("https://x.example.dev:8443/api/", &u) && !strcmp(u.base, "https://x.example.dev:8443/api"),
       "URL with a path");
    BT(cloud_crack("http://127.0.0.1:8787", &u) && !u.secure && cloud_crack("http://[::1]:8787/", &u),
       "http to this PC");
#endif
    BT(!cloud_crack("http://board.example.dev", &u) && !cloud_crack("ftp://x.dev", &u) && !cloud_crack("", &u) &&
       !cloud_crack("https://u:p@x.dev", &u) && !cloud_crack("https://x.dev/?k=1", &u), "a URL not refused");

    EnterCriticalSection(&lock);
    memcpy(saved, board, sizeof saved);
    s_n = n_board, s_loaded = loaded, s_sent = cl_sent_ms, s_state = cl_state_loaded;
    memcpy(s_id, my_id, sizeof s_id);
    memcpy(s_alias, my_alias, sizeof s_alias);
    no_save = 1, loaded = 1, n_board = 0, cl_state_loaded = 1;
    snprintf(my_id, sizeof my_id, "0123abcd");
    alias_of(my_id, my_alias);
    put(my_id, 80000, 411, "Me");
    put("89abcdef", 79000, 411, "Friend");
    LeaveCriticalSection(&lock);
    char fa[12], na[12];
    alias_of("89abcdef", fa);
    alias_of("12345678", na);
    Entry in[4] = {{.ms = 60000, .car = 411, .name = "Thief"}, {.ms = 78000, .car = 411, .name = "Cloud name"},
                   {.ms = 85000, .car = 411, .name = "New"}, {.ms = 86000, .car = 411, .name = "Later met"}};
    snprintf(in[0].id, 12, "%s", my_alias);
    snprintf(in[1].id, 12, "%s", fa);
    snprintf(in[2].id, 12, "aaaaaaaaaa");
    snprintf(in[3].id, 12, "%s", na);
    for (int i = 0; i < 4; i++)
        memcpy(in[i].alias, in[i].id, sizeof in[i].alias);
    int ch = cloud_merge(in, 4);
    Entry *me = find(my_id), *fr = find("89abcdef");
    BT(me && me->ms == 80000 && !strcmp(me->name, "Me"), "the server changed our own line");
    BT(fr && fr->ms == 78000 && !strcmp(fr->name, "Friend") && !find(fa), "a friend's server row: not merged by alias");
    BT(ch == 3 && n_board == 4 && find("aaaaaaaaaa") && find(na), "the server's new players: %d lines, %d changes", n_board, ch);
    snprintf(buf, sizeof buf, "GTLB1 12345678\n12345678\t84000\t411\tLater met\n%s\t1000\t411\tThief\n0123abcd\t1000\t411\tX\n",
             my_alias);
    merge(buf, (u32)strlen(buf), "selftest");
    Entry *lm = find("12345678");
    BT(lm && !find(na) && lm->ms == 84000 && !strcmp(lm->alias, na) && n_board == 4,
       "a server player met in multiplayer: not one line under the own id");
    BT(me->ms == 80000 && !strcmp(me->name, "Me"), "a multiplayer line changed our own");

    char out[40];
    show_name("Racer", "edd6d2700d", 0, 0, out, sizeof out);
    BT(!strcmp(out, "Racer"), "unique name: %s", out);
    show_name("iPad", "edd6d2700d", 0, 0, out, sizeof out);
    BT(!strcmp(out, "iPad#EDD6"), "iPad: %s", out);
    show_name("IPAD", "0b1c2d3e4f", 0, 0, out, sizeof out);
    BT(!strcmp(out, "IPAD#0B1C"), "IPAD: %s", out);
    show_name("Sixteen Letters!", "abcdef0123", 1, 0, out, sizeof out);
    BT(!strcmp(out, "Sixteen Let#ABCD") && strlen(out) == SHOW_LEN, "16-letter twin: %s", out);
    show_name("Racer", "edd6d2700d", 0, 1, out, sizeof out);
    BT(!strcmp(out, "Racer (You)"), "own row: %s", out);
    show_name("iPad", "edd6d2700d", 0, 1, out, sizeof out);
    BT(!strcmp(out, "iPad#EDD6 (You)"), "own iPad row: %s", out);
    show_name("Sixteen Letters!", "abcdef0123", 1, 1, out, sizeof out);
    BT(!strcmp(out, "Sixteen Let#ABCD"), "own 16-letter twin, no room for (You): %s", out);
    show_name("Sixteen Letters!", "abcdef0123", 0, 1, out, sizeof out);
    BT(!strcmp(out, "Sixteen Letters!"), "own 16-letter row: %s", out);
    show_name("Twenty letters long!", "abcdef0123", 0, 0, out, sizeof out);
    BT(!strcmp(out, "Twenty letters l"), "a 20-letter name: %s", out);
    Entry tw[4] = {{.name = "iPad"}, {.name = "ipad"}, {.name = "Sixteen Letters!"}, {.name = "SIXTEEN LETTERS!xy"}};
    BT(twin_of(tw, 4, 0) && twin_of(tw, 4, 2) && twin_of(tw, 4, 3) && !twin_of(tw, 3, 2), "twins");

    EnterCriticalSection(&lock);
    memcpy(board, saved, sizeof saved);
    n_board = s_n, loaded = s_loaded, cl_sent_ms = s_sent, cl_state_loaded = s_state, no_save = 0;
    memcpy(my_id, s_id, sizeof s_id);
    memcpy(my_alias, s_alias, sizeof s_alias);
    LeaveCriticalSection(&lock);
#undef BT
    return fails;
}
