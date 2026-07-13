// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Wii U VPAD implementation of the JOYSTICK.CPP public surface. Mirrors the
// SDL3 backend's contract: UpdateJoystick() writes bit-packed virtual
// scancodes (K_GAMEPAD_*, KEYBOARD_KEYS.H) into TabKeys[256..] so CheckKey()
// and the engine's input bindings consume gamepad state with no LIB386
// changes. Sticks use the VPAD digital-emulation bits (system deadzone).
//
// Button mapping is SEMANTIC, not positional: Wii U A (east) = primary
// action = K_GAMEPAD_A (SDL "south"), B = cancel = K_GAMEPAD_B.

#include "JOYSTICK.H"
#include "TOUCHPANEL_WIIU.H"

#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOGPRINT.H>

#include <vpad/input.h>

#ifdef __cplusplus
extern "C" {
#endif

S32 JoystickDeadzone = 32;

// Mirror of the SDL backend's bit-packing helpers (KEYBOARD.CPP layout).
#define PAD_KEY_INDEX(x) (256 - 32 + ((x) >> 3))
#define PAD_KEY_BIT(x) (1 << ((x) & 7))

static inline void SetVirtualKeyDown(U32 scancode) {
    TabKeys[PAD_KEY_INDEX(scancode)] |= (U8)PAD_KEY_BIT(scancode);
}

static inline void ClearGamepadBits(void) {
    U32 first = PAD_KEY_INDEX(K_GAMEPAD_BASE);
    U32 last = PAD_KEY_INDEX(K_GAMEPAD_BASE + K_GAMEPAD_COUNT - 1);
    for (U32 i = first; i <= last; i++) {
        TabKeys[i] = 0;
    }
}

static U32 s_firstPressedThisFrame = 0;
static int s_padPresent = 0;
static U32 s_lastHold = 0;

// VPAD hold-bit → engine virtual scancode. Order defines the priority for
// the "first pressed this frame" scan (buttons before dirs, like SDL side).
typedef struct {
    U32 vpadBit;
    U32 key;
} VPadMap;
static const VPadMap kMap[] = {
    {VPAD_BUTTON_A, K_GAMEPAD_A},
    {VPAD_BUTTON_B, K_GAMEPAD_B},
    {VPAD_BUTTON_Y, K_GAMEPAD_X},    // Wii U Y (west) = SDL WEST
    {VPAD_BUTTON_X, K_GAMEPAD_Y},    // Wii U X (north) = SDL NORTH
    {VPAD_BUTTON_MINUS, K_GAMEPAD_BACK},
    {VPAD_BUTTON_PLUS, K_GAMEPAD_START},
    {VPAD_BUTTON_STICK_L, K_GAMEPAD_LSTICK_BTN},
    {VPAD_BUTTON_STICK_R, K_GAMEPAD_RSTICK_BTN},
    {VPAD_BUTTON_L, K_GAMEPAD_LSHOULDER},
    {VPAD_BUTTON_R, K_GAMEPAD_RSHOULDER},
    {VPAD_BUTTON_ZL, K_GAMEPAD_LTRIGGER},
    {VPAD_BUTTON_ZR, K_GAMEPAD_RTRIGGER},
    {VPAD_BUTTON_UP, K_GAMEPAD_DPAD_UP},
    {VPAD_BUTTON_DOWN, K_GAMEPAD_DPAD_DOWN},
    {VPAD_BUTTON_LEFT, K_GAMEPAD_DPAD_LEFT},
    {VPAD_BUTTON_RIGHT, K_GAMEPAD_DPAD_RIGHT},
    // Left stick (digital emulation bits) — movement.
    {VPAD_STICK_L_EMULATION_UP, K_GAMEPAD_LSTICK_UP},
    {VPAD_STICK_L_EMULATION_DOWN, K_GAMEPAD_LSTICK_DOWN},
    {VPAD_STICK_L_EMULATION_LEFT, K_GAMEPAD_LSTICK_LEFT},
    {VPAD_STICK_L_EMULATION_RIGHT, K_GAMEPAD_LSTICK_RIGHT},
    // D-pad also lights the LSTICK dirs (stick/D-pad interchangeable for
    // movement — same convention as the SDL backend).
    {VPAD_BUTTON_UP, K_GAMEPAD_LSTICK_UP},
    {VPAD_BUTTON_DOWN, K_GAMEPAD_LSTICK_DOWN},
    {VPAD_BUTTON_LEFT, K_GAMEPAD_LSTICK_LEFT},
    {VPAD_BUTTON_RIGHT, K_GAMEPAD_LSTICK_RIGHT},
    {VPAD_STICK_R_EMULATION_UP, K_GAMEPAD_RSTICK_UP},
    {VPAD_STICK_R_EMULATION_DOWN, K_GAMEPAD_RSTICK_DOWN},
    {VPAD_STICK_R_EMULATION_LEFT, K_GAMEPAD_RSTICK_LEFT},
    {VPAD_STICK_R_EMULATION_RIGHT, K_GAMEPAD_RSTICK_RIGHT},
};

static U32 s_holdCache = 0;
static VPADTouchData s_tpCache = {0, 0, 0, 0};

static int ReadPad(VPADStatus *status) {
    VPADReadError err = VPAD_READ_SUCCESS;
    int n = VPADRead(VPAD_CHAN_0, status, 1, &err);
    if (n > 0 && err == VPAD_READ_SUCCESS) {
        s_holdCache = status->hold;
        s_tpCache = status->tpNormal;
        return 1;
    }
    // VPAD samples at ~200 Hz: polling twice in one frame (InitWaitNoInput
    // right after the frame's ManageKeyboard) or at unlucky timing yields
    // VPAD_READ_NO_SAMPLES. Treating that as "all buttons released" broke
    // the engine's norepeat latch (inventory button re-firing on close) and
    // blanked movement for one poll (camera recenter halting the run).
    // "No new sample" means "state unchanged", so replay the last good hold
    // (and touch sample — *status is stack garbage on this path otherwise).
    if (err == VPAD_READ_NO_SAMPLES && s_padPresent) {
        status->hold = s_holdCache;
        status->tpNormal = s_tpCache;
        return 1;
    }
    s_holdCache = 0;
    s_tpCache.touched = 0;
    return 0;
}

// ── DRC touchscreen → keyboard scancode injection ───────────────────────────
// Quick-action panel plumbing: UpdateJoystick calibrates the touch sample
// carried by the frame's VPADStatus into DRC screen space (854x480),
// hit-tests it against the panel layout (TOUCHPANEL_WIIU.CPP), and latches
// the matching keyboard scancode for as long as the finger stays down.
// ManageKeyboard() (WIIU_BACKEND.CPP) re-injects the latched scancode into
// TabKeys[] after its per-call memset, so the engine sees a plain held key
// through the standard CheckKey/GetInput path — same trick as the
// K_GAMEPAD_* bits, but in the <256 keyboard range the binding tables
// already point at.
static U32 s_touchKey = 0;     // scancode latched by the current touch, 0 = none
static int s_touchWasDown = 0;
static S32 s_touchX = -1;      // last calibrated position (DRC 854x480 space)
static S32 s_touchY = -1;

static void ProcessTouch(const VPADStatus *st) {
    // The panel only exists during gameplay; while a menu owns the DRC
    // (AsciiMode set by DoGameMenu/selector loops) touches must not fire
    // gameplay shortcuts.
    if (AsciiMode) {
        s_touchKey = 0;
        s_touchWasDown = 0;
        return;
    }

    VPADTouchData raw = st->tpNormal; // wut wants a non-const source
    VPADTouchData cal;
    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &cal, &raw);

    int down = (cal.touched != 0) && (cal.validity == VPAD_VALID);
    if (down) {
        s_touchX = (S32)cal.x;
        s_touchY = (S32)cal.y;
        if (!s_touchWasDown) {
            // Latch on the down edge only: dragging across buttons must
            // not morph one press into another action.
            s_touchKey = WiiUTouchPanel_HitTest(s_touchX, s_touchY);
            LogPrintf("TOUCH down (%ld,%ld) -> scancode %lu\n",
                      (long)s_touchX, (long)s_touchY, (unsigned long)s_touchKey);
        }
    } else if (s_touchWasDown) {
        s_touchKey = 0;
    }
    s_touchWasDown = down;
}

