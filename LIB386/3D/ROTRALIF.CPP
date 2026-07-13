
#include <3D/ROTRALIS.H>

#include <math.h>
#include <SYSTEM/DEBUG_TRACES.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/LOGPRINT.H>
#endif

/*
 * Batch rotate+translate matching ASM RotTransListF.
 * Uses long double + lrintl to match x87 fistp round-to-nearest-even.
 */
void RotTransListF(TYPE_MAT *Mat, TYPE_VT16 *Dst, TYPE_VT16 *Src,
                   S32 NbPoints) {
    LIB386_TRACE_CPP("RotTransListF", "1", "Mat=%p dst=%p src=%p n=%d", (void *)Mat, (void *)Dst, (void *)Src, NbPoints);
    for (S32 i = 0; i < NbPoints; i++) {
        long double x = (long double)Src[i].X;
        long double y = (long double)Src[i].Y;
        long double z = (long double)Src[i].Z;

#ifdef LBA2_TARGET_WIIU
        // "Body Y flies" diagnostic: if a result exceeds S16 range the (S16)
        // cast below wraps and the body teleports vertically. On x86 the same
        // wrap would happen, so overflow here means corrupted INPUT (matrix
        // or translation) — dump both to find the upstream culprit.
        {
            long double rx = x * Mat->F.M11 + y * Mat->F.M12 + z * Mat->F.M13 + (long double)Mat->F.TX;
            long double ry = x * Mat->F.M21 + y * Mat->F.M22 + z * Mat->F.M23 + (long double)Mat->F.TY;
            long double rz = x * Mat->F.M31 + y * Mat->F.M32 + z * Mat->F.M33 + (long double)Mat->F.TZ;
            static int s_ovf = 0;
            if ((rx > 32767.0L || rx < -32768.0L ||
                 ry > 32767.0L || ry < -32768.0L ||
                 rz > 32767.0L || rz < -32768.0L) && s_ovf < 16) {
                LogPrintf("[RotTrans OVF] i=%ld out=%ld,%ld,%ld src=(%d,%d,%d) T=%ld,%ld,%ld",
                          (long)i, (long)rx, (long)ry, (long)rz,
                          (int)Src[i].X, (int)Src[i].Y, (int)Src[i].Z,
                          (long)(double)Mat->F.TX,
                          (long)(double)Mat->F.TY,
                          (long)(double)Mat->F.TZ);
                s_ovf++;
            }
        }
#endif
        Dst[i].X = (S16)lrintl(x * Mat->F.M11 + y * Mat->F.M12 + z * Mat->F.M13 + (long double)Mat->F.TX);
        Dst[i].Y = (S16)lrintl(x * Mat->F.M21 + y * Mat->F.M22 + z * Mat->F.M23 + (long double)Mat->F.TY);
        Dst[i].Z = (S16)lrintl(x * Mat->F.M31 + y * Mat->F.M32 + z * Mat->F.M33 + (long double)Mat->F.TZ);
    }
}

// -----------------------------------------------------------------------------
Func_RotTransList *RotTransList = RotTransListF;
