// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Wii U video backend (V1 link & boot stub).
//
// Replaces SVGA/SDL.CPP and the SVGA-side rendering hooks normally exposed
// by SYSTEM/WINDOW.CPP (PresentRendererFrame, LockRendererTexture,
// UnlockAndPresentRendererTexture). The actual GX2 paletted blit comes
// in V2 — for V1 we just allocate the 8bpp software framebuffer (Phys),
// accept palette updates into a LUT, and route every frame to a no-op
// presentation that keeps the engine timing loop stable.

#include <SVGA/DIRTYBOX.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>           // PrePresentFn typedef + SetPrePresentCallback decl
#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/WINDOW.H>

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Public state (engine-visible globals; mirrored from SDL.CPP / KOS.CPP).
// =============================================================================
void *Phys = NULL;
U32 ModeResX = 0;
U32 ModeResY = 0;
U32 TabOffPhysLine[ADELINE_MAX_Y_RES];

// =============================================================================
// Private state
// =============================================================================
#define PAL_MAX_COLORS 256
static U32 s_paletteLUT[PAL_MAX_COLORS];
static S32 s_screenLockCount = 0;
static PrePresentFn s_prePresent = NULL;

// Forward decl so CreateVideoSurface (above the def) can call it for the
// grayscale-identity fallback that keeps Phys frames visible if the engine
// never uploads a palette.
static inline U32 PackRGBA8(U8 r, U8 g, U8 b);

// =============================================================================
// Initialization
// =============================================================================
bool InitVideo() {
    if (!IsWindowInitialized()) {
        LogPrintf("Error: InitVideo before InitWindow.\n");
        return false;
    }
    return true;
}

void EndVideo() {
    if (Phys) {
        free(Phys);
        Phys = NULL;
    }
    ModeResX = 0;
    ModeResY = 0;
    s_screenLockCount = 0;
}

bool CreateVideoSurface(U32 resX, U32 resY) {
    if (Phys) free(Phys);

    ModeResX = resX;
    ModeResY = resY;

    Phys = malloc((size_t)resX * (size_t)resY);
    if (!Phys) {
        LogPrintf("Error: CreateVideoSurface OOM (%lu x %lu).\n",
                  (unsigned long)resX, (unsigned long)resY);
        return false;
    }
    memset(Phys, 0, (size_t)resX * (size_t)resY);

    for (U32 y = 0; y < resY && y < ADELINE_MAX_Y_RES; ++y) {
        TabOffPhysLine[y] = y * resX;
    }

    // Default-init LUT to a grayscale identity (index i -> RGB(i,i,i)). If
    // the engine somehow fails to upload a palette but Phys has paletted
    // pixels, OSScreen still shows a visible grey image instead of pure
    // black. The engine's SetVideoPalette will overwrite indices once it
    // actually pushes a palette (FadeToPal/PaletteSync).
    for (S32 i = 0; i < PAL_MAX_COLORS; ++i) {
        U8 v = (U8)i;
        s_paletteLUT[i] = PackRGBA8(v, v, v);
    }

    LogPrintf("Wii U Video: %lux%lu 8bpp software fb @%p\n",
              (unsigned long)resX, (unsigned long)resY, Phys);
    return true;
}

U32 VideoSurfacePitch() {
    return ModeResX;
}

// Forward decl: the blit hook lives in SYSTEM/WIIU_BACKEND.CPP because it
// owns OSScreen state; we just call it when the engine releases the surface.
extern "C" void WiiU_BlitGameFramebuffer(const void *phys, U32 resX, U32 resY,
                                         const U32 *paletteLUT);

// Set to 1 by MainLoop on first iteration. Until then the boot path performs
// many Lock/Unlock pairs (BoxBlit during palette fades, splash screens, etc.)
// and the per-pixel OSScreen blit at each one would slow boot from <1s to
// ~30s. Gate the present so we only push frames once gameplay is running.
extern "C" volatile U8 wiiu_gameloop_active = 0;

void LockVideoSurface() {
    ++s_screenLockCount;
    if (wiiu_gameloop_active) {
        static int s_log = 0;
        if (s_log < 4) {
            LogPrintf("[LockVid#post-gameloop] count=%d\n", (int)s_screenLockCount);
            s_log++;
        }
    }
}

