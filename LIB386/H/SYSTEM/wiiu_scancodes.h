// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// SDL3 scancode constants required by the engine, inlined for the Wii U
// build (which does not link SDL3). Values follow the USB HID Usage Tables
// v1.12, Keyboard/Keypad Page (0x07) — the same numeric space SDL3 uses
// internally — so source code referencing SDL_SCANCODE_* keeps the same
// resolved value on Wii U as on desktop SDL3 platforms.
//
// Real Wii U keyboard input (USB HID via WPAD) is not wired in V1; the
// gamepad bit-packed scancodes from KEYBOARD_KEYS.H::K_GAMEPAD_* are what
// JOYSTICK_WIIU_STUB will populate in V2.

#pragma once

#ifdef LBA2_TARGET_WIIU

// ---- Letters ----------------------------------------------------------------
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_B 5
#define SDL_SCANCODE_C 6
#define SDL_SCANCODE_D 7
#define SDL_SCANCODE_E 8
#define SDL_SCANCODE_F 9
#define SDL_SCANCODE_G 10
#define SDL_SCANCODE_H 11
#define SDL_SCANCODE_I 12
#define SDL_SCANCODE_J 13
#define SDL_SCANCODE_K 14
#define SDL_SCANCODE_L 15
#define SDL_SCANCODE_M 16
#define SDL_SCANCODE_N 17
#define SDL_SCANCODE_O 18
#define SDL_SCANCODE_P 19
#define SDL_SCANCODE_Q 20
#define SDL_SCANCODE_R 21
#define SDL_SCANCODE_S 22
#define SDL_SCANCODE_T 23
#define SDL_SCANCODE_U 24
#define SDL_SCANCODE_V 25
#define SDL_SCANCODE_W 26
#define SDL_SCANCODE_X 27
#define SDL_SCANCODE_Y 28
#define SDL_SCANCODE_Z 29

// ---- Top-row digits ---------------------------------------------------------
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_3 32
#define SDL_SCANCODE_4 33
#define SDL_SCANCODE_5 34
#define SDL_SCANCODE_6 35
#define SDL_SCANCODE_7 36
#define SDL_SCANCODE_8 37
#define SDL_SCANCODE_9 38
#define SDL_SCANCODE_0 39

// ---- Editing keys -----------------------------------------------------------
#define SDL_SCANCODE_RETURN     40
#define SDL_SCANCODE_ESCAPE     41
#define SDL_SCANCODE_BACKSPACE  42
#define SDL_SCANCODE_TAB        43
#define SDL_SCANCODE_SPACE      44

// ---- Punctuation / OEM ------------------------------------------------------
#define SDL_SCANCODE_MINUS        45
#define SDL_SCANCODE_PLUS         46  // = / + key (HID "Equal")
#define SDL_SCANCODE_LEFTBRACKET  47
#define SDL_SCANCODE_RIGHTBRACKET 48
#define SDL_SCANCODE_BACKSLASH    49
#define SDL_SCANCODE_SEMICOLON    51
#define SDL_SCANCODE_APOSTROPHE   52
#define SDL_SCANCODE_GRAVE        53
#define SDL_SCANCODE_COMMA        54
#define SDL_SCANCODE_PERIOD       55
#define SDL_SCANCODE_SLASH        56

// ---- Locks / function keys --------------------------------------------------
#define SDL_SCANCODE_CAPSLOCK 57
#define SDL_SCANCODE_F1  58
#define SDL_SCANCODE_F2  59
#define SDL_SCANCODE_F3  60
#define SDL_SCANCODE_F4  61
#define SDL_SCANCODE_F5  62
#define SDL_SCANCODE_F6  63
#define SDL_SCANCODE_F7  64
#define SDL_SCANCODE_F8  65
#define SDL_SCANCODE_F9  66
#define SDL_SCANCODE_F10 67
#define SDL_SCANCODE_F11 68
#define SDL_SCANCODE_F12 69
#define SDL_SCANCODE_F13 104
#define SDL_SCANCODE_F14 105
#define SDL_SCANCODE_F15 106
#define SDL_SCANCODE_F16 107
#define SDL_SCANCODE_F17 108
#define SDL_SCANCODE_F18 109
#define SDL_SCANCODE_F19 110
#define SDL_SCANCODE_F20 111
#define SDL_SCANCODE_F21 112
#define SDL_SCANCODE_F22 113
#define SDL_SCANCODE_F23 114
#define SDL_SCANCODE_F24 115

// ---- Navigation / system ----------------------------------------------------
#define SDL_SCANCODE_PRINTSCREEN  70
#define SDL_SCANCODE_SCROLLLOCK   71
#define SDL_SCANCODE_PAUSE        72
#define SDL_SCANCODE_INSERT       73
#define SDL_SCANCODE_HOME         74
#define SDL_SCANCODE_PAGEUP       75
#define SDL_SCANCODE_DELETE       76
#define SDL_SCANCODE_END          77
#define SDL_SCANCODE_PAGEDOWN     78
#define SDL_SCANCODE_RIGHT        79
#define SDL_SCANCODE_LEFT         80
#define SDL_SCANCODE_DOWN         81
#define SDL_SCANCODE_UP           82

