#include <SVGA/CLIP.H>
#include <SVGA/VIDEO.H>
#include <SVGA/SCREEN.H>
#include <stdio.h>

S32 ClipXMin = 0; // ClipXMin		dd	0
S32 ClipYMin = 0; // ClipYMin		dd	0
S32 ClipXMax = 0; // ClipXMax		dd	0
S32 ClipYMax = 0; // ClipYMax		dd	0

S32 ClipWindowXMin = 0; // ClipWindowXMin		dd	0
S32 ClipWindowYMin = 0; // ClipWindowYMin		dd	0
S32 ClipWindowXMax = 0; // ClipWindowXMax		dd	0
S32 ClipWindowYMax = 0; // ClipWindowYMax		dd	0

S32 MemoClipXMin = 0; // MemoClipXMin		dd	0
S32 MemoClipYMin = 0; // MemoClipYMin		dd	0
S32 MemoClipXMax = 0; // MemoClipXMax		dd	0
S32 MemoClipYMax = 0; // MemoClipYMax		dd	0

S32 MemoClipWindowXMin = 0; // MemoClipWindowXMin	dd	0
S32 MemoClipWindowYMin = 0; // MemoClipWindowYMin	dd	0
S32 MemoClipWindowXMax = 0; // MemoClipWindowXMax	dd	0
S32 MemoClipWindowYMax = 0; // MemoClipWindowYMax	dd	0

void SetClipWindow(S32 x0, S32 y0, S32 x1, S32 y1) {
    if (x0 >= 0)
        ClipWindowXMin = x0;
    else
        ClipWindowXMin = 0;

    if (y0 >= 0)
        ClipWindowYMin = y0;
    else
        ClipWindowYMin = 0;

    if (x1 < ModeDesiredX)
        ClipWindowXMax = x1;
    else
        ClipWindowXMax = ModeDesiredX - 1;

    if (y1 < ModeDesiredY)
        ClipWindowYMax = y1;
    else
        ClipWindowYMax = ModeDesiredY - 1;

    SetClip(ClipXMin, ClipYMin, ClipXMax, ClipYMax);
}

void UnsetClipWindow() {
    ClipWindowXMax = ModeDesiredX - 1;
    ClipWindowYMax = ModeDesiredY - 1;
    ClipWindowXMin = 0;
    ClipWindowYMin = 0;
    ClipXMax = ModeDesiredX - 1;
    ClipYMax = ModeDesiredY - 1;
    ClipXMin = 0;
    ClipYMin = 0;
}

void MemoClipWindow() {
    MemoClipWindowXMin = ClipWindowXMin;
    MemoClipWindowYMin = ClipWindowYMin;
    MemoClipWindowXMax = ClipWindowXMax;
    MemoClipWindowYMax = ClipWindowYMax;
}

void RestoreClipWindow() {
    SetClipWindow(MemoClipWindowXMin, MemoClipWindowYMin, MemoClipWindowXMax, MemoClipWindowYMax);
}

void SetClip(S32 x0, S32 y0, S32 x1, S32 y1) {
    if (x0 >= ClipWindowXMin)
        ClipXMin = x0;
    else
        ClipXMin = ClipWindowXMin;

    if (y0 >= ClipWindowYMin)
        ClipYMin = y0;
    else
        ClipYMin = ClipWindowYMin;

    if (x1 <= ClipWindowXMax)
        ClipXMax = x1;
    else
        ClipXMax = ClipWindowXMax;

    if (y1 <= ClipWindowYMax)
        ClipYMax = y1;
    else
        ClipYMax = ClipWindowYMax;
}

void UnsetClip() {
    ClipXMin = ClipWindowXMin;
    ClipYMin = ClipWindowYMin;
    ClipXMax = ClipWindowXMax;
    ClipYMax = ClipWindowYMax;
}

void MemoClip() {
    MemoClipXMin = ClipXMin;
    MemoClipYMin = ClipYMin;
    MemoClipXMax = ClipXMax;
    MemoClipYMax = ClipYMax;
}

void RestoreClip() {
    SetClip(MemoClipXMin, MemoClipYMin, MemoClipXMax, MemoClipYMax);
}

// [renderscale] see SCREEN.H
void RS_PushPhysClip(S32 save[4]) {
    save[0] = ClipXMin;
    save[1] = ClipYMin;
    save[2] = ClipXMax;
    save[3] = ClipYMax;
#if RS_SHIFT
    ClipXMin = RS_V2P(ClipXMin);
    ClipYMin = RS_V2P(ClipYMin);
    ClipXMax = RS_V2P(ClipXMax);
    ClipYMax = RS_V2P(ClipYMax);
#endif
}

void RS_PopClip(const S32 save[4]) {
    ClipXMin = save[0];
    ClipYMin = save[1];
    ClipXMax = save[2];
    ClipYMax = save[3];
}
