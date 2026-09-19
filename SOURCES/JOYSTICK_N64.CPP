// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Nintendo 64 (libdragon joypad) implementation of the JOYSTICK.CPP public
// surface, modeled on JOYSTICK_WIIU_STUB.CPP: UpdateJoystick() writes
// bit-packed virtual scancodes (K_GAMEPAD_*, KEYBOARD_KEYS.H) into
// TabKeys[256..] so CheckKey() and the engine bindings consume gamepad
// state with no LIB386 changes.
//
// N64 pad mapping (V2, controls phase). The engine reads gameplay AND the
// spell/protopack shortcuts through the PC keyboard scancodes bound in
// DefKeysDefault95 (SOURCES/INPUT.CPP) — the spells (Foudre/Protection/
// Protopack) are polled directly with CheckKey(DefKeys[32..35]) in PERSO.CPP
// and never touch the K_GAMEPAD_* combined table. So instead of the WiiU
// K_GAMEPAD_* -> GamepadKeys indirection (which cannot reach those slots),
// every N64 control injects the matching PC scancode straight into the
// keyboard range of TabKeys, exactly like the WiiU touch panel did. This is
// the single mechanism that covers movement, actions and spells uniformly;
// GamepadKeysDefault is neutralised on N64 so the K_GAMEPAD_* bits below only
// serve menu navigation / the on-screen keyboard / "press any key" skips.
//
//   Physical      Action                         PC key injected
//   D-pad         Move (turn / walk)             arrows
//   A             Behaviour action               SPACE
//   B             Directional dodge (hold)       X
//   L             Behaviour menu / toggle (hold) LCTRL
//   R             Throw weapon / magic ball      LALT
//   Z             Inventory                      LSHIFT
//   C-up          Confirm item + recenter cam    RETURN
//   C-down        Behaviour-agnostic interaction W
//   C-right       Lightning spell (Foudre)       F
//   C-left        Protection spell               C
//   Start         In-game menu                   F10
//   Stick-up      Protopack / Jetpack            J
//   Stick-left    Behaviour: Normal              F5
//   Stick-right   Behaviour: Athletic            F6
//   Stick-down    Behaviour: Aggressive          F7
//
// The stick is decoupled from movement (D-pad only) so its four cardinals are
// free for the behaviour/protopack shortcuts. Injected keys are held for as
// long as the control is; N64Joy_ReinjectKeys() re-applies them after
// ManageKeyboard()'s per-call clear of the keyboard range (N64_BACKEND.CPP).
//
// The WiiUTouch_* / WiiUTouchPanel_* symbols referenced by WIIU-gated engine
// code are provided here as no-op shims (no touch screen on N64).

#include "JOYSTICK.H"
#include "TOUCHPANEL_WIIU.H"

#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOGPRINT.H>

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kept for the JOYSTICK.H contract / extern references. NOT used as the stick
// threshold on N64: ReadGamepadConfig() overwrites this with the cfg's
// GamepadDeadzone (default 8000), a value in SDL axis units (0..32767) that is
// meaningless for the N64 stick's raw ±85 range and would leave it dead. The
// stick uses N64_STICK_DEADZONE (raw units) below instead.
S32 JoystickDeadzone = 32;

// Digital-emulation threshold in the N64 stick's own raw units (roughly ±85 at
// full deflection). ~28% of full travel: past the loose-centre slop, short of
// requiring a hard push for the behaviour/protopack shortcuts.
#define N64_STICK_DEADZONE 24

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

// Synthesized hold mask: joypad_buttons_t bits + stick-emulation dirs, so the
// map/edge-detect structure stays identical to the Wii U backend.
#define HB_A            (1u << 0)
#define HB_B            (1u << 1)
#define HB_START        (1u << 2)
#define HB_Z            (1u << 3)
#define HB_L            (1u << 4)
#define HB_R            (1u << 5)
#define HB_C_UP         (1u << 6)
#define HB_C_DOWN       (1u << 7)
#define HB_C_LEFT       (1u << 8)
#define HB_C_RIGHT      (1u << 9)
#define HB_D_UP         (1u << 10)
#define HB_D_DOWN       (1u << 11)
#define HB_D_LEFT       (1u << 12)
#define HB_D_RIGHT      (1u << 13)
#define HB_STICK_UP     (1u << 14)
#define HB_STICK_DOWN   (1u << 15)
#define HB_STICK_LEFT   (1u << 16)
#define HB_STICK_RIGHT  (1u << 17)

