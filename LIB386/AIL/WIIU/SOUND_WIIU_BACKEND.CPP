// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/WIIU/SOUND_WIIU_BACKEND.CPP
// Wii U (WUT/AX) implementation of the AIL SAMPLE + COMMON + MIDI contracts.
//
// Architecture: AX is used as a dumb stereo DAC. Two mono AXVoices (L/R)
// loop forever over two small S16 ring buffers; everything the game plays
// is SOFTWARE-mixed into those rings from the AX app-frame callback (fires
// every 3 ms on the AX callback thread), so SFX latency is a few ms and the
// mix is immune to game-thread stalls (HQR loads etc). The voice structs,
// decoders (RIFF WAV PCM / IMA ADPCM, Creative VOC, raw 8-bit) and handle
// semantics are derived from the desktop AIL/SDL backend so gameplay-facing
// behaviour is identical; only the output device differs.
//
// The music/jingle stream (STREAM_WIIU.CPP) decodes OGG on the game thread
// into a 1-second FIFO; this file's frame callback drains that FIFO into
// the same rings. AudioStreamPump() (called from ManageTime, mirroring the
// Dreamcast KOS backend's pump model) runs the decoder and frees buffers of
// voices that finished — free() never runs on the AX callback thread.

#include "AIL.H"

#include <SYSTEM/LOGPRINT.H>

#include <coreinit/cache.h>
#include <coreinit/fastmutex.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>
#include <sndcore2/device.h>

#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100 // default assumed rate for raw/headerless samples
#define OUTPUT_RATE 32000 // AX renderer rate (fixed by AX_INIT_RENDERER_32KHZ)
#define NUM_VOICES 12
#define PITCHBEND_NORMAL 4096

// Output rings: power of two so cursor deltas wrap with a mask. 8192 frames
// at 32 kHz = 256 ms of buffer; the callback keeps the write cursor a fixed
// small distance ahead of the AX read cursor.
#define RING_FRAMES 8192
#define RING_MASK (RING_FRAMES - 1)
// How far ahead of the AX read cursor we keep the mix (16 ms). One AX frame
// is 96 samples / 3 ms, so this absorbs ~5 missed callbacks before the ring
// replays stale data.
#define AHEAD_FRAMES 512
// Max frames mixed per callback invocation (stack accumulator size).
#define MIX_CHUNK_FRAMES 1024

#ifdef __cplusplus
extern "C" {
#endif

// STREAM_WIIU.CPP exports (music FIFO drain + decoder pump + log toggle).
void Stream_MixInto(float *accL, float *accR, int frames);
void Stream_Pump(void);
void MusicLogEnable(int enable);

#ifdef USE_MVIDEO_SMACKER
// VIDEO_AUDIO_WIIU.CPP export (FMV audio FIFO drain).
void VideoAudio_MixInto(float *accL, float *accR, int frames);
#endif

extern volatile U32 TimerSystemHR;

/* ── IMA ADPCM tables and decoder (derived from AIL/SDL/SAMPLE.CPP; all
      reads are byte-composed little-endian, so the code is endian-safe on
      big-endian PPC as-is) ─────────────────────────────────────────────── */

static const S16 imaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};

