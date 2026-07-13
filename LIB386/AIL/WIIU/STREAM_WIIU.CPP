// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/WIIU/STREAM_WIIU.CPP
// STREAM + CD contracts for the Wii U AX backend.
//
// Model (mirrors the DC KOS backend's pump design): Stream_Pump() runs on
// the game thread from AudioStreamPump()/ManageTime and decodes the active
// source (OGG via stb_vorbis, or PCM WAV) into a ~1 s FIFO, linear-resampled
// to the 32 kHz AX output rate. The AX app-frame callback in
// SOUND_WIIU_BACKEND.CPP drains the FIFO via Stream_MixInto(). A 1 s FIFO
// rides out frame stalls (scene loads) without audio dropouts.
//
// Path resolution (.WAV name → .ogg / track<N>.ogg Steam-GOG fallback) is
// derived from AIL/SDL/STREAM.CPP. CD tracks route through the same stream
// (the retail CD-audio never existed on this distribution), probing the SD
// data dir first and the .wuhb romfs as fallback — same order as the
// engine's own base-path discovery in wiiu_sdl_compat.h.

#include "AIL.H"

#include <SYSTEM/LOGPRINT.H>

extern "C" {
#include "stb_vorbis.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ADELINE_MAX_PATH
#define ADELINE_MAX_PATH 512
#endif

#define OUTPUT_RATE 32000

// FIFO: power of two frames, ~1 s at 32 kHz.
#define FIFO_FRAMES 32768
#define FIFO_MASK (FIFO_FRAMES - 1)

// Decode chunk (source frames per stb_vorbis/PCM pull).
#define DEC_CHUNK 2048

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stream state ────────────────────────────────────────────────────────── */

enum SourceKind { SRC_NONE = 0, SRC_VORBIS, SRC_PCM };

static SourceKind srcKind = SRC_NONE;
static stb_vorbis *srcVorbis = NULL;
static U8 *srcFileBuf = NULL;   // slurped file (vorbis) or WAV payload owner
static const U8 *srcPcm = NULL; // PCM walk cursor base (into srcFileBuf)
static S32 srcPcmFrames = 0;
static S32 srcPcmPos = 0; // frames consumed
static int srcPcmIs8bit = 0;
static int srcRate = OUTPUT_RATE;
static int srcChannels = 2;
static int srcEof = 0;

static volatile int streamActive = 0;
static volatile int streamPaused = 0;
static char streamPathName[ADELINE_MAX_PATH] = "";
S32 streamVolume = 0;
static int musicLogEnabled = 0;

// FIFO (single producer = pump, single consumer = AX callback).
static S16 fifoL[FIFO_FRAMES];
static S16 fifoR[FIFO_FRAMES];
static volatile U32 fifoWr = 0;
static volatile U32 fifoRd = 0;

// Resampler window.
static S16 winL[2], winR[2];
static float rsPos = 0.0f;
static int rsPrimed = 0;

// Decoded source chunk.
static short decBuf[DEC_CHUNK * 2];
static int decCount = 0; // frames available
static int decPos = 0;   // frames consumed

/* ── Source pulls ────────────────────────────────────────────────────────── */

static void RefillDecBuf(void) {
    decPos = 0;
    decCount = 0;
    if (srcKind == SRC_VORBIS && srcVorbis) {
        int n = stb_vorbis_get_samples_short_interleaved(
            srcVorbis, srcChannels, decBuf, DEC_CHUNK * srcChannels);
        if (n > 0)
            decCount = n;
        else
            srcEof = 1;
    } else if (srcKind == SRC_PCM && srcPcm) {
        int remain = srcPcmFrames - srcPcmPos;
        int n = (remain > DEC_CHUNK) ? DEC_CHUNK : remain;
        if (n <= 0) {
            srcEof = 1;
            return;
        }
        for (int i = 0; i < n; i++) {
            int f = srcPcmPos + i;
            if (srcPcmIs8bit) {
                for (int c = 0; c < srcChannels; c++)
                    decBuf[i * srcChannels + c] =
                        (short)(((int)srcPcm[f * srcChannels + c] - 128) << 8);
            } else {
                const U8 *p = srcPcm + (size_t)(f * srcChannels) * 2;
                for (int c = 0; c < srcChannels; c++)
                    decBuf[i * srcChannels + c] = (short)(p[c * 2] | (p[c * 2 + 1] << 8));
            }
        }
        srcPcmPos += n;
        decCount = n;
    } else {
        srcEof = 1;
    }
}

static int GetSrcFrame(S16 *l, S16 *r) {
    if (decPos >= decCount) {
        if (srcEof)
            return 0;
        RefillDecBuf();
        if (decCount == 0)
            return 0;
    }
    if (srcChannels >= 2) {
        *l = decBuf[decPos * srcChannels];
        *r = decBuf[decPos * srcChannels + 1];
    } else {
        *l = *r = decBuf[decPos];
    }
    decPos++;
    return 1;
}

/* ── Pump: decode + resample into the FIFO (game thread) ─────────────────── */

void Stream_Pump(void) {
    if (!streamActive || srcKind == SRC_NONE)
        return;

    float ratio = (float)srcRate / (float)OUTPUT_RATE;

    U32 wr = fifoWr;
    while (((wr - fifoRd) & 0xFFFFFFFFu) < FIFO_FRAMES - 1) {
        if (!rsPrimed) {
            if (!GetSrcFrame(&winL[0], &winR[0]))
                break;
            if (!GetSrcFrame(&winL[1], &winR[1])) {
                winL[1] = winL[0];
                winR[1] = winR[0];
                srcEof = 1;
            }
            rsPrimed = 1;
            rsPos = 0.0f;
        }
        int starved = 0;
        while (rsPos >= 1.0f) {
            winL[0] = winL[1];
            winR[0] = winR[1];
            if (!GetSrcFrame(&winL[1], &winR[1])) {
                starved = 1;
                break;
            }
            rsPos -= 1.0f;
        }
        if (starved)
            break;
        float fl = (float)winL[0] + rsPos * (float)(winL[1] - winL[0]);
        float fr = (float)winR[0] + rsPos * (float)(winR[1] - winR[0]);
        fifoL[wr & FIFO_MASK] = (S16)fl;
        fifoR[wr & FIFO_MASK] = (S16)fr;
        wr++;
        rsPos += ratio;
    }
    fifoWr = wr;
}

/* ── Drain: called from the AX frame callback ────────────────────────────── */

void Stream_MixInto(float *accL, float *accR, int frames) {
    if (!streamActive || streamPaused)
        return;
    float gain = (streamVolume <= 0) ? 0.0f : (float)streamVolume / 127.0f;
    if (gain > 1.0f)
        gain = 1.0f;
    U32 rd = fifoRd;
    U32 avail = (fifoWr - rd) & 0xFFFFFFFFu;
    U32 n = ((U32)frames < avail) ? (U32)frames : avail;
    for (U32 i = 0; i < n; i++) {
        accL[i] += (float)fifoL[rd & FIFO_MASK] * gain;
        accR[i] += (float)fifoR[rd & FIFO_MASK] * gain;
        rd++;
    }
    fifoRd = rd;
}

/* ── Teardown ────────────────────────────────────────────────────────────── */

static void StopStreamInternal(void) {
    streamActive = 0;
    streamPaused = 0;
    fifoRd = fifoWr; // callback sees an empty FIFO immediately
    if (srcVorbis) {
        stb_vorbis_close(srcVorbis);
        srcVorbis = NULL;
    }
    if (srcFileBuf) {
        free(srcFileBuf);
        srcFileBuf = NULL;
    }
    srcPcm = NULL;
    srcKind = SRC_NONE;
    srcEof = 0;
    rsPrimed = 0;
    decCount = decPos = 0;
    streamPathName[0] = '\0';
}

void OpenStream() {
    StopStreamInternal();
    if (musicLogEnabled)
        LogPuts("[MUSIC] OpenStream init");
}

/* ── Path resolution (derived from AIL/SDL/STREAM.CPP) ───────────────────── */

static void BuildOggPath(const char *wavPath, char *oggPath, int pathMax,
                         const char *ext, int lowerName) {
    strncpy(oggPath, wavPath, pathMax - 1);
    oggPath[pathMax - 1] = '\0';

    if (lowerName) {
        char *sep = strrchr(oggPath, '/');
        char *start = sep ? sep + 1 : oggPath;
        for (char *p = start; *p; p++) {
            if (*p >= 'A' && *p <= 'Z')
                *p += 32;
        }
    }

    char *dot = strrchr(oggPath, '.');
    if (dot) {
        strcpy(dot, ext);
    } else {
        int len = (int)strlen(oggPath);
        if (len + 5 <= pathMax)
            strcpy(oggPath + len, ext);
    }
}

static int FileExistsCheck(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static int ResolveOggPath(const char *wavPath, char *resolvedPath, int pathMax) {
    static const char *exts[] = {".ogg", ".OGG"};
    for (int lower = 0; lower <= 1; lower++) {
        for (int e = 0; e < 2; e++) {
            BuildOggPath(wavPath, resolvedPath, pathMax, exts[e], lower);
            if (FileExistsCheck(resolvedPath))
                return 1;
        }
    }
    /* Steam/GOG fallback: TADPCM<N> / Track<N> files renamed to track<N>.ogg */
    const char *sep = strrchr(wavPath, '/');
    const char *base = sep ? sep + 1 : wavPath;
    const char *dot = strrchr(base, '.');
    if (!dot)
        dot = base + strlen(base);
    const char *p = dot;
    while (p > base && p[-1] >= '0' && p[-1] <= '9')
        p--;
    if (p < dot) {
        int n = atoi(p);
        if (n > 0 && n < 100) {
            int dirLen = (int)(base - wavPath);
            snprintf(resolvedPath, pathMax, "%.*strack%d.ogg", dirLen, wavPath, n);
            if (FileExistsCheck(resolvedPath))
                return 1;
            snprintf(resolvedPath, pathMax, "%.*sTrack%d.ogg", dirLen, wavPath, n);
            if (FileExistsCheck(resolvedPath))
                return 1;
        }
    }
    resolvedPath[0] = '\0';
    return 0;
}

/* ── WAV loading (PCM 8/16-bit; byte-composed LE reads, endian-safe) ─────── */

static U8 *SlurpFile(const char *path, S32 *outSize) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    U8 *buf = (U8 *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *outSize = (S32)sz;
    return buf;
}

static int TryStartWav(const char *path) {
    S32 size = 0;
    U8 *buf = SlurpFile(path, &size);
    if (!buf)
        return 0;
    if (size < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return 0;
    }
    S32 pos = 12;
    int rate = 0, channels = 0, bits = 0, fmtTag = 0;
    const U8 *data = NULL;
    S32 dataSize = 0;
    while (pos + 8 <= size) {
        S32 chunkSize = (S32)(buf[pos + 4] | (buf[pos + 5] << 8) |
                              (buf[pos + 6] << 16) | (buf[pos + 7] << 24));
        if (memcmp(buf + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            const U8 *f = buf + pos + 8;
            fmtTag = f[0] | (f[1] << 8);
            channels = f[2] | (f[3] << 8);
            rate = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
            bits = f[14] | (f[15] << 8);
        }
        if (memcmp(buf + pos, "data", 4) == 0 && chunkSize > 0 && pos + 8 + chunkSize <= size) {
            data = buf + pos + 8;
            dataSize = chunkSize;
        }
        pos += 8 + ((chunkSize + 1) & ~1);
    }
    if (!data || fmtTag != 1 || (bits != 8 && bits != 16) ||
        channels < 1 || channels > 2 || rate <= 0 || rate > 192000) {
        free(buf);
        return 0;
    }
    srcFileBuf = buf;
    srcPcm = data;
    srcPcmIs8bit = (bits == 8);
    srcChannels = channels;
    srcRate = rate;
    srcPcmFrames = dataSize / (channels * (bits / 8));
    srcPcmPos = 0;
    srcKind = SRC_PCM;
    return 1;
}

static int TryStartOgg(const char *path) {
    S32 size = 0;
    // Slurp the whole file so decode never touches the SD mid-game (the DC
    // port learned this the hard way with GD-ROM head contention; on SD it
    // also keeps music decode from fighting HQR streaming for I/O).
    U8 *buf = SlurpFile(path, &size);
    if (!buf)
        return 0;
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(buf, size, &err, NULL);
    if (!v) {
        free(buf);
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    if (info.channels <= 0 || info.sample_rate <= 0) {
        stb_vorbis_close(v);
        free(buf);
        return 0;
    }
    srcFileBuf = buf;
    srcVorbis = v;
    srcChannels = (info.channels >= 2) ? 2 : 1;
    srcRate = (int)info.sample_rate;
    srcKind = SRC_VORBIS;
    return 1;
}

/* ── Public STREAM API ───────────────────────────────────────────────────── */

void PlayStream(char *name) {
    if (!name || !name[0])
        return;

    StopStreamInternal();

    int started = 0;
    if (TryStartWav(name)) {
        started = 1;
    } else {
        char resolved[ADELINE_MAX_PATH];
        if (ResolveOggPath(name, resolved, (int)sizeof(resolved)) && TryStartOgg(resolved)) {
            started = 1;
            if (musicLogEnabled)
                LogPrintf("[MUSIC] PlayStream OGG: %s (from %s)\n", resolved, name);
        }
    }

    if (!started) {
        LogPrintf("Wii U AIL: could not find '%s' or OGG fallbacks\n", name);
        return;
    }

    srcEof = 0;
    rsPrimed = 0;
    decCount = decPos = 0;
    fifoRd = fifoWr; // start empty

    strncpy(streamPathName, name, ADELINE_MAX_PATH - 1);
    streamPathName[ADELINE_MAX_PATH - 1] = '\0';
    streamActive = 1;

    Stream_Pump(); // prime the FIFO so playback starts this audio frame
}

void ChangeVolumeStream(S32 volume) {
    streamVolume = volume;
}

S32 GetVolumeStream() {
    return streamVolume;
}

void StopStream() {
    if (musicLogEnabled)
        LogPuts("[MUSIC] StopStream");
    StopStreamInternal();
}

void PauseStream() {
    if (streamActive)
        streamPaused = 1;
}

void ResumeStream() {
    streamPaused = 0;
}

S32 IsStreamPlaying() {
    if (!streamActive)
        return FALSE;
    if (streamPaused)
        return FALSE;
    if (srcEof && fifoRd == fifoWr) {
        StopStreamInternal(); // fully drained (or nothing ever decoded)
        return FALSE;
    }
    return TRUE;
}

char *StreamName() {
    // GetMusic() decides "music still playing" from this name alone: on the
    // jingle path IsCDPlaying() short-circuits (cdPlaying=0) so the lazy
    // EOF collection in IsStreamPlaying() never runs, and a name that
    // outlives the stream wedges the whole music queue — every later
    // PlayMusic sees cur!=0 and re-queues forever (no outdoor music after
    // the first indoor jingle). Collect the drained stream here too.
    if (streamActive && !streamPaused && srcEof && fifoRd == fifoWr)
        StopStreamInternal();
    return streamPathName;
}

void MusicLogEnable(int enable) {
    musicLogEnabled = (enable != 0) ? 1 : 0;
}

int MusicLogIsEnabled(void) {
    return musicLogEnabled;
}

/* ── CD contract: tracks route through the stream (no CD audio exists on
      the GOG distribution; music files live in <data>/music/) ───────────── */

#define FIRST_CD_TRACK_EU 6
#define FIRST_CD_TRACK_US 2

static S32 cdVolume = 0;
static int cdPlaying = 0;

static int BuildCDTrackPath(char *outPath, S32 pathMax, S32 trackOneBased) {
    // Same base-path order as the engine's data discovery: physical SD
    // first, .wuhb romfs fallback.
    static const char *bases[] = {
        "/vol/external01/wiiu/apps/lba2/data/",
        "/vol/content/data/",
    };
    static const char *musicDirs[] = {"music", "Music", "MUSIC"};
    char wavPath[ADELINE_MAX_PATH];
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 3; i++) {
            snprintf(wavPath, sizeof(wavPath), "%s%s/Track%02d.wav",
                     bases[b], musicDirs[i], (int)trackOneBased);
            if (FileExistsCheck(wavPath)) {
                strncpy(outPath, wavPath, pathMax - 1);
                outPath[pathMax - 1] = '\0';
                return 1;
            }
            if (ResolveOggPath(wavPath, outPath, pathMax))
                return 1;
        }
    }
    outPath[0] = '\0';
    return 0;
}

const char *OpenCD(char *volume_name) {
    (void)volume_name;
    cdVolume = 127;
    cdPlaying = 0;
    return "D";
}

void PlayCD(S32 track) {
    char path[ADELINE_MAX_PATH];
    if (BuildCDTrackPath(path, sizeof(path), FIRST_CD_TRACK_EU + track) ||
        BuildCDTrackPath(path, sizeof(path), FIRST_CD_TRACK_US + track)) {
        LogPrintf("[CD] PlayCD track=%d path=%s\n", (int)track, path);
        PlayStream(path);
        ChangeVolumeStream(cdVolume);
        cdPlaying = 1;
    } else {
        // Silent-failure trap: without this line a missing/mis-named track
        // file is indistinguishable from the call never happening.
        LogPrintf("[CD] PlayCD track=%d: no Track%02d/%02d file found (wav/ogg)\n",
                  (int)track, (int)(FIRST_CD_TRACK_EU + track),
                  (int)(FIRST_CD_TRACK_US + track));
    }
}

void ChangeVolumeCD(S32 volume) {
    cdVolume = volume;
    if (cdPlaying)
        ChangeVolumeStream(cdVolume);
}

S32 GetVolumeCD() {
    return cdVolume;
}

void StopCD() {
    StopStream();
    cdPlaying = 0;
}

void PauseCD() {
    PauseStream();
}

void ResumeCD() {
    ResumeStream();
}

S32 IsCDPlaying() {
    return (cdPlaying && IsStreamPlaying()) ? TRUE : FALSE;
}

#ifdef __cplusplus
}
#endif