typedef struct {
    U32 holdBit;
    U32 key;
} PadMap;
static const PadMap kMap[] = {
    {HB_A, K_GAMEPAD_A},
    {HB_B, K_GAMEPAD_B},
    {HB_START, K_GAMEPAD_START},
    {HB_Z, K_GAMEPAD_LTRIGGER},
    {HB_L, K_GAMEPAD_LSHOULDER},
    {HB_R, K_GAMEPAD_RSHOULDER},
    {HB_C_LEFT, K_GAMEPAD_X},
    {HB_C_UP, K_GAMEPAD_Y},
    {HB_C_RIGHT, K_GAMEPAD_BACK},
    {HB_C_DOWN, K_GAMEPAD_RSTICK_DOWN},
    {HB_D_UP, K_GAMEPAD_DPAD_UP},
    {HB_D_DOWN, K_GAMEPAD_DPAD_DOWN},
    {HB_D_LEFT, K_GAMEPAD_DPAD_LEFT},
    {HB_D_RIGHT, K_GAMEPAD_DPAD_RIGHT},
    // Stick / D-pad also light the LSTICK dirs purely so JoystickMenuAction /
    // JoystickMenuNavOnly (which read these bits) keep working for menu
    // navigation. They no longer drive gameplay movement: GamepadKeysDefault
    // is neutralised on N64 (SOURCES/INPUT.CPP) and movement comes from the
    // injected arrow keys in kKbMap. Both stick and D-pad navigate menus.
    {HB_STICK_UP, K_GAMEPAD_LSTICK_UP},
    {HB_STICK_DOWN, K_GAMEPAD_LSTICK_DOWN},
    {HB_STICK_LEFT, K_GAMEPAD_LSTICK_LEFT},
    {HB_STICK_RIGHT, K_GAMEPAD_LSTICK_RIGHT},
    {HB_D_UP, K_GAMEPAD_LSTICK_UP},
    {HB_D_DOWN, K_GAMEPAD_LSTICK_DOWN},
    {HB_D_LEFT, K_GAMEPAD_LSTICK_LEFT},
    {HB_D_RIGHT, K_GAMEPAD_LSTICK_RIGHT},
};

// Gameplay layer: each physical control injects the PC keyboard scancode that
// DefKeysDefault95 binds to the desired action (the spells at DefKeys[32..35]
// are only reachable this way — see the file header). Movement is on the
// D-pad; the stick's four cardinals are the behaviour / protopack shortcuts.
typedef struct {
    U32 holdBit;
    U32 scancode; // PC keyboard scancode (< 256), injected into TabKeys
} PadKbMap;
static const PadKbMap kKbMap[] = {
    {HB_D_UP, K_GRAY_UP},          // I_UP    (avancer)
    {HB_D_DOWN, K_GRAY_DOWN},      // I_DOWN  (reculer)
    {HB_D_LEFT, K_GRAY_LEFT},      // I_LEFT  (tourner)
    {HB_D_RIGHT, K_GRAY_RIGHT},    // I_RIGHT (tourner)
    {HB_A, K_SPACE},               // I_ACTION_M      (behaviour action)
    {HB_B, K_X},                   // I_ESQUIVE       (dodge)
    {HB_L, K_CTRL_LEFT},           // I_COMPORTEMENT  (behaviour menu/toggle)
    {HB_R, K_ALT_LEFT},            // I_THROW         (weapon / magic ball)
    {HB_Z, K_SHIFT_LEFT},          // I_INVENTORY
    {HB_C_UP, K_ENTER},            // I_RETURN        (confirm + recenter cam)
    {HB_C_DOWN, K_W},              // I_ACTION_ALWAYS (dialogue / search)
    {HB_C_RIGHT, K_F},             // I_FOUDRE   (DefKeys[35])
    {HB_C_LEFT, K_C},              // I_PROTECTION (DefKeys[34])
    {HB_START, K_F10},             // I_MENUS         (in-game menu)
    {HB_STICK_UP, K_J},            // I_JETPACK  (DefKeys[33]: protopack/jetpack)
    {HB_STICK_LEFT, K_F5},         // I_NORMAL
    {HB_STICK_RIGHT, K_F6},        // I_SPORTIF (athletic)
    {HB_STICK_DOWN, K_F7},         // I_AGRESSIF
};

// Current hold snapshot (set by UpdateJoystick), consumed by
// N64Joy_ReinjectKeys() to rebuild the injected keyboard keys every time
// ManageKeyboard() clears the keyboard range.
static U32 s_curHold = 0;

// Reads the pad and synthesizes the hold mask. Returns 1 if a pad is present.
static int ReadPad(U32 *hold) {
    joypad_poll();

    if (!joypad_is_connected(JOYPAD_PORT_1)) {
        *hold = 0;
        return 0;
    }

    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    U32 h = 0;
    if (in.btn.a) h |= HB_A;
    if (in.btn.b) h |= HB_B;
    if (in.btn.start) h |= HB_START;
    if (in.btn.z) h |= HB_Z;
    if (in.btn.l) h |= HB_L;
    if (in.btn.r) h |= HB_R;
    if (in.btn.c_up) h |= HB_C_UP;
    if (in.btn.c_down) h |= HB_C_DOWN;
    if (in.btn.c_left) h |= HB_C_LEFT;
    if (in.btn.c_right) h |= HB_C_RIGHT;
    if (in.btn.d_up) h |= HB_D_UP;
    if (in.btn.d_down) h |= HB_D_DOWN;
    if (in.btn.d_left) h |= HB_D_LEFT;
    if (in.btn.d_right) h |= HB_D_RIGHT;

    // Stick digital emulation. Raw N64 stick range is roughly ±85; the
    // threshold is N64_STICK_DEADZONE in those raw units (NOT the global
    // JoystickDeadzone, which the cfg pins to an SDL-unit value that would
    // leave the stick dead — see the note by its definition).
    if (in.stick_y > N64_STICK_DEADZONE) h |= HB_STICK_UP;
    if (in.stick_y < -N64_STICK_DEADZONE) h |= HB_STICK_DOWN;
    if (in.stick_x < -N64_STICK_DEADZONE) h |= HB_STICK_LEFT;
    if (in.stick_x > N64_STICK_DEADZONE) h |= HB_STICK_RIGHT;

    *hold = h;
    return 1;
}

