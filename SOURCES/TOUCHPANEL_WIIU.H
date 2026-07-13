// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Wii U GamePad (DRC) quick-action touch panel. Renders a button grid on the
// pad screen during gameplay and maps touches to the engine's original
// keyboard shortcuts (behaviours F5-F8, weapons 1-7, holomap H, etc.).
//
// Owners:
//   - Layout + drawing + hit test: TOUCHPANEL_WIIU.CPP (this module)
//   - Touch sampling + scancode latch: JOYSTICK_WIIU_STUB.CPP
//   - Per-frame render hook + TabKeys injection: LIB386/SYSTEM/WIIU_BACKEND.CPP

#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

// Hit test a calibrated DRC touch point (854x480 space). Returns the keyboard
// scancode of the button under the point, or 0 for dead space.
U32 WiiUTouchPanel_HitTest(S32 x, S32 y);

// Draw the panel into both halves of the DRC double buffer (called every
// game frame; redraws only when dirty or when pressed/state changed).
// pressedKey is the scancode currently latched by a touch (0 = none).
// Returns 1 if the buffers were written (caller must cache-flush), 0 if
// the frame was skipped because nothing changed.
S32 WiiUTouchPanel_Render(U32 *buf0, U32 *buf1, U32 pitch, U32 height,
                          U32 pressedKey);

// Mark the panel dirty (call while something else — e.g. the menu mirror —
// owns the DRC, so the panel repaints fully when it takes over again).
void WiiUTouchPanel_Invalidate(void);

#ifdef __cplusplus
}
#endif