static const int imaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static S16 *DecodeImaAdpcm(const U8 *adpcmData, S32 adpcmSize,
                           S32 blockAlign, S32 *outSamples) {
    if (blockAlign < 8 || adpcmSize < blockAlign) {
        *outSamples = 0;
        return NULL;
    }
    S32 samplesPerBlock = (blockAlign - 4) * 2 + 1;
    S32 numBlocks = adpcmSize / blockAlign;
    S32 remainder = adpcmSize % blockAlign;
    S32 totalSamples = numBlocks * samplesPerBlock;
    if (remainder >= 8)
        totalSamples += (remainder - 4) * 2 + 1;
    S16 *pcm = (S16 *)malloc((size_t)totalSamples * sizeof(S16));
    if (!pcm) {
        *outSamples = 0;
        return NULL;
    }
    S32 outIdx = 0;
    const U8 *src = adpcmData;
    S32 bytesLeft = adpcmSize;
    while (bytesLeft >= 8) {
        S32 blockSize = (bytesLeft >= blockAlign) ? blockAlign : bytesLeft;
        S16 predictor = (S16)(src[0] | (src[1] << 8));
        int stepIndex = src[2];
        if (stepIndex < 0)
            stepIndex = 0;
        if (stepIndex > 88)
            stepIndex = 88;
        pcm[outIdx++] = predictor;
        for (S32 i = 4; i < blockSize; i++) {
            for (int nibbleIdx = 0; nibbleIdx < 2 && outIdx < totalSamples; nibbleIdx++) {
                int nibble = (nibbleIdx == 0) ? (src[i] & 0x0F) : ((src[i] >> 4) & 0x0F);
                int step = imaStepTable[stepIndex];
                int diff = step >> 3;
                if (nibble & 1)
                    diff += step >> 2;
                if (nibble & 2)
                    diff += step >> 1;
                if (nibble & 4)
                    diff += step;
                if (nibble & 8)
                    diff = -diff;
                int sample = (int)predictor + diff;
                if (sample > 32767)
                    sample = 32767;
                if (sample < -32768)
                    sample = -32768;
                predictor = (S16)sample;
                pcm[outIdx++] = predictor;
                stepIndex += imaIndexTable[nibble];
                if (stepIndex < 0)
                    stepIndex = 0;
                if (stepIndex > 88)
                    stepIndex = 88;
            }
        }
        src += blockSize;
        bytesLeft -= blockSize;
    }
    *outSamples = outIdx;
    return pcm;
}

/* ── VOC parser (derived from AIL/SDL/SAMPLE.CPP) ────────────────────────── */

#define VOC_HEADER_MAGIC "reative Voice File\x1A"

static int ParseVocIfPresent(const U8 *buffer, S32 sizeBytes,
                             const U8 **data, S32 *payloadSize, S32 *outHz, int *outIs8bit) {
    if (sizeBytes < 26)
        return 0;
    if (buffer[0] != 'C' && buffer[0] != 'W' && buffer[0] != 0)
        return 0;
    if (memcmp(buffer + 1, VOC_HEADER_MAGIC, 19) != 0)
        return 0;
    *outHz = SAMPLE_RATE;
    *outIs8bit = 1;
    S32 pos = 26;
    S32 extendedRate = 0;
    while (pos + 4 <= sizeBytes) {
        U8 type = buffer[pos];
        S32 blockSize = (S32)(buffer[pos + 1] | (buffer[pos + 2] << 8) | (buffer[pos + 3] << 16));
        if (type == 0x00)
            break;
        if (pos + 4 + blockSize > sizeBytes)
            break;
        if (type == 0x08 && blockSize >= 4) {
            S32 freqDiv = (S32)(buffer[pos + 4] | (buffer[pos + 5] << 8));
            int ch = (int)(buffer[pos + 7]) + 1;
            if (ch < 1 || ch > 8)
                ch = 1;
            if (freqDiv != 65535 && ch > 0)
                extendedRate = 256000000 / ((65536 - freqDiv) * ch);
        }
        if (type == 0x01 && blockSize >= 2) {
            U8 freqDiv = buffer[pos + 4];
            U8 codec = buffer[pos + 5];
            if (extendedRate > 0)
                *outHz = extendedRate;
            else if (freqDiv != 255)
                *outHz = 1000000 / (256 - (S32)freqDiv);
            if (*outHz <= 0 || *outHz > 192000)
                *outHz = SAMPLE_RATE;
            *data = buffer + pos + 6;
            *payloadSize = blockSize - 2;
            *outIs8bit = (codec == 0) ? 1 : 0;
            return 1;
        }
        pos += 4 + blockSize;
    }
    return 0;
}

/* ── WAV parser (derived from AIL/SDL/SAMPLE.CPP — including the permissive
      first-byte check for .VOX records whose leading 'R' is overwritten by
      MESSAGE.CPP::PlaySpeakVoc's FlagNextVoc byte) ─────────────────────── */

