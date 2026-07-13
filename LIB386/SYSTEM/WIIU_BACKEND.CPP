// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Wii U / WUT backend replacing the SDL3-using SYSTEM files. Provides
// symbols normally exported by:
//   AVAILMEM.CPP DISPOS.CPP EVENTS.CPP KEYBOARD.CPP MOUSE.CPP
//   STRING.CPP TIMER.CPP WINDOW.CPP
//
// V1 scope is "link & boot": OSScreen + ProcUI lifecycle, stubbed input,
// timer fed by OSGetSystemTime(). Visual output for the engine framebuffer
// lives in LIB386/SVGA/WIIU.CPP. Audio (AIL/WIIU/) is forced to null in V1.
//
// Platform init is done from a constructor so the engine's existing main()
// in SOURCES/PERSO.CPP doesn't need to be patched.

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/AVAILMEM.H>
#include <SYSTEM/DISPOS.H>
#include <SYSTEM/EVENTS.H>
#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/MOUSE.H>
#include <SYSTEM/STRING.H>
#include <SYSTEM/TIMER.H>
#include <SYSTEM/WINDOW.H>

#include <coreinit/screen.h>
#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/foreground.h>
#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// DRC touch-panel hooks, implemented in SOURCES/JOYSTICK_WIIU_STUB.CPP (VPAD
// owner) and SOURCES/TOUCHPANEL_WIIU.CPP (panel layout/renderer). Resolved at
// final link like the other cross-module hooks.
void WiiUTouch_InjectKeys(void);
U32 WiiUTouch_ActiveKey(void);
S32 WiiUTouchPanel_Render(U32 *buf0, U32 *buf1, U32 pitch, U32 height,
                          U32 pressedKey);
void WiiUTouchPanel_Invalidate(void);

// Blit cost accumulators for the MainLoop profiler (read+reset by the
// [prof] heartbeat in SOURCES/PERSO.CPP). The HW numbers showed a fixed
// ~41.5 ms per blit (~2.5 vblanks) regardless of scene — the sub-phase
// split (pixel conversion vs cache flush vs OSScreen flip) pins down
// whether the cost is memory traffic or a blocking flip.
OSTime WiiUProf_BlitTicks = 0;
OSTime WiiUProf_BlitConvTicks  = 0; // TV+DRC pixel conversion/writes
OSTime WiiUProf_BlitFlushTicks = 0; // DCFlushRange
OSTime WiiUProf_BlitFlipTicks  = 0; // OSScreenFlipBuffersEx
U32    WiiUProf_BlitCount = 0;

// =============================================================================
// Wii U platform: ProcUI + OSScreen lifecycle
// =============================================================================
static void *s_tvBuffer  = NULL;
static void *s_drcBuffer = NULL;
static U32 s_tvBufferSize  = 0;
static U32 s_drcBufferSize = 0;
static bool s_platformReady = false;
static bool s_exitRequested = false;
static int  s_statusRow = 0;

static EventFilterFn s_eventFilter = NULL;

static void WiiUInitOSScreen(void) {
    OSScreenInit();
    s_tvBufferSize  = OSScreenGetBufferSizeEx(SCREEN_TV);
    s_drcBufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);
    s_tvBuffer  = MEMAllocFromDefaultHeapEx(s_tvBufferSize,  0x100);
    s_drcBuffer = MEMAllocFromDefaultHeapEx(s_drcBufferSize, 0x100);
    OSScreenSetBufferEx(SCREEN_TV,  s_tvBuffer);
    OSScreenSetBufferEx(SCREEN_DRC, s_drcBuffer);
    OSScreenEnableEx(SCREEN_TV,  1);
    OSScreenEnableEx(SCREEN_DRC, 1);
}

static void WiiUFreeOSScreen(void) {
    OSScreenShutdown();
    if (s_tvBuffer)  { MEMFreeToDefaultHeap(s_tvBuffer);  s_tvBuffer  = NULL; }
    if (s_drcBuffer) { MEMFreeToDefaultHeap(s_drcBuffer); s_drcBuffer = NULL; }
}

// OSScreen uses 2 framebuffers per screen and OSScreenFlipBuffersEx swaps
// which one the display reads from — so a write-then-flip pattern only
// populates one buffer, and the display alternates between "new line" and
// "previous state". To keep the boot trace fully visible we maintain a
// shadow array of all printed lines in main RAM, redraw the full state on
// every update, and flip twice so both physical buffers carry the same
// content.
#define WIIU_STATUS_MAX_LINES 32
#define WIIU_STATUS_LINE_LEN  256  // Was 80, bumped after we saw multi-line
                                   // LogPrintf calls (Paths used / HQR errors)
                                   // get truncated mid-message.
