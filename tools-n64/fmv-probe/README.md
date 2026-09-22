# FMV probe

Can the Nintendo 64 play LBA2's cutscenes? This directory answers that with
measurements instead of guesses. It re-encodes the retail Smacker movies for
the console and builds a ROM that plays them while reporting, per clip, how
many frames were actually displayed and how much wall-clock time that took.

It is a probe, not a feature: nothing here is part of `build-n64.sh`, and the
game still ships with FMV disabled. See "Full-motion video" in
[DEVLOG-N64.md](../../DEVLOG-N64.md) for what the numbers mean for the port.

## Running it

```sh
cd tools-n64/fmv-probe
VIDEOHQR=/path/to/GOG/Common/VIDEO/VIDEO.HQR ./fmv-probe.sh      # INTRO
./fmv-probe.sh list                                              # what's in the archive
```

The script re-executes itself inside a Docker image built from
`Dockerfile.preview`, so the only host requirements are Docker and a retail
install. `VIDEO.HQR` lives in `Common/VIDEO/` of the GOG release — 223 MB that
a bring-your-own-assets copy usually does not carry, hence the separate
variable; `RESS.HQR` comes from `$GAMEDATA` as usual (its entry 48 is the movie
name list the engine indexes into).

Then run `fmvprobe.z64` and read the `[fmvprobe]` lines on the ISViewer channel
(`tools-n64/run-ares.ps1` captures them to `ares_log.txt`):

```
[fmvprobe] intro_h26455 t=23.9s pres=360 src=360 skip=0 (0.0%) wall=23.9s rt=1.00x scr=15.1fps
```

`skip` is the number of frames the player dropped to stay in sync with the
audio, and `rt` is video seconds per wall-clock second. A clip that runs at
1.00x with no drops is one the machine can afford; anything else is the
console telling you the decoder is too expensive.

`CLIPS` picks the encodings (`codec:quality[:fps]`, default
`h264:55 h264:80 mpeg1:55:24`). MPEG-1 only allows the standard frame rate
codes, so the 15 fps source has to be forced to 24 or 25 — H.264 takes it as
it is, which is both cheaper to decode and truer to the original timing.
`VOICE` chooses the language track mixed under the music (Smacker track 0 is
the music, 1/2/3 are FR/DE/EN), matching what `SOURCES/PLAYACF.CPP` does.

## What it measured (2026-09-22, Ares)

The intro — 3:53, 320x200, 15 fps, 72.8 MB of Smacker — encoded at 320x192 and
played from the cartridge:

| encoding | video size | result |
|---|---|---|
| H.264, quality 55 | 3.72 MiB | 377/377 frames, 1.00x realtime, 15.1 fps |
| H.264, quality 80 | 9.05 MiB | 377/377 frames, 1.00x realtime, 15.1 fps |
| MPEG-1, quality 55 @24fps | 14.2 MiB | 600/601 frames, 1.00x realtime, 24.1 fps |

Audio adds 3.66 MiB per copy of the intro as VADPCM (what the table above was
measured with), or 0.95 MiB as Opus — untested here, and worth testing before
being believed: Opus decodes on the VR4300, which is exactly why the game's
music was moved back to VADPCM (see `build-n64.sh`).

All 34 movies are 14.0 minutes and 223 MB of Smacker in total; at the H.264
quality that already looked flawless, that scales to roughly 17-18 MiB — a
figure to re-measure rather than trust, since bitrate follows content.

Two things this does not prove. Ares renders with paraLLEl-RDP, where the YUV
blit costs far less than on real hardware, so the verdict belongs to a console
test. And the video module lives in libdragon's `preview` branch: adopting it
means moving the port off the trunk commit pinned in `docker/Dockerfile.n64`,
carrying the `inthandler.S` patch along.