static void SkipWavHeaderIfPresent(const U8 *buffer, S32 sizeBytes,
                                   const U8 **data, S32 *payloadSize, int *hadWav,
                                   S32 *outWavHz, S32 *outWavChannels, S32 *outWavBits,
                                   S32 *outFmtTag, S32 *outBlockAlign) {
    *data = (const U8 *)buffer;
    *payloadSize = sizeBytes;
    *hadWav = 0;
    *outWavHz = SAMPLE_RATE;
    *outWavChannels = 1;
    *outWavBits = 16;
    *outFmtTag = 1;
    *outBlockAlign = 0;
    if (sizeBytes < 12)
        return;
    if ((memcmp(buffer, "RIFF", 4) != 0 || memcmp(buffer + 8, "WAVE", 4) != 0) &&
        (buffer[1] != 'I' || buffer[2] != 'F' || buffer[3] != 'F' || memcmp(buffer + 8, "WAVE", 4) != 0))
        return;
    S32 pos = 12;
    S32 wavHz = SAMPLE_RATE, wavChannels = 1, wavBits = 16, fmtTag = 1, blkAlign = 0;
    while (pos + 8 <= sizeBytes) {
        S32 chunkSize = (S32)(buffer[pos + 4] | (buffer[pos + 5] << 8) |
                              (buffer[pos + 6] << 16) | (buffer[pos + 7] << 24));
        if (memcmp(buffer + pos, "fmt ", 4) == 0 && chunkSize >= 16 && pos + 8 + 16 <= sizeBytes) {
            const U8 *f = buffer + pos + 8;
            fmtTag = (S32)(f[0] | (f[1] << 8));
            wavChannels = (S32)(f[2] | (f[3] << 8));
            wavHz = (S32)(f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24));
            blkAlign = (S32)(f[12] | (f[13] << 8));
            wavBits = (S32)(f[14] | (f[15] << 8));
            if (wavHz <= 0 || wavHz > 192000)
                wavHz = SAMPLE_RATE;
            if (wavChannels <= 0 || wavChannels > 8)
                wavChannels = 1;
            if (fmtTag == 1) {
                if (blkAlign > 0 && wavChannels > 0)
                    wavBits = (blkAlign * 8) / wavChannels;
                if (wavBits != 8 && wavBits != 16)
                    wavBits = 16;
            }
        }
        if (memcmp(buffer + pos, "data", 4) == 0 && chunkSize > 0 && pos + 8 + chunkSize <= sizeBytes) {
            *data = buffer + pos + 8;
            *payloadSize = chunkSize;
            *hadWav = 1;
            *outWavHz = wavHz;
            *outWavChannels = wavChannels;
            *outWavBits = wavBits;
            *outFmtTag = fmtTag;
            *outBlockAlign = blkAlign;
            return;
        }
        pos += 8 + ((chunkSize + 1) & ~1);
    }
}

/* ── Voice pool (struct + handle semantics identical to AIL/SDL) ─────────── */

struct Voice {
    const U8 *data;
    S32 sizeBytes;
    bool is8bit;
    S32 sourceRateHz;
    S32 sourceChannels;
    float posSamples;
    float stepSamples;
    S32 volume;
    S32 baseVolume;
    S32 pan;
    S32 repeat;
    S32 repeatCount;
    U32 handle;
    U32 timer;
    bool active;
    bool dead;      // finished in the mix callback; buffer freed by the pump
    void *ownedBuf;
};

static Voice voices[NUM_VOICES];
static U32 nextHandle = 0x1000000;
static S32 samplesPaused = 0;
static S32 inverseStereo = 0;
static bool samplesFade = false;
static U32 fadeStartTime = 0;

S32 Sample_Driver_Enabled = FALSE;
S32 UseWaveMixer = FALSE;
S32 sampleVolume = 0;
static int sfxLogEnabled = 0;

// Guards voices[] between the game thread and the AX callback thread.
// Critical sections on the game side are kept to pointer/struct swaps —
// decode and memcpy happen before taking the lock.
static OSFastMutex voicesMutex;