static char s_statusLines[WIIU_STATUS_MAX_LINES][WIIU_STATUS_LINE_LEN];
static int  s_statusCount = 0;

// OSScreenPutFontEx does NOT bounds-check the row: glyph cells are 24 px
// tall, so TV (720 px) fits 30 rows and DRC (480 px) fits 20. Rows beyond
// that write past the visible framebuffer — under Cemu this lands on heap
// allocations right after the screen buffers (libc FILE/devoptab state) and
// hard-crashes the emulator once the boot trace exceeds ~28 lines. Draw a
// sliding window of only the most recent lines that physically fit.
#define WIIU_TV_FONT_ROWS  28
#define WIIU_DRC_FONT_ROWS 18

static void WiiURedrawAndFlip(void) {
    int tvStart  = (s_statusCount > WIIU_TV_FONT_ROWS)  ? s_statusCount - WIIU_TV_FONT_ROWS  : 0;
    int drcStart = (s_statusCount > WIIU_DRC_FONT_ROWS) ? s_statusCount - WIIU_DRC_FONT_ROWS : 0;
    for (int pass = 0; pass < 2; ++pass) {
        OSScreenClearBufferEx(SCREEN_TV,  0x00000000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);
        for (int i = tvStart; i < s_statusCount; ++i) {
            OSScreenPutFontEx(SCREEN_TV,  0, i - tvStart, s_statusLines[i]);
        }
        for (int i = drcStart; i < s_statusCount; ++i) {
            OSScreenPutFontEx(SCREEN_DRC, 0, i - drcStart, s_statusLines[i]);
        }
        DCFlushRange(s_tvBuffer,  s_tvBufferSize);
        DCFlushRange(s_drcBuffer, s_drcBufferSize);
        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }
}

// True once the game has presented its first framebuffer; from that point on
// we stop drawing the boot status text over OSScreen so the engine's render
// owns the display.
static bool s_gameRenderActive = false;

static void WiiUStatusLine(const char *line) {
    if (s_statusCount >= WIIU_STATUS_MAX_LINES) {
        // Scroll: drop oldest line, shift the rest up by one row.
        memmove(s_statusLines[0], s_statusLines[1],
                (WIIU_STATUS_MAX_LINES - 1) * WIIU_STATUS_LINE_LEN);
        s_statusCount = WIIU_STATUS_MAX_LINES - 1;
    }
    strncpy(s_statusLines[s_statusCount], line, WIIU_STATUS_LINE_LEN - 1);
    s_statusLines[s_statusCount][WIIU_STATUS_LINE_LEN - 1] = '\0';
    ++s_statusCount;
    if (!s_gameRenderActive) {
        WiiURedrawAndFlip();
    }
}

// Public blit: copy the engine's 8bpp paletted framebuffer (Phys) to OSScreen
// TV+DRC, applying the provided palette LUT (RGBA8 per index).
//
// V2: direct framebuffer writes (no OSScreenPutPixelEx call overhead).
// TV at 720p gets a 1.5x nearest upscale (640x480 -> 960x720): full screen
// height, 4:3 preserved, 160px pillarbox each side. The DRC (854x480) shows
// the image 1:1 centered — already full height. Both halves of each double
// buffer are written identically, so we stay agnostic of OSScreen's internal
// work-buffer parity and flip once per screen.
//
// 1.5x nearest expansion: dstX = x*2/3 -> per 2 source px (a,b) emit (a,a,b);
// dstY same pattern, so even source rows are emitted twice — the duplicate
// is a straight memcpy of the row just converted.

// Espresso dcbz row pre-zero: allocate-and-zero each 32-byte cache line of
// a row that is about to be fully overwritten, skipping the DRAM
// read-for-ownership a store to a cold line otherwise performs. The TV band
// streams 5.5 MB/frame — far beyond the 2 MB L2 — so every line is cold and
// RFO reads doubled the bus traffic (measured on HW: conv ~40 ms/frame,
// capping the game at ~23 fps while flush/flip cost ~3 ms combined).
// No-op (skip) unless dst and length are 32-byte aligned.
static inline void WiiURowPrezero(void *dst, U32 bytes) {
    if ((((uintptr_t)dst | bytes) & 31u) != 0) return;
    U8 *p = (U8 *)dst;
    for (U32 off = 0; off < bytes; off += 32u) {
        __asm__ volatile("dcbz 0, %0" :: "r"(p + off) : "memory");
    }
}

