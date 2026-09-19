#include <SVGA/MASK.H>
#include <SVGA/CLIP.H>
#include <SVGA/SCREEN.H>
#include <SVGA/SCREENXY.H>
#include <stdio.h>
#include <string.h>
#include <SYSTEM/DEBUG_TRACES.H>

U8 ColMask = 0;

S32 ClippingMask(S32 x0, S32 y0, S32 x1, S32 y1, U8 *graphics) {
    LIB386_TRACE_CPP("ClippingMask", "1", "x0=%d y0=%d x1=%d y1=%d gfx=%p", x0, y0, x1, y1, (void *)graphics);
    if (x0 > ClipXMax || y0 > ClipYMax ||
        x1 < ClipXMin || y1 < ClipYMin) {
        return 0;
    }

    if (y0 < ClipYMin) {
        // ---------------------- Clipping Top, Skip ClipYMin-ECX Line(s)

        S32 yDiff = ClipYMin - y0;
        for (int i = 0; i < yDiff; i++) {
            // NextH:
            U8 nbBlock = graphics[0];
            graphics += nbBlock + 1; // Jump data
        }

        y0 = ClipYMin;
    }

    // ;---------------------- Clipping Bottom
    // PasHaut:
    if (y1 > ClipYMax) {
        y1 = ClipYMax;
    }

    // ;---------------------- Clipping Left
    // PasBas:
    U32 OffsetBegin = 0;
    if (x0 < ClipXMin) {
        OffsetBegin = ClipXMin - x0;
    }

    // PasGauche:
    // ;---------------------- Clipping Right
    S32 offsetEnd = x1 - x0 - OffsetBegin + 1;

    if (x1 > ClipXMax) {
        x1 -= ClipXMax;
        offsetEnd -= x1;
        x1 = ClipXMax;
    }

    // PasDroit:
    ScreenXMax = x1;
    x1 = OffsetBegin;

    x1 += x0;
    ScreenYMin = y0;
    ScreenXMin = x1;
    ScreenYMax = y1;

    U8 *screen = (U8 *)Log + TabOffLine[y0] + x0 + OffsetBegin;
    U8 color = ColMask;

    S32 deltaY = y1 - y0;
    for (int i = 0; i <= deltaY; i++) {
        U8 *lineScreen = screen;
        U8 nbBlock = graphics[0];
        graphics++;
        S32 bytesToSkip = OffsetBegin;
        S32 remainingWidth = offsetEnd;

        for (U8 blockIndex = 0; blockIndex < nbBlock; blockIndex++) {
            U8 count = graphics[0];
            graphics++;

            if ((blockIndex & 1) == 0) {
                if (bytesToSkip >= count) {
                    bytesToSkip -= count;
                    continue;
                }

                S32 visibleJump = count - bytesToSkip;
                bytesToSkip = 0;

                if (visibleJump >= remainingWidth) {
                    graphics += (nbBlock - blockIndex - 1);
                    remainingWidth = 0;
                    break;
                }

                lineScreen += visibleJump;
                remainingWidth -= visibleJump;
                continue;
            }

            if (bytesToSkip >= count) {
                bytesToSkip -= count;
                continue;
            }

            S32 visibleFill = count - bytesToSkip;
            bytesToSkip = 0;

            if (visibleFill > remainingWidth) {
                visibleFill = remainingWidth;
            }

            for (int j = 0; j < visibleFill; j++) {
                *lineScreen = color;
                lineScreen++;
            }

            remainingWidth -= visibleFill;
            if (remainingWidth <= 0) {
                graphics += (nbBlock - blockIndex - 1);
                break;
            }
        }

        screen += TabOffLine[1];
    }

    return 0; // TODO: Remove, make void
}

