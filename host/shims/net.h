#ifndef GT_NET_H
#define GT_NET_H

#include "rt.h"

int net_lan_on(void);
u32 net_test_ip(void);
int net_addr_allowed(u32 addr_be);
int net_own_addr(u32 addr_be);
void net_p2p_board(int i, const void *p, u32 n);
int net_is_socket(u32 fd);
u32 net_close(u32 fd);
u32 net_fcntl(u32 fd, u32 cmd, u32 arg);
u32 net_ioctl(u32 fd, u32 req, u32 argp);
u32 net_iflist(u32 index, u32 oldp, u32 oldlenp);
int net_sockets_open(void);
int gk_sessions_open(void);
void gt_net_init(void);
void gt_gk_init(void);
#if defined(_WINSOCKAPI_) || defined(GT_COMPAT_NET_H)
int net_broadcast(SOCKET h, const void *p, int n, u16 port_be);
#endif

#endif