extern "C" void WiiU_BlitGameFramebuffer(const void *phys, U32 resX, U32 resY,
                                         const U32 *paletteLUT) {
    {
        static int s_firstLog = 0;
        if (s_firstLog < 2) {
            LogPrintf("[blit] call#%d phys=%p lut=%p res=%ux%u tvBuf=%p drcBuf=%p\n",
                      s_firstLog, phys, (const void *)paletteLUT,
                      (unsigned)resX, (unsigned)resY, s_tvBuffer, s_drcBuffer);
            s_firstLog++;
        }
    }
    if (!phys || !paletteLUT || resX == 0 || resY == 0) return;
    if (!s_tvBuffer || !s_drcBuffer) return;
    s_gameRenderActive = true;

    const OSTime profT0 = OSGetSystemTime();

    const U8 *src = (const U8 *)phys;

    // OSScreen geometry. TV: 1280x720 (720p) or 854x480-in-896 pitch (480p);
    // derive the pitch from the buffer size so both modes work. DRC is
    // always 480 rows (854 visible, pitch derived — typically 896).
    const U32 tvHeight = (s_tvBufferSize >= 1280u * 720u * 4u * 2u) ? 720u : 480u;
    const U32 tvPitch  = s_tvBufferSize / 2u / 4u / tvHeight;
    const U32 drcHeight = 480u;
    const U32 drcPitch  = s_drcBufferSize / 2u / 4u / drcHeight;

    U32 *tv0  = (U32 *)s_tvBuffer;
    U32 *tv1  = (U32 *)((U8 *)s_tvBuffer + s_tvBufferSize / 2u);
    U32 *drc0 = (U32 *)s_drcBuffer;
    U32 *drc1 = (U32 *)((U8 *)s_drcBuffer + s_drcBufferSize / 2u);

    // NOTE (V10.3 experiment, reverted): writing only the flip-parity work
    // half saved ~2.8 MB/frame but bought no measurable fps on hardware
    // (the bottleneck is the CPU-bound engine renderer, not blit bandwidth)
    // and produced visible ghosting there — with the halves holding
    // different frames, the HW flip/scan-out mixes old and new images
    // (Cemu was clean; its OSScreen work-buffer parity evidently differs
    // from real coreinit, or HW flips aren't vblank-latched). Writing BOTH
    // halves identically stays immune to all of that by construction.

    // Clear both screens once when the game takes over (kills boot-trace
    // remnants in the pillarbox bands); afterwards only the image band is
    // rewritten each frame.
    bool justCleared = false;
    static bool s_clearedOnce = false;
    if (!s_clearedOnce) {
        memset(s_tvBuffer, 0, s_tvBufferSize);
        memset(s_drcBuffer, 0, s_drcBufferSize);
        s_clearedOnce = true;
        justCleared = true;
    }

    // ── TV ────────────────────────────────────────────────────────────────
    if (tvHeight == 720u && resX == 640u && resY == 480u) {
        // 1.5x upscale to 960x720, centered.
        const U32 dstW = 960u;
        const U32 xOfs = (tvPitch >= 1280u) ? (1280u - dstW) / 2u : 0u;
        U32 prevSrcY = 0xFFFFFFFFu;

        for (U32 y = 0; y < 720u; ++y) {
            const U32 srcY = (y * 2u) / 3u;
            U32 *d0 = tv0 + (size_t)y * tvPitch + xOfs;
            U32 *d1 = tv1 + (size_t)y * tvPitch + xOfs;

            WiiURowPrezero(d0, dstW * 4u);
            WiiURowPrezero(d1, dstW * 4u);

            if (srcY == prevSrcY) {
                // Duplicate of the row emitted at y-1.
                memcpy(d0, d0 - tvPitch, dstW * 4u);
                memcpy(d1, d1 - tvPitch, dstW * 4u);
                continue;
            }
            prevSrcY = srcY;

            const U8 *s = src + (size_t)srcY * 640u;
            U32 *w = d0;
            for (U32 sx = 0; sx < 640u; sx += 2u) {
                const U32 a = paletteLUT[s[sx]];
                const U32 b = paletteLUT[s[sx + 1u]];
                w[0] = a;
                w[1] = a;
                w[2] = b;
                w += 3;
            }
            memcpy(d1, d0, dstW * 4u);
        }
    } else {
        // Fallback: 1:1 centered (also covers a 480p TV mode).
        const U32 w = (resX < tvPitch) ? resX : tvPitch;
        const U32 h = (resY < tvHeight) ? resY : tvHeight;
        const U32 xOfs = (tvPitch - w) / 2u;
        const U32 yOfs = (tvHeight - h) / 2u;
        for (U32 y = 0; y < h; ++y) {
            const U8 *s = src + (size_t)y * resX;
            U32 *d0 = tv0 + (size_t)(y + yOfs) * tvPitch + xOfs;
            U32 *d1 = tv1 + (size_t)(y + yOfs) * tvPitch + xOfs;
            WiiURowPrezero(d0, w * 4u);
            WiiURowPrezero(d1, w * 4u);
            for (U32 x = 0; x < w; ++x)
                d0[x] = paletteLUT[s[x]];
            memcpy(d1, d0, w * 4u);
        }
    }

    // ── DRC ──────────────────────────────────────────────────────────────
    // Gameplay: quick-action touch panel. Menus (AsciiMode set by
    // DoGameMenu/selector loops): mirror the game 1:1 so menus stay readable
    // on the pad; the panel is invalidated so it repaints fully when it
    // takes the screen back.
    bool drcDirty = justCleared;
    {
        // Panel is 854 wide, mirror only 640 centered: clear once per mode
        // switch so no stale pixels survive in the side bands.
        static U32 s_lastDrcPanelMode = 0xFFu;
        const U32 panelMode = (AsciiMode == 0) ? 1u : 0u;
        if (panelMode != s_lastDrcPanelMode) {
            memset(s_drcBuffer, 0, s_drcBufferSize);
            s_lastDrcPanelMode = panelMode;
            drcDirty = true;
        }
    }
    if (AsciiMode == 0) {
        // Panel writes both halves, but only when its state changed —
        // most gameplay frames leave the DRC buffer untouched.
        if (WiiUTouchPanel_Render(drc0, drc1, drcPitch, drcHeight,
                                  WiiUTouch_ActiveKey())) {
            drcDirty = true;
        }
    } else {
        WiiUTouchPanel_Invalidate();
        const U32 w = (resX < drcPitch) ? resX : drcPitch;
        const U32 h = (resY < drcHeight) ? resY : drcHeight;
        const U32 xOfs = (854u > w) ? (854u - w) / 2u : 0u;
        const U32 yOfs = (drcHeight - h) / 2u;
        for (U32 y = 0; y < h; ++y) {
            const U8 *s = src + (size_t)y * resX;
            U32 *d0 = drc0 + (size_t)(y + yOfs) * drcPitch + xOfs;
            for (U32 x = 0; x < w; ++x)
                d0[x] = paletteLUT[s[x]];
            memcpy(drc1 + (size_t)(y + yOfs) * drcPitch + xOfs, d0, w * 4u);
        }
        drcDirty = true;
    }

    const OSTime profT1 = OSGetSystemTime(); // conversion done

    // TV is rewritten every frame; the DRC is flushed only when the
    // panel/mirror actually touched it (most gameplay frames skip it).
    DCFlushRange(s_tvBuffer, s_tvBufferSize);
    if (drcDirty) {
        DCFlushRange(s_drcBuffer, s_drcBufferSize);
    }

    const OSTime profT2 = OSGetSystemTime(); // flush done

    OSScreenFlipBuffersEx(SCREEN_TV);
    // DRC content only changes when drcDirty (panel state change / menu
    // mirror); with both halves always written identically a flip is a
    // no-op visually, and on real HW flips look suspiciously vblank-priced
    // — skip the second one on the (vast majority of) clean frames.
    if (drcDirty) {
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    const OSTime profT3 = OSGetSystemTime(); // flips done

    WiiUProf_BlitConvTicks  += profT1 - profT0;
    WiiUProf_BlitFlushTicks += profT2 - profT1;
    WiiUProf_BlitFlipTicks  += profT3 - profT2;
    WiiUProf_BlitTicks += profT3 - profT0;
    WiiUProf_BlitCount++;
}

// =============================================================================
// LOGPRINT.CPP replacement — every engine LogPrintf/LogPuts goes to BOTH:
//   1. OSScreen (TV + DRC), live diagnostic during boot
//   2. /vol/external01/wiiu/apps/lba2/log.txt on SD, persistent trace that
//      survives engine crashes/hangs and can be read on PC after pulling SD
// =============================================================================
U32 Log2File = 1; // Always on for V1 diagnostics.
U32 QuietLog = 0;

#define WIIULBA_BUILD_TAG "V10.11-QUIT"

static FILE *s_logFile = NULL;

static void WiiULogFileEnsureOpen(void) {
    static int s_attempted = 0;
    if (s_logFile || s_attempted) return;
    s_attempted = 1; // Under Cemu there is no SD mount; don't retry per line.
    // Rotate the previous run's log instead of destroying it: a cold boot
    // must never cost us the [prof] data of the session before it.
    remove("/vol/external01/wiiu/apps/lba2/log_prev.txt");
    rename("/vol/external01/wiiu/apps/lba2/log.txt",
           "/vol/external01/wiiu/apps/lba2/log_prev.txt");
    s_logFile = fopen("/vol/external01/wiiu/apps/lba2/log.txt", "w");
    if (s_logFile) {
        fprintf(s_logFile, "[WiiULBA] log start " WIIULBA_BUILD_TAG "\n");
        fflush(s_logFile);
    }
}

static void WiiULogFileWrite(const char *line) {
    // Mirror every line to OSReport: on real HW it is a no-op without a
    // debugger attached; under Cemu it lands in Cemu's log.txt / console,
    // which is our only persistent trace there (no SD to write to).
    OSReport("[LBA2] %s\n", line);
    WiiULogFileEnsureOpen();
    if (!s_logFile) return;
    fputs(line, s_logFile);
    fputc('\n', s_logFile);
    fflush(s_logFile); // Flush libc buffer.
    // Wii U: libc fflush alone may not push bytes to the SD driver. Force a
    // syscall-level sync so the file on disk reflects every line, even if the
    // engine then crashes hard (DSI exception) without unwinding.
    fsync(fileno(s_logFile));
}

void CreateLog(const char *filePath) {
    (void)filePath; // We use a fixed path on SD; engine's request is ignored.
}

void LogPrintf(const char *format, ...) {
    if (QuietLog) return;
    char buf[WIIU_STATUS_LINE_LEN];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    if (n > 0) {
        WiiUStatusLine(buf);
        WiiULogFileWrite(buf);
    }
}

void LogPuts(const char *string) {
    if (QuietLog) return;
    if (!string) return;
    char buf[WIIU_STATUS_LINE_LEN];
    strncpy(buf, string, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    if (n > 0) {
        WiiUStatusLine(buf);
        WiiULogFileWrite(buf);
    }
}

static void WiiUBootBanner(void) {
    s_statusCount = 0;
    s_statusRow = 0;
    WiiUStatusLine("===========================================");
    WiiUStatusLine(" WiiULBA - LBA2 engine boot " WIIULBA_BUILD_TAG);
    WiiUStatusLine("===========================================");
    WiiUStatusLine("");
    WiiUStatusLine("[init] OSScreen + ProcUI ready");
}

// Engine entry point lives in SOURCES/PERSO.CPP under this name on Wii U.
extern "C" int lba2_engine_main(int argc, char *argv[]);

// Return-to-Wii-U-Menu dance, shared by the normal engine return and every
// exit() path (TheEnd/TheEndCheckFile/WINSYS all call exit() directly, which
// used to skip main()'s epilogue entirely — Quit showed a black screen and
// hung, on Cemu and on real HW). Registered with atexit() before the engine
// starts; the guard keeps the pair of callers from running it twice.
static void WiiUReturnToMenuOnce(void) {
    static bool s_done = false;

    if (s_done) {
        return;
    }
    s_done = true;

    // SYSLaunchMenu must come BEFORE ProcUIShutdown, and there must be no
    // wait-for-HOME hold here: the old hold screen was never drawn
    // (s_gameRenderActive suppresses status redraws after the first game
    // frame), so quitting looked like a black-screen hang.
    SYSLaunchMenu();
    for (;;) {
        ProcUIStatus st = ProcUIProcessMessages(true);
        if (st == PROCUI_STATUS_EXITING) break;
        if (st == PROCUI_STATUS_RELEASE_FOREGROUND) ProcUIDrawDoneRelease();
    }

    WiiUFreeOSScreen();
    ProcUIShutdown();
}

// Real Wii U entry point. Brings up ProcUI/OSScreen, waits for foreground
// (so anything we draw is actually visible), then hands off to the engine.
int main(int argc, char *argv[]) {
    ProcUIInit(&OSSavesDone_ReadyToRelease);
    WiiUInitOSScreen();
    WiiUBootBanner();

    WiiUStatusLine("[init] post-banner reached");

    // (Probe removed — confirmed default heap is 512 MB. Probe's repeated
    //  alloc/free pairs may have left the exp heap fragmented before engine
    //  init, contributing to the 3 KB failure. Removing it isolates the
    //  question: does the engine's own pattern alone produce the failure?)

    // Base-heap query — will show 0/0 (runtime carved entire base heap into
    // sub-heaps for the default heap, foreground bucket, etc.). Informational.
    {
        char hbuf[120];
        MEMHeapHandle h1 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
        MEMHeapHandle h2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
        if (h1) {
            uint32_t f = MEMGetTotalFreeSizeForExpHeap(h1);
            uint32_t a = MEMGetAllocatableSizeForExpHeapEx(h1, 4);
            snprintf(hbuf, sizeof(hbuf), "[heap MEM1] free=%lu KB largest=%lu KB",
                     (unsigned long)(f/1024UL), (unsigned long)(a/1024UL));
            WiiUStatusLine(hbuf);
            LogPrintf("%s", hbuf);
        }
        if (h2) {
            uint32_t f = MEMGetTotalFreeSizeForExpHeap(h2);
            uint32_t a = MEMGetAllocatableSizeForExpHeapEx(h2, 4);
            snprintf(hbuf, sizeof(hbuf), "[heap MEM2] free=%lu MB largest=%lu MB",
                     (unsigned long)(f/(1024UL*1024UL)), (unsigned long)(a/(1024UL*1024UL)));
            WiiUStatusLine(hbuf);
            LogPrintf("%s", hbuf);
        }
    }

    // Diagnostic A: confirm wrapper is still alive after the banner, no ProcUI.
    char buf[80];
    for (int i = 0; i < 3; ++i) {
        OSSleepTicks(OSMillisecondsToTicks(200));
        snprintf(buf, sizeof(buf), "[init] sleep tick #%d (no procui yet)", i + 1);
        WiiUStatusLine(buf);
    }

    // Diagnostic B: try blocking ProcUIProcessMessages — pattern that the
    // bare-WUT helloworld used successfully. If non-blocking is what was
    // killing us, blocking should let us see the IN_FOREGROUND transition.
    WiiUStatusLine("[init] about to call ProcUIProcessMessages(TRUE)");
    ProcUIStatus first = ProcUIProcessMessages(true);
    snprintf(buf, sizeof(buf), "[init] proc(TRUE) #1 -> %d", (int)first);
    WiiUStatusLine(buf);

    if (first == PROCUI_STATUS_RELEASE_FOREGROUND) ProcUIDrawDoneRelease();

    s_platformReady = true;

    // The engine's quit ("Abandon" in the pause menu) and every error path
    // go through exit(), which never returns here — hook the menu dance so
    // those exits also land on the Wii U Menu instead of a black screen.
    atexit(WiiUReturnToMenuOnce);

    WiiUStatusLine("[boot] entering engine main()");
    int rc = lba2_engine_main(argc, argv);

    snprintf(buf, sizeof(buf), "[boot] engine returned: %d", rc);
    WiiUStatusLine(buf);

    WiiUReturnToMenuOnce();
    return rc;
}

// libc gap filler — utime() lives in <utime.h> on POSIX but WUT newlib for
// PPC doesn't provide it. FILES.CPP::Touch is the only caller; saves don't
// strictly need mtime updates on Wii U.
struct utimbuf;
int utime(const char *path, const struct utimbuf *times) {
    (void)path; (void)times;
    return 0;
}

// =============================================================================
// AVAILMEM.CPP replacement
// =============================================================================
U64 AvailableMem(void) {
    // Wii U normal-mode app heap is ~1 GB. Returning a conservative figure
    // here is consistent with SDL_GetSystemRAM semantics (total, not free).
    return 1024ULL * 1024 * 1024;
}

// =============================================================================
// DISPOS.CPP replacement
// =============================================================================
#define OS_DOS   0
#define OS_WIN31 1
#define OS_WIN95 2
#define OS_NT    3

U32 OSType = OS_NT;

void DisplayOS(void) {
    LogPrintf("Platform: Wii U (devkitPPC + WUT)\n");
}

// =============================================================================
// TIMER.CPP replacement
// =============================================================================
volatile U32 TimerSystemHR;
volatile U32 TimerRefHR;
volatile S32 CmptFrame;
volatile S32 NbFramePerSecond;
volatile S32 TimerLock;
volatile S32 CmptMemoTimerRef;
static U32 MemoTimerRefHR;
U32 LastTime;
U32 LastEvaluate;

static U32 WiiUMs(void) {
    return (U32)OSTicksToMilliseconds(OSGetSystemTime());
}

void InitTimer() {
    LastTime = LastEvaluate = WiiUMs();
    ManageTime();
}

void EndTimer() {}

void LockTimer() {
    ManageTime();
    ++TimerLock;
}

void UnlockTimer() {
    if (TimerLock > 0) {
        ManageTime();
        --TimerLock;
    }
}

void SaveTimer() {
    if (!CmptMemoTimerRef++) {
        ManageTime();
        MemoTimerRefHR = TimerRefHR;
    }
}

void RestoreTimer() {
    if (CmptMemoTimerRef) {
        if (!--CmptMemoTimerRef) {
            ManageTime();
            TimerRefHR = MemoTimerRefHR;
        }
    }
}

void SetTimerHR(U32 time) {
    LockTimer();
    TimerRefHR = time;
    UnlockTimer();
}

void ManageTime() {
    TimerSystemHR = WiiUMs();
    if (!TimerLock) {
        TimerRefHR += TimerSystemHR - LastTime;
    }
    LastTime = TimerSystemHR;

    // Audio pump (AIL/WIIU): tops up the music-decode FIFO and reaps
    // finished SFX voices. Same per-frame pump model as the DC KOS backend;
    // ManageTime also runs inside load loops, so music survives HQR I/O.
    {
        extern void AudioStreamPump(void);
        AudioStreamPump();
    }

    S32 elapsed = TimerSystemHR - LastEvaluate;
    if (elapsed >= 1000) {
        NbFramePerSecond = (CmptFrame * 1000 + elapsed / 2) / elapsed;
        CmptFrame = 0;
        LastEvaluate += (elapsed / 1000) * 1000;
    }
}

void HandleEventsTimer(const void *event) {
    (void)event;
}

// =============================================================================
// WINDOW.CPP replacement (PresentRendererFrame & friends live in SVGA/WIIU.CPP)
// =============================================================================
bool AppActive = true;
static bool s_windowInit = false;
static U32 s_winResX = 0;
static U32 s_winResY = 0;
static bool s_windowFullscreen = true;

bool InitWindow(const char *title) {
    (void)title;
    s_windowInit = true;
    return true;
}

void EndWindow() {
    if (s_windowInit) {
        DestroyWindowSurface();
        s_windowInit = false;
    }
}

bool IsWindowInitialized() {
    return s_windowInit;
}

bool CreateWindowSurface(U32 resX, U32 resY) {
    s_winResX = resX;
    s_winResY = resY;
    return true; // Engine framebuffer alloc is in SVGA/WIIU.CPP::CreateVideoSurface.
}

void DestroyWindowSurface() {
    s_winResX = 0;
    s_winResY = 0;
}

void WindowToSurfaceCoords(S32 windowX, S32 windowY, S32 *surfaceX, S32 *surfaceY) {
    if (surfaceX) *surfaceX = windowX;
    if (surfaceY) *surfaceY = windowY;
}

void SetWindowFullscreen(bool fullscreen) {
    s_windowFullscreen = fullscreen;
}

bool GetWindowFullscreen() {
    return s_windowFullscreen;
}

void ManageWindow() {
    // Per-frame ProcUI tick. Without it, HOME button + system overlay don't
    // work and the app gets force-closed by the OS after a few seconds.
    ProcUIStatus status = ProcUIProcessMessages(true);
    if (status == PROCUI_STATUS_EXITING) {
        s_exitRequested = true;
        AppActive = false;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        ProcUIDrawDoneRelease();
    }
}

void HandleEventsWindow(const void *event) {
    (void)event;
}

// =============================================================================
// EVENTS.CPP replacement
// =============================================================================
bool InitEvents() {
    return true;
}

void EndEvents() {
    s_eventFilter = NULL;
}

void ManageEvents() {
    // VPAD/Wiimote polling will be wired in V2 alongside the JOYSTICK backend.
}

void SetEventFilter(EventFilterFn fn) {
    s_eventFilter = fn;
}

// =============================================================================
// KEYBOARD.CPP replacement (V1: stub; engine's gamepad bindings will read 0)
// =============================================================================
S32 AsciiMode;
S32 Key;
U8 TabKeys[TABKEYS_NUM_KEYS];

void InitKeyboard() {
    memset(TabKeys, 0, sizeof(TabKeys));
    AsciiMode = 0;
    Key = 0;
}

void EndKeyboard() {}

S32 GetAscii() {
    return 0;
}

S32 CheckKey(U32 key) {
    if (key < 256) return TabKeys[key];
    U32 idx = 256 - 32 + (key >> 3);
    if (idx >= TABKEYS_NUM_KEYS) return 0;
    return TabKeys[idx] & (1 << (key & 7));
}

void GetKeyScancodeName(char *string, U8 strMaxLen, U32 scanCode) {
    if (!string || strMaxLen == 0) return;
    snprintf(string, strMaxLen, "K%lu", (unsigned long)scanCode);
}

void ManageKeyboard() {
    memset(TabKeys, 0, 256);
    // Gamepad bits at TabKeys[256..] are owned by JOYSTICK_WIIU_STUB::UpdateJoystick.
    // DRC touch panel presses live in the keyboard range and must survive the
    // memset above, so they are re-injected here on every call (GetInput may
    // run several times per frame). State owned by JOYSTICK_WIIU_STUB.
    WiiUTouch_InjectKeys();
}

void HandleEventsKeyboard(const void *event) {
    (void)event;
}

// =============================================================================
// MOUSE.CPP replacement (V1: stub; Wii U has no system pointer in our scope)
// =============================================================================
bool FlagMouse = false;
volatile S32 Click = 0;
volatile U32 MouseX = 0;
volatile U32 MouseY = 0;
S32 MouseXDep = 0;
S32 MouseYDep = 0;
volatile S32 MouseWheelYAccum = 0;
S32 MouseSpriteGraphicNum = 0;
const void *MouseSpriteGraphic = NULL;

bool MouseSpriteShouldDraw(void) { return false; }
bool MouseConsumeMotionFlag(void) { return false; }

void InitMouse() {}
void EndMouse() {}
void ShowMouse(bool show) { (void)show; }
void ShowMouseAfterFirstMotion(void) {}
void SetMousePos(U32 x, U32 y) { (void)x; (void)y; }
void SetMouseBox(U32 x0, U32 y0, U32 x1, U32 y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void ClearMouseBox() {}
void ManageMouse() {}
void HandleEventsMouse(const void *event) { (void)event; }

S32 ConsumeMouseWheelY(void) {
    S32 v = MouseWheelYAccum;
    MouseWheelYAccum = 0;
    return v;
}

// =============================================================================
// STRING.CPP replacement
//
// V1 simplification vs the desktop / DC versions: UnicodeToCp850 is
// ASCII-only here (non-Latin1 codepoints fall through to '?'). The full
// CP850 table is engine state we'll port when the engine actually renders
// text on Wii U; until V2 lands a real text path, no user-visible diff.
// =============================================================================
char *ade_strcasestr(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return NULL;
}

// Real UTF-8 → CP850 conversion, mirroring LIB386/SYSTEM/STRING.CPP (that
// file is excluded from the Wii U build for its SDL dependency). The old V1
// stub here turned every byte >= 0x80 into '?', which is exactly what made
// the FR/ES/PT menu labels render as "Contr??les". Table covers U+00A0..FF;
// bytes that are not valid UTF-8 pass through unchanged, so CP850-encoded
// literals (AFFKEY/CONFIG key names, TabLanguage) survive a trip through
// the converter too.
static const U8 kCp850FromLatin1[0x60] = {
    0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, // U+00A0..A7
    0xF9, 0xB8, 0xA6, 0xAE, 0xAA, 0xF0, 0xA9, 0xEE, // U+00A8..AF
    0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA, // U+00B0..B7
    0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, // U+00B8..BF
    0xB7, 0xB5, 0xB6, 0xC7, 0x8E, 0x8F, 0x92, 0x80, // U+00C0..C7
    0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8, // U+00C8..CF
    0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, // U+00D0..D7
    0x9D, 0xEB, 0xE9, 0xEA, 0x9A, 0xED, 0xE8, 0xE1, // U+00D8..DF
    0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87, // U+00E0..E7
    0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, // U+00E8..EF
    0xD0, 0xA4, 0x95, 0xA2, 0x93, 0xE4, 0x94, 0xF6, // U+00F0..F7
    0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98, // U+00F8..FF
};

U8 UnicodeToCp850(U32 codePoint) {
    if (codePoint < 0x80) {
        return (U8)codePoint;
    }
    if (codePoint >= 0xA0 && codePoint <= 0xFF) {
        return kCp850FromLatin1[codePoint - 0xA0];
    }
    switch (codePoint) {
    case 0x0131:
        return 0xD5;
    case 0x0192:
        return 0x9F;
    case 0x2018:
    case 0x2019:
        return '\'';
    case 0x201C:
    case 0x201D:
        return '"';
    case 0x2013:
    case 0x2014:
        return '-';
    case 0x2026:
        return '.';
    default:
        return '?';
    }
}

void CopyUtf8ToCp850(char *dst, size_t dstSize, const char *src) {
    if (!dst || dstSize == 0 || !src) return;
    const U8 *ptr = (const U8 *)src;
    size_t di = 0;

    while (*ptr != '\0' && di + 1 < dstSize) {
        U8 first = *ptr;

        if (first < 0x80) {
            dst[di++] = (char)first;
            ptr++;
            continue;
        }

        if ((first & 0xE0) == 0xC0 && (ptr[1] & 0xC0) == 0x80) {
            U32 codePoint = ((U32)(first & 0x1F) << 6) | (U32)(ptr[1] & 0x3F);
            dst[di++] = (char)UnicodeToCp850(codePoint);
            ptr += 2;
            continue;
        }

        if ((first & 0xF0) == 0xE0 && (ptr[1] & 0xC0) == 0x80 && (ptr[2] & 0xC0) == 0x80) {
            U32 codePoint = ((U32)(first & 0x0F) << 12) |
                            ((U32)(ptr[1] & 0x3F) << 6) |
                            (U32)(ptr[2] & 0x3F);
            dst[di++] = (char)UnicodeToCp850(codePoint);
            ptr += 3;
            continue;
        }

        // Not valid UTF-8: pass the raw byte through (already-CP850 text).
        dst[di++] = (char)first;
        ptr++;
    }

    dst[di] = '\0';
}

void FormatUtf8ToCp850(char *dst, size_t dstSize, const char *fmt, ...) {
    if (!dst || dstSize == 0) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CopyUtf8ToCp850(dst, dstSize, buf);
}

#ifdef __cplusplus
}
#endif