#if RS_SHIFT
// [renderscale] Half-scale mask blit (fonts): virtual coordinates and
// full-resolution glyphs onto the physical screen. Each physical pixel is
// the OR of its 2x2 virtual block, so 1-pixel strokes survive the downscale.
S32 AffMask(S32 nummask, S32 x, S32 y, void *bankmask) {
    U8 *data = (U8 *)bankmask + ((U32 *)bankmask)[nummask];
    U8 deltaX = data[0];
    U8 nbLine = data[1];
    S32 hotX = data[2];
    S32 hotY = data[3];
    U8 *graphics = data + 4;

    x += hotX;
    y += hotY;
    S32 x1 = x + deltaX - 1;
    S32 y1 = y + nbLine - 1;

    ScreenXMin = x;
    ScreenXMax = x1;
    ScreenYMin = y;
    ScreenYMax = y1;

    if (x > ClipXMax || y > ClipYMax || x1 < ClipXMin || y1 < ClipYMin)
        return 0;

    const S32 pcx0 = RS_V2P(ClipXMin), pcy0 = RS_V2P(ClipYMin);
    const S32 pcx1 = RS_V2P(ClipXMax), pcy1 = RS_V2P(ClipYMax);
    const S32 px0 = RS_V2P(x);
    const S32 accLen = RS_V2P(x1) - px0 + 1; // <= 129
    static U8 acc[256];
    const U8 color = ColMask;

    memset(acc, 0, accLen);
    for (S32 i = 0; i < nbLine; i++) {
        S32 vy = y + i;
        // decode one line: alternating [jump][write] runs
        U8 nbBlock = *graphics++;
        S32 vx = x;
        while (nbBlock > 0) {
            vx += *graphics++; // zeros to jump
            nbBlock--;
            if (nbBlock == 0)
                break;
            U8 toWrite = *graphics++;
            nbBlock--;
            for (U8 k = 0; k < toWrite; k++, vx++)
                acc[RS_V2P(vx) - px0] = 1;
        }
        // flush on the second row of each physical line (or the last one)
        if ((vy & 1) || i == nbLine - 1) {
            S32 py = RS_V2P(vy);
            if (py >= pcy0 && py <= pcy1) {
                U8 *row = (U8 *)Log + TabOffLine[py];
                for (S32 k = 0; k < accLen; k++) {
                    if (!acc[k])
                        continue;
                    S32 px = px0 + k;
                    if (px >= pcx0 && px <= pcx1)
                        row[px] = color;
                }
            }
            memset(acc, 0, accLen);
        }
    }
    return 0;
}
#else
S32 AffMask(S32 nummask, S32 x, S32 y, void *bankmask) {
    U8 *data = (U8 *)bankmask + ((U32 *)bankmask)[nummask];
    U8 deltaX = data[0];
    U8 nbLine = data[1]; // delta Y
    S32 hotX = data[2];
    S32 hotY = data[3];

    U8 *graphics = data + 4;

    x += hotX;
    y += hotY;

    S32 x1 = x + deltaX - 1;
    S32 y1 = y + nbLine - 1;

    // ----------------------------------------------- Test Clipping
    if (x < ClipXMin || y < ClipYMin || x1 > ClipXMax || y1 > ClipYMax) {
        return ClippingMask(x, y, x1, y1, graphics);
    }

    ScreenXMin = x;
    ScreenXMax = x1;
    ScreenYMin = y;
    ScreenYMax = y1;

    // ----------------------------------------------- Compute Offset Ecran
    U8 *screen = (U8 *)Log + TabOffLine[y] + x;
    U32 offsetScreen = TabOffLine[1] - deltaX;
    U8 color = ColMask;

    for (int i = 0; i < nbLine; i++) {
        // ;-------------------- Init NbBlock for this line
        // NextLine:
        U8 nbBlock = graphics[0];
        graphics++;

        while (nbBlock > 0) {
            // ;-------------------- Manage One Line
            // SameLine:
            U8 toJump = graphics[0]; // Nb Zero to jump
            screen += toJump;
            nbBlock--;
            if (nbBlock == 0) {
                // EndBlock:
                graphics++;
                break;
            }

            U8 toWrite = graphics[1]; // Nb Zero to write
            graphics += 2;
            for (int k = 0; k < toWrite; k++) {
                *(screen++) = color;
            }

            nbBlock--;
        }
        screen += offsetScreen;
    }

    return 0; // TODO: make it void?
}
#endif /* RS_SHIFT */