static void LockVoices(void) { OSFastMutex_Lock(&voicesMutex); }
static void UnlockVoices(void) { OSFastMutex_Unlock(&voicesMutex); }

// free() only from the game thread (pump): the AX callback marks `dead`.
static void FreeVoiceBuf(Voice *V) {
    if (V->ownedBuf) {
        free(V->ownedBuf);
        V->ownedBuf = NULL;
    }
    V->data = NULL;
}

static int VoiceIndexFromHandle(U32 sample) {
    if (sample < 0x1000000) {
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].active && ((voices[i].handle >> 8) & 0xFFFF) == sample)
                return i;
        }
    } else {
        int slot = sample & 0xFF;
        if (slot < NUM_VOICES && voices[slot].active && voices[slot].handle == sample)
            return slot;
    }
    return -1;
}

/* ── AX output device ────────────────────────────────────────────────────── */

static AXVoice *axVoiceL = NULL;
static AXVoice *axVoiceR = NULL;
__attribute__((aligned(64))) static S16 ringL[RING_FRAMES];
__attribute__((aligned(64))) static S16 ringR[RING_FRAMES];
static U32 ringWrite = 0;   // next frame index we will mix into
static U32 lastReadPos = 0; // AX read cursor at the previous callback

static inline S16 ClampToS16(float sample) {
    if (sample > 32767.0f)
        return 32767;
    if (sample < -32768.0f)
        return -32768;
    return (S16)sample;
}

// Mix `numFrames` of all active game voices into accL/accR (adds on top of
// whatever the stream already deposited). Derived from AIL/SDL MixVoices;
// output cadence is OUTPUT_RATE instead of 44100.
static void MixVoices(float *accL, float *accR, int numFrames) {
    for (int v = 0; v < NUM_VOICES; v++) {
        Voice *V = &voices[v];
        if (!V->active || V->dead || !V->data)
            continue;
        float pos = V->posSamples;
        float step = V->stepSamples;
        int maxSamples = V->is8bit ? V->sizeBytes : (V->sizeBytes / 2);
        const int fadeLen = 64;
        float leftGain = (V->pan <= 64) ? 1.0f : (float)(127 - V->pan) / 63.0f;
        float rightGain = (V->pan >= 64) ? 1.0f : (float)V->pan / 64.0f;
        if (inverseStereo) {
            float tmp = leftGain;
            leftGain = rightGain;
            rightGain = tmp;
        }
        float vol = (float)(sampleVolume * V->volume) / (127.0f * 127.0f);
        if (vol > 1.0f)
            vol = 1.0f;
        leftGain *= vol;
        rightGain *= vol;
        for (int i = 0; i < numFrames; i++) {
            int idx = (int)pos;
            if (idx >= maxSamples) {
                if (V->repeat == 0) {
                    pos = 0;
                    idx = 0;
                } else {
                    V->repeatCount++;
                    if (V->repeatCount >= V->repeat) {
                        V->dead = true; // freed by AudioStreamPump
                        break;
                    }
                    pos = 0;
                    idx = 0;
                }
            }
            float frac = pos - (float)idx;
            int idx1 = (idx + 1 < maxSamples) ? idx + 1 : idx;
            float s;
            if (V->is8bit) {
                float s0 = (float)((int)V->data[idx] - 128) * 256.0f;
                float s1 = (float)((int)V->data[idx1] - 128) * 256.0f;
                s = s0 + frac * (s1 - s0);
            } else {
                const S16 *src = (const S16 *)V->data;
                float s0 = (float)src[idx];
                float s1 = (float)src[idx1];
                s = s0 + frac * (s1 - s0);
            }
            int samplesLeft = maxSamples - idx;
            if (V->repeat != 0 && samplesLeft < fadeLen) {
                float fade = (float)samplesLeft / (float)fadeLen;
                s *= fade;
            }
            accL[i] += s * leftGain;
            accR[i] += s * rightGain;
            pos += step;
        }
        V->posSamples = pos;
    }
}

