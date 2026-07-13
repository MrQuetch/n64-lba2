# Little Big Adventure 2 — Wii U port

A native [devkitPro/WUT](https://devkitpro.org/) port of the **original 1997
Adeline source** of *Twinsen's Odyssey* / *Little Big Adventure 2*, released
by [2.21] in 2024 ([upstream `lba2-classic-community`](https://github.com/LBALab/lba2-classic-community)).

> **Status: V1.0 — the game has been completed start to finish on real
> Wii U hardware**: full story, both planets, all vehicles, all cutscenes and
> FMVs, through the ending credits. Runs at 50–80 fps on the console. Cemu is
> used as the day-to-day development mirror and behaves at parity.

This is a sibling of the [Dreamcast port](https://github.com/vs-sr-dev/lba2-dreamcast)
of the same codebase, and shares its philosophy: no engine rewrite, no
external framework — the original DOS/Win95 engine compiled for the target,
with a platform backend and (this being a big-endian PowerPC console) a long
tail of little-endian data fixes.

## What works (validated on real hardware)

- **Complete playthrough.** Story progression from the intro to the ending
  credits, including Zeelich, the Emerald Moon, the Undergas mines, wagon and
  buggy sequences, holomap, and every interior/exterior scene visited along
  the way.
- **Video.** Full-screen 960×720 blit, 50–80 fps after a PowerPC
  cache-instruction (`dcbz`) optimization of the frame upload path.
- **FMV.** All Smacker movies stream from disk via libsmacker
  `SMK_MODE_DISK` (the 76 MB intro never touches RAM) with AX audio in sync.
- **Audio.** Native AX backend with software mixing: music (OGG via
  `stb_vorbis`), English voice-overs, positional SFX, menu fades.
- **Saves.** Save/load anywhere, custom save names via an on-screen
  keyboard, save directory auto-created on first run.
- **Input.** Full GamePad support — left stick + buttons, with an in-game
  remap menu, plus a **touch panel** on the GamePad screen for behaviours,
  weapons and spells (see [CONTROLS-WIIU.md](CONTROLS-WIIU.md)).
- **Languages.** All six retail text languages, voice language selectable
  independently.

## Bring your own assets

**No game data is included in this repository**, and none will ever be.
You need to own *Little Big Adventure 2 / Twinsen's Odyssey* (the GOG or
Steam classic release). Copy the game's data files to your SD card:

```
sd:/wiiu/apps/lba2/
├── lba2.wuhb
└── data/
    ├── *.HQR, *.ILE, *.OBL   (all top-level data files)
    ├── music/
    ├── video/
    └── vox/
```

Saves are written to `sd:/wiiu/apps/lba2/saves/`.

## Building

Requires Docker (the build runs inside the official `devkitpro/devkitppc`
image — no local toolchain setup):

```
./build-wiiu.sh            # configure + build + package lba2.wuhb
./build-wiiu.sh clean      # wipe the build directory
```

The output is `build-wiiu/lba2.wuhb`. If a `CommonGOG` directory with retail
data is present two levels up, the core `.HQR` files (~28 MB, no
video/music/voices) are embedded in the `.wuhb` as a fallback romfs so the
engine can boot in Cemu without an SD card layout; this is a local
convenience only and nothing from it is committed.

## Running

`lba2.wuhb` is a standard Wii U homebrew bundle: launch it from any homebrew
environment on the console (confirmed working on real hardware), or with
[Cemu](https://cemu.info/) on PC (point it at the `.wuhb`, and map a
controller as a Wii U GamePad).

## Known limitations / WIP

Nothing blocking — parity with the original is at ~99.9% across a full
playthrough. Remaining items:

- **Controls polish.** The default GamePad mapping works but was tuned "by
  feel" and needs a usability pass (the dodge binding above all).
- **Touch panel polish.** The GamePad touch layout is functional but could
  use some visual sprucing; labels are English-only for now (sourcing them
  from the game's own per-language text banks is planned).
- **Music-change stutter.** A very slight hitch when a new music track
  starts loading.
- **Fullscreen-art transition.** Loading a fullscreen picture briefly shows
  the previous scene "emptied" before the art appears.
- Auto-camera integration for the right stick is minimal (camera level
  up/down only).
- Quit returns to the system menu on hardware; under Cemu it simply ends
  emulation (Cemu has no system menu to return to).

## Credits

- **Adeline Software International** — the original game and engine (1997).
- **[2.21] / LBALab** — the official source release and the
  `lba2-classic-community` project this port is based on.
- The Dreamcast port of the same source, which pioneered the
  platform-backend approach reused here.
