
#include <SVGA/SCALESPT.H>

#include <SVGA/CLIP.H>
#include <SVGA/SCREEN.H>
#include <SVGA/SCREENXY.H>
#include <SYSTEM/DEBUG_TRACES.H>

#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
// LE U32 offset table at the bank head — see GRAPH.CPP for the rationale.
#define WIIU_FIX_BANK_TABLE(bank) WiiU_SwapHqrSubOffsetTable((unsigned char *)(bank))
#else
#define WIIU_FIX_BANK_TABLE(bank) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    U8 Delta_X;
    U8 Delta_Y;
    S8 Hot_X;
    S8 Hot_Y;
} Struc_Sprite;

static void set_exit_bounds(void) {
    ScreenXMin = 32000;
    ScreenXMax = -32000;
    ScreenYMin = 32000;
    ScreenYMax = -32000;
}

static S32 mul_shift16(S32 left, S32 right) {
    return (S32)(((long long)left * (long long)right) >> 16);
}

static S32 divide_16_16(S32 value) {
    return (S32)(((long long)1 << 32) / (long long)value);
}

static S32 sprite_center_16_16(U8 delta) {
    return (S32)(((U32)(delta >> 1) << 16) + ((delta & 1u) ? 0x8000u : 0u));
}

// Opaque twin of ScaleSpriteTransp (SCALESPT.CPP): same 16.16 fixed-point
// scaling as SCALESPI.ASM (65536 = 1:1), plain write instead of the blend
// table. The pre-fix version ignored factorx/factory entirely, so every
// opaque scaled sprite (e.g. the Pistolaser impact particles, magic-ball
// sprite at scale 400 ≈ 1/6) rendered at full size.
static void ScaleSpritePhys(S32 num, S32 x, S32 y, S32 factorx, S32 factory, void *ptrbank);

// [renderscale] public entry: virtual coordinates / 16.16 factor relative to
// the virtual resolution -> physical blit, then ScreenXMin.. back to virtual.
static void RS_BoundsToVirtual(void) {
#if RS_SHIFT
    if (ScreenXMin != 32000) { // not the "nothing drawn" sentinel
        ScreenXMin = RS_P2V(ScreenXMin);
        ScreenYMin = RS_P2V(ScreenYMin);
        ScreenXMax = RS_P2V(ScreenXMax);
        ScreenYMax = RS_P2V(ScreenYMax);
    }
#endif
}

void ScaleSprite(S32 num, S32 x, S32 y, S32 factorx, S32 factory, void *ptrbank) {
    S32 clip[4];
    RS_PushPhysClip(clip);
    ScaleSpritePhys(num, RS_V2P(x), RS_V2P(y), factorx >> RS_SHIFT, factory >> RS_SHIFT, ptrbank);
    RS_PopClip(clip);
    RS_BoundsToVirtual();
}

