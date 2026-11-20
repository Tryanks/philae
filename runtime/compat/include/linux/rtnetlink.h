// Routing netlink constants (<linux/rtnetlink.h>).
#pragma once
#include <linux/netlink.h>

#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_SITE 200
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RT_SCOPE_NOWHERE 255

#define RTMGRP_LINK 0x1
#define RTMGRP_NOTIFY 0x2
#define RTMGRP_NEIGH 0x4
#define RTMGRP_TC 0x8
#define RTMGRP_IPV4_IFADDR 0x10
#define RTMGRP_IPV4_MROUTE 0x20
#define RTMGRP_IPV4_ROUTE 0x40
#define RTMGRP_IPV4_RULE 0x80
#define RTMGRP_IPV6_IFADDR 0x100
#define RTMGRP_IPV6_MROUTE 0x200
#define RTMGRP_IPV6_ROUTE 0x400
#define RTMGRP_IPV6_IFINFO 0x800
#define RTMGRP_IPV6_PREFIX 0x20000