// AX app-frame callback: every 3 ms, advance the mix so the write cursor
// stays AHEAD_FRAMES in front of the AX read cursor.
static void AxFrameCallback(void) {
    if (!axVoiceL || !axVoiceR)
        return;

    AXVoiceOffsets offs;
    AXGetVoiceOffsets(axVoiceL, &offs);
    U32 readPos = offs.currentOffset & RING_MASK;

    U32 consumed = (readPos - lastReadPos) & RING_MASK;
    lastReadPos = readPos;
    if (consumed == 0)
        return;
    if (consumed > MIX_CHUNK_FRAMES)
        consumed = MIX_CHUNK_FRAMES; // after a long stall just jump forward

    static float accL[MIX_CHUNK_FRAMES];
    static float accR[MIX_CHUNK_FRAMES];
    memset(accL, 0, consumed * sizeof(float));
    memset(accR, 0, consumed * sizeof(float));

    Stream_MixInto(accL, accR, (int)consumed);
#ifdef USE_MVIDEO_SMACKER
    // FMV audio is intentionally outside the samplesPaused gate: PlayAcf
    // pauses game samples for the whole video, but its own track must play.
    VideoAudio_MixInto(accL, accR, (int)consumed);
#endif

    LockVoices();
    if (!samplesPaused)
        MixVoices(accL, accR, (int)consumed);
    UnlockVoices();

    U32 w = ringWrite;
    for (U32 i = 0; i < consumed; i++) {
        ringL[w] = ClampToS16(accL[i]);
        ringR[w] = ClampToS16(accR[i]);
        w = (w + 1) & RING_MASK;
    }
    // Flush the written span (two spans if it wrapped).
    if (w > ringWrite) {
        DCFlushRange(&ringL[ringWrite], (w - ringWrite) * sizeof(S16));
        DCFlushRange(&ringR[ringWrite], (w - ringWrite) * sizeof(S16));
    } else {
        DCFlushRange(&ringL[ringWrite], (RING_FRAMES - ringWrite) * sizeof(S16));
        DCFlushRange(&ringR[ringWrite], (RING_FRAMES - ringWrite) * sizeof(S16));
        if (w) {
            DCFlushRange(&ringL[0], w * sizeof(S16));
            DCFlushRange(&ringR[0], w * sizeof(S16));
        }
    }
    ringWrite = w;
}

static void SetupRingVoice(AXVoice *v, S16 *ring, int channel) {
    AXVoiceOffsets offs;
    memset(&offs, 0, sizeof(offs));
    offs.dataType = AX_VOICE_FORMAT_LPCM16;
    offs.loopingEnabled = AX_VOICE_LOOP_ENABLED;
    offs.loopOffset = 0;
    offs.endOffset = RING_FRAMES - 1;
    offs.currentOffset = 0;
    offs.data = ring;

    AXVoiceVeData ve;
    memset(&ve, 0, sizeof(ve));
    ve.volume = 0x8000;

    // One mix entry per output channel; route this voice to `channel` only.
    AXVoiceDeviceMixData mix[6];
    memset(mix, 0, sizeof(mix));
    mix[channel].bus[0].volume = 0x8000;

    AXVoiceBegin(v);
    AXSetVoiceOffsets(v, &offs);
    AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_NONE);
    AXSetVoiceVe(v, &ve);
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, mix);
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, mix);
    AXSetVoiceState(v, AX_VOICE_STATE_PLAYING);
    AXVoiceEnd(v);
}

/* ── Driver init / control ───────────────────────────────────────────────── */

