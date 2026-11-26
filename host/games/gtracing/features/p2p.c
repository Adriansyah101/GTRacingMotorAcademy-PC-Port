#define _CRT_RAND_S
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include "compat_net.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "features.h"
#include "game.h"
#include "net.h"

u16 net_p2p_open(u16 port);
void net_p2p_reset(void);
void net_p2p_stop(void);
void net_p2p_add_peer(u32 ip_be, u16 port_be);
void net_p2p_set_key(u32 key);

#define PORT_HOST 28190
#define PORT_SEARCH 54046
#define LOBBY_VTBL 0x787ef8u
#define LOBBY_UPDATE_SLOT (LOBBY_VTBL + 0x1c)
#define LOBBY_UPDATE_FN 0x1b4c64u

static int p2p_online;
static volatile int host_state;
static char room_code[32], host_msg[256];
static int joining;
static u32 join_ip, join_key, host_key;
static u16 join_port;
static volatile int form_kind, form_enter, form_open_code, name_form_pending;
static HANDLE host_thread;
static volatile LONG host_gen;

static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static u8 crc8(const u8 *p, int n)
{
    u8 c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (u8)(c & 0x80 ? c << 1 ^ 0x07 : c << 1);
    }
    return c;
}

static void code_make(u32 ip_be, u16 port_be, u32 key, char *out)
{
    u8 b[10];
    memcpy(b, &ip_be, 4);
    memcpy(b + 4, &port_be, 2);
    b[6] = (u8)(key >> 16), b[7] = (u8)(key >> 8), b[8] = (u8)key;
    b[9] = crc8(b, 9);
    char d[16];
    int acc = 0, nb = 0, k = 0;
    for (int i = 0; i < 10; i++) {
        acc = acc << 8 | b[i];
        nb += 8;
        while (nb >= 5) {
            nb -= 5;
            d[k++] = B32[acc >> nb & 31];
        }
    }
    sprintf(out, "GT-%.4s-%.4s-%.4s-%.4s", d, d + 4, d + 8, d + 12);
}

static int b32v(int ch)
{
    ch = toupper(ch);
    if (ch == 'O')
        ch = '0';
    if (ch == 'I' || ch == 'L')
        ch = '1';
    const char *p = ch ? strchr(B32, ch) : NULL;
    return p ? (int)(p - B32) : -1;
}

static int code_parse(const char *t, u32 *ip_be, u16 *port_be, u32 *key)
{
    int bad = 0;
    for (const char *p = t; p && *p; p++) {
        if (toupper(p[0]) != 'G' || toupper(p[1]) != 'T' || p[2] != '-')
            continue;
        u8 b[10];
        int acc = 0, nb = 0, k = 0, n = 0;
        for (const char *q = p + 3; *q && n < 16; q++) {
            if (*q == '-' || *q == ' ')
                continue;
            int d = b32v((u8)*q);
            if (d < 0)
                break;
            n++;
            acc = acc << 5 | d;
            nb += 5;
            if (nb >= 8) {
                nb -= 8;
                b[k++] = (u8)(acc >> nb);
            }
        }
        if (n != 16 || k != 10 || crc8(b, 9) != b[9]) {
            bad = 1;
            continue;
        }
        memcpy(ip_be, b, 4);
        memcpy(port_be, b + 4, 2);
        *key = (u32)b[6] << 16 | (u32)b[7] << 8 | b[8];
        if (*ip_be && *port_be)
            return 1;
        bad = 1;
    }
    return bad ? -1 : 0;
}

static void clip_put(const char *s)
{
    const char *f = dev_getenv("GT_P2P_CODE_FILE");
    if (f && *f) {
        FILE *o = fopen(f, "w");
        if (o) {
            fputs(s, o);
            fclose(o);
        }
        return;
    }
#ifdef _WIN32
    if (!OpenClipboard(NULL))
        return;
    EmptyClipboard();
    size_t n = strlen(s) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * 2);
    if (h) {
        WCHAR *w = GlobalLock(h);
        for (size_t i = 0; i < n; i++)
            w[i] = (WCHAR)(u8)s[i];
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h))
            GlobalFree(h);
    }
    CloseClipboard();
#else
    SDL_SetClipboardText(s);
#endif
}

static int clip_get(char *out, size_t cap)
{
    out[0] = 0;
    const char *f = dev_getenv("GT_P2P_CODE_FILE");
    if (f && *f) {
        FILE *i = fopen(f, "r");
        if (!i)
            return 0;
        size_t n = fread(out, 1, cap - 1, i);
        out[n] = 0;
        fclose(i);
        return n > 0;
    }
#ifdef _WIN32
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
        return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = h ? GlobalLock(h) : NULL;
    if (w) {
        size_t i = 0;
        for (; w[i] && i < cap - 1; i++)
            out[i] = w[i] < 128 ? (char)w[i] : ' ';
        out[i] = 0;
        GlobalUnlock(h);
    }
    CloseClipboard();
#else
    char *t = SDL_GetClipboardText();
    if (t) {
        size_t i = 0;
        for (; t[i] && i < cap - 1; i++)
            out[i] = (u8)t[i] < 128 ? t[i] : ' ';
        out[i] = 0;
        SDL_free(t);
    }
#endif
    return out[0] != 0;
}

static int is_private(u32 ip_be)
{
    u32 h = ntohl(ip_be);
    return (h >> 24) == 10 || (h >> 24) == 127 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8 ||
           (h >> 16) == 0xa9fe || (h >> 22) == (0x64400000u >> 22) || !h;
}

