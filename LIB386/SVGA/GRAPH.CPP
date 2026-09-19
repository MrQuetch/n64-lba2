#include <SVGA/GRAPH.H>

#include <SVGA/SCREENXY.H>
#include <SVGA/CLIP.H>
#include <SVGA/SCREEN.H>

#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
// Sprite banks (SPRITES.HQR / SPRIRAW.HQR / ANIM3DS.HQR records) start with a
// LE U32 offset table; the pixel payload after it is byte-only (RLE/raw), so
// the table is the only thing to swap. Banks built at runtime in host order
// (e.g. BufferBrick) pass through untouched: WiiU_SwapHqrSubOffsetTable
// validates tab[0] as a plausible LE table start and a host-order (or already
// swapped) first entry byte-reverses to a huge value that fails the check.
// HQR cache eviction reloads records LE from disk, so the swap must live at
// the consumer entry points, not at a one-time load site.
#define WIIU_FIX_BANK_TABLE(bank) WiiU_SwapHqrSubOffsetTable((unsigned char *)(bank))
#else
#define WIIU_FIX_BANK_TABLE(bank) ((void)0)
#endif

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

/*
;----------------------------------------------------------------------------
;               DWORD   TabOffset[]     ( Un de plus pointe fin de fichier )
;
;                       Brick:
;
;                           - BYTE Delta X
;                           - BYTE Delta Y
;                           - BYTE HotX (Signed)
;                           - BYTE HotY (Signed)
;
;                               Pour chaque Ligne ( Delta Y ):
;
;                               - BYTE NbBlock
;
;                                  Par Block:
;                                   - 00xxxxxxb  xxxxxx zeros to jump
;                                   - 01xxxxxxb  xxxxxx Copy Pix
;                                   - 10xxxxxxb  xxxxxx Repeat Pix
;                                   - BYTE datas[]

;----------------------------------------------------------------------------
*/
// OffsetBegin	dd	0
// NbPix		dd	0
U8 BufferClip[512];

// -----------------------------------------------------------------------------
S32 GetDxDyGraph(U8 numgraph, S32 *x, S32 *y, const void *bankgraph) {
    WIIU_FIX_BANK_TABLE(bankgraph);
    const U32 *graphMem = (const U32 *)bankgraph;
    const U8 *graphBytes = (const U8 *)bankgraph;

    // Don't know if the value is really a signed int, but works for now
    U32 offset = graphMem[numgraph];

    *x = (graphBytes[offset] & 0xFF);
    *y = (graphBytes[offset + 1] & 0xFF);

    return 0; // TODO: Remove, make void
}

#include <stdio.h>
#include <SYSTEM/DEBUG_TRACES.H>

S32 ClippingGraph(S32 x0, S32 y0, S32 x1, S32 y1, U8 *graphics);

// [renderscale] Blit a bank whose graphics are already at the PHYSICAL
// resolution, at PHYSICAL coordinates (the pre-halved brick bank, GRILLE.CPP).
// ScreenXMin.. are reported in virtual coordinates like every other primitive.
// On hosts where RS_SHIFT == 0 this is simply AffGraph.
static S32 AffGraphPhys(S32 numgraph, S32 x, S32 y, const void *bankgraph);

S32 AffGraphNative(S32 numgraph, S32 x, S32 y, const void *bankgraph) {
    S32 clip[4];
    RS_PushPhysClip(clip);
    S32 r = AffGraphPhys(numgraph, x, y, bankgraph);
    RS_PopClip(clip);
    ScreenXMin = RS_P2V(ScreenXMin);
    ScreenYMin = RS_P2V(ScreenYMin);
    ScreenXMax = RS_P2V(ScreenXMax) + (RS_P2V(1) - 1);
    ScreenYMax = RS_P2V(ScreenYMax) + (RS_P2V(1) - 1);
    return r;
}

#if RS_SHIFT
// Skip one RLE line without drawing it.
static inline const U8 *SkipGraphLine(const U8 *g) {
    U8 nbBlock = *g++;
    for (U8 j = 0; j < nbBlock; j++) {
        U8 control = *g++;
        U8 opCode = control >> 6;
        if (opCode == 1)
            g += (control & 0x3F) + 1;
        else if (opCode == 2)
            g += 1;
    }
    return g;
}

