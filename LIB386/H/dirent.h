// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Shadow header for <dirent.h>. Newlib for mips64-elf (N64/libdragon) ships
// no dirent support (#error in <sys/dirent.h>); provide a minimal POSIX shim
// over libdragon's dir_findfirst/dir_findnext — implementation lives in
// LIB386/SYSTEM/N64_BACKEND.CPP. The engine only uses opendir/readdir/
// closedir and d_name. Every other platform falls through to the real
// system header via include_next.

#ifdef LBA2_TARGET_N64

#ifndef LBA2_N64_DIRENT_H
#define LBA2_N64_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    char d_name[256];
    int d_type;
};

typedef struct lba2_n64_DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif // LBA2_N64_DIRENT_H

#else // !LBA2_TARGET_N64

#include_next <dirent.h>

#endif
