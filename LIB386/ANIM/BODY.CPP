#include <ANIM/BODY.H>
#include <ANIM.H>
#include <SYSTEM/DEBUG_TRACES.H>

void ObjectInitBody(T_OBJ_3D *obj, void *body) {
    LIB386_TRACE_CPP("ObjectInitBody", "1", "obj=%p body=%p", (void *)obj, body);
    obj->Status |= FLAG_BODY;

    // NOTE: `body` here is either a real pointer (CREDITS path passes
    // PtrCredits+offset, HOLO passes NULL) or a body INDEX cast to void*
    // (gameplay path passes (void *)(uintptr_t)numbody). Do NOT dereference
    // it — resolution from index to pointer happens later in ObjectDisplay
    // via TransFctBody (GivePtrBody → HQR_Get). The endian swap for the
    // body header is done in GivePtrBody, which is the only spot where we
    // know the pointer is real.
    obj->NextBody.Ptr = body;
    if (obj->Body.Num == -1) {
        obj->Body.Ptr = body;
    }
}