#ifdef _WIN32
static int default_route(u32 *gw_be, u32 *local_be)
{
    MIB_IPFORWARDROW r;
    if (GetBestRoute(inet_addr("8.8.8.8"), 0, &r) != NO_ERROR)
        return 0;
    *gw_be = r.dwForwardNextHop;
    *local_be = 0;
    ULONG len = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = malloc(len);
    if (aa && GetAdaptersAddresses(AF_INET, 0, NULL, aa, &len) == NO_ERROR)
        for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next)
            if (a->IfIndex == r.dwForwardIfIndex && a->FirstUnicastAddress)
                *local_be = ((struct sockaddr_in *)a->FirstUnicastAddress->Address.lpSockaddr)->sin_addr.s_addr;
    free(aa);
    return *gw_be != 0;
}
#else
static int default_route(u32 *gw_be, u32 *local_be)
{
    *gw_be = *local_be = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return 0;
    char line[256], dev[IFNAMSIZ] = "";
    unsigned long dst, gw, flags;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%15s %lx %lx %lx", dev, &dst, &gw, &flags) == 4 && !dst && (flags & 3) == 3) {
            *gw_be = (u32)gw;
            break;
        }
    fclose(f);
    struct ifaddrs *ia;
    if (*gw_be && !getifaddrs(&ia)) {
        for (struct ifaddrs *a = ia; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, dev)) {
                *local_be = ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr;
                break;
            }
        freeifaddrs(ia);
    }
    return *gw_be != 0;
}
#endif

static int natpmp(u32 gw_be, u16 port, u32 lifetime, u32 *ext_ip, u16 *ext_port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD to = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    struct sockaddr_in g = {0};
    g.sin_family = AF_INET;
    g.sin_addr.s_addr = gw_be;
    g.sin_port = htons(5351);
    u8 rq[12], rs[16];
    int ok = 0;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        rq[0] = 0, rq[1] = 0;
        sendto(s, (const char *)rq, 2, 0, (struct sockaddr *)&g, sizeof g);
        int n = recv(s, (char *)rs, sizeof rs, 0);
        if (n >= 12 && rs[1] == 128 && !rs[2] && !rs[3]) {
            memcpy(ext_ip, rs + 8, 4);
            ok = 1;
        }
    }
    if (ok) {
        ok = 0;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            memset(rq, 0, sizeof rq);
            rq[1] = 1;
            rq[4] = (u8)(port >> 8), rq[5] = (u8)port, rq[6] = (u8)(port >> 8), rq[7] = (u8)port;
            rq[8] = (u8)(lifetime >> 24), rq[9] = (u8)(lifetime >> 16), rq[10] = (u8)(lifetime >> 8), rq[11] = (u8)lifetime;
            sendto(s, (const char *)rq, 12, 0, (struct sockaddr *)&g, sizeof g);
            int n = recv(s, (char *)rs, sizeof rs, 0);
            if (n >= 16 && rs[1] == 129 && !rs[2] && !rs[3]) {
                *ext_port = (u16)(rs[10] << 8 | rs[11]);
                ok = 1;
            }
        }
    }
    closesocket(s);
    return ok;
}

static int http(u32 ip_be, u16 port, const char *req, char *out, int cap, u32 *local_be)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip_be;
    a.sin_port = htons(port);
    int n = 0;
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        if (local_be) {
            struct sockaddr_in l;
            int ll = sizeof l;
            getsockname(s, (struct sockaddr *)&l, &ll);
            *local_be = l.sin_addr.s_addr;
        }
        send(s, req, (int)strlen(req), 0);
        for (int r; n < cap - 1 && (r = recv(s, out + n, cap - 1 - n, 0)) > 0;)
            n += r;
    }
    closesocket(s);
    out[n] = 0;
    return n;
}

static int url_split(const char *url, u32 *ip_be, u16 *port, char *path, size_t cap)
{
    char host[64];
    unsigned p = 80;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t i = 0;
    while (h[i] && h[i] != ':' && h[i] != '/' && i < sizeof host - 1)
        host[i] = h[i], i++;
    host[i] = 0;
    h += i;
    if (*h == ':')
        p = (unsigned)strtoul(h + 1, (char **)&h, 10);
    snprintf(path, cap, "%s", *h ? h : "/");
    *port = (u16)p;
    return inet_pton(AF_INET, host, ip_be) == 1;
}

static const char *tag(const char *x, const char *name, char *out, size_t cap)
{
    char open[64];
    snprintf(open, sizeof open, "<%s>", name);
    const char *p = x ? strstr(x, open) : NULL;
    if (!p)
        return NULL;
    p += strlen(open);
    const char *e = strchr(p, '<');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return p;
}

static int soap(u32 ip_be, u16 port, const char *path, const char *svc, const char *action, const char *args,
                char *out, int cap)
{
    char body[1536], req[2560];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n", action, svc, args, action);
    char a[16];
    inet_ntop(AF_INET, &ip_be, a, sizeof a);
    snprintf(req, sizeof req,
             "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
             path, a, port, svc, action, (unsigned)strlen(body), body);
    int n = http(ip_be, port, req, out, cap, NULL);
    return n > 0 && strstr(out, " 200 ");
}

static struct { u32 ip; u16 port, ext; char path[256]; const char *svc; } um;
static void upnp_unmap(void)
{
    if (!um.svc)
        return;
    char args[256], out[2048];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                "<NewProtocol>UDP</NewProtocol>", um.ext);
    soap(um.ip, um.port, um.path, um.svc, "DeletePortMapping", args, out, sizeof out);
    um.svc = NULL;
}