static void ScaleSpritePhys(S32 num, S32 x, S32 y, S32 factorx, S32 factory, void *ptrbank) {
    LIB386_TRACE_CPP("ScaleSprite", "1", "num=%d x=%d y=%d fx=%d fy=%d bank=%p", num, x, y, factorx, factory, (void *)ptrbank);
    if (factorx <= 0) {
        if (factorx == 0) {
            set_exit_bounds();
            return;
        }
        factorx = 0x7FFFFFFF;
    }
    if (factory <= 0) {
        if (factory == 0) {
            set_exit_bounds();
            return;
        }
        factory = 0x7FFFFFFF;
    }

    WIIU_FIX_BANK_TABLE(ptrbank);
    U32 *tab = (U32 *)ptrbank;
    Struc_Sprite *sprite = (Struc_Sprite *)((U8 *)tab + tab[num]);
    U8 *srcBase = (U8 *)sprite + sizeof(Struc_Sprite);
    const S32 clipXMaxPlusOne = ClipXMax + 1;
    const S32 clipYMaxPlusOne = ClipYMax + 1;

    // Fast mode: 1:1 scaling
    if (factorx == 65536 && factory == 65536) {
        S32 sx = sprite->Hot_X + x;
        S32 sy = sprite->Hot_Y + y;
        S32 sw = sprite->Delta_X;
        S32 sh = sprite->Delta_Y;

        // Clipping
        S32 start_x = 0, start_y = 0;
        S32 end_x = sx + sw;
        S32 end_y = sy + sh;
        if (sx < ClipXMin) {
            start_x = ClipXMin - sx;
            sx = ClipXMin;
        }
        if (sy < ClipYMin) {
            start_y = ClipYMin - sy;
            sy = ClipYMin;
        }
        if (end_x > clipXMaxPlusOne) {
            end_x = clipXMaxPlusOne;
        }
        if (end_y > clipYMaxPlusOne) {
            end_y = clipYMaxPlusOne;
        }
        if (sx >= end_x || sy >= end_y) {
            set_exit_bounds();
            return;
        }
        ScreenXMin = sx;
        ScreenXMax = end_x;
        ScreenYMin = sy;
        ScreenYMax = end_y;

        U8 *src = srcBase + start_y * sw + start_x;
        U8 *dst = (U8 *)Log + TabOffLine[sy] + sx;
        S32 dst_line_offset = TabOffLine[1] - (end_x - sx);
        S32 src_line_offset = sw - (end_x - sx);
        for (S32 yy = sy; yy < end_y; ++yy) {
            for (S32 xx = sx; xx < end_x; ++xx) {
                if (*src) {
                    *dst = *src;
                }
                ++src;
                ++dst;
            }
            src += src_line_offset;
            dst += dst_line_offset;
        }
        return;
    }

    // Scaled mode — mirrors ScaleSpriteTransp() minus the blend table
    S32 newDeltaX = mul_shift16(sprite->Delta_X, factorx);
    S32 newDeltaY = mul_shift16(sprite->Delta_Y, factory);
    S32 newHotX = mul_shift16((S8)sprite->Hot_X, factorx);
    S32 newHotY = mul_shift16((S8)sprite->Hot_Y, factory);
    S32 invFactorX = divide_16_16(factorx);
    S32 invFactorY = divide_16_16(factory);
    S32 startTexX = sprite_center_16_16(sprite->Delta_X) - invFactorX * (newDeltaX >> 1);
    S32 startTexY = sprite_center_16_16(sprite->Delta_Y) - invFactorY * (newDeltaY >> 1);

    S32 leftX = x + newHotX;
    S32 topY = y + newHotY;
    S32 rightX = leftX + newDeltaX;
    S32 bottomY = topY + newDeltaY;

    ScreenXMin = leftX;
    if (leftX > clipXMaxPlusOne) {
        set_exit_bounds();
        return;
    }

    if (leftX < ClipXMin) {
        ScreenXMin = ClipXMin;
        S32 clipDelta = (ClipXMin - 1) - leftX;
        S32 adjusted = startTexX + clipDelta * invFactorX;
        startTexX += adjusted;
    }

    if (rightX < ClipXMin) {
        set_exit_bounds();
        return;
    }
    ScreenXMax = rightX;
    if (rightX >= clipXMaxPlusOne) {
        ScreenXMax = clipXMaxPlusOne;
    }

    ScreenYMin = topY;
    if (topY > clipYMaxPlusOne) {
        set_exit_bounds();
        return;
    }

    if (topY < ClipYMin) {
        ScreenYMin = ClipYMin;
        S32 clipDelta = (ClipYMin - 1) - topY;
        S32 adjusted = startTexY + clipDelta * invFactorY;
        startTexY += adjusted;
    }

    if (bottomY <= ClipYMin) {
        set_exit_bounds();
        return;
    }
    ScreenYMax = bottomY;
    if (bottomY > clipYMaxPlusOne) {
        ScreenYMax = clipYMaxPlusOne;
    }

    S32 height = ScreenYMax - ScreenYMin;
    S32 width = ScreenXMax - ScreenXMin;
    if (height <= 0 || width <= 0) {
        set_exit_bounds();
        return;
    }

    U8 *dstPtr = (U8 *)Log + TabOffLine[ScreenYMin] + ScreenXMin;

    U8 *srcPtr = srcBase + ((U32)startTexY >> 16) * sprite->Delta_X;
    U32 xFrac = ((U32)startTexX) << 16;
    U32 yFrac = ((U32)startTexY) << 16;
    U32 xDec = ((U32)invFactorX) << 16;
    U32 yDec = ((U32)invFactorY) << 16;
    U32 xInt = ((U32)invFactorX) >> 16;
    U32 yInt = ((U32)invFactorY) >> 16;
    S32 srcRowWidth = sprite->Delta_X;

    srcPtr += ((U32)startTexX >> 16);

    U32 pendingRows = 0;
    for (S32 scanY = height; scanY != 0; --scanY) {
        S32 remaining = width;
        U8 *lineSrc = srcPtr;
        U8 *lineDst = dstPtr;
        U32 lineFrac = xFrac;

        while (remaining-- != 0) {
            U8 srcPixel = *lineSrc;
            if (srcPixel != 0) {
                *lineDst = srcPixel;
            }

            U32 prevFrac = lineFrac;
            lineFrac += xDec;
            lineSrc += xInt;
            if (lineFrac < prevFrac) {
                ++lineSrc;
            }
            ++lineDst;
        }

        U32 prevYFrac = yFrac;
        yFrac += yDec;
        pendingRows += yInt;
        if (yFrac < prevYFrac) {
            ++pendingRows;
        }

        if (pendingRows != 0) {
            srcPtr += srcRowWidth * pendingRows;
            pendingRows = 0;
        }

        dstPtr += TabOffLine[1];
    }
}

#ifdef __cplusplus
}
#endif