// Decode one RLE line into dst (0 = transparent, like ClippingGraph).
static inline const U8 *DecodeGraphLine(const U8 *g, U8 *dst) {
    U8 nbBlock = *g++;
    for (U8 j = 0; j < nbBlock; j++) {
        U8 control = *g++;
        U8 opCode = control >> 6;
        U32 counter = (control & 0x3F) + 1;
        if (opCode == 0) {
            while (counter--) *dst++ = 0;
        } else if (opCode == 1) {
            while (counter--) *dst++ = *g++;
        } else {
            U8 color = *g++;
            while (counter--) *dst++ = color;
        }
    }
    return g;
}

// Virtual-coordinate, full-resolution bank -> physical screen at half scale
// (even rows / even columns, nearest). Clip tests in virtual space first
// (so ScreenXMin.. and the early-out match the original), then per pixel
// against the physical rectangle.
S32 AffGraph(S32 numgraph, S32 x, S32 y, const void *bankgraph) {
    LIB386_TRACE_CPP("AffGraph", "1", "num=%d x=%d y=%d bank=%p", numgraph, x, y, (void *)bankgraph);
    WIIU_FIX_BANK_TABLE(bankgraph);
    const U8 *data = (const U8 *)bankgraph + ((const U32 *)bankgraph)[numgraph];
    U8 deltaX = data[0];
    U8 nbLine = data[1];
    S8 hotX = (S8)data[2];
    S8 hotY = (S8)data[3];
    const U8 *graphics = data + 4;

    S32 x0 = ScreenXMin = x + hotX;
    S32 y0 = ScreenYMin = y + hotY;
    S32 x1 = ScreenXMax = x0 + deltaX - 1;
    S32 y1 = ScreenYMax = y0 + nbLine - 1;

    if (x0 > ClipXMax || y0 > ClipYMax || x1 < ClipXMin || y1 < ClipYMin)
        return 0;

    const S32 pcx0 = RS_V2P(ClipXMin), pcy0 = RS_V2P(ClipYMin);
    const S32 pcx1 = RS_V2P(ClipXMax), pcy1 = RS_V2P(ClipYMax);
    const S32 jStart = (x0 & 1) ? 1 : 0; // first even virtual column

    for (S32 i = 0; i < nbLine; i++) {
        S32 vy = y0 + i;
        S32 py = RS_V2P(vy);
        if ((vy & 1) || py < pcy0 || py > pcy1) {
            graphics = SkipGraphLine(graphics);
            continue;
        }
        graphics = DecodeGraphLine(graphics, BufferClip);
        U8 *row = (U8 *)Log + TabOffLine[py];
        for (S32 j = jStart; j < deltaX; j += 2) {
            S32 px = RS_V2P(x0 + j);
            if (px < pcx0 || px > pcx1)
                continue;
            U8 v = BufferClip[j];
            if (v)
                row[px] = v;
        }
    }
    return 0;
}
#else
S32 AffGraph(S32 numgraph, S32 x, S32 y, const void *bankgraph) {
    return AffGraphPhys(numgraph, x, y, bankgraph);
}
#endif

static S32 AffGraphPhys(S32 numgraph, S32 x, S32 y, const void *bankgraph) {
    LIB386_TRACE_CPP("AffGraph", "1", "num=%d x=%d y=%d bank=%p", numgraph, x, y, (void *)bankgraph);
    WIIU_FIX_BANK_TABLE(bankgraph);
    U8 *data = (U8 *)bankgraph + ((U32 *)bankgraph)[numgraph];
    U8 deltaX = data[0];
    U8 nbLine = data[1]; // delta Y
    S8 hotX = data[2];
    S8 hotY = data[3];
#ifdef LBA2_TARGET_WIIU
    {
        extern void LogPrintf(const char *fmt, ...);
        static int s_affgraph_n = 0;
        if (s_affgraph_n < 3) {
            U32 *tab = (U32 *)bankgraph;
            LogPrintf("[AffGraph] num=%d offset=0x%08X data=%p deltaX=%u nbLine=%u hotX=%d hotY=%d\n",
                      (int)numgraph, (unsigned)tab[numgraph], (void*)data,
                      (unsigned)deltaX, (unsigned)nbLine, (int)hotX, (int)hotY);
            s_affgraph_n++;
        }
    }
#endif

    U8 *graphics = data + 4;

    // ;-------------------- Test Clipping
    S32 x0 = ScreenXMin = x + hotX;
    S32 y0 = ScreenYMin = y + hotY;
    S32 x1 = ScreenXMax = (x + hotX) + deltaX - 1;
    S32 y1 = ScreenYMax = (y + hotY) + nbLine - 1;

    if (x0 < ClipXMin || y0 < ClipYMin ||
        x1 > ClipXMax || y1 > ClipYMax) {
        return ClippingGraph(x0, y0, x1, y1, graphics);
    }

    x1++;
    y1++;
    x1 -= x0;
    y1 -= y0;

    // ;-------------------- Calcul Offset Ecran
    U8 *screen = (U8 *)Log + TabOffLine[y0] + x0;
    U32 offsetScreen = TabOffLine[1] - x1;

    for (int i = 0; i < nbLine; i++) {
        // ;-------------------- Init NbBlock for this line
        // NextLine:
        U8 nbBlock = graphics[0];
        graphics++;

        // ;-------------------- Manage One Line
        for (int j = 0; j < nbBlock; j++) {
            // SameLine:
            U8 control = graphics[0];
            U8 opCode = (control & 0b11000000) >> 6;
            U32 counter = control & 0b00111111;
            if (opCode == 0) // Incrust
            {
                // JumpZero:
                graphics++;
                screen = screen + counter + 1;
            } else {
                counter++; // One More Please...
                if (opCode == 1) {
                    // jmp	WriteDiffPix	; Pix Differents
                    graphics++;
                    while (counter > 0) {
                        *(screen++) = *(graphics++);
                        counter--;
                    }
                } else {
                    // ;-------------------- Repeat Same Color
                    U8 color = graphics[1]; // Color to Repeat
                    graphics += 2;
                    while (counter > 0) {
                        *(screen++) = color;
                        counter--;
                    }
                }
            }
        }
        screen += offsetScreen;
    }

    return 0;
}