S32 InitSampleDriver(char *driver_name) {
    (void)driver_name;
    if (Sample_Driver_Enabled)
        return TRUE;

    memset(voices, 0, sizeof(voices));
    OSFastMutex_Init(&voicesMutex, "AIL voices");

    if (!AXIsInit()) {
        AXInitParams params;
        memset(&params, 0, sizeof(params));
        params.renderer = AX_INIT_RENDERER_32KHZ;
        params.pipeline = AX_INIT_PIPELINE_SINGLE;
        AXInitWithParams(&params);
    }

    memset(ringL, 0, sizeof(ringL));
    memset(ringR, 0, sizeof(ringR));
    DCFlushRange(ringL, sizeof(ringL));
    DCFlushRange(ringR, sizeof(ringR));

    axVoiceL = AXAcquireVoice(31, NULL, NULL);
    axVoiceR = AXAcquireVoice(31, NULL, NULL);
    if (!axVoiceL || !axVoiceR) {
        LogPuts("Wii U AIL: AXAcquireVoice failed — audio disabled.");
        return FALSE;
    }

    ringWrite = AHEAD_FRAMES;
    lastReadPos = 0;

    SetupRingVoice(axVoiceL, ringL, 0);
    SetupRingVoice(axVoiceR, ringR, 1);

    AXRegisterAppFrameCallback(AxFrameCallback);

    Sample_Driver_Enabled = TRUE;
    LogPuts("Wii U AIL: AX sample driver up (32 kHz, 2 ring voices).");
    return TRUE;
}

void SetMasterVolumeSample(S32 volume) {
    if (volume)
        volume++;
    LockVoices();
    sampleVolume = volume;
    UnlockVoices();
}

static void ScaleVolumeSamples(S32 ratio) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].active)
            continue;
        S32 base = voices[i].baseVolume;
        S32 scaled = (base * ratio + 64) >> 7;
        voices[i].volume = (scaled > 127) ? 127 : scaled;
    }
}

// Live wall clock for the fade loops. HQ_PauseSamples spins
// `while (FadeOutSamples(200));` WITHOUT calling ManageTime, so basing the
// fade on TimerSystemHR (which only advances inside ManageTime) never
// converges and the pause menu hangs. The SDL backend used SDL_GetTicks —
// same idea.
static U32 NowMs(void) {
    return (U32)OSTicksToMilliseconds(OSGetSystemTime());
}

S32 FadeOutSamples(S32 delay) {
    if (delay <= 0)
        return 0;
    U32 now = NowMs();
    if (!samplesFade) {
        fadeStartTime = now;
        samplesFade = true;
    }
    S32 delta = (S32)(fadeStartTime + (U32)delay - now);
    if (delta < 0)
        delta = 0;
    LockVoices();
    ScaleVolumeSamples(delta * 128 / delay);
    UnlockVoices();
    return delta;
}

S32 FadeInSamples(S32 delay) {
    if (delay <= 0) {
        if (samplesFade) {
            samplesFade = false;
            LockVoices();
            ScaleVolumeSamples(128);
            UnlockVoices();
        }
        return 0;
    }
    U32 now = NowMs();
    if (samplesFade) {
        fadeStartTime = now;
        samplesFade = false;
    }
    S32 delta = (S32)(now - fadeStartTime);
    if (delta > delay)
        delta = delay;
    if (delta < 0)
        delta = 0;
    LockVoices();
    ScaleVolumeSamples(delta * 128 / delay);
    UnlockVoices();
    return delta;
}

void InverseStereoSample(S32 inverse) {
    inverseStereo = (inverse != 0);
}

/* ── PlaySample (semantics identical to AIL/SDL) ─────────────────────────── */

