#ifndef GT_COMPAT_NET_H
#define GT_COMPAT_NET_H

#include "compat.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef int SOCKET;
typedef struct { WORD wVersion; } WSADATA;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define WSAStartup(v, d) ((void)(v), (void)(d), 0)
#define WSAGetLastError() errno
#define closesocket close
#define ioctlsocket(s, c, a) ioctl((s), (c), (a))
#define IOC_VENDOR 0
#define _WSAIOW(a, b) 0
#define WSAIoctl(s, c, i, il, o, ol, r, ov, cb) ((void)(s), (void)(i), (void)(r))

#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEMFILE EMFILE

static inline int gt_len_call(int r, socklen_t t, int *l)
{
    if (l)
        *l = (int)t;
    return r;
}
#define GT_LEN_IN(l) ((l) ? (socklen_t)*(l) : 0)
#define GT_LEN_PTR(l, t) ((l) ? &(t) : NULL)

static inline int gt_getsockname(SOCKET s, struct sockaddr *a, int *l)
{
    socklen_t t = GT_LEN_IN(l);
    return gt_len_call(getsockname(s, a, GT_LEN_PTR(l, t)), t, l);
}
static inline int gt_getpeername(SOCKET s, struct sockaddr *a, int *l)
{
    socklen_t t = GT_LEN_IN(l);
    return gt_len_call(getpeername(s, a, GT_LEN_PTR(l, t)), t, l);
}
static inline SOCKET gt_accept(SOCKET s, struct sockaddr *a, int *l)
{
    socklen_t t = GT_LEN_IN(l);
    return gt_len_call(accept4(s, a, GT_LEN_PTR(l, t), SOCK_CLOEXEC), t, l);
}
static inline int gt_recvfrom(SOCKET s, void *b, int n, int f, struct sockaddr *a, int *l)
{
    socklen_t t = GT_LEN_IN(l);
    return gt_len_call((int)recvfrom(s, b, (size_t)n, f, a, GT_LEN_PTR(l, t)), t, l);
}
static inline int gt_getsockopt(SOCKET s, int level, int opt, void *v, int *l)
{
    socklen_t t = GT_LEN_IN(l);
    return gt_len_call(getsockopt(s, level, opt, v, GT_LEN_PTR(l, t)), t, l);
}
static inline int gt_setsockopt(SOCKET s, int level, int opt, const void *v, int n)
{
    if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) && n == (int)sizeof(DWORD)) {
        DWORD ms = *(const DWORD *)v;
        struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
        return setsockopt(s, level, opt, &tv, sizeof tv);
    }
    return setsockopt(s, level, opt, v, (socklen_t)n);
}
static inline SOCKET gt_socket(int dom, int type, int proto) { return socket(dom, type | SOCK_CLOEXEC, proto); }

#define getsockname gt_getsockname
#define getpeername gt_getpeername
#define accept gt_accept
#define recvfrom gt_recvfrom
#define getsockopt gt_getsockopt
#define setsockopt gt_setsockopt
#define socket gt_socket
#define send(s, b, n, f) send((s), (b), (size_t)(n), (f) | MSG_NOSIGNAL)
#define sendto(s, b, n, f, a, l) sendto((s), (b), (size_t)(n), (f) | MSG_NOSIGNAL, (a), (l))
#define select(n, r, w, e, t) select(FD_SETSIZE, (r), (w), (e), (t))

#endif
