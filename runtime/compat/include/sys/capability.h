#pragma once
#include <linux/capability.h>

#ifdef __cplusplus
extern "C" {
#endif
int capget(cap_user_header_t header, cap_user_data_t data);
int capset(cap_user_header_t header, const cap_user_data_t data);
#ifdef __cplusplus
}
#endif
