#include <3D/IMATTRA.H>
#include <SYSTEM/DEBUG_TRACES.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/LOGPRINT.H>
#endif

void InitMatrixTransF(TYPE_MAT *MatDst, S32 tx, S32 ty, S32 tz) {
    LIB386_TRACE_CPP("InitMatrixTransF", "1", "Dst=%p tx=%d ty=%d tz=%d", (void *)MatDst, tx, ty, tz);
#ifdef LBA2_TARGET_WIIU
    // Body-Y diagnostic: if the huge TY seen in RotTransListF enters through
    // here, the group loop passed it (TransY/Y0); if OVF fires without this,
    // the translation came in via a matrix copy (Return_BodyDisplay path).
    {
        static int s_log = 0;
        if ((ty > 30000 || ty < -30000) && s_log < 8) {
            LogPrintf("[IMatTra BIG] tx=%ld ty=%ld tz=%ld", (long)tx, (long)ty, (long)tz);
            s_log++;
        }
    }
#endif
    MatDst->F.TX = (float)tx;
    MatDst->F.TY = (float)ty;
    MatDst->F.TZ = (float)tz;
}

// -----------------------------------------------------------------------------
Func_InitMatrixTrans *InitMatrixTrans = InitMatrixTransF; ///< Do Not call from C