static int upnp(u32 gw_be, u16 *port, u32 *ext_ip, u16 *ext_port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD to = 700;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    struct sockaddr_in m = {0};
    m.sin_family = AF_INET;
    m.sin_addr.s_addr = inet_addr("239.255.255.250");
    m.sin_port = htons(1900);
    static const char *const st[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                     "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                     "urn:schemas-upnp-org:service:WANIPConnection:1"};
    char loc[256] = "", buf[2048];
    for (int round = 0; round < 3 && !loc[0]; round++)
        for (int k = 0; k < 3 && !loc[0]; k++) {
            char q[256];
            snprintf(q, sizeof q, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                  "MX: 1\r\nST: %s\r\n\r\n", st[k]);
            sendto(s, q, (int)strlen(q), 0, (struct sockaddr *)&m, sizeof m);
            struct sockaddr_in g = m;
            g.sin_addr.s_addr = gw_be;
            sendto(s, q, (int)strlen(q), 0, (struct sockaddr *)&g, sizeof g);
            for (;;) {
                struct sockaddr_in from;
                int fl = sizeof from;
                int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0)
                    break;
                buf[n] = 0;
                if (from.sin_addr.s_addr != gw_be)
                    continue;
                for (char *l = buf; l && *l; l = strstr(l, "\r\n") ? strstr(l, "\r\n") + 2 : NULL)
                    if (!_strnicmp(l, "LOCATION:", 9)) {
                        const char *v = l + 9;
                        while (*v == ' ')
                            v++;
                        size_t i = 0;
                        while (v[i] && v[i] != '\r' && i < sizeof loc - 1)
                            loc[i] = v[i], i++;
                        loc[i] = 0;
                        break;
                    }
                if (loc[0])
                    break;
            }
        }
    closesocket(s);
    if (!loc[0]) {
        rt_log("[p2p] UPnP: no answer from the router\n");
        return 0;
    }
    u32 dip;
    u16 dport;
    char path[256];
    if (!url_split(loc, &dip, &dport, path, sizeof path))
        return 0;
    static char desc[65536];
    char req[512], dh[16];
    inet_ntop(AF_INET, &dip, dh, sizeof dh);
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, dh, dport);
    u32 local = 0;
    if (http(dip, dport, req, desc, sizeof desc, &local) <= 0)
        return 0;
    static const char *const svcs[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                       "urn:schemas-upnp-org:service:WANIPConnection:1",
                                       "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (int k = 0; k < 3; k++) {
        const char *sv = strstr(desc, svcs[k]);
        char ctl[256];
        if (!sv || !tag(sv, "controlURL", ctl, sizeof ctl))
            continue;
        u32 cip = dip;
        u16 cport = dport;
        char cpath[256];
        if (strstr(ctl, "://"))
            url_split(ctl, &cip, &cport, cpath, sizeof cpath);
        else if (snprintf(cpath, sizeof cpath, "%s%s", ctl[0] == '/' ? "" : "/", ctl) >= (int)sizeof cpath)
            continue;
        char out[4096], args[768], lip[16], ip[32];
        inet_ntop(AF_INET, &local, lip, sizeof lip);
        if (!soap(cip, cport, cpath, svcs[k], "GetExternalIPAddress", "", out, sizeof out) ||
            !tag(out, "NewExternalIPAddress", ip, sizeof ip) || inet_pton(AF_INET, ip, ext_ip) != 1)
            continue;
        u16 base = *port;
        for (int t = 0; t < 10; t++) {
            u16 p = (u16)(base + t);
            if (t && net_p2p_open(p) != p)
                continue;
            snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                        "<NewProtocol>UDP</NewProtocol>", p);
            char owner[32];
            if (soap(cip, cport, cpath, svcs[k], "GetSpecificPortMappingEntry", args, out, sizeof out) &&
                tag(out, "NewInternalClient", owner, sizeof owner) && strcmp(owner, lip)) {
                rt_log("[p2p] UPnP: UDP %u is another PC's (%s), trying the next port\n", p, owner);
                continue;
            }
            for (int lease = 0; lease < 2; lease++) {
                snprintf(args, sizeof args,
                         "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                         "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
                         "<NewEnabled>1</NewEnabled><NewPortMappingDescription>GT Racing online</NewPortMappingDescription>"
                         "<NewLeaseDuration>%u</NewLeaseDuration>", p, p, lip, lease ? 0u : 7200u);
                if (soap(cip, cport, cpath, svcs[k], "AddPortMapping", args, out, sizeof out)) {
                    *port = p, *ext_port = p;
                    rt_log("[p2p] UPnP: UDP %u mapped on the router (%s)\n", p, svcs[k] + 27);
                    um.ip = cip, um.port = cport, um.ext = p;
                    snprintf(um.path, sizeof um.path, "%s", cpath);
                    um.svc = svcs[k];
                    static int once;
                    if (!once++)
                        atexit(upnp_unmap);
                    return 1;
                }
            }
            rt_log("[p2p] UPnP: the router refused UDP %u, trying the next port\n", p);
        }
        net_p2p_open(base);
        rt_log("[p2p] UPnP: the router refused the mapping\n");
        return 0;
    }
    rt_log("[p2p] UPnP: no WAN connection service in the router's description\n");
    return 0;
}

static DWORD WINAPI host_work(void *arg)
{
    LONG gen = (LONG)(intptr_t)arg;
    u16 port = net_p2p_open((u16)rt_setting_int("GT_P2P_PORT", 28200, 1, 65535));
    if (!port) {
        host_state = 3;
        return 0;
    }
    u32 ext_ip = 0;
    u16 ext_port = 0;
    int natpmp_on = 0;
    u32 gw = 0, local = 0;
    if (net_test_ip()) {
        ext_ip = net_test_ip(), ext_port = port;
    } else if (default_route(&gw, &local)) {
        if (natpmp(gw, port, 60, &ext_ip, &ext_port) && !is_private(ext_ip)) {
            natpmp_on = 1;
            if (ext_port != port) {
                u16 moved = net_p2p_open(ext_port);
                if (moved == ext_port) {
                    port = moved;
                    u32 ip2;
                    u16 p2;
                    if (natpmp(gw, port, 60, &ip2, &p2))
                        ext_port = p2;
                }
            }
            rt_log("[p2p] NAT-PMP: UDP %u open as %u on the gateway\n", port, ext_port);
        } else if (!upnp(gw, &port, &ext_ip, &ext_port) || is_private(ext_ip)) {
            ext_ip = 0;
        }
    }
    if (gen != host_gen)
        return 0;
    if (!ext_ip || !ext_port) {
        snprintf(host_msg, sizeof host_msg, "ONLINE: your router did not open a port (UPnP off, or no public "
                                            "address). Turn on ProtonVPN Port Forwarding, or let your friend host.");
        host_state = 3;
        rt_log("[p2p] hosting: no open port\n");
        return 0;
    }
    code_make(ext_ip, htons(ext_port), host_key, room_code);
    clip_put(room_code);
    snprintf(host_msg, sizeof host_msg, "ROOM CODE  %s  (copied) - send it to your friend", room_code);
    host_state = 2;
    rt_log("[p2p] hosting: room code copied (UDP %u)\n", ext_port);
    while (natpmp_on && gen == host_gen) {
        Sleep(45000);
        u32 ip2;
        u16 p2;
        if (gen == host_gen && natpmp(gw, port, 60, &ip2, &p2) && p2 != ext_port)
            rt_log("[p2p] NAT-PMP: the gateway moved the port to %u\n", p2);
    }
    return 0;
}

