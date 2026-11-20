// Linux netlink sockets. Not available on Darwin; see netpacket/packet.h for why the
// address family number is deliberately not Linux's. Protocol and group numbers are
// Linux's: they only ever reach a netlink socket, which cannot be created.
#pragma once
#include <stdint.h>

#define AF_NETLINK 0x7a10
#define PF_NETLINK AF_NETLINK

#define NETLINK_ROUTE 0
#define NETLINK_INET_DIAG 4
#define NETLINK_XFRM 6
#define NETLINK_NETFILTER 12

struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};
