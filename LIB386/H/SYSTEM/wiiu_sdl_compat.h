// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Compat shim that lets engine call sites in PERSO/PLAYACF/RES_DISCOVERY/
// DIRECTORIES keep their `SDL_*` syntax on Wii U without dragging in SDL3.
// Each SDL_* identifier is mapped to a libc / WUT equivalent.
//
// On non-Wii U platforms this header is a transparent pass-through to the
// real <SDL3/SDL.h>. (On Dreamcast, dc_sdl_compat.h serves the same purpose.)

#pragma once

#ifdef LBA2_TARGET_WIIU

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LOGPRINT.H>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef LBA2_TARGET_N64
#include <libdragon.h>

// WUT time-API shims: the WIIU-gated engine code (profiling heartbeat in
// PERSO.CPP, blit timing) uses OSTime/OSGetSystemTime; map them to libdragon
// ticks so those blocks compile unchanged on N64.
typedef int64_t OSTime;
static inline OSTime OSGetSystemTime(void) { return (OSTime)get_ticks(); }
static inline int64_t OSTicksToMilliseconds(OSTime t) {
    return (int64_t)((uint64_t)t / (TICKS_PER_SECOND / 1000));
}
static inline OSTime OSMillisecondsToTicks(int64_t ms) {
    return (OSTime)((uint64_t)ms * (TICKS_PER_SECOND / 1000));
}
static inline int64_t OSTicksToMicroseconds(OSTime t) {
    return (int64_t)((uint64_t)t / (TICKS_PER_SECOND / 1000000));
}
static inline void OSSleepTicks(OSTime t) { wait_ticks((unsigned long)t); }
static inline void DCFlushRange(void *p, unsigned int len) {
    data_cache_hit_writeback(p, len);
}
#define OSReport(...) debugf(__VA_ARGS__)
#else
#include <coreinit/time.h>
#include <coreinit/thread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef U32 SDL_Keymod;
#define SDL_KMOD_NONE 0u
#define SDL_KMOD_ALT  0u

static inline int  SDL_Init(unsigned int flags)         { (void)flags; return 1; }
static inline void SDL_Quit(void)                       {}
static inline int  SDL_InitSubSystem(unsigned int flags){ (void)flags; return 1; }
static inline void SDL_QuitSubSystem(unsigned int flags){ (void)flags; }
static inline const char *SDL_GetError(void)            { return ""; }

// Wii U system tick → ms. OSGetSystemTime() returns OSTime (CPU ticks);
// OSTicksToMilliseconds() converts to int64_t. SDL_GetTicks returns Uint32.
#ifdef LBA2_TARGET_N64
static inline U32 SDL_GetTicks(void) {
    return (U32)get_ticks_ms();
}
static inline void SDL_Delay(U32 ms) {
    wait_ms(ms);
}
#else
static inline U32 SDL_GetTicks(void) {
    return (U32)OSTicksToMilliseconds(OSGetSystemTime());
}
static inline void SDL_Delay(U32 ms) {
    OSSleepTicks(OSMillisecondsToTicks((int64_t)ms));
}
#endif

static inline SDL_Keymod SDL_GetModState(void) { return SDL_KMOD_NONE; }

// On the console the SD card is mounted at `/vol/external01/` — the raw FS
// path the Wii U OS sees. The `sd:/` prefix common in homebrew docs is a libwhb
// convention; without libwhb the newlib runtime in WUT does NOT recognise
// it, and stat()/fopen() on `sd:/...` paths silently returns -1 even when
// the file is physically present. Use the OS-canonical `/vol/external01/`
// prefix so the engine's filesystem checks find our data on real hardware.
//
// Under Cemu (or any launcher without an SD mount) that path does not
// exist; fall back to the .wuhb romfs at `/vol/content/`, which carries the
// essential game data plus a `saves/` dir (read-only — good enough for
// render/debug iteration, not for persisting saves).
#ifdef LBA2_TARGET_N64
// N64: all assets live in the DFS filesystem inside the ROM ("rom:/" devoptab
// registered by dfs_init in the backend). Saves go to cartridge SRAM later;
// V1 points the pref path at a rom:/saves/ placeholder (read-only).
static inline int WiiU_SdRootPresent(void) { return 0; }
static inline char *SDL_GetBasePath(void) {
    const char *p = "rom:/";
    char *r = (char *)malloc(strlen(p) + 1);
    if (r) strcpy(r, p);
    return r;
}
#else
static inline int WiiU_SdRootPresent(void) {
    static int s_checked = 0;
    static int s_present = 0;
    if (!s_checked) {
        struct stat st;
        s_present = (stat("/vol/external01/wiiu/apps/lba2", &st) == 0);
        s_checked = 1;
    }
    return s_present;
}
static inline char *SDL_GetBasePath(void) {
    const char *p = WiiU_SdRootPresent() ? "/vol/external01/wiiu/apps/lba2/"
                                         : "/vol/content/";
    char *r = (char *)malloc(strlen(p) + 1);
    if (r) strcpy(r, p);
    return r;
}
#endif
static inline char *SDL_GetCurrentDirectory(void) {
    return SDL_GetBasePath();
}
static inline char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org; (void)app;
#ifdef LBA2_TARGET_N64
    // User dir = cartridge SRAM (saves under save/, lba2.cfg at the root),
    // served by the "sram:/" devoptab in LIB386/SYSTEM/N64_SRAMFS.CPP.
    const char *n64p = "sram:/";
    char *n64r = (char *)malloc(strlen(n64p) + 1);
    if (n64r) strcpy(n64r, n64p);
    return n64r;
#endif
    // Real SDL3 SDL_GetPrefPath creates the directory on demand. Match that
    // semantic so DIRECTORIES.CPP's ExistsFileOrDir() check on the user dir
    // doesn't bail with "Invalid user directory" on a fresh SD install.
    // mkdir on an existing path returns -1/EEXIST which we silently ignore.
    // On the romfs fallback mkdir is pointless (read-only); the wuhb ships a
    // saves/ dir so the existence check passes.
    const char *p;
    if (WiiU_SdRootPresent()) {
        mkdir("/vol/external01/wiiu/apps/lba2/saves", 0777);
        p = "/vol/external01/wiiu/apps/lba2/saves/";
    } else {
        p = "/vol/content/saves/";
    }
    char *r = (char *)malloc(strlen(p) + 1);
    if (r) strcpy(r, p);
    return r;
}

static inline size_t SDL_strlen(const char *s)                          { return strlen(s); }
static inline void   SDL_free(void *p)                                  { free(p); }
static inline char  *SDL_strdup(const char *s)                          { return strdup(s); }
static inline int    SDL_strncasecmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline char  *SDL_strupr(char *s) {
    if (s) for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}
static inline char  *SDL_strlwr(char *s) {
    if (s) for (char *p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}

// SDL_Log → LogPrintf (variadic forwarding)
#define SDL_Log LogPrintf

#ifdef __cplusplus
}
#endif

#else  // !LBA2_TARGET_WIIU
#include <SDL3/SDL.h>
#endif