void UnlockVideoSurface() {
    if (s_screenLockCount > 0) --s_screenLockCount;
    if (wiiu_gameloop_active) {
        static int s_log = 0;
        if (s_log < 8) {
            U32 px0 = 0, px1 = 0, pxMid = 0, pxLast = 0;
            U32 nonZeroCount = 0;
            if (Phys && ModeResX > 0 && ModeResY > 0) {
                const U8 *src = (const U8 *)Phys;
                size_t total = (size_t)ModeResX * (size_t)ModeResY;
                px0 = src[0];
                px1 = src[1];
                size_t midOff = ((size_t)ModeResY / 2) * (size_t)ModeResX + (size_t)ModeResX / 2;
                pxMid = src[midOff];
                pxLast = src[total - 1];
                // Sample 1 in 1024 to estimate populated pixels without scanning all 300K.
                for (size_t i = 0; i < total; i += 1024) {
                    if (src[i] != 0) nonZeroCount++;
                }
            }
            LogPrintf("[UnlockVid#post-gameloop] count=%d Phys=%p px0=%02X px1=%02X pxMid=%02X pxLast=%02X nonZeroSampled=%u/300\n",
                      (int)s_screenLockCount, Phys,
                      (unsigned)px0, (unsigned)px1, (unsigned)pxMid, (unsigned)pxLast,
                      (unsigned)nonZeroCount);
            s_log++;
        }
    }
    // Present on every unlock, including BEFORE MainLoop sets
    // wiiu_gameloop_active. The gate dated from the V1 PutPixelEx blit
    // (per-pixel OSScreen calls made the boot's many Lock/Unlock pairs cost
    // ~30 s); the V2 direct-write blit is a few ms, and the gate had a real
    // bug: it was never hit while saves failed to persist (every boot was
    // "first launch" and auto-flowed into gameplay), but once V10.6 made
    // saves work, boots with existing saves stop at the main menu WAITING
    // FOR INPUT on a never-presented frame — black screens + music.
    if (s_screenLockCount == 0 && Phys) {
        static int s_paletteDumped = 0;
        if (s_paletteDumped < 3) {
            LogPrintf("[Palette] LUT[0]=0x%08X [1]=0x%08X [B7]=0x%08X [FF]=0x%08X\n",
                      (unsigned)s_paletteLUT[0], (unsigned)s_paletteLUT[1],
                      (unsigned)s_paletteLUT[0xB7], (unsigned)s_paletteLUT[0xFF]);
            s_paletteDumped++;
        }
        WiiU_BlitGameFramebuffer(Phys, ModeResX, ModeResY, s_paletteLUT);
    }
}

void WaitVideoSync() {
    // V1: software loop, no vsync coupling. V2 will sync to GX2 swap.
}

// OSScreenPutPixelEx on Wii U expects color in RGBA8 byte order (high byte =
// R, low byte = A). Building 0xAARRGGBB here would make the alpha byte land
// in the R slot, which is why an all-index-0 framebuffer (logically black)
// rendered as a solid red rectangle. Pack as 0xRRGGBBAA instead.
static inline U32 PackRGBA8(U8 r, U8 g, U8 b) {
    return ((U32)r << 24) | ((U32)g << 16) | ((U32)b << 8) | 0xFFu;
}

void SetVideoPalette(const U8 src[], S32 startIdx, S32 count) {
    if (!src || startIdx < 0 || count <= 0) return;
    if (startIdx + count > PAL_MAX_COLORS) count = PAL_MAX_COLORS - startIdx;
    {
        static int s_preLog = 0, s_postLog = 0;
        bool isPost = (wiiu_gameloop_active != 0);
        int *cnt = isPost ? &s_postLog : &s_preLog;
        if (*cnt < 8) {
            // Sample a few entries (not just src[0..5]) — palette indices used
            // by terrain tend to be in the higher range (0xB7 etc).
            LogPrintf("[SetVideoPal-%s] start=%d cnt=%d src[0..2]=%02X%02X%02X [B7*3]=%02X%02X%02X [FE*3]=%02X%02X%02X\n",
                      isPost ? "POST" : "pre", (int)startIdx, (int)count,
                      (unsigned)src[0], (unsigned)src[1], (unsigned)src[2],
                      (unsigned)src[0xB7*3+0], (unsigned)src[0xB7*3+1], (unsigned)src[0xB7*3+2],
                      (unsigned)src[0xFE*3+0], (unsigned)src[0xFE*3+1], (unsigned)src[0xFE*3+2]);
            (*cnt)++;
        }
    }
    for (S32 i = 0; i < count; ++i) {
        U8 r = src[i * 3 + 0];
        U8 g = src[i * 3 + 1];
        U8 b = src[i * 3 + 2];
        s_paletteLUT[startIdx + i] = PackRGBA8(r, g, b);
    }
}

