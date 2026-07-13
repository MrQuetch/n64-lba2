// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// DRC quick-action touch panel: layout, software rendering and hit test.
// Self-contained — draws with its own 5x7 bitmap font directly into the
// OSScreen DRC pixel buffers (RGBA8, same PackRGBA8 layout as the palette
// LUT in LIB386/SVGA/WIIU.CPP). No engine state is read here; activation
// (gameplay vs menu) is decided by the caller in WIIU_BACKEND.CPP.

#include "TOUCHPANEL_WIIU.H"

#include <SYSTEM/KEYBOARD_KEYS.H>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Engine global (SOURCES/GLOBAL.CPP): current hero behaviour, C_NORMAL..
// C_DISCRET = 0..3 (protopack/jetpack modes sit above 3 and get no
// highlight). GCC does not mangle global variable names, so this plain
// declaration links against the C++ definition regardless of extern "C".
extern U8 Comportement;

// ─── 5x7 bitmap font ─────────────────────────────────────────────────────────
// One U8 per row, bits 4..0 = leftmost..rightmost pixel.
// Charset: A-Z, 0-9, '-', '.' (space handled as pure advance).
static const U8 kFont[38][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}, // .
};

static const U8 *GlyphFor(char c) {
    if (c >= 'A' && c <= 'Z') return kFont[c - 'A'];
    if (c >= 'a' && c <= 'z') return kFont[c - 'a'];
    if (c >= '0' && c <= '9') return kFont[26 + (c - '0')];
    if (c == '-') return kFont[36];
    if (c == '.') return kFont[37];
    return NULL; // space / unknown: advance only
}

// ─── Layout ──────────────────────────────────────────────────────────────────
#define TP_SCREEN_W 854
#define TP_SCREEN_H 480

#define TP_MARGIN_X 12
#define TP_GAP 8
#define TP_CAPTION_H 20
#define TP_ROW_H 120
#define TP_BLOCK_PITCH 152 // caption + buttons + inter-row spacing
#define TP_TOP 8

typedef struct {
    S16 x, y, w, h;
    U16 scancode;
    const char *label;
} TP_Button;

typedef struct {
    const char *caption;
    U32 fill;
    U32 border;
    U8 nbButtons;
} TP_Row;

// English labels for now — the game's text banks are per-language, so a
// future pass can source these from TEXT.HQR based on the selected language.
static const TP_Row kRows[3] = {
    {"BEHAVIOUR", 0x24344EFFu, 0x51719EFFu, 4},
    {"WEAPONS",   0x442E28FFu, 0xA06B50FFu, 7},
    {"ACTIONS",   0x24402EFFu, 0x5A9E6EFFu, 5},
};

// Scancodes are the engine's original PC shortcuts (SOURCES/INPUT.CPP
// DefKeysDefault95): behaviours F5-F8, weapons 1-7, then H holomap,
// J proto/jetpack, C protection, F lightning, N penguin.
static TP_Button s_buttons[16] = {
    // Row 0 — behaviours
    {0, 0, 0, 0, K_F5, "NORMAL"},
    {0, 0, 0, 0, K_F6, "ATHLETIC"},
    {0, 0, 0, 0, K_F7, "AGGRESSIVE"},
    {0, 0, 0, 0, K_F8, "DISCREET"},
    // Row 1 — weapons 1..7
    {0, 0, 0, 0, K_1, "BALL"},
    {0, 0, 0, 0, K_2, "DARTS"},
    {0, 0, 0, 0, K_3, "BLOWGUN"},
    {0, 0, 0, 0, K_4, "CONCH"},
    {0, 0, 0, 0, K_5, "GLOVE"},
    {0, 0, 0, 0, K_6, "LASER"},
    {0, 0, 0, 0, K_7, "SABRE"},
    // Row 2 — actions / spells
    {0, 0, 0, 0, K_H, "HOLOMAP"},
    {0, 0, 0, 0, K_J, "PROTOPACK"},
    {0, 0, 0, 0, K_C, "PROTECTION"},
    {0, 0, 0, 0, K_F, "LIGHTNING"},
    {0, 0, 0, 0, K_N, "PENGUIN"},
};

static U8 s_buttonRow[16]; // button index -> row index (for colors)
static int s_layoutDone = 0;

static void LayoutInit(void) {
    if (s_layoutDone) return;
    s_layoutDone = 1;

    int b = 0;
    for (int r = 0; r < 3; r++) {
        int n = kRows[r].nbButtons;
        int usable = TP_SCREEN_W - 2 * TP_MARGIN_X - (n - 1) * TP_GAP;
        int w = usable / n;
        int y = TP_TOP + r * TP_BLOCK_PITCH + TP_CAPTION_H;
        int x = TP_MARGIN_X;
        for (int i = 0; i < n; i++, b++) {
            s_buttons[b].x = (S16)x;
            s_buttons[b].y = (S16)y;
            s_buttons[b].w = (S16)w;
            s_buttons[b].h = (S16)TP_ROW_H;
            s_buttonRow[b] = (U8)r;
            x += w + TP_GAP;
        }
    }
}

#define TP_NB_BUTTONS ((int)(sizeof(s_buttons) / sizeof(s_buttons[0])))