S32 ClippingGraph(S32 x0, S32 y0, S32 x1, S32 y1, U8 *graphics) {
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
            graphics++;
            for (int j = 0; j < nbBlock; j++) {
                // SameH:
                U8 control = graphics[0];
                graphics++;
                U8 opCode = (control & 0b11000000) >> 6;
                if (opCode != 0) {
                    U32 jump = 0;
                    if (opCode == 1) {
                        jump = control & 0b00111111;
                    }
                    graphics += jump + 1;
                }
            }
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
    U32 offset = 0;
    if (x0 < ClipXMin) {
        offset = ClipXMin - x0;
    }

    // PasGauche:
    U32 OffsetBegin = offset;
    // ;---------------------- Clipping Right
    offset += x0 - 1;
    offset -= x1;
    offset = -offset;
    U32 NbPix = offset;
    x1 -= ClipXMax;

    if (x1 > 0) {
        NbPix -= x1;
    }

    // PasDroit:
    U8 *screen = (U8 *)Log + TabOffLine[y0] + x0;
    S32 deltaY = y1 - y0;
    for (int i = 0; i <= deltaY; i++) {
        U8 *buffer = BufferClip;
        U8 nbBlock = graphics[0];
        graphics++;

        for (int j = 0; j < nbBlock; j++) {
            U8 control = graphics[0];
            graphics++;
            U8 opCode = (control & 0b11000000) >> 6;
            U32 counter = control & 0b00111111;
            counter++;
            if (opCode == 0) {
                // jmp JumpL
                while (counter > 0) {
                    *(buffer++) = 0;
                    counter--;
                }
            } else if (opCode == 1) {
                // jne	WriteDiffL
                while (counter > 0) {
                    *(buffer++) = *(graphics++);
                    counter--;
                }
            } else {
                U8 color = graphics[0];
                graphics++;
                while (counter > 0) {
                    *(buffer++) = color;
                    counter--;
                }
            }
        }

        U8 *src = BufferClip + OffsetBegin;
        U8 *dst = screen + OffsetBegin;
        for (int j = 0; j < NbPix; j++) {
            U8 value = *(src++);
            if (value != 0) {
                *dst = value;
            }
            dst++;
        }

        screen += TabOffLine[1];
    }

    return 0; // TODO: Remove, make void
}

S32 GetBoxGraph(S32 numgraph, S32 *x0, S32 *y0, S32 *x1, S32 *y1,
                const void *bankgraph) {
    WIIU_FIX_BANK_TABLE(bankgraph);
    U8 *bank = (U8 *)bankgraph + ((U32 *)bankgraph)[numgraph];

    U8 dx = bank[0];
    U8 dy = bank[1];
    S8 hotX = (S8)bank[2];
    S8 hotY = (S8)bank[3];

    *x0 = hotX;
    *y0 = hotY;
    *x1 = hotX + dx;
    *y1 = hotY + dy;

    return 0;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
