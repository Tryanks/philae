// Linux process control (<sys/prctl.h>). Only thread naming has a Darwin equivalent.
#pragma once

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0

#ifdef __cplusplus
extern "C"
#endif
int prctl(int option, ...);
