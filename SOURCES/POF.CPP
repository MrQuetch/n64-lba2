#include "C_EXTERN.H"

// POF records (RESS.HQR → BufferPof) = U32 offset table + per-record data:
// colour byte, nb-points byte, S16 point pairs (little-endian on disk),
// nb-lines byte, U8 index pairs. Only the table and the point coords are
// endian-sensitive.
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
#define POF_RD16(p) ((S16)LBA2_LE16(*(U16 *)(p)))
#define POF_RD32(p) ((S32)LBA2_LE32(*(U32 *)(p)))
#else
#define POF_RD16(p) (*(S16 *)(p))
#define POF_RD32(p) (*(S32 *)(p))
#endif

//***************************************************************************
S32 PofDisplay3DExt(S32 x, S32 y, S32 z, S32 num_pof, S32 scale, S32 rotation) {
    S32 coul, n;
    S32 x0, y0, scale3d;
    S32 xpc, ypc;
    S16 *ptr_point, *ptrw;
    U8 p;
    U8 *ptr_pof;

    //	ptr_pof = (U8*)HQR_Get( HQR_Pof, num_pof ) ;

    ptr_pof = BufferPof + (U32)POF_RD32(((U32 *)BufferPof) + num_pof);

    LongWorldRotatePoint(x, y, z);
    x0 = Z0;

    if (!LongProjectPoint(X0, Y0 + 1000, Z0))
        return FALSE;
    y0 = Yp;

    if (!LongProjectPoint(X0, Y0, Z0))
        return FALSE;
    xpc = Xp * 65536 + 32768;
    ypc = Yp * 65536 + 32768;

    scale3d = ((Yp - y0) * 65536 + scale / 2) / scale;

    coul = *ptr_pof++; // couleur

    n = *ptr_pof++; // nb point

    ClearScreenMinMax();

    if (rotation != 0) {
        ptrw = (S16 *)ptr_pof;
        ptr_point = (S16 *)Obj_ListRotatedPoints;

        for (p = 0; p < n; p++) {
            Rotate(POF_RD16(&ptrw[0]), POF_RD16(&ptrw[1]), rotation);

            Xp = (xpc + X0 * scale3d) / 65536;
            Yp = (ypc + Z0 * scale3d) / 65536;

            AdjustScreenMinMax();

            *ptr_point++ = (S16)Xp;
            *ptr_point++ = (S16)Yp;
            ptrw += 2;
        }

        ptr_pof = (U8 *)ptrw;
    } else {
        ptrw = (S16 *)ptr_pof;
        ptr_point = (S16 *)Obj_ListRotatedPoints;

        for (p = 0; p < n; p++) {
            Xp = (xpc + POF_RD16(ptrw) * scale3d) / 65536;
            ptrw++;
            Yp = (ypc + POF_RD16(ptrw) * scale3d) / 65536;
            ptrw++;

            AdjustScreenMinMax();

            *ptr_point++ = (S16)Xp;
            *ptr_point++ = (S16)Yp;
        }

        ptr_pof = (U8 *)ptrw;
    }

    n = *ptr_pof++; // nb line
    while (n > 0) {
        p = *ptr_pof++;

        ptr_point = (S16 *)Obj_ListRotatedPoints + p * 2;

        x0 = *ptr_point++;
        y0 = *ptr_point;

        p = *ptr_pof++;

        ptr_point = (S16 *)Obj_ListRotatedPoints + p * 2;

        Xp = *ptr_point++;
        Yp = *ptr_point;

        Line(x0, y0, Xp, Yp, coul);

        n--;
    }

    return (TRUE);
}