// Scancode currently held by touch — read by WIIU_BACKEND.CPP for the
// pressed-button visual feedback on the panel.
U32 WiiUTouch_ActiveKey(void) {
    return s_touchKey;
}

// Called by ManageKeyboard() after it clears the keyboard range of TabKeys
// (possibly several times per frame — must stay idempotent).
void WiiUTouch_InjectKeys(void) {
    if (s_touchKey != 0 && s_touchKey < 256) {
        TabKeys[s_touchKey] = 1;
    }
}

void InitJoystick(void) {
    VPADInit();
    LogPuts("Wii U JOYSTICK: VPAD wired (GamePad chan 0).");
}

void EndJoystick(void) {}
void DetectJoys(void) {}
void HandleEventsJoystick(const void *event) { (void)event; }

void UpdateJoystick(void) {
    ClearGamepadBits();
    s_firstPressedThisFrame = 0;

    VPADStatus st;
    if (!ReadPad(&st)) {
        s_padPresent = 0;
        s_lastHold = 0;
        s_touchKey = 0; // pad gone: release any latched touch action
        s_touchWasDown = 0;
        return;
    }
    s_padPresent = 1;

    ProcessTouch(&st);

    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (st.hold & kMap[i].vpadBit) {
            SetVirtualKeyDown(kMap[i].key);
        }
    }

    // Edge detection for "press any key" wait loops: first mapped bit that
    // is held now but was not held last frame. st.trigger covers buttons but
    // not the stick emulation bits consistently, so diff against our own
    // snapshot like the SDL backend does.
    U32 newBits = st.hold & ~s_lastHold;
    s_lastHold = st.hold;
    if (newBits) {
        for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
            if (newBits & kMap[i].vpadBit) {
                s_firstPressedThisFrame = kMap[i].key;
                break;
            }
        }
    }
}

U32 JoystickFirstPressedScancode(void) {
    return s_firstPressedThisFrame;
}

S32 JoystickIsPresent(void) {
    return s_padPresent;
}

// Legacy entry point: call-site passes &TabKeys[256]; the SDL backend also
// ignores the parameter and just refreshes state.
void GetJoys(U32 *bitfield) {
    (void)bitfield;
    UpdateJoystick();
}

S32 JoystickMenuAction(void) {
    VPADStatus st;
    if (!ReadPad(&st)) {
        return 0;
    }
    if (st.hold & (VPAD_BUTTON_A | VPAD_BUTTON_PLUS)) {
        return K_ENTER;
    }
    if (st.hold & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP)) return K_UP;
    if (st.hold & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN)) return K_DOWN;
    if (st.hold & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT)) return K_LEFT;
    if (st.hold & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)) return K_RIGHT;
    return 0;
}

S32 JoystickMenuNavOnly(void) {
    VPADStatus st;
    if (!ReadPad(&st)) {
        return 0;
    }
    if (st.hold & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP)) return K_UP;
    if (st.hold & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN)) return K_DOWN;
    if (st.hold & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT)) return K_LEFT;
    if (st.hold & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)) return K_RIGHT;
    return 0;
}

#ifdef __cplusplus
}
#endif