// ---- Keypad -----------------------------------------------------------------
#define SDL_SCANCODE_NUMLOCKCLEAR 83
#define SDL_SCANCODE_KP_DIVIDE    84
#define SDL_SCANCODE_KP_MULTIPLY  85
#define SDL_SCANCODE_KP_MINUS     86
#define SDL_SCANCODE_KP_PLUS      87
#define SDL_SCANCODE_KP_ENTER     88
#define SDL_SCANCODE_KP_1         89
#define SDL_SCANCODE_KP_2         90
#define SDL_SCANCODE_KP_3         91
#define SDL_SCANCODE_KP_4         92
#define SDL_SCANCODE_KP_5         93
#define SDL_SCANCODE_KP_6         94
#define SDL_SCANCODE_KP_7         95
#define SDL_SCANCODE_KP_8         96
#define SDL_SCANCODE_KP_9         97
#define SDL_SCANCODE_KP_0         98
#define SDL_SCANCODE_KP_PERIOD    99
#define SDL_SCANCODE_KP_DECIMAL   99   // SDL3 alias; same HID slot as KP_PERIOD
#define SDL_SCANCODE_KP_LESS      100  // Non-US backslash, used by engine as KP_LESS

// ---- Modifiers (HID page 0x07 modifier byte range, 224-231) -----------------
#define SDL_SCANCODE_LCTRL        224
#define SDL_SCANCODE_LSHIFT       225
#define SDL_SCANCODE_LALT         226
#define SDL_SCANCODE_LGUI         227
#define SDL_SCANCODE_RCTRL        228
#define SDL_SCANCODE_RSHIFT       229
#define SDL_SCANCODE_RALT         230
#define SDL_SCANCODE_RGUI         231

#define SDL_SCANCODE_APPLICATION  101

// ---- Keycode aliases --------------------------------------------------------
// SDL3 distinguishes scancodes (physical position) from keycodes (logical key).
// On Wii U V1 we don't have a real keyboard and don't care about the distinction;
// SDLK_* maps to the same numeric value as the corresponding scancode. V2 may
// refine this if a HID USB keyboard is attached.
#define SDL_SCANCODE_TO_KEYCODE(x) (x)

#define SDLK_UP            SDL_SCANCODE_UP
#define SDLK_DOWN          SDL_SCANCODE_DOWN
#define SDLK_LEFT          SDL_SCANCODE_LEFT
#define SDLK_RIGHT         SDL_SCANCODE_RIGHT
#define SDLK_HOME          SDL_SCANCODE_HOME
#define SDLK_END           SDL_SCANCODE_END
#define SDLK_PAGEUP        SDL_SCANCODE_PAGEUP
#define SDLK_PAGEDOWN      SDL_SCANCODE_PAGEDOWN
#define SDLK_SPACE         SDL_SCANCODE_SPACE
#define SDLK_RETURN        SDL_SCANCODE_RETURN
#define SDLK_DELETE        SDL_SCANCODE_DELETE
#define SDLK_INSERT        SDL_SCANCODE_INSERT
#define SDLK_ESCAPE        SDL_SCANCODE_ESCAPE
#define SDLK_BACKSPACE     SDL_SCANCODE_BACKSPACE
#define SDLK_TAB           SDL_SCANCODE_TAB
#define SDLK_NUMLOCKCLEAR  SDL_SCANCODE_NUMLOCKCLEAR
#define SDLK_KP_PLUS       SDL_SCANCODE_KP_PLUS
#define SDLK_KP_MINUS      SDL_SCANCODE_KP_MINUS
#define SDLK_F1  SDL_SCANCODE_F1
#define SDLK_F2  SDL_SCANCODE_F2
#define SDLK_F3  SDL_SCANCODE_F3
#define SDLK_F4  SDL_SCANCODE_F4
#define SDLK_F5  SDL_SCANCODE_F5
#define SDLK_F6  SDL_SCANCODE_F6
#define SDLK_F7  SDL_SCANCODE_F7
#define SDLK_F8  SDL_SCANCODE_F8
#define SDLK_F9  SDL_SCANCODE_F9
#define SDLK_F10 SDL_SCANCODE_F10
#define SDLK_F11 SDL_SCANCODE_F11
#define SDLK_F12 SDL_SCANCODE_F12
#define SDLK_F13 SDL_SCANCODE_F13
#define SDLK_F14 SDL_SCANCODE_F14
#define SDLK_F15 SDL_SCANCODE_F15
#define SDLK_F16 SDL_SCANCODE_F16
#define SDLK_F17 SDL_SCANCODE_F17
#define SDLK_F18 SDL_SCANCODE_F18
#define SDLK_F19 SDL_SCANCODE_F19
#define SDLK_F20 SDL_SCANCODE_F20
#define SDLK_F21 SDL_SCANCODE_F21
#define SDLK_F22 SDL_SCANCODE_F22
#define SDLK_F23 SDL_SCANCODE_F23
#define SDLK_F24 SDL_SCANCODE_F24

#endif // LBA2_TARGET_WIIU
