#include <ANIM/INTERDEP.H>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <SYSTEM/TIMER.H>
#include <ANIM.H>
#include <3D/IMATSTD.H>
#include <3D/ROT3D.H>
#include <3D/CAMERA.H>
#include <SYSTEM/DEBUG_TRACES.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
#include <SYSTEM/LOGPRINT.H>
#  define ANIM_RD_S16(p) ((S16)LBA2_LE16((U16)(p)))
#else
#  define ANIM_RD_S16(p) ((S16)(p))
#endif

S32 ObjectSetInterDep(T_OBJ_3D *obj) {
    LIB386_TRACE_CPP("ObjectSetInterDep", "1", "obj=%p", (void *)obj);
    obj->Texture = obj->NextTexture;
    obj->Body = obj->NextBody;

    // Same time as before?
    if (TimerRefHR == obj->Time) {
        if (!(obj->Status & FLAG_BODY)) {
            obj->Status = 0; // didn't do anything
            return obj->Status;
        }
    } else if (TimerRefHR < obj->Time) {
        U32 delta = obj->Time - TimerRefHR;
        obj->NextTimer -= delta;
        obj->LastTimer -= delta;
        obj->Time = TimerRefHR;
        obj->Status = 0;
        return obj->Status; // didn't do anything
    }

    obj->Time = TimerRefHR; // Upgrade time
    T_PTR_NUM objAnim = obj->Anim;

    if (TransFctAnim != NULL) {
        objAnim.Ptr = TransFctAnim(objAnim.Num);
    }

    void *anim = objAnim.Ptr;
    S16 *frame = (S16 *)((U8 *)objAnim.Ptr + (U64)obj->NextOfsFrame); // esi

    U32 interpolator = 0; // ebp@6

    if (obj->Time >= obj->NextTimer) {
        interpolator = 0x10000;
        obj->Status = FLAG_FRAME + FLAG_CHANGE;
    } else {
        obj->Status = FLAG_CHANGE;
        U32 nbSteps = obj->NextTimer - obj->LastTimer;
        U32 deltaSteps = obj->Time - obj->LastTimer;
        interpolator = ((deltaSteps << 16) + ((nbSteps + 1) >> 1)) / nbSteps;
    }

    S32 newAlpha = obj->Alpha;
    S32 newBeta = obj->Beta;
    S32 newGamma = obj->Gamma;

    if (obj->Master & 1) {
        S32 alpha = ANIM_RD_S16(frame[5]); // Alpha
        S32 alphaDelta = (S32)(interpolator * (alpha << 20 >> 20)) >> 16;
        newAlpha = (obj->Alpha + alphaDelta - obj->LastAnimStepAlpha) & 0xFFF;
        obj->LastAnimStepAlpha = alphaDelta;
        obj->Alpha = newAlpha;

        S32 beta = ANIM_RD_S16(frame[6]); // Beta
        S32 betaDelta = (S32)(interpolator * (beta << 20 >> 20)) >> 16;
        newBeta = (obj->Beta + betaDelta - obj->LastAnimStepBeta) & 0xFFF;
        obj->LastAnimStepBeta = betaDelta;
        obj->Beta = newBeta;

        S32 gamma = ANIM_RD_S16(frame[7]); // Gamma
        S32 gammaDelta = (S32)(interpolator * (gamma << 20 >> 20)) >> 16;
        newGamma = (obj->Gamma + gammaDelta - obj->LastAnimStepGamma) & 0xFFF;
        obj->LastAnimStepGamma = gammaDelta;
        obj->Gamma = newGamma;
    }

    InitMatrixStd(&MatrixLib2, newAlpha, newBeta, newGamma);

    S32 frameX = ANIM_RD_S16(frame[1]); // X
    S32 frameY = ANIM_RD_S16(frame[2]); // Y
    S32 frameZ = ANIM_RD_S16(frame[3]); // Z

    S32 xDelta = (S32)(interpolator * frameX) >> 16;
    S32 yDelta = (S32)(interpolator * frameY) >> 16;
    S32 zDelta = (S32)(interpolator * frameZ) >> 16;

    S32 rotX = xDelta - obj->LastAnimStepX;
    S32 rotY = yDelta - obj->LastAnimStepY;
    S32 rotZ = zDelta - obj->LastAnimStepZ;

    obj->LastAnimStepX = xDelta;
    obj->LastAnimStepY = yDelta;
    obj->LastAnimStepZ = zDelta;
    obj->Interpolator = interpolator;

    RotatePoint(&MatrixLib2, rotX, rotY, rotZ);

    obj->X += X0;
    obj->Y += Y0;
    obj->Z += Z0;

#ifdef LBA2_TARGET_WIIU
    // Walk diagnostic: a sane per-tick root-motion delta is tens of units;
    // thousands = position teleport (model vanishes off-scene). Dump the
    // whole chain to see which ingredient blew up.
    {
        static int s_jump = 0;
        if (s_jump < 8 &&
            (X0 > 2000 || X0 < -2000 || Y0 > 2000 || Y0 < -2000 ||
             Z0 > 2000 || Z0 < -2000)) {
            LogPrintf("[InterDep JUMP] d=%ld,%ld,%ld rot=%ld,%ld,%ld frameXYZ=%ld,%ld,%ld last=%ld,%ld,%ld interp=%lu angles=%ld,%ld,%ld",
                      (long)X0, (long)Y0, (long)Z0,
                      (long)rotX, (long)rotY, (long)rotZ,
                      (long)frameX, (long)frameY, (long)frameZ,
                      (long)obj->LastAnimStepX, (long)obj->LastAnimStepY,
                      (long)obj->LastAnimStepZ,
                      (unsigned long)interpolator,
                      (long)newAlpha, (long)newBeta, (long)newGamma);
            s_jump++;
        }
    }
#endif

    if (obj->Status == FLAG_CHANGE) {
        return obj->Status;
    }

    obj->Interpolator = 0;

    obj->LastAnimStepX = 0;
    obj->LastAnimStepY = 0;
    obj->LastAnimStepZ = 0;

    obj->LastAnimStepAlpha = 0;
    obj->LastAnimStepBeta = 0;
    obj->LastAnimStepGamma = 0;

    PTR_U32 prevNextOfsFrame = obj->NextOfsFrame;
    obj->LastFrame = obj->NextFrame;
    obj->LastOfsFrame = prevNextOfsFrame;
    obj->LastOfsIsPtr = 0;

    S32 nextFrame = obj->NextFrame + 1;

    S32 nextNbGroups = obj->NextNbGroups;
    obj->LastNbGroups = nextNbGroups;
    obj->NbGroups = nextNbGroups;

    S32 size = 2 * nextNbGroups - 2;

    PTR_U32 nextOfsFrame = NULL; // eax@14

    if (nextFrame >= obj->NbFrames) {
        nextFrame = obj->LoopFrame;
        nextOfsFrame = (PTR_U32)(uintptr_t)obj->LoopOfsFrame;
        obj->Status = FLAG_FRAME + FLAG_LAST_FRAME + FLAG_CHANGE;
    } else {
        nextOfsFrame = (U32 *)((U8 *)prevNextOfsFrame + 4 * size + 16);
    }

    obj->NextOfsFrame = nextOfsFrame;
    S16 *nextFrameData = (S16 *)((U8 *)anim + (U64)nextOfsFrame);
    obj->Master = ANIM_RD_S16(nextFrameData[4]);
    obj->NextFrame = nextFrame;
    obj->LastTimer = obj->Time;
    obj->NextTimer = obj->Time + ANIM_RD_S16(nextFrameData[0]);
#ifdef LBA2_TARGET_WIIU
    {
        static int s_log = 0;
        if (s_log < 6) {
            S16 *cf = (S16 *)obj->CurrentFrame;
            LogPrintf("[InterDep frame-advance #%d] obj=%p frame=%d nbGroups=%d size=%d Master=%d frameXYZ=%d,%d,%d cf[0..3]=%d,%d,%d,%d cf[4..7]=%d,%d,%d,%d obj->XYZ=%d,%d,%d\n",
                      s_log, obj, (int)nextFrame, (int)nextNbGroups, (int)size,
                      (int)obj->Master,
                      (int)frameX, (int)frameY, (int)frameZ,
                      (int)cf[0], (int)cf[1], (int)cf[2], (int)cf[3],
                      (int)cf[4], (int)cf[5], (int)cf[6], (int)cf[7],
                      (int)obj->X, (int)obj->Y, (int)obj->Z);
            s_log++;
        }
    }
#endif
#ifdef LBA2_TARGET_WIIU
    // Copy LE on-disk group frame data into obj->CurrentFrame with per-S16
    // swap so the ObjectDisplay inner loop reads sane Type/Alpha/Beta/Gamma.
    // Without this, every frame advance re-corrupts CurrentFrame and bodies
    // collapse to the stretched/flying look right after the first render.
    {
        S16 *src = (S16 *)((U8 *)anim + (U64)prevNextOfsFrame + 16);
        S16 *dst = (S16 *)obj->CurrentFrame;
        S32 nS16 = size * 2; // size is U32 count, ×2 = S16 count
        for (S32 i = 0; i < nS16; ++i) dst[i] = ANIM_RD_S16(src[i]);
    }
#else
    memcpy(obj->CurrentFrame,
           ((U8 *)anim + (U64)prevNextOfsFrame + 16),
           4 * size);
#endif

    return obj->Status;
}