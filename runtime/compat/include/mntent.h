// Linux mount table access (<mntent.h>). iOS has no mount table an app may read;
// see linux_compat.c.
#pragma once
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

FILE *setmntent(const char *path, const char *mode);
struct mntent *getmntent_r(FILE *fp, struct mntent *entry, char *buffer, int buffer_size);
int endmntent(FILE *fp);

#ifdef __cplusplus
}
#endif