static void host_start(void)
{
    LONG gen = InterlockedIncrement(&host_gen);
    host_state = 1;
    room_code[0] = 0;
    snprintf(host_msg, sizeof host_msg, "ONLINE: opening the port...");
    net_p2p_reset();
    unsigned r = 0;
    while (!(r & 0xffffff))
        rand_s(&r);
    host_key = r & 0xffffff;
    net_p2p_set_key(host_key);
    joining = 0;
    if (host_thread)
        CloseHandle(host_thread);
    host_thread = CreateThread(NULL, 0, host_work, (void *)(intptr_t)gen, 0, NULL);
    rt_log("[p2p] hosting an online server\n");
}

static void join_start(void)
{
    InterlockedIncrement(&host_gen);
    host_state = 0;
    joining = 1;
    join_ip = 0, join_port = 0, join_key = 0;
    net_p2p_open(0);
    net_p2p_reset();
    form_open_code = 1;
}

void p2p_bound(u16 port)
{
    if (!p2p_online)
        return;
    if (port == PORT_HOST)
        host_start();
    else if (port == PORT_SEARCH)
        join_start();
}

void p2p_poll(void)
{
}

static DWORD redirect_until;
static int note_pending;

u32 p2p_menu_redirect(u32 name)
{
    static const char server_type[] = "ServerTypeMenu";
    if (redirect_until && (int)(redirect_until - GetTickCount()) > 0 && name &&
        !strcmp((const char *)GP(name), "GLLiveLoginMenu")) {
        redirect_until = 0;
        rt_log("[p2p] GLLiveLoginMenu -> ServerTypeMenu\n");
        return GA((void *)server_type);
    }
    return name;
}

void __real_F_1bb6c0(CPU *c);
void __wrap_F_1bb6c0(CPU *c)
{
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    if (!strcmp(name, "btnPCName") && rd32(ev + 8) == 6 && form_kind != 1)
        name_form_pending = 1;
    if (form_kind == 1 && !strcmp(name, "btnNo")) {
        if (rd32(ev + 8) == 6)
            form_enter = 1;
        static const char none[] = "";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)none));
        __real_F_1bb6c0(c);
        wr32(ev + 4, old);
        return;
    }
    if (rd32(ev + 8) != 6 || !net_lan_on()) {
        __real_F_1bb6c0(c);
        return;
    }
    int online = !strcmp(name, "btnOnline"), online2 = !strcmp(name, "btnOnline2");
    if (online || online2) {
        static const char local[] = "btnWireless", local2[] = "btnWireless2";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)(online ? local : local2)));
        p2p_online = 1;
        redirect_until = GetTickCount() + 3000;
        note_pending = 1;
        rt_log("[p2p] Online: the Local game over the internet (room codes)\n");
        __real_F_1bb6c0(c);
        wr32(ev + 4, old);
        return;
    }
    if (!strcmp(name, "btnWireless") || !strcmp(name, "btnWireless2") || !strcmp(name, "btnBluetooth")) {
        if (p2p_online) {
            p2p_online = 0;
            joining = 0;
            InterlockedIncrement(&host_gen);
            host_state = 0;
            net_p2p_stop();
        }
    }
    __real_F_1bb6c0(c);
}

static void (*lobby_update_fn)(CPU *);
static void lobby_update_hook(CPU *c)
{
    u32 self = c->r[0];
    lobby_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl || !p2p_online || !host_state)
        return;
    static char shown[256];
    static u32 shown_pl;
    static const char box[] = "LobbyMenu.ServerMessage", field[] = "LobbyMenu.ServerMessage.txtField";
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)field)))
        return;
    if (strcmp(shown, host_msg) || shown_pl != pl) {
        snprintf(shown, sizeof shown, "%s", host_msg);
        shown_pl = pl;
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)field), GA(shown), 0);
    }
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)box), 1);
}

