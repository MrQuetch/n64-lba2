#pragma once

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LIMITS.H>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// --- Public state ------------------------------------------------------------
extern void *Log;
extern void *Screen;
extern U32 ModeDesiredX;                  ///< X resolution for Log and Screen
extern U32 ModeDesiredY;                  ///< Y resolution for Log and Screen
extern U32 TabOffLine[ADELINE_MAX_Y_RES]; ///< Line offsets for Log and Screen

// --- Initialization ----------------------------------------------------------
bool InitScreen();
void EndScreen();

// --- Interface ---------------------------------------------------------------
bool CreateScreenMemory(U32 resX, U32 resY);

// -----------------------------------------------------------------------------
// [renderscale] N64: the game logic keeps addressing a VIRTUAL 640x480 screen
// (ModeDesiredX/Y, clip rectangle, ScreenXMin.., Xp/Yp, dirty boxes, every
// call site in SOURCES) while Log/Screen/Phys are PHYSICAL 320x240 buffers
// (TabOffLine pitch, ModeResX/Y). Only the pixel-writing cores in LIB386
// convert, right before touching memory: RS_V2P() on coordinates, RS_PushPhysClip()
// around the fill so the core's clip tests see the physical rectangle.
#ifdef LBA2_TARGET_N64
#define RS_SHIFT 1
#else
#define RS_SHIFT 0
#endif
#define RS_V2P(v) ((S32)(v) >> RS_SHIFT) /* virtual coordinate -> physical */
#define RS_P2V(v) ((S32)(v) << RS_SHIFT) /* physical coordinate -> virtual */
#define PhysResX (ModeDesiredX >> RS_SHIFT)
#define PhysResY (ModeDesiredY >> RS_SHIFT)

/// Save the (virtual) clip rectangle in save[4] and replace it with its
/// physical counterpart; RS_PopClip() restores it. No-op when RS_SHIFT == 0.
void RS_PushPhysClip(S32 save[4]);
void RS_PopClip(const S32 save[4]);

/// In-place downscale of a linear 640x480 8bpp image (SCREEN.HQR loading
/// screens, menu backgrounds) to the physical layout of Log/Screen. No-op on
/// hosts where the physical and virtual resolutions coincide.
void RS_HalveScreenImage(void *buf);

/// Load_HQR() of a full-screen image straight into a screen buffer, followed
/// by RS_HalveScreenImage(). Returns what Load_HQR() returned.
U32 Load_HQR_Image(const char *name, void *ptr, S32 index);

#define CopyScreen(src, dst) FastCopy(dst, src, PhysResX * PhysResY)

// =============================================================================
#ifdef __cplusplus
}
#endif
