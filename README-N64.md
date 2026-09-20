# Little Big Adventure 2 — Nintendo 64 port

A native [libdragon](https://github.com/DragonMinded/libdragon) port of the
**original 1997 Adeline source** of *Twinsen's Odyssey* / *Little Big
Adventure 2*, released by [2.21] in 2024
([upstream `lba2-classic-community`](https://github.com/LBALab/lba2-classic-community)).
Open-source toolchain only — no leaked SDK anywhere in the pipeline.

> **Status: playable, work in progress.** The whole game boots and runs on
> the N64 (Expansion Pak required): interiors, exteriors, weapons, dialogues,
> music, sound effects and English voice-overs. Interiors run at 30–60 fps,
> exteriors at 20–30 fps. Tested on the Ares emulator; real-hardware runs
> via flashcart are welcome. Saving is **not implemented yet** (see below).

This is a sibling of the [Wii U port](https://github.com/vs-sr-dev/wiiu-lba2)
and of the [Dreamcast port](https://github.com/vs-sr-dev/lba2-dreamcast) of
the same codebase, and it starts from the Wii U tree on purpose: the N64's
VR4300 is big-endian like the Wii U's PowerPC, so the long tail of
little-endian data fixes from that port is reused as-is. What the N64 adds
on top is a small platform backend plus a **memory and resolution diet** for
a 93 MHz CPU with 8 MB of RAM and a 64 MB cartridge.

## What works

- **Gameplay.** Twinsen's house through the Citadel island exteriors and
  beyond: scene scripts, zones, interiors and exteriors, vehicles, weapons,
  magic, inventory, holomap, in-game menu.
- **Video.** The engine's software rasterizer draws into a 320×240 8bpp
  buffer that the RDP presents as a CI8 texture with a 256-entry TLUT
  (palette fades are a 512-byte TMEM upload). The game logic still lives in
  its native 640×480 coordinate space; the LIB386 pixel cores downscale
  (see [DEVLOG-N64.md](DEVLOG-N64.md), "Render scale").
- **Audio.** libdragon RSP mixer: 12 SFX voices, music streamed as VADPCM
  `.wav64` (RSP-decoded), English voice-overs re-encoded to Opus at
  12 kHz so they fit in the cartridge.
- **Input.** The N64 pad emulates the PC keyboard bindings; D-pad moves,
  the stick holds the behaviour shortcuts. See [CONTROLS-N64.md](CONTROLS-N64.md).
- **Assets in ROM.** All game data (except FMV) is packed in a DFS image
  inside the 64 MB cartridge: ~28 MB of HQR/ILE/OBL, 16 MB of music,
  18 MB of voices.

## Bring your own assets

**No game data is included in this repository**, and none will ever be.
You need to own *Little Big Adventure 2 / Twinsen's Odyssey* (the GOG or
Steam classic release). Point the build at the game's data directory (the
one with `LBA2.HQR`, `*.ILE`, `*.OBL`, `music/`, `vox/`):

```
export GAMEDATA=/path/to/LBA2/Common    # default: ../../CommonGOG
./build-n64.sh
```

The build converts the assets at staging time (`ffmpeg` + `audioconv64`
inside the container) and packs them into `lba2.z64`. Nothing derived from
retail data is ever committed.

## Building

Requires Docker. The first run builds the `n64lba-toolchain` image from
[`docker/Dockerfile.n64`](docker/Dockerfile.n64): libdragon at a pinned
commit, compiled on top of the official toolchain image **with one patch to
`inthandler.S`** — on the way out of a CPU exception libdragon does not
reload the callee-saved registers s0–s7 from the exception frame, which
silently discards the result of this port's unaligned-access emulator
whenever the faulting load targets one of them (see the devlog, "First
contact with hardware"). If you already have the image from an earlier
checkout, rebuild it: `docker build -t n64lba-toolchain:latest -f
docker/Dockerfile.n64 docker`.

```
./build-n64.sh            # stage assets + build build-n64/lba2.elf + lba2.z64
./build-n64.sh clean      # wipe the build directory
./build-n64.sh shell      # interactive shell inside the container
```

The output is `lba2.z64` (~62 MB with English voices). The ROM header
declares 256 Kbit SRAM (saves and settings live there, see below) and the
game needs the **Expansion Pak** (8 MB): the engine's static footprint alone
is ~2.2 MB and the HQR caches, framebuffers and Z-buffer take another 5 MB.

## Running

- **Emulator:** [Ares](https://ares-emu.net/) (needs a Vulkan GPU). It prints
  the ROM's ISViewer debug output to stdout; `tools-n64/run-ares.ps1`
  launches it with that output redirected to `ares_log.txt`, which is the
  port's main diagnostic channel (asserts come with a symbolic backtrace).
- **Hardware:** any flashcart that supports 64 MB ROMs and SRAM
  (EverDrive-64, SummerCart64, …) with an Expansion Pak fitted.
- **Saves and settings** are kept in the cartridge's 32 KB SRAM, presented
  to the engine as a tiny `sram:/` filesystem (`LIB386/SYSTEM/N64_SRAMFS.CPP`).
  A save is ~4 KB (LZSS, 80×60 thumbnail), so about six slots plus the
  "resume game" file fit; when the memory is full the save menu says so and
  a slot has to be deleted (load menu). `lba2.cfg` lives there too, so
  language, volumes and options persist. Emulators keep the image next to the
  ROM (`lba2.ram` in Ares); flashcarts write it back to the SD card on
  reset/power-off as usual. N64 saves are not byte-compatible with PC ones.
- **Debug start:** `DebugStartCube: N` in `LBA2.CFG` skips the menus and
  starts a new game in cube N (the debug console's `cube` command is not
  available on N64). Scene numbers are in [docs/SCENES.md](docs/SCENES.md).
  Developer keys go in `build-n64/lba2.cfg.local` (appended to the staged
  default cfg); since the cfg is copied into SRAM on first boot only, delete
  the emulator's `lba2.ram` to pick up a changed default. `DebugSaveTest: 1`
  (or `2` to fill the SRAM) runs an input-free self-test of the save path
  once the first cube is up and logs the result (`[sramtest]`).
  `DebugFullRedraw: 1` redraws the whole exterior every frame (the cost of a
  camera jump, measurable without input; expect ~1 fps), `TerrainLod: 1`
  draws the horizon cubes flat instead of textured (an experiment: no
  measurable gain, see the devlog); `[terrprof]` splits the terrain time.
- **CRT safe area:** the picture is scaled by the VI into a window 6 % smaller
  on each side, so consumer CRTs (which overscan by 4–8 %) show the whole
  dialogue text; emulators and upscalers see a thin black border instead.
  `ScreenMargin: N` in `LBA2.CFG` (0–15, percent per side) overrides it — the
  built-in default is `N64_DEFAULT_SCREEN_MARGIN` in
  `LIB386/SYSTEM/N64_BACKEND.CPP`.

## Known limitations / WIP

- **Save slots.** 32 KB of SRAM hold about six saves plus the resume file; the
  game tells you when it is full. (768 Kbit SRAM would triple that but is
  not supported by every flashcart.)
- **Exterior stalls.** Every camera jump outdoors (re-centring on Twinsen,
  entering an area) is a full redraw of the terrain, the 8 horizon cubes and
  their ~100 decor objects: ~0.85 s in Ares (less on hardware). Normal
  frames only redraw the objects, which is why the frame rate between jumps
  is fine. Earlier versions stalled for 6–7 s because the HQR caches were
  too small to hold the 9 cubes and re-streamed them from ROM every frame;
  that part is fixed. The remaining cost is per-triangle setup and cache
  misses in the software fillers (the current cube alone is ~2400 tiny
  triangles at ~100 µs each), not texturing — see the devlog.
- **Frame rate.** Exteriors are 20–30 fps; the rasterizer is fill-bound and
  the terrain is still drawn every full-refresh frame.
- **Audio hitches** follow the frame rate: the mixer is pumped once per
  frame, so a 1 s frame is a 1 s dropout.
- **No FMV.** The Smacker movies are skipped (no room in the cartridge and no
  decoder budget on the CPU).
- **Voices are English only** and slightly "telephone" quality (Opus at
  12 kHz — the only way ~130 minutes of speech fit in 18 MB).
- **Scene-change heisenbug.** Very rarely, coming back from the cellar into
  Twinsen's house teleports him onto a wall (a zone `Info3` read as 2048).
  A checksum net is in place to catch it; see the devlog.

## Repository layout (N64 additions)

| Path | Role |
|---|---|
| `Makefile.n64`, `build-n64.sh` | libdragon build + asset staging (Docker) |
| `docker/Dockerfile.n64` | toolchain image |
| `LIB386/SYSTEM/N64_BACKEND.CPP` | main, timers, logging, keyboard/dirent shims, unaligned-access emulator, frame profiler |
| `LIB386/SVGA/N64.CPP` | video surface + RDP CI8/TLUT present |
| `LIB386/AIL/N64/SOUND_N64_BACKEND.CPP` | RSP mixer backend (SFX, music, voices) |
| `SOURCES/JOYSTICK_N64.CPP` | pad → keyboard-scancode mapping |
| `LIB386/H/SVGA/SCREEN.H` (`RS_*`) | virtual 640×480 → physical 320×240 render scale |
| `tools-n64/` | VOX repacker, unaligned-access rewrite helper, Ares launcher |

## Credits

- **Adeline Software International** — the original game and engine (1997).
- **[2.21] / LBALab** — the official source release and the
  `lba2-classic-community` project this port is based on.
- **DragonMinded and the libdragon contributors** — the open-source N64 SDK
  this port is built on.
- The Wii U and Dreamcast ports of the same source, whose platform-backend
  approach and big-endian fixes made this one possible.
