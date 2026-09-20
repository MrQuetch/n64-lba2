// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Cartridge SRAM filesystem for the Nintendo 64 port. Registers a "sram:/"
// libdragon devoptab so the engine's POSIX file layer (FILES.CPP, the dirent
// shim, direct stat() calls) reads and writes game saves and lba2.cfg to the
// 32 KB battery-backed SRAM declared in the ROM header, with no changes to
// SAVEGAME.CPP / GAMEMENU.CPP. Implementation: LIB386/SYSTEM/N64_SRAMFS.CPP.

#ifndef LIB_SYSTEM_N64_SRAMFS_H
#define LIB_SYSTEM_N64_SRAMFS_H

#ifdef __cplusplus
extern "C" {
#endif

// Mount prefix, used by the SDL_GetPrefPath shim as the engine user dir.
#define N64_SRAMFS_PREFIX "sram:/"

// Total bytes of the SRAM part backing the filesystem (256 Kbit).
#define N64_SRAMFS_SIZE 32768

// Load the SRAM image (formatting it when blank or corrupt) and attach the
// filesystem. Call once at boot, before the engine starts. Returns 0 on
// success.
int N64SramFs_Init(void);

// Bytes of SRAM currently occupied by files + directory (for diagnostics).
unsigned N64SramFs_UsedBytes(void);

#ifdef __cplusplus
}
#endif

#endif // LIB_SYSTEM_N64_SRAMFS_H