// ─── Drawing primitives ──────────────────────────────────────────────────────
#define TP_COL_BG      0x0C1220FFu
#define TP_COL_CAPTION 0x8FA3C8FFu
#define TP_COL_TEXT    0xE8EEF8FFu
#define TP_COL_PRESSED 0xE8B23CFFu
#define TP_COL_TEXT_PR 0x1A1A1AFFu
#define TP_COL_ACTIVE  0x3E639AFFu // fill of the currently-active behaviour
#define TP_COL_ACT_BRD 0xFFFFFFFFu

static void FillRect(U32 *buf, U32 pitch, U32 height,
                     int x, int y, int w, int h, U32 col) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if ((U32)(y + h) > height) h = (int)height - y;
    for (int j = 0; j < h; j++) {
        U32 *d = buf + (size_t)(y + j) * pitch + x;
        for (int i = 0; i < w; i++) d[i] = col;
    }
}

static void BorderRect(U32 *buf, U32 pitch, U32 height,
                       int x, int y, int w, int h, int t, U32 col) {
    FillRect(buf, pitch, height, x, y, w, t, col);
    FillRect(buf, pitch, height, x, y + h - t, w, t, col);
    FillRect(buf, pitch, height, x, y, t, h, col);
    FillRect(buf, pitch, height, x + w - t, y, t, h, col);
}

static int TextWidth(const char *s, int scale) {
    return (int)strlen(s) * 6 * scale - scale; // 5px glyph + 1px advance
}

static void DrawText(U32 *buf, U32 pitch, U32 height,
                     int x, int y, int scale, U32 col, const char *s) {
    for (; *s; s++, x += 6 * scale) {
        const U8 *g = GlyphFor(*s);
        if (!g) continue;
        for (int row = 0; row < 7; row++) {
            U8 bits = g[row];
            if (!bits) continue;
            for (int cx = 0; cx < 5; cx++) {
                if (!(bits & (0x10 >> cx))) continue;
                FillRect(buf, pitch, height,
                         x + cx * scale, y + row * scale, scale, scale, col);
            }
        }
    }
}

static void DrawPanel(U32 *buf, U32 pitch, U32 height, U32 pressedKey) {
    FillRect(buf, pitch, height, 0, 0, TP_SCREEN_W, (int)height, TP_COL_BG);

    for (int r = 0; r < 3; r++) {
        int capY = TP_TOP + r * TP_BLOCK_PITCH + 2;
        DrawText(buf, pitch, height, TP_MARGIN_X, capY, 2,
                 TP_COL_CAPTION, kRows[r].caption);
    }

    // Row-0 buttons are declared in Comportement order (0..3).
    int activeBehaviour = (Comportement <= 3) ? (int)Comportement : -1;

    for (int b = 0; b < TP_NB_BUTTONS; b++) {
        const TP_Button *bt = &s_buttons[b];
        const TP_Row *row = &kRows[s_buttonRow[b]];
        int pressed = (pressedKey != 0 && pressedKey == bt->scancode);
        int active = (s_buttonRow[b] == 0 && b == activeBehaviour);

        U32 fill = pressed ? TP_COL_PRESSED : (active ? TP_COL_ACTIVE : row->fill);
        U32 brd  = pressed ? TP_COL_TEXT    : (active ? TP_COL_ACT_BRD : row->border);
        FillRect(buf, pitch, height, bt->x, bt->y, bt->w, bt->h, fill);
        BorderRect(buf, pitch, height, bt->x, bt->y, bt->w, bt->h,
                   active ? 3 : 2, brd);

        int scale = 2;
        int tw = TextWidth(bt->label, scale);
        if (tw > bt->w - 8) scale = 1, tw = TextWidth(bt->label, 1);
        int tx = bt->x + (bt->w - tw) / 2;
        int ty = bt->y + (bt->h - 7 * scale) / 2;
        DrawText(buf, pitch, height, tx, ty, scale,
                 pressed ? TP_COL_TEXT_PR : TP_COL_TEXT, bt->label);
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────
static U32 s_dirty = 1;

void WiiUTouchPanel_Invalidate(void) {
    s_dirty = 1;
}

U32 WiiUTouchPanel_HitTest(S32 x, S32 y) {
    LayoutInit();
    for (int b = 0; b < TP_NB_BUTTONS; b++) {
        const TP_Button *bt = &s_buttons[b];
        if (x >= bt->x && x < bt->x + bt->w &&
            y >= bt->y && y < bt->y + bt->h) {
            return bt->scancode;
        }
    }
    return 0;
}

S32 WiiUTouchPanel_Render(U32 *buf0, U32 *buf1, U32 pitch, U32 height,
                          U32 pressedKey) {
    static U32 s_lastPressed = 0xFFFFFFFFu;
    static U32 s_lastBehaviour = 0xFFFFFFFFu;

    if (!buf0 || !buf1 || pitch < TP_SCREEN_W) return 0;
    U32 behaviourNow = (U32)Comportement;
    if (!s_dirty && pressedKey == s_lastPressed &&
        behaviourNow == s_lastBehaviour) return 0;

    LayoutInit();
    DrawPanel(buf0, pitch, height, pressedKey);
    DrawPanel(buf1, pitch, height, pressedKey);

    s_dirty = 0;
    s_lastPressed = pressedKey;
    s_lastBehaviour = behaviourNow;
    return 1;
}

#ifdef __cplusplus
}
#endif
