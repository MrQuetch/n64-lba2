// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Nintendo 64 video backend (libdragon). Same contract as SVGA/WIIU.CPP:
// the engine renders into an 8bpp paletted framebuffer (Phys); on unlock we
// hand it to N64_BlitGameFramebuffer (SYSTEM/N64_BACKEND.CPP), which blits
// it through the RDP as a CI8 texture with a 256-entry RGBA5551 TLUT.
// Unlike the Wii U, the palette lookup costs zero CPU here: the palette is
// 512 bytes of TMEM and the RDP resolves indices in hardware.

#include <SVGA/DIRTYBOX.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/WINDOW.H>

#include <libdragon.h>

#include <malloc.h> // memalign
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Public state (engine-visible globals; mirrored from SDL.CPP / WIIU.CPP).
// =============================================================================
void *Phys = NULL;
U32 ModeResX = 0;
U32 ModeResY = 0;
U32 TabOffPhysLine[ADELINE_MAX_Y_RES];

// =============================================================================
// Private state
// =============================================================================
#define PAL_MAX_COLORS 256

// RGBA5551 TLUT for the RDP, double-buffered: palette fades rewrite it every
// frame, and the previous frame's rdpq_tex_upload_tlut may still be in
// flight (rdpq is asynchronous) — rewriting the buffer the RDP is reading
// corrupts the RSP queue (RSP CRASH in display_get at scene changes).
// Writes go to the back buffer, the blit reads the freshest complete one.
static U16 s_tlutBuf[2][PAL_MAX_COLORS] __attribute__((aligned(16)));
static int s_tlutCur = 0; // buffer the NEXT write set goes to / blit reads
static S32 s_screenLockCount = 0;
static PrePresentFn s_prePresent = NULL;

// Blit hook implemented in SYSTEM/N64_BACKEND.CPP — owns the display state.
extern void N64_BlitGameFramebuffer(const void *phys, U32 resX, U32 resY,
                                    const U16 *tlut);

static inline U16 PackRGBA5551(U8 r, U8 g, U8 b) {
    return (U16)(((U16)(r >> 3) << 11) | ((U16)(g >> 3) << 6) |
                 ((U16)(b >> 3) << 1) | 1u);
}

static int s_tlutDirty = 0;

// Palette writes accumulate into the back buffer (copy-on-first-write);
// the flip to the front happens once per presented frame, when the previous
// front is guaranteed no longer referenced by in-flight RDP work.
static U16 *TlutWrite(void) {
    int back = s_tlutCur ^ 1;
    if (!s_tlutDirty) {
        memcpy(s_tlutBuf[back], s_tlutBuf[s_tlutCur], sizeof(s_tlutBuf[0]));
        s_tlutDirty = 1;
    }
    return s_tlutBuf[back];
}

static U16 *TlutForBlit(void) {
    if (s_tlutDirty) {
        s_tlutCur ^= 1;
        data_cache_hit_writeback(s_tlutBuf[s_tlutCur], sizeof(s_tlutBuf[0]));
        s_tlutDirty = 0;
    }
    return s_tlutBuf[s_tlutCur];
}

// Set to 1 by MainLoop on first iteration (PERSO.CPP, WIIU-gated code path).
volatile U8 wiiu_gameloop_active = 0;

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

    // Phys only ever holds the presented frame (dirty-box copies from Log),
    // never a loaded image asset: physical size is enough.
    size_t physBytes = (size_t)resX * (size_t)resY;

    // 64-byte alignment: Phys is read by the RDP as a texture (cache-line
    // clean boundaries keep the writeback from clobbering neighbours).
    Phys = memalign(64, physBytes);
    if (!Phys) {
        LogPrintf("Error: CreateVideoSurface OOM (%lu x %lu).\n",
                  (unsigned long)resX, (unsigned long)resY);
        return false;
    }
    memset(Phys, 0, physBytes);

    for (U32 y = 0; y < resY && y < ADELINE_MAX_Y_RES; ++y) {
        TabOffPhysLine[y] = y * resX;
    }

    // Grayscale identity fallback so un-paletted frames stay visible.
    for (S32 i = 0; i < PAL_MAX_COLORS; ++i) {
        U8 v = (U8)i;
        s_tlutBuf[0][i] = PackRGBA5551(v, v, v);
    }
    s_tlutCur = 0;
    s_tlutDirty = 0;
    data_cache_hit_writeback(s_tlutBuf[0], sizeof(s_tlutBuf[0]));

    LogPrintf("N64 Video: %lux%lu 8bpp software fb @%p (RDP CI8+TLUT present)\n",
              (unsigned long)resX, (unsigned long)resY, Phys);
    return true;
}

U32 VideoSurfacePitch() {
    return ModeResX;
}

void LockVideoSurface() {
    ++s_screenLockCount;
}

void UnlockVideoSurface() {
    if (s_screenLockCount > 0) --s_screenLockCount;
    // Present on every unlock, including before MainLoop starts: the main
    // menu waits for input on a frame presented from the boot path (same
    // lesson the Wii U port learned in V10.6).
    if (s_screenLockCount == 0 && Phys) {
        N64_BlitGameFramebuffer(Phys, ModeResX, ModeResY, TlutForBlit());
    }
}

void WaitVideoSync() {
    // display_get() in the blit already paces to vsync.
}

// =============================================================================
// Palette
// =============================================================================
void SetVideoPalette(const U8 src[], S32 startIdx, S32 count) {
    if (!src || startIdx < 0 || count <= 0) return;
    if (startIdx + count > PAL_MAX_COLORS) count = PAL_MAX_COLORS - startIdx;
    U16 *tlut = TlutWrite();
    for (S32 i = 0; i < count; ++i) {
        U8 r = src[i * 3 + 0];
        U8 g = src[i * 3 + 1];
        U8 b = src[i * 3 + 2];
        tlut[startIdx + i] = PackRGBA5551(r, g, b);
    }
}

void SetVideoPaletteCol(S32 colorIdx, U8 r, U8 g, U8 b) {
    if (colorIdx < 0 || colorIdx >= PAL_MAX_COLORS) return;
    TlutWrite()[colorIdx] = PackRGBA5551(r, g, b);
}

void SetVideoPaletteSync(const U8 src[]) {
    SetVideoPalette(src, 0, PAL_MAX_COLORS);
}

// =============================================================================
// Area copy (8bpp buffer to 8bpp buffer, engine-side compositing)
// =============================================================================
void CopyVideoArea(void *dst, const void *src, const U32 tabOffDst[],
                   const T_BOX *area) {
    if (!dst || !src || !area || !tabOffDst) return;
    if (ModeResX == 0 || ModeResY == 0) return;

    S32 x0 = area->x0, x1 = area->x1;
    S32 y0 = area->y0, y1 = area->y1;

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
// Renderer hooks (WINDOW.CPP surface on SDL; owned here on console ports)
// =============================================================================
void PresentRendererFrame(const void *pixels, U32 pitch) {
    (void)pixels;
    (void)pitch;
    N64_BlitGameFramebuffer(Phys, ModeResX, ModeResY, TlutForBlit());
}

bool LockRendererTexture(void **pixels, int *pitch) {
    if (pixels) *pixels = Phys;
    if (pitch)  *pitch  = (int)ModeResX;
    return Phys != NULL;
}

void UnlockAndPresentRendererTexture() {
    N64_BlitGameFramebuffer(Phys, ModeResX, ModeResY, TlutForBlit());
}

#ifdef __cplusplus
}
#endif
