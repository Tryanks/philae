// Linux packet sockets (<netpacket/packet.h>). Darwin has no AF_PACKET; the family gets a
// number no Darwin family uses, so creating such a socket fails with EAFNOSUPPORT instead
// of silently opening whatever Darwin assigns to Linux's value (17 is AF_ROUTE there).
#pragma once

#define AF_PACKET 0x7a11
#define PF_PACKET AF_PACKET

struct sockaddr_ll {
    unsigned short sll_family;
    unsigned short sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
};
