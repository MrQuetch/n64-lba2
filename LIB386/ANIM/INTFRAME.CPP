#include <ANIM/INTFRAME.H>
#include <ANIM.H>
#include <stddef.h>
#include <SYSTEM/DEBUG_TRACES.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
// nextPtr always points into the on-disk anim buffer (LE).
// lastPtr points into either the on-disk anim buffer (LastOfsIsPtr==0, LE)
// or PtrLib3DBufferAnim (LastOfsIsPtr==1) which holds host-native values
// (the code that fills bufferAnim copies from obj->CurrentFrame, which is
// already in host order on Wii U thanks to the ObjectSetFrame/InitAnim
// swap-on-copy). So lastPtr needs LE16 only when LastOfsIsPtr==0.
#  define ANIM_LE16(x) ((U16)LBA2_LE16((U16)(x)))
#else
#  define ANIM_LE16(x) (x)
#endif

void ObjectSetInterFrame(T_OBJ_3D *obj) {
    LIB386_TRACE_CPP("ObjectSetInterFrame", "1", "obj=%p", (void *)obj);
    if (obj->Status & FLAG_FRAME) {
        return;
    }

    if (!(obj->Status & FLAG_CHANGE)) {
        return;
    }

    obj->Status = 0;

    void *anim = obj->Anim.Ptr;
    if (TransFctAnim != NULL) {
        anim = TransFctAnim(obj->Anim.Num);
    }

    U16 *nextPtr = (U16 *)((U8 *)anim + (U64)obj->NextOfsFrame + 16);
    U16 *lastPtr = (U16 *)((U8 *)obj->LastOfsFrame + 16);

    bool lastIsLE = false;
    if (obj->LastOfsIsPtr != 1) {
        lastPtr = (U16 *)((U8 *)lastPtr + (U64)anim);
        lastIsLE = true;  // lastPtr now points into LE anim buffer
    }

    // only one group? exit
    if (obj->NbGroups == 1) {
        return;
    }

    T_GROUP_INFO *group = obj->CurrentFrame;

    for (U32 i = 0; i < obj->NbGroups - 1; i++) {
        U16 type = ANIM_LE16(nextPtr[0]);
        group->Type = type;

        // Rotations
        if (type == TYPE_ROTATE) {
            S16 next = (S16)ANIM_LE16(nextPtr[1]);
            S16 last = lastIsLE ? (S16)ANIM_LE16(lastPtr[1]) : (S16)lastPtr[1];
            S32 diffAlpha = (next - last) & 0xFFF; // & 4095
            if (diffAlpha != 0) {
                diffAlpha = (diffAlpha << 20) >> 20;
                diffAlpha = (diffAlpha * obj->Interpolator) >> 16;
                last = (last + diffAlpha) & 0xFFF;
            }

            next = (S16)ANIM_LE16(nextPtr[2]);
            group->Alpha = last;
            last = lastIsLE ? (S16)ANIM_LE16(lastPtr[2]) : (S16)lastPtr[2];

            S32 diffBeta = (next - last) & 0xFFF;
            if (diffBeta != 0) {
                diffBeta = (diffBeta << 20) >> 20;
                diffBeta = (diffBeta * obj->Interpolator) >> 16;
                last = (last + diffBeta) & 0xFFF;
            }

            next = (S16)ANIM_LE16(nextPtr[3]);
            group->Beta = last;
            last = lastIsLE ? (S16)ANIM_LE16(lastPtr[3]) : (S16)lastPtr[3];

            S32 diffGamma = (next - last) & 0xFFF;
            if (diffGamma != 0) {
                diffGamma = (diffGamma << 20) >> 20;
                diffGamma = (diffGamma * obj->Interpolator) >> 16;
                last = (last + diffGamma) & 0xFFF;
            }

            group->Gamma = last;
        } else // Translations
        {
            S16 next = (S16)ANIM_LE16(nextPtr[1]);
            S16 last = lastIsLE ? (S16)ANIM_LE16(lastPtr[1]) : (S16)lastPtr[1];
            S32 diffX = (next - last);
            if (diffX != 0) {
                diffX = (diffX * obj->Interpolator) >> 16;
                last += diffX;
            }

            next = (S16)ANIM_LE16(nextPtr[2]);
            group->Alpha = last; // X
            last = lastIsLE ? (S16)ANIM_LE16(lastPtr[2]) : (S16)lastPtr[2];

            S32 diffY = (next - last);
            if (diffY != 0) {
                diffY = (diffY * obj->Interpolator) >> 16;
                last += diffY;
            }

            next = (S16)ANIM_LE16(nextPtr[3]);
            group->Beta = last; // Y
            last = lastIsLE ? (S16)ANIM_LE16(lastPtr[3]) : (S16)lastPtr[3];

            S32 diffZ = (next - last);
            if (diffZ != 0) {
                diffZ = (diffZ * obj->Interpolator) >> 16;
                last += diffZ;
            }

            group->Gamma = last; // Z
        }

        nextPtr += 4;
        lastPtr += 4;
        group++;
    }
}