#define STYPE_UPDATE_SLOT (0x7864f8u + 0x1c)
#define STYPE_UPDATE_FN 0x1c1d18u
static void (*stype_update_fn)(CPU *);
static void stype_update_hook(CPU *c)
{
    u32 self = c->r[0];
    stype_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl || !p2p_online)
        return;
    static const char *const fields[] = {"ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.NoCashText.TxtNoCash",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.NoCashText",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.field",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses",
                                         "NoUpNp.PopupCongrats.txtExcuses", "ServerTypeMenu.NoUpNp.PopupCongrats.TxtCongrats",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats", "ServerTypeMenu.NoUpNp.PopupCongrats.btnOk",
                                         NULL};
    static DWORD at;
    static char text[400], shown[400];
    static u32 shown_pl;
    if (GetTickCount() - at > 1000 || shown_pl != pl) {
        at = GetTickCount();
        char t[1024], code[40] = "";
        u32 ip, key;
        u16 port;
        int got = clip_get(t, sizeof t) ? code_parse(t, &ip, &port, &key) : 0;
        if (got == 1)
            code_make(ip, port, key, code);
        if (code[0])
            snprintf(text, sizeof text, "ONLINE: room code %s found. Press Join server. To host, press "
                                        "Host server.", code);
        else if (got < 0)
            snprintf(text, sizeof text, "ONLINE: the room code in the clipboard is not right (a typo?). Copy "
                                        "it again from your friend's message.");
        else
            snprintf(text, sizeof text, "ONLINE: to host, press Host server and send the room code. To join, "
                                        "press Join server and type your friend's code.");
    }
    static int rewrite;
    if (note_pending)
        rewrite = 120;
    if (rewrite > 0 && rewrite-- % 10 == 0)
        shown[0] = 0;
    if (strcmp(shown, text) || shown_pl != pl) {
        snprintf(shown, sizeof shown, "%s", text);
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)"ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses"),
                 GA((void *)"NoCash"), 0);
        if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)fields[0])))
            rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)fields[0]), GA(shown), 1);
        shown_pl = pl;
    }
    if (note_pending) {
        note_pending = 0;
        rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)"ServerTypeMenu.NoUpNp"), 1);
    }
}

#define NAME_MAX 16
#define GT_UPDATE_SLOT (0x785f78u + 0x1c)
#define GT_UPDATE_FN 0x1bb57cu
#define JOIN_UPDATE_SLOT (0x7865d8u + 0x1c)
#define JOIN_UPDATE_FN 0x1c26fcu
#define JOIN_EVENT_SLOT (0x7865d8u + 0x2c)
#define JOIN_EVENT_FN 0x1c28d4u

enum { FORM_NONE, FORM_NAME, FORM_CODE, FORM_FIRST };
static CRITICAL_SECTION form_cs;
static volatile int form_kind, form_enter, form_esc, form_open_code;
static volatile u32 form_tick;
static volatile LONG form_ver;
static char form_text[48], form_msg[96];

int p2p_form_open(void)
{
    if (form_kind && eagl_frames() - form_tick > 30) {
        form_kind = FORM_NONE;
        rt_log("[p2p] form closed: its menu was left\n");
    }
    return form_kind != FORM_NONE;
}

static void form_set(const char *s, const char *msg)
{
    EnterCriticalSection(&form_cs);
    snprintf(form_text, sizeof form_text, "%s", s);
    snprintf(form_msg, sizeof form_msg, "%s", msg ? msg : "");
    LeaveCriticalSection(&form_cs);
    InterlockedIncrement(&form_ver);
}

static int form_char_ok(int kind, int ch)
{
    if (kind == FORM_CODE)
        return isalnum(ch) || ch == '-';
    return ch >= 0x20 && ch < 0x7f && ch != ',' && ch != '\\' && ch != '"';
}

static void form_add(const char *s)
{
    EnterCriticalSection(&form_cs);
    size_t n = strlen(form_text), max = form_kind == FORM_CODE ? 22 : NAME_MAX;
    for (; *s; s++) {
        int ch = (u8)*s;
        if (form_kind == FORM_CODE)
            ch = toupper(ch);
        if (!form_char_ok(form_kind, ch))
            continue;
        if (n >= max)
            break;
        if (form_kind != FORM_CODE && ch == ' ' && (!n || form_text[n - 1] == ' '))
            continue;
        form_text[n++] = (char)ch;
    }
    form_text[n] = 0;
    form_msg[0] = 0;
    LeaveCriticalSection(&form_cs);
    InterlockedIncrement(&form_ver);
}

int p2p_form_event(const SDL_Event *e)
{
    if (!p2p_form_open())
        return 0;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        form_add(e->text.text);
        return 1;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode k = e->key.key;
        if (k == SDLK_BACKSPACE) {
            EnterCriticalSection(&form_cs);
            size_t n = strlen(form_text);
            if (n)
                form_text[n - 1] = 0;
            form_msg[0] = 0;
            LeaveCriticalSection(&form_cs);
            InterlockedIncrement(&form_ver);
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            form_enter = 1;
        } else if (k == SDLK_ESCAPE) {
            form_esc = 1;
        } else if (k == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char t[1024], code[40];
            u32 ip, key;
            u16 port;
            if (clip_get(t, sizeof t)) {
                if (form_kind == FORM_CODE && code_parse(t, &ip, &port, &key) == 1) {
                    code_make(ip, port, key, code);
                    form_set(code, NULL);
                } else {
                    char *nl = strpbrk(t, "\r\n");
                    if (nl)
                        *nl = 0;
                    if (form_kind == FORM_CODE)
                        form_set("", NULL);
                    form_add(t);
                }
            }
        }
        return 1;
    }
    case SDL_EVENT_KEY_UP:
        return 1;
    }
    return 0;
}

static char box_path[128];

static int box_on(u32 pl, const char *input)
{
    snprintf(box_path, sizeof box_path, "%s.field", input);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)))
        return 1;
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)input)))
        return 0;
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)input), GA((void *)"On"), 1);
    return rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)) ? 2 : 0;
}

static void box_off(u32 pl, const char *input)
{
    snprintf(box_path, sizeof box_path, "%s.field", input);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)))
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)input), GA((void *)"Off"), 1);
}

static void form_draw(u32 pl, const char *field, const char *input, const char *title, int opened)
{
    static LONG drawn = -1;
    static int rewrite;
    static char buf[200], val[64];
    int box = box_on(pl, input);
    if (opened)
        rewrite = 120;
    if ((rewrite > 0 && rewrite-- % 10 == 0) || box == 2)
        drawn = -1;
    if (drawn == form_ver)
        return;
    drawn = form_ver;
    EnterCriticalSection(&form_cs);
    if (box) {
        snprintf(buf, sizeof buf, "%s", form_msg[0] ? form_msg : title);
        snprintf(val, sizeof val, "%s_", form_text);
    } else {
        snprintf(buf, sizeof buf, "%s\n%s_%s%s", title, form_text, form_msg[0] ? "\n" : "", form_msg);
    }
    LeaveCriticalSection(&form_cs);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)field)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)field), GA(buf), 1);
    if (box)
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(box_path), GA(val), 1);
}

