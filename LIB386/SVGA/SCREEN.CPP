#include <SVGA/SCREEN.H>

#include <POLYGON/POLY.H> // For SetScreenPitch()
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/N_MALLOC.H>
#include <SYSTEM/HQRLOAD.H>

#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// --- Public state ------------------------------------------------------------
void *Log = NULL;
void *Screen = NULL;
U32 ModeDesiredX = 0;
U32 ModeDesiredY = 0;
U32 TabOffLine[ADELINE_MAX_Y_RES];

// --- Initialization ----------------------------------------------------------
bool InitScreen() {
    // Log and Screen are not asserted to be NULL since they may be already
    //  allocated at InitMainBuffer()

    return true;
}

void EndScreen() {
    if (Log) {
        Free(Log);
        Log = NULL;
    }

    if (Screen) {
        Free(Screen);
        Screen = NULL;
    }
}

// --- Interface ---------------------------------------------------------------
bool CreateScreenMemory(U32 resX, U32 resY) {
    // Log and Screen are not asserted to be NULL since they may be already
    //  allocated at InitMainBuffer()

    // These buffers double as storage for full-screen 640x480 image assets
    // (SCREEN.HQR loading screens, menu backgrounds) loaded linearly via
    // Load_HQR — so they must hold at least a 640x480 frame regardless of the
    // (possibly smaller) 3D render resolution. Rendering still uses resX pitch.
    U32 bufBytes = resX * resY;
    if (bufBytes < 640u * 480u)
        bufBytes = 640u * 480u;

    // Allocate Log
    if (Log == NULL) {
        Log = Malloc(bufBytes + 512);
        if (!Log) {
            LogPuts("Error: Not Enough Memory to allocate Log");
            return false;
        }
    }
    memset(Log, 0, bufBytes + 512);

    // Allocate Screen
    if (Screen == NULL) {
        Screen = Malloc(bufBytes + 512);
        if (!Screen) {
            LogPuts("Error: Not Enough Memory to allocate Screen");
            Free(Log);
            Log = NULL;
            return false;
        }
    }
    memset(Screen, 0, bufBytes + 512);

    // Update public state
    ModeDesiredX = resX;
    ModeDesiredY = resY;

    // Fill the log and screen table. [renderscale] the pitch is the PHYSICAL
    // width: the buffers are rendered at resX >> RS_SHIFT (see SCREEN.H).
    S32 off = 0;
    for (S32 i = 0; i < ADELINE_MAX_Y_RES; i++) {
        TabOffLine[i] = off;
        off += PhysResX;
    }

    // Adjust memory according to pitch
    {
        /*
#ifdef __MSC_VER
    __asm {
      mov ebx, offset TabOffLine
    }
#elif __GNUC__
    __asm__ __volatile__("movl %0, %%ebx\n\t" : : "r"(&TabOffLine) : "%ebx");
#else
#error "Unsupported compiler!"
#endif
*/
        SetScreenPitch(TabOffLine);
    }

    return true;
}

// --- [renderscale] virtual -> physical helpers --------------------------------
void RS_HalveScreenImage(void *buf) {
#if RS_SHIFT
    // 640x480 linear -> 320x240 linear, sampling even rows/columns. The
    // destination offset never exceeds the source offset, so an in-place
    // front-to-back pass is safe.
    U8 *b = (U8 *)buf;
    const U32 vw = ModeDesiredX, vh = ModeDesiredY;
    const U32 pw = PhysResX, ph = PhysResY;
    for (U32 y = 0; y < ph; y++) {
        const U8 *src = b + (y << RS_SHIFT) * vw;
        U8 *dst = b + y * pw;
        for (U32 x = 0; x < pw; x++)
            dst[x] = src[x << RS_SHIFT];
    }
    (void)vh;
#else
    (void)buf;
#endif
}

U32 Load_HQR_Image(const char *name, void *ptr, S32 index) {
    U32 r = Load_HQR(name, ptr, index);
    if (r)
        RS_HalveScreenImage(ptr);
    return r;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