// ── Touch panel shims (no touch screen on N64) ──────────────────────────────
U32 WiiUTouch_ActiveKey(void) {
    return 0;
}

void WiiUTouch_InjectKeys(void) {}

U32 WiiUTouchPanel_HitTest(S32 x, S32 y) {
    (void)x;
    (void)y;
    return 0;
}

S32 WiiUTouchPanel_Render(U32 *buf0, U32 *buf1, U32 pitch, U32 height, U32 pressedKey) {
    (void)buf0; (void)buf1; (void)pitch; (void)height; (void)pressedKey;
    return 0;
}

void WiiUTouchPanel_Invalidate(void) {}

// ── Gameplay keyboard injection ─────────────────────────────────────────────
// Re-applies the currently-held pad controls as PC keyboard scancodes into the
// keyboard range of TabKeys. ManageKeyboard() (N64_BACKEND.CPP) memsets
// TabKeys[0..255] on every call — possibly several times per frame — so it
// invokes this right after, keeping the injected keys visible to CheckKey()
// for both the GetInput() combined table and the direct spell polls in
// PERSO.CPP. Must stay idempotent (no edge state here; edges live in s_lastHold
// and the engine's WaitNoKey latches).
void N64Joy_ReinjectKeys(void) {
    if (!s_padPresent || !s_curHold) {
        return;
    }
    for (size_t i = 0; i < sizeof(kKbMap) / sizeof(kKbMap[0]); i++) {
        if (s_curHold & kKbMap[i].holdBit) {
            U32 sc = kKbMap[i].scancode;
            if (sc < 256) {
                TabKeys[sc] = 0x80; // N64 CheckKey tests != 0; 0x80 matches KeyBit
            }
        }
    }
}

// ── JOYSTICK.H contract ─────────────────────────────────────────────────────
void InitJoystick(void) {
    // joypad_init() runs in main() before the engine starts.
    LogPuts("N64 JOYSTICK: libdragon joypad wired (port 1).");
}

void EndJoystick(void) {}
void DetectJoys(void) {}
void HandleEventsJoystick(const void *event) { (void)event; }

void UpdateJoystick(void) {
    ClearGamepadBits();
    s_firstPressedThisFrame = 0;

    U32 hold = 0;
    if (!ReadPad(&hold)) {
        s_padPresent = 0;
        s_lastHold = 0;
        s_curHold = 0; // pad gone: drop the injected keys on the next clear
        return;
    }
    s_padPresent = 1;
    s_curHold = hold; // snapshot for N64Joy_ReinjectKeys()

    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (hold & kMap[i].holdBit) {
            SetVirtualKeyDown(kMap[i].key);
        }
    }

    // Edge detection for "press any key" wait loops.
    U32 newBits = hold & ~s_lastHold;
    s_lastHold = hold;
    if (newBits) {
        for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
            if (newBits & kMap[i].holdBit) {
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

// Legacy entry point: call-site passes &TabKeys[256]; parameter ignored.
void GetJoys(U32 *bitfield) {
    (void)bitfield;
    UpdateJoystick();
}

S32 JoystickMenuAction(void) {
    U32 hold = 0;
    if (!ReadPad(&hold)) {
        return 0;
    }
    if (hold & (HB_A | HB_START)) return K_ENTER;
    if (hold & (HB_D_UP | HB_STICK_UP)) return K_UP;
    if (hold & (HB_D_DOWN | HB_STICK_DOWN)) return K_DOWN;
    if (hold & (HB_D_LEFT | HB_STICK_LEFT)) return K_LEFT;
    if (hold & (HB_D_RIGHT | HB_STICK_RIGHT)) return K_RIGHT;
    return 0;
}

S32 JoystickMenuNavOnly(void) {
    U32 hold = 0;
    if (!ReadPad(&hold)) {
        return 0;
    }
    if (hold & (HB_D_UP | HB_STICK_UP)) return K_UP;
    if (hold & (HB_D_DOWN | HB_STICK_DOWN)) return K_DOWN;
    if (hold & (HB_D_LEFT | HB_STICK_LEFT)) return K_LEFT;
    if (hold & (HB_D_RIGHT | HB_STICK_RIGHT)) return K_RIGHT;
    return 0;
}

#ifdef __cplusplus
}
#endif