static u32 name_buf(void)
{
    u32 player = rd32(GT_PROFILE_SLOT), p = player ? rd32(player) : 0;
    return p ? rd32(p + 4) : 0;
}

static void name_get(char *out, size_t cap)
{
    u32 b = name_buf();
    out[0] = 0;
    if (!b || cap < 2)
        return;
    const u8 *p = GP(b);
    size_t n = 0;
    if (p[0] && p[1]) {
        for (; p[n] && n < 20 && n < cap - 1; n++)
            out[n] = p[n] < 0x80 ? (char)p[n] : '?';
    } else {
        for (size_t i = 0; i < 21 && n < cap - 1; i++) {
            unsigned ch = p[2 * i] | p[2 * i + 1] << 8;
            if (!ch)
                break;
            out[n++] = ch < 0x80 ? (char)ch : '?';
        }
    }
    out[n] = 0;
}

static void name_write(const char *s)
{
    u32 b = name_buf();
    if (!b)
        return;
    u8 *p = GP(b);
    memset(p, 0, 42);
    for (int i = 0; s[i] && i < 20; i++)
        p[2 * i] = (u8)s[i];
}

static char name_saved[32];
static int name_loaded;

static const char *name_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/player_name.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static void name_set(const char *s)
{
    snprintf(name_saved, sizeof name_saved, "%s", s);
    name_loaded = 1;
    char p[1024];
    FILE *f = fopen(name_path(p, sizeof p), "w");
    if (f) {
        fputs(name_saved, f);
        fclose(f);
    }
    name_write(name_saved);
    rt_log("[p2p] player name set (%u characters; the name itself is not logged)\n", (unsigned)strlen(s));
}

static void name_load(void)
{
    if (name_loaded)
        return;
    name_loaded = 1;
    char p[1024];
    FILE *f = fopen(name_path(p, sizeof p), "r");
    if (f) {
        if (fgets(name_saved, sizeof name_saved, f))
            name_saved[strcspn(name_saved, "\r\n")] = 0;
        fclose(f);
    }
}

void p2p_name_now(char *out, size_t cap)
{
    name_load();
    if (name_saved[0])
        snprintf(out, cap, "%s", name_saved);
    else
        name_get(out, cap);
    if (!out[0])
        snprintf(out, cap, "iPad");
}

static void name_apply(void)
{
    name_load();
    u32 b = name_buf();
    if (!b)
        return;
    char now[32];
    name_get(now, sizeof now);
    const u8 *q = GP(b);
    if (name_saved[0] && strcmp(now, name_saved))
        name_write(name_saved);
    else if (!name_saved[0] && q[0] && q[1] && now[0])
        name_write(now);
}

static void (*gt_update_fn)(CPU *);
static volatile int name_form_pending;
static void gt_update_hook(CPU *c)
{
    u32 self = c->r[0];
    gt_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl)
        return;
    static const char tile[] = "GameTypeMenu.btnPCName.txtTitle.field";
    static const char popup[] = "GameTypeMenu.needToUpdate", text[] = "GameTypeMenu.needToUpdate.Texts.field";
    static const char input[] = "GameTypeMenu.needToUpdate.Input";
    static char shown[64], label[64];
    static u32 shown_pl;
    char nm[32];
    name_apply();
    name_get(nm, sizeof nm);
    snprintf(label, sizeof label, "NAME: %s", nm[0] ? nm : "-");
    if ((strcmp(shown, label) || shown_pl != pl) && rt_call2(GT_FN_SWF_FIND, pl, GA((void *)tile))) {
        snprintf(shown, sizeof shown, "%s", label);
        shown_pl = pl;
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)tile), GA(shown), 1);
    }
    int opened = 0;
    if (name_form_pending) {
        name_form_pending = 0;
        form_tick = eagl_frames();
        form_kind = FORM_NAME;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : nm, NULL);
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Show"), 1);
        opened = 1;
        rt_log("[p2p] name form open\n");
    }
    if (form_kind != FORM_NAME) {
        box_off(pl, input);
        return;
    }
    form_tick = eagl_frames();
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"GameTypeMenu.needToUpdate.txtOK.field")))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)"GameTypeMenu.needToUpdate.txtOK.field"), GA((void *)"OK"), 1);
    form_draw(pl, text, input, "YOUR NAME (type it, Enter = OK):", opened);
    if (form_esc || form_enter) {
        int ok = form_enter;
        form_enter = form_esc = 0;
        char v[48];
        EnterCriticalSection(&form_cs);
        snprintf(v, sizeof v, "%s", form_text);
        LeaveCriticalSection(&form_cs);
        size_t n = strlen(v);
        while (n && v[n - 1] == ' ')
            v[--n] = 0;
        if (ok && !n) {
            form_set("", "Type a name first.");
            return;
        }
        if (ok)
            name_set(v);
        form_kind = FORM_NONE;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Hide"), 1);
    }
}

#define TUTORIAL_Q "MainMenu.Tutorial_00"
#define CH_VISIBLE 0xab
#define MAIN_UPDATE_SLOT (GT_VT_MAIN_MENU + GT_VT_UPDATE)
#define MAIN_UPDATE_FN 0x1dfb50u

static int first_ask;
static int first_seen;
static u32 first_reshow, first_labels;
static int first_menu = -1;
static int tut_hidden;
static u8 tut_want;

static char fp_popup[32], fp_yes[64], fp_txtyes[64], fp_yesf[64], fp_nof[64], fp_field[64], fp_input[64];

