// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/WIIU/VIDEO_AUDIO_WIIU.CPP
// VIDEO_AUDIO contract for the Wii U AX backend (Smacker FMV audio).
//
// Push-driven counterpart of the music FIFO in STREAM_WIIU.CPP: PlayAcf
// pushes each Smacker frame's PCM (8/16-bit, mono/stereo, track rate) from
// the game thread; PushVideoAudio converts + linear-resamples it to the
// 32 kHz AX output rate into a lock-free S16 ring, and the AX app-frame
// callback in SOUND_WIIU_BACKEND.CPP drains it via VideoAudio_MixInto().
//
// The FIFO must swallow PlayAcf's pre-fill (up to ~2.5 s of audio pushed
// before ResumeVideoAudio) plus the one-frame-ahead steady state, hence
// 4 s of headroom. Stream is born paused per the VIDEO_AUDIO.H contract.

#include "AIL.H"

#include <SYSTEM/LOGPRINT.H>

#ifdef __cplusplus
extern "C" {
#endif

#define VA_OUTPUT_RATE 32000

// Power of two: 131072 frames = 4.1 s at 32 kHz (512 KB BSS).
#define VA_FIFO_FRAMES 131072
#define VA_FIFO_MASK (VA_FIFO_FRAMES - 1)

// FIFO (single producer = game thread, single consumer = AX callback).
static S16 vaFifoL[VA_FIFO_FRAMES];
static S16 vaFifoR[VA_FIFO_FRAMES];
static volatile U32 vaWr = 0;
static volatile U32 vaRd = 0;

static volatile int vaActive = 0;
static volatile int vaPaused = 0;

// Source format (fixed for the lifetime of one video).
static int vaRate = VA_OUTPUT_RATE;
static int vaChannels = 1;
static int vaIs16 = 1;

// Resampler window, carried across PushVideoAudio calls.
static S16 vaWinL[2], vaWinR[2];
static float vaPos = 0.0f;
static int vaPrimed = 0;

static U32 vaDropped = 0;

S32 StartVideoAudio(S32 freq, S32 channels, S32 is16bit) {
    if (freq <= 0 || freq > 192000)
        return FALSE;
    vaActive = 0; // stop the consumer while we reset
    vaRate = (int)freq;
    vaChannels = (channels >= 2) ? 2 : 1;
    vaIs16 = (is16bit != 0) ? 1 : 0;
    vaRd = vaWr;
    vaPrimed = 0;
    vaPos = 0.0f;
    vaDropped = 0;
    vaPaused = 1; // contract: born paused, ResumeVideoAudio() starts playback
    vaActive = 1;
    return TRUE;
}

void ResumeVideoAudio(void) {
    vaPaused = 0;
}

void PauseVideoAudio(void) {
    vaPaused = 1;
}

void PushVideoAudio(const void *data, U32 sizeBytes) {
    if (!vaActive || !data)
        return;

    const U8 *p = (const U8 *)data;
    const U32 frameBytes = (U32)vaChannels * (vaIs16 ? 2u : 1u);
    U32 nFrames = sizeBytes / frameBytes;
    const float step = (float)vaRate / (float)VA_OUTPUT_RATE;

    U32 wr = vaWr;
    for (U32 i = 0; i < nFrames; i++) {
        S16 l, r;
        if (vaIs16) {
            // libsmacker assembles decoded samples arithmetically, so the
            // pushed buffer holds native-endian shorts — read them as-is.
            const S16 *s = (const S16 *)(p + (size_t)i * frameBytes);
            l = s[0];
            r = (vaChannels == 2) ? s[1] : l;
        } else {
            const U8 *s = p + (size_t)i * frameBytes;
            l = (S16)(((int)s[0] - 128) << 8);
            r = (vaChannels == 2) ? (S16)(((int)s[1] - 128) << 8) : l;
        }

        if (!vaPrimed) {
            vaWinL[1] = l;
            vaWinR[1] = r;
            vaPos = 0.0f;
            vaPrimed = 1;
            continue;
        }

        vaWinL[0] = vaWinL[1];
        vaWinR[0] = vaWinR[1];
        vaWinL[1] = l;
        vaWinR[1] = r;

        while (vaPos < 1.0f) {
            if (((wr - vaRd) & 0xFFFFFFFFu) < VA_FIFO_FRAMES - 1) {
                vaFifoL[wr & VA_FIFO_MASK] =
                    (S16)((float)vaWinL[0] + vaPos * (float)(vaWinL[1] - vaWinL[0]));
                vaFifoR[wr & VA_FIFO_MASK] =
                    (S16)((float)vaWinR[0] + vaPos * (float)(vaWinR[1] - vaWinR[0]));
                wr++;
            } else {
                vaDropped++;
            }
            vaPos += step;
        }
        vaPos -= 1.0f;
    }
    vaWr = wr;
}

void StopVideoAudio(void) {
    vaActive = 0;
    vaPaused = 0;
    vaRd = vaWr; // consumer sees an empty FIFO immediately
    if (vaDropped) {
        LogPrintf("[VIDEO] audio FIFO overflow: %lu frames dropped\n",
                  (unsigned long)vaDropped);
        vaDropped = 0;
    }
}

// Drain hook, called from the AX app-frame callback in SOUND_WIIU_BACKEND.CPP.
void VideoAudio_MixInto(float *accL, float *accR, int frames) {
    if (!vaActive || vaPaused)
        return;
    U32 rd = vaRd;
    U32 avail = (vaWr - rd) & 0xFFFFFFFFu;
    U32 n = ((U32)frames < avail) ? (U32)frames : avail;
    for (U32 i = 0; i < n; i++) {
        accL[i] += (float)vaFifoL[rd & VA_FIFO_MASK];
        accR[i] += (float)vaFifoR[rd & VA_FIFO_MASK];
        rd++;
    }
    vaRd = rd;
}

#ifdef __cplusplus
}
#endif