void SetVideoPaletteCol(S32 colorIdx, U8 r, U8 g, U8 b) {
    if (colorIdx < 0 || colorIdx >= PAL_MAX_COLORS) return;
    s_paletteLUT[colorIdx] = PackRGBA8(r, g, b);
}

void SetVideoPaletteSync(const U8 src[]) {
    SetVideoPalette(src, 0, PAL_MAX_COLORS);
}

void CopyVideoArea(void *dst, const void *src, const U32 tabOffDst[],
                   const T_BOX *area) {
    if (!dst || !src || !area || !tabOffDst) return;
    if (ModeResX == 0 || ModeResY == 0) return;

    S32 x0 = area->x0, x1 = area->x1;
    S32 y0 = area->y0, y1 = area->y1;

#ifdef LBA2_TARGET_WIIU
    {
        static int s_log = 0;
        if (s_log < 6 && wiiu_gameloop_active) {
            // Sample 1-in-1024 from src (Log) to know if anything's actually
            // been drawn into the offscreen buffer before this copy.
            U32 srcNonZero = 0;
            const U8 *s8 = (const U8 *)src;
            size_t total = (size_t)ModeResX * (size_t)ModeResY;
            for (size_t i = 0; i < total; i += 1024) {
                if (s8[i] != 0) srcNonZero++;
            }
            LogPrintf("[CopyVideoArea] dst=%p src=%p box={x0=%d y0=%d x1=%d y1=%d} srcNZ=%u/300\n",
                      dst, src, (int)x0, (int)y0, (int)x1, (int)y1, (unsigned)srcNonZero);
            s_log++;
        }
    }
#endif

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (S32)ModeResX) x1 = (S32)ModeResX;
    if (y1 > (S32)ModeResY) y1 = (S32)ModeResY;
    if (x1 <= x0 || y1 <= y0) return;

    U8       *dst8 = (U8 *)dst;
    const U8 *src8 = (const U8 *)src;
    const size_t width = (size_t)(x1 - x0);
    const size_t srcStride = (size_t)ModeResX;

    for (S32 y = y0; y < y1; ++y) {
        U8       *drow = dst8 + tabOffDst[y] + x0;
        const U8 *srow = src8 + (size_t)y * srcStride + x0;
        memcpy(drow, srow, width);
    }
}

void SetPrePresentCallback(PrePresentFn fn) {
    s_prePresent = fn;
}

void HandleEventsVideo(const void *event) {
    (void)event;
}

// =============================================================================
// Renderer hooks (normally in WINDOW.CPP for SDL build; KOS port moved them
// to SVGA/KOS.CPP because they own the platform texture. Same on Wii U.)
// =============================================================================

// Blit hook implemented in SYSTEM/WIIU_BACKEND.CPP — owns OSScreen state.
extern "C" void WiiU_BlitGameFramebuffer(const void *phys, U32 resX, U32 resY,
                                         const U32 *paletteLUT);

void PresentRendererFrame(const void *pixels, U32 pitch) {
    (void)pixels; (void)pitch;
    {
        static int s_log = 0;
        if (s_log < 2) {
            LogPrintf("[PresentRendererFrame] call#%d Phys=%p\n", s_log, Phys);
            s_log++;
        }
    }
    // Engine has fully drawn into Phys; push it to OSScreen so the player
    // actually sees the frame. V2 will move to GX2 with a paletted-LUT shader
    // and skip the per-pixel CPU loop.
    WiiU_BlitGameFramebuffer(Phys, ModeResX, ModeResY, s_paletteLUT);
}

bool LockRendererTexture(void **pixels, int *pitch) {
    if (pixels) *pixels = Phys;
    if (pitch)  *pitch  = (int)ModeResX;
    return Phys != NULL;
}

void UnlockAndPresentRendererTexture() {
    WiiU_BlitGameFramebuffer(Phys, ModeResX, ModeResY, s_paletteLUT);
}

#ifdef __cplusplus
}
#endif