static void first_paths(int menu)
{
    const char *m = menu ? "NavigationMenu" : "MainMenu";
    snprintf(fp_popup, sizeof fp_popup, "%s.FlurryPopup", m);
    snprintf(fp_yes, sizeof fp_yes, "%s.btnYes", fp_popup);
    snprintf(fp_txtyes, sizeof fp_txtyes, "%s.Texts.txtYes", fp_popup);
    snprintf(fp_yesf, sizeof fp_yesf, "%s.Texts.txtYes.Field", fp_popup);
    snprintf(fp_nof, sizeof fp_nof, "%s.Texts.txtNo.Field", fp_popup);
    snprintf(fp_field, sizeof fp_field, "%s.Texts.field", fp_popup);
    snprintf(fp_input, sizeof fp_input, "%s.Input", fp_popup);
}

int p2p_first_open(void)
{
    return form_kind == FORM_FIRST;
}

static void flurry_set(u32 pl, const char *path, const char *s)
{
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)path), GA((void *)s), 0);
}

static void first_show(u32 pl)
{
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA(fp_popup), GA((void *)"Show"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA(fp_yes), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA(fp_txtyes), 1);
    first_labels = eagl_frames() + 120;
}

static u32 tut_var_back, tut_var_pl;
static u8 tut_val[16];

static void tut_var(u32 pl, int v)
{
    memset(tut_val, 0, sizeof tut_val);
    tut_val[1] = 1;
    tut_val[4] = (u8)v;
    rt_call4(GT_FN_SWF_SET_MEMBER, pl, GA((void *)"_root"), GA((void *)"isInTutorial"), GA(tut_val));
}

static u8 hint_want;
static void hide_one(u32 pl, const char *path, int hide, u8 *want)
{
    u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path));
    if (!ch)
        return;
    volatile u8 *v = (volatile u8 *)GP(ch + CH_VISIBLE);
    if (hide) {
        if (!tut_hidden || *v)
            *want = *v;
        *v = 0;
    } else {
        *v = *want;
    }
}

static void tut_hide(u32 pl, int hide)
{
    static char hint[64];
    if (!hide && !tut_hidden)
        return;
    snprintf(hint, sizeof hint, "%s.Tutorial_06", fp_popup);
    hide_one(pl, TUTORIAL_Q, hide, &tut_want);
    hide_one(pl, hint, hide, &hint_want);
    tut_hidden = hide;
    if (!hide)
        rt_log("[p2p] the tutorial question %s\n", tut_want ? "back" : "stays hidden (the game had not shown it)");
}

static void first_update(u32 pl, int menu)
{
    if (!pl || !first_ask)
        return;
    if (first_ask == 2 && menu != first_menu)
        return;
    if (tut_var_back && (int)(eagl_frames() - tut_var_back) >= 0) {
        tut_var_back = 0;
        tut_var(tut_var_pl, 1);
        rt_log("[p2p] _root.isInTutorial true again\n");
    }
    int opened = 0;
    if (first_ask == 1) {
        first_paths(menu);
        if (++first_seen < 6 || !rt_call2(GT_FN_SWF_FIND, pl, GA(fp_popup)))
            return;
        first_ask = 2;
        first_menu = menu;
        if (!net_lan_on()) {
            rt_log("[p2p] first start: no name asked (GT_LAN_MULTIPLAYER=0)\n");
            return;
        }
        form_tick = eagl_frames();
        form_kind = FORM_FIRST;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : "", NULL);
        first_show(pl);
        opened = 1;
        rt_log("[p2p] first start: name form open (frame %u)\n", eagl_frames());
    }
    if (form_kind != FORM_FIRST) {
        if (tut_hidden) {
            box_off(pl, fp_input);
            tut_hide(pl, 0);
        }
        return;
    }
    form_tick = eagl_frames();
    tut_hide(pl, 1);
    if (first_reshow && (int)(eagl_frames() - first_reshow) >= 0) {
        first_reshow = 0;
        first_show(pl);
        opened = 1;
    }
    if ((int)(first_labels - eagl_frames()) > 0 && eagl_frames() % 10 == 0) {
        flurry_set(pl, fp_yesf, "OK");
        flurry_set(pl, fp_nof, "LATER");
    }
    form_draw(pl, fp_field, fp_input, "YOUR NAME FOR ONLINE RACES:", opened);
    if (form_esc || form_enter) {
        int ok = form_enter;
        form_enter = form_esc = 0;
        char v[48];
        EnterCriticalSection(&form_cs);
        snprintf(v, sizeof v, "%s", form_text);
        LeaveCriticalSection(&form_cs);
        size_t n = strlen(v);
        while (n && v[n - 1] == ' ')
            v[--n] = 0;
        if (ok && !n) {
            form_set("", "Type a name first.");
            return;
        }
        if (ok)
            name_set(v);
        else
            rt_log("[p2p] first start: name left for later\n");
        form_kind = FORM_NONE;
        first_reshow = 0;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA(fp_popup), GA((void *)"Hided"), 0);
        box_off(pl, fp_input);
        tut_hide(pl, 0);
    }
}

void p2p_nav_update(u32 pl)
{
    first_update(pl, 1);
}

static void (*main_update_fn)(CPU *);
static void main_update_hook(CPU *c)
{
    u32 self = c->r[0];
    main_update_fn(c);
    first_update(rd32(self + 4), 0);
}

int p2p_nav_event(CPU *c)
{
    if (form_kind != FORM_FIRST)
        return 0;
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    int yes = !strcmp(name, "btnYes"), no = !strcmp(name, "btnNo");
    if (!yes && !no)
        return 0;
    u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
    if (!path || !strstr((const char *)GP(path), "FlurryPopup"))
        return 0;
    u32 pl = rd32(c->r[0] + 4);
    if (pl && hint_want) {
        if (!tut_var_back)
            tut_var(pl, 0);
        tut_var_pl = pl;
        tut_var_back = eagl_frames() + 30;
    }
    if (rd32(ev + 8) == 6) {
        if (yes)
            form_enter = 1;
        else
            form_esc = 1;
        first_reshow = eagl_frames() + 45;
    }
    return 1;
}