U32 PlaySample(void *buffer, S32 sizeBytes, U32 userhandle, S32 pitchbend,
               S32 repeat, S32 volume, S32 pan) {
    if (!Sample_Driver_Enabled || !buffer || sizeBytes <= 0)
        return 0xFFFFFFFF;

    const U8 *data = NULL;
    S32 payloadSize = 0;
    S32 vocHz = SAMPLE_RATE;
    int vocIs8bit = 1;
    int hadWav = 0;
    S32 wavHz = SAMPLE_RATE, wavChannels = 1, wavBits = 16, fmtTag = 1, blkAlign = 0;
    int hadVoc = ParseVocIfPresent((const U8 *)buffer, sizeBytes, &data, &payloadSize, &vocHz, &vocIs8bit);
    if (!hadVoc)
        SkipWavHeaderIfPresent((const U8 *)buffer, sizeBytes, &data, &payloadSize,
                               &hadWav, &wavHz, &wavChannels, &wavBits, &fmtTag, &blkAlign);
    if (!data || payloadSize <= 0)
        return 0xFFFFFFFF;
    if (pitchbend <= 0)
        pitchbend = PITCHBEND_NORMAL;

    // Decode/copy OUTSIDE the lock so the 3 ms mix callback never waits on
    // a large memcpy.
    void *ownedBuf = NULL;
    const U8 *voiceData = NULL;
    S32 voiceSize = 0;
    bool is8bit;
    S32 rateHz, channels;

    if (hadWav && fmtTag == 17 && blkAlign > 0) {
        S32 decodedSamples = 0;
        S16 *pcm = DecodeImaAdpcm(data, payloadSize, blkAlign, &decodedSamples);
        if (!pcm || decodedSamples <= 0)
            return 0xFFFFFFFF;
        ownedBuf = pcm;
        voiceData = (const U8 *)pcm;
        voiceSize = decodedSamples * 2;
        is8bit = false;
        rateHz = wavHz;
        channels = wavChannels;
    } else {
        void *copy = malloc((size_t)payloadSize);
        if (!copy)
            return 0xFFFFFFFF;
        memcpy(copy, data, (size_t)payloadSize);
        ownedBuf = copy;
        voiceData = (const U8 *)copy;
        voiceSize = payloadSize;
        is8bit = hadVoc ? (vocIs8bit != 0) : (hadWav ? (wavBits == 8) : true);
        rateHz = hadVoc ? vocHz : (hadWav ? wavHz : SAMPLE_RATE);
        channels = hadWav ? wavChannels : 1;
    }

    LockVoices();

    int slot = -1;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // Steal: oldest one-shot first, then oldest finite-repeat, then oldest.
        U32 bestHandle = 0xFFFFFFFF;
        int bestSlot = -1;
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].repeat == 1 && voices[i].handle < bestHandle) {
                bestHandle = voices[i].handle;
                bestSlot = i;
            }
        }
        if (bestSlot < 0) {
            for (int i = 0; i < NUM_VOICES; i++) {
                if (voices[i].repeat != 0 && voices[i].handle < bestHandle) {
                    bestHandle = voices[i].handle;
                    bestSlot = i;
                }
            }
        }
        if (bestSlot < 0) {
            bestHandle = 0xFFFFFFFF;
            for (int i = 0; i < NUM_VOICES; i++) {
                if (voices[i].handle < bestHandle) {
                    bestHandle = voices[i].handle;
                    bestSlot = i;
                }
            }
        }
        slot = (bestSlot >= 0) ? bestSlot : 0;
    }

    Voice *V = &voices[slot];
    void *oldBuf = V->ownedBuf; // free after unlock
    V->ownedBuf = NULL;
    V->data = NULL;
    V->active = false;
    V->dead = false;

    V->ownedBuf = ownedBuf;
    V->data = voiceData;
    V->sizeBytes = voiceSize;
    V->is8bit = is8bit;
    V->sourceRateHz = rateHz;
    V->sourceChannels = channels;

    V->posSamples = 0;
    {
        float rateRatio = (float)V->sourceRateHz / (float)OUTPUT_RATE;
        int ch = (V->sourceChannels >= 2) ? 2 : 1;
        V->stepSamples = rateRatio * (float)ch * (float)pitchbend / (float)PITCHBEND_NORMAL;
    }
    V->baseVolume = (volume > 127) ? 127 : volume;
    V->volume = samplesFade ? 0 : V->baseVolume;
    V->pan = (pan > 127) ? 127 : ((pan < 0) ? 0 : pan);
    V->repeat = repeat;
    V->repeatCount = 0;
    V->handle = nextHandle | ((userhandle & 0xFFFF) << 8) | (slot & 0xFF);
    V->timer = TimerSystemHR;
    nextHandle += 0x1000000;
    if (nextHandle == 0)
        nextHandle = 0x1000000;
    V->active = true;

    U32 handle = V->handle;
    UnlockVoices();

    if (oldBuf)
        free(oldBuf);
    if (sfxLogEnabled)
        LogPrintf("[SFX] PlaySample user=0x%04X slot=%d rate=%d %s size=%d rep=%d vol=%d pan=%d\n",
                  userhandle & 0xFFFF, slot, (int)rateHz, is8bit ? "u8" : "s16",
                  (int)voiceSize, (int)repeat, (int)volume, (int)pan);
    return handle;
}