static void first_init(void)
{
    const char *e = getenv("GT_FIRST_NAME");
    if (e && *e == '0') {
        rt_log("[p2p] FIRST_NAME off (GT_FIRST_NAME=0)\n");
        return;
    }
    if (e && !_stricmp(e, "force")) {
        first_ask = 1;
        rt_log("[p2p] FIRST_NAME: asked on this start (GT_FIRST_NAME=force)\n");
        return;
    }
    char g[512], p[1024];
    static const char *const saves[] = {"Documents/Profile_Free.0.gts", "Documents/data.sav"};
    for (int i = 0; i < 2; i++) {
        snprintf(g, sizeof g, "%s/%s", gt_guest_home, saves[i]);
        if (GetFileAttributesA(gt_host_path(g, p, sizeof p)) != INVALID_FILE_ATTRIBUTES)
            return;
    }
    if (GetFileAttributesA(name_path(p, sizeof p)) != INVALID_FILE_ATTRIBUTES)
        return;
    first_ask = 1;
    rt_log("[p2p] first start (no save): the name is asked at the main menu\n");
}

static void join_apply(u32 ip, u16 port, u32 key)
{
    join_ip = ip, join_port = port, join_key = key;
    net_p2p_reset();
    net_p2p_set_key(key);
    net_p2p_add_peer(ip, port);
    char a[16];
    inet_ntop(AF_INET, &ip, a, sizeof a);
    rt_log("[p2p] joining with a room code: %s:%u\n", a, ntohs(port));
}

static void (*join_update_fn)(CPU *), (*join_event_fn)(CPU *);
static void join_update_hook(CPU *c)
{
    u32 self = c->r[0];
    join_update_fn(c);
    u32 pl = rd32(self + 4);
    static const char popup[] = "JoinMenu.DisconnectedPopup";
    static const char text[] = "JoinMenu.DisconnectedPopup.PopupCongrats.TxtCongrats";
    static const char ok[] = "JoinMenu.DisconnectedPopup.PopupCongrats.txtOk";
    static const char input[] = "JoinMenu.DisconnectedPopup.PopupCongrats.Input";
    if (pl && form_kind != FORM_CODE && !form_open_code)
        box_off(pl, input);
    if (!pl || !p2p_online)
        return;
    int opened = 0;
    if (form_open_code) {
        form_open_code = 0;
        char t[1024], code[40] = "";
        u32 ip, key;
        u16 port;
        if (clip_get(t, sizeof t) && code_parse(t, &ip, &port, &key) == 1)
            code_make(ip, port, key, code);
        form_tick = eagl_frames();
        form_kind = FORM_CODE;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : code, code[0] ? "From the clipboard: press OK." : NULL);
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Show"), 1);
        opened = 1;
        rt_log("[p2p] room code form open%s\n", code[0] ? " (from the clipboard)" : "");
    }
    if (form_kind != FORM_CODE)
        return;
    form_tick = eagl_frames();
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)ok)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)ok), GA((void *)"OK"), 1);
    form_draw(pl, text, input, "ROOM CODE (type or Ctrl+V, Enter = OK):", opened);
    if (form_esc || form_enter) {
        int go = form_enter;
        form_enter = form_esc = 0;
        if (go) {
            char v[64];
            u32 ip, key;
            u16 port;
            EnterCriticalSection(&form_cs);
            snprintf(v, sizeof v, "GT-%s", form_text + (!_strnicmp(form_text, "GT-", 3) ? 3 : 0));
            LeaveCriticalSection(&form_cs);
            int r = code_parse(v, &ip, &port, &key);
            if (r != 1) {
                form_set(form_text, r < 0 ? "Not a room code: check it." : "Type the whole code.");
                return;
            }
            join_apply(ip, port, key);
        }
        form_kind = FORM_NONE;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Hide"), 1);
    }
}

static void join_event_hook(CPU *c)
{
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    if (p2p_online && rd32(ev + 8) == 6 && !strcmp(name, "btnRestart") && form_kind != FORM_CODE)
        form_open_code = 1;
    if (form_kind == FORM_CODE && !strcmp(name, "btnOk")) {
        u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
        if (path && strstr((const char *)GP(path), "DisconnectedPopup")) {
            if (rd32(ev + 8) == 6)
                form_enter = 1;
            static const char none[] = "";
            u32 old = rd32(ev + 4);
            wr32(ev + 4, GA((void *)none));
            join_event_fn(c);
            wr32(ev + 4, old);
            return;
        }
    }
    join_event_fn(c);
}

void p2p_init(void)
{
    InitializeCriticalSection(&form_cs);
    first_init();
    if (first_ask)
        gt_hook_slot(MAIN_UPDATE_SLOT, MAIN_UPDATE_FN, &main_update_fn, main_update_hook, "p2p main_update_hook");
    gt_hook_slot(STYPE_UPDATE_SLOT, STYPE_UPDATE_FN, &stype_update_fn, stype_update_hook, "p2p stype_update_hook");
    gt_hook_slot(LOBBY_UPDATE_SLOT, LOBBY_UPDATE_FN, &lobby_update_fn, lobby_update_hook, "p2p lobby_update_hook");
    gt_hook_slot(GT_UPDATE_SLOT, GT_UPDATE_FN, &gt_update_fn, gt_update_hook, "p2p gt_update_hook");
    gt_hook_slot(JOIN_UPDATE_SLOT, JOIN_UPDATE_FN, &join_update_fn, join_update_hook, "p2p join_update_hook");
    gt_hook_slot(JOIN_EVENT_SLOT, JOIN_EVENT_FN, &join_event_fn, join_event_hook, "p2p join_event_hook");
}