void ChangePitchbendSample(U32 sample, S32 pitchbend) {
    LockVoices();
    int i = VoiceIndexFromHandle(sample);
    if (i >= 0 && pitchbend > 0) {
        Voice *V = &voices[i];
        float rateRatio = (float)V->sourceRateHz / (float)OUTPUT_RATE;
        int ch = (V->sourceChannels >= 2) ? 2 : 1;
        V->stepSamples = rateRatio * (float)ch * (float)pitchbend / (float)PITCHBEND_NORMAL;
    }
    UnlockVoices();
}

void ChangeVolumePanSample(U32 sample, S32 volume, S32 pan) {
    LockVoices();
    int i = VoiceIndexFromHandle(sample);
    if (i >= 0) {
        voices[i].baseVolume = (volume > 127) ? 127 : volume;
        voices[i].volume = voices[i].baseVolume;
        voices[i].pan = (pan > 127) ? 127 : ((pan < 0) ? 0 : pan);
    }
    UnlockVoices();
}

void StopOneSample(U32 sample) {
    LockVoices();
    int i = VoiceIndexFromHandle(sample);
    void *buf = NULL;
    if (i >= 0) {
        buf = voices[i].ownedBuf;
        voices[i].ownedBuf = NULL;
        voices[i].data = NULL;
        voices[i].active = false;
    }
    UnlockVoices();
    if (buf)
        free(buf);
}

void StopSamples() {
    void *bufs[NUM_VOICES];
    LockVoices();
    for (int i = 0; i < NUM_VOICES; i++) {
        bufs[i] = voices[i].ownedBuf;
        voices[i].ownedBuf = NULL;
        voices[i].data = NULL;
        voices[i].active = false;
    }
    nextHandle = 0x1000000;
    samplesPaused = 0;
    UnlockVoices();
    for (int i = 0; i < NUM_VOICES; i++) {
        if (bufs[i])
            free(bufs[i]);
    }
}

void PauseSamples() {
    samplesPaused++;
}

void ResumeSamples() {
    if (samplesPaused <= 0)
        return;
    samplesPaused--;
}

S32 GetSamplesPausedCount() {
    return samplesPaused;
}

U32 IsSamplePlaying(U32 sample) {
    LockVoices();
    int i = VoiceIndexFromHandle(sample);
    U32 result = (i >= 0 && !voices[i].dead) ? voices[i].timer : (U32)FALSE;
    UnlockVoices();
    return result;
}

void SfxLogEnable(int enable) {
    sfxLogEnabled = (enable != 0) ? 1 : 0;
    MusicLogEnable(sfxLogEnabled);
}

int SfxLogIsEnabled(void) {
    return sfxLogEnabled;
}

/* ── Per-frame pump (called from ManageTime, like the DC KOS backend) ────── */

void AudioStreamPump(void) {
    if (!Sample_Driver_Enabled)
        return;

    // Reap voices the mix callback finished — free on the game thread only.
    void *bufs[NUM_VOICES];
    int nbufs = 0;
    LockVoices();
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].active && voices[i].dead) {
            bufs[nbufs++] = voices[i].ownedBuf;
            voices[i].ownedBuf = NULL;
            voices[i].data = NULL;
            voices[i].active = false;
            voices[i].dead = false;
        }
    }
    UnlockVoices();
    for (int i = 0; i < nbufs; i++) {
        if (bufs[i])
            free(bufs[i]);
    }

    Stream_Pump();
}

/* ── COMMON + MIDI ───────────────────────────────────────────────────────── */

void InitAIL() {}

S32 InitMidiDriver(char *driver_name) {
    (void)driver_name;
    return TRUE;
}

#ifdef __cplusplus
}
#endif
