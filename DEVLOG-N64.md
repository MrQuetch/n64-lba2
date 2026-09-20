# Nintendo 64 port — development log (condensed)

A chronological summary of how the port came together. The recurring theme
is **budget**: a 93 MHz MIPS with an 8 KB data cache, 8 MB of RAM (with the
Expansion Pak), a 64 MB cartridge, and an engine that was designed to
rasterize 640×480 frames in software on a Pentium 90.

## Why start from the Wii U tree

The N64's VR4300 is big-endian, like the Wii U's PowerPC. The Wii U port had
already fought the whole little-endian campaign (scene loader, bodies,
bricks, islands, sort keys, text banks, saves…), so its tree was cloned and
built with `-DLBA2_TARGET_WIIU -DLBA2_TARGET_N64`: the first define keeps
every endianness fix, the second carves out the few WUT-only paths (heap,
FMV, SDL compatibility shims). The engine is fixed-point almost everywhere
(floats only in joystick and timing code), all x86 assembly had already been
ported to C, and the static footprint is ~2.2 MB — feasible, with the
Expansion Pak.

## Boot and first light

- Docker image with libdragon trunk on top of the official toolchain image;
  `Makefile.n64` mirrors the CMake source lists; `build-n64.sh` generates
  what CMake used to (version header, embedded `LBA2.CFG`) and stages the
  DFS.
- Backends: `N64_BACKEND.CPP` (main, timer, `LogPrintf` → ISViewer, keyboard
  and `dirent` shims over libdragon's `dir_findfirst`), `SVGA/N64.CPP`
  (8bpp `Phys` presented by the RDP as CI8 + TLUT), `JOYSTICK_N64.CPP`.
- Assets: the DFS is case-sensitive and the engine composes lowercase
  names, so everything is staged lowercase at the DFS root; `lba2.cfg` is
  pre-staged because the DFS is read-only.
- `stat()` is unreliable on `rom:/`: existence checks fall back to
  `fopen` / `dir_findfirst`, and only a return of 0 counts as "exists".
- **Unaligned accesses.** `GET_S16`/`GET_U32` and dozens of
  `LBA2_LE16(*(U16*)p)` sites cast unaligned pointers — a trap on MIPS.
  The 32 obvious sites were rewritten to `memcpy` helpers
  (`tools-n64/fix_unaligned_swaps.sh`); the long tail is handled by an
  **unaligned-access emulator in the exception handler** (lh/lhu/lw/lwu/ld/
  sh/sw/sd, full branch-delay-slot handling, sign-extended 64-bit GPRs, a
  BadVAddr/EPC sanity check so a wild pointer reaches the inspector instead
  of double-faulting silently).
- Memory diet: `AvailableMem()` reports 1 MB so the HQR pools clamp to
  their minima; display at 320×240 (a 640×480 16bpp double buffer alone is
  1.2 MB).
- First light: Twinsen's house at 58 fps, dialogues and scripts working.

## Audio

- `AIL/N64/SOUND_N64_BACKEND.CPP` on the libdragon RSP mixer: 12 SFX voices
  (VOC/WAV parsers from the Wii U backend), music channel, voice channel.
- Music: GOG `.ogg` → `.wav64` at staging time. Opus decodes on the CPU and
  was suspected of the frame drops; switching to **VADPCM** (RSP-decoded)
  did not change them — profiling showed the audio pump costs <1 ms/frame
  and the stutter is a *symptom* of the variable frame rate (the mixer is
  fed once per frame). Music is mono 16 kHz VADPCM to make room for voices.
- Two libdragon lessons: `mixer_ch_play` only reconfigures a channel when
  the `waveform_t` *pointer* changes (reusing one descriptor per slot made
  long samples inherit a footstep's length — fixed by alternating two
  descriptors per slot); and an 8-bit waveform played with pitch can make
  the mixer request odd-length chunks, tripping the RSP DMA alignment
  assert — samples are now stored as signed 16-bit.
- Voices: the VOX banks are 83 MB of IMA-ADPCM per language. English only,
  clips re-encoded to Opus at 12 kHz (~18 MB), the `.VOX` HQR replaced by
  placeholders that carry the clip name; the engine's `SPEAK_SAMPLE`
  handle is routed to a dedicated streaming channel.
- Jingle names `TADPCMn` are the CD tracks (`trackn.ogg` on GOG); mapping
  them fixed both the silent main-menu theme and the silent Citadel
  exteriors. `ResumeMusic` no longer stops the stream before resuming it
  (on PC "CD" and "jingle" were two players; here they are one).
- Two playtester reports. *Rain kept falling after the storm*: the engine
  addresses voices either by the handle `PlaySample` returned or by the
  bare sample number (`IsSamplePlaying(SAMPLE_RAIN)`, `StopOneSample(
  SAMPLE_RAIN)`); the N64 lookup only resolved full handles, so every
  number-based query was a no-op — the rain loop could not be stopped, the
  `SAMPLE_TIME_REPEAT` throttle never fired, `SampleAlways` loops stacked.
  Handles now use the MILES layout (`counter<<24 | user<<8 | slot`) and
  numbers resolve by user handle. *Lines cut off mid-sentence*: 20 long
  lines are split across `FlagNextVoc` chains whose continuation clips sit
  physically after the first part with **no index slot**; `vox_repack.py`
  iterated slots and dropped them, so the engine chained into whatever
  entry came next. The repack now follows the chain (1261 clips, up to 5
  parts per line).

## Input

The pad **emulates the PC keyboard**: every control injects the scancode of
the default keyboard binding into `TabKeys`, which is the only path that
reaches the spell shortcuts polled directly in `PERSO.CPP`. The stick is
decoupled from movement (D-pad) and carries the behaviour shortcuts. The
config file's `GamepadDeadzone=8000` killed the stick (raw range ±85): a
local deadzone of 24 is used instead.

## Render scale

Profiling (`[renderprof]`/`[affprof]` in the log, per 60 frames) showed the
frame is **fill-bound**: 75–80% of `AffScene` is pixel writes at 640×480.

A first proof of concept halved the *projection* (iso and perspective
scales, sphere radii, brick coordinates) so the world rendered at 320×240:
interiors went from 30 to 52–60 fps and proved the approach. But the UI,
fonts, menus, sprites and every one of ~700 2D call sites still spoke
640×480, and the clip rectangle, `ScreenXMin..`, `Xp/Yp` are global state
read by both worlds — unfixable site by site.

The final architecture keeps the **whole game logic in a virtual 640×480
space** (`ModeDesiredX/Y`, clip, dirty boxes, projection, all call sites
untouched) while `Log`/`Screen`/`Phys` are **physical 320×240** buffers, and
**only the pixel-writing cores in LIB386 convert** right before touching
memory (`RS_V2P()`, `RS_PushPhysClip()` in `SVGA/SCREEN.H`): `Fill_Poly`
(the single polygon entry point — a private copy of the point list with
halved screen coordinates), `Fill_Sphere`, lines, `AffGraph` (a half-scale
RLE decoder), fonts (`AffMask`, 2×2 OR so thin strokes survive), boxes,
block copies, sprite scalers, the dirty-box lists, shade/flow/rain/z-buffer
overwrite helpers, and full-screen images (downscaled in place after
`Load_HQR`). The brick pipeline is the one part that works natively in
physical space: `Map2Screen` is halved and the brick bank is pre-downscaled
once at load (`HalveBrickBank`, an RLE re-encoder — the masks derived from
it come for free), with `AffGraphNative` for the blit.

## Exterior stalls

Entering a new exterior area froze the game for 6–7 s. Instrumenting HQR
loads, `LoadCube` and the frame phases per slow frame showed six
consecutive ~1 s frames each **reloading the same 9 terrain cubes from ROM
and evicting them again**: with the 1 MB budget the `MapPGround` pool held
a single 32 KB cube while the exterior renderer needs the current cube plus
its 8 neighbours every frame. The island pools are now sized for 9 cubes
(plus more room for decor and character bodies); the RAM came from shrinking
`Phys` to its physical size and dropping the unused Smacker buffer. Cube
loads now hit the cache (9 cubes in 1 ms); what remains is 2–3 frames of
~1 s for the first full render of the new cubes and their ~100 decor
objects — the next target.

## First contact with hardware

A playtester ran the ROM on a real console (flashcart) into a consumer CRT.
Two things the emulator could not tell us: exteriors run noticeably
*smoother* on the silicon than under Ares (which models the VR4300's memory
and exception costs conservatively, and this port takes an exception per
unaligned access), so the profiler numbers are pessimistic; and the CRT's
overscan ate the first and last letters of every dialogue line, since the VI
presets fill the whole raster and the dialogue box sits 8 virtual pixels
from the edge. The fix is in the VI, not the engine: after `display_init`
the active window is shrunk by 6 % per side and the X/Y scales recomputed
from the registers libdragon wrote (so NTSC and PAL presets are handled
alike). The framebuffer and the 320×240 pixel cores are untouched;
emulators show a small black border.

The tester's first real crash came from the Temple of Bu (Desert island):
dropping through the pit of the secret passage into the temple never loaded
the scene — a black screen with the VI still refreshing. Reproduced in Ares
by starting a new game directly in cube 10 (`DebugStartCube`), where the
emulator logs "CPU frozen because of cached access to non-RDRAM area": the
first render read a block offset of ~100 MB out of the block library.
Integrity checks showed the library and the decoded grid were fine after
`InitGrille`, and a software watchpoint hooked into the log calls narrowed
the corruption to Twinsen's life script, opcode `LM_SET_GRM`: `IncrustGrm`
was given a GRM index of `0x800Fxxxx` — DoLife's opcode jump-table pointer,
still sitting in s1 — although the zone's `Info0` bytes were zero. The load
`lw s1,24(s0)` hits an unaligned zone table and is emulated by the port's
address-error handler, which writes the result into `reg_block_t::gpr[17]`…
and libdragon's `inthandler.S`, on the way out of an exception, only reloads
the caller-saved registers: s0–s7 are preserved by the C handler's ABI, so
they are never restored from the frame, and the emulated value is dropped.
Every emulated unaligned load whose destination is an s-register kept a
stale value; it depends on register allocation, which is why it showed up as
rare, scene-specific weirdness (the cellar scene-change heisenbug, where a
zone's `Info3` read as 2048 with zero bytes, has the same signature). Fixed
by patching `inthandler.S` in the toolchain image to reload s0–s7 from the
frame after `__onCriticalException`; a self-test confirmed `lw` into s1 now
returns the loaded value. `AffBrickBlock` additionally skips (and logs) a
cell whose block index is outside the library instead of freezing.

## Saving to the cartridge

The engine saves through plain POSIX calls — `open`/`read`/`write`/`stat`/
`unlink` in `LIB386/SYSTEM/FILES.CPP`, a directory scan for `*.LBA` in the
load menu, `lba2.cfg` rewritten key by key. On N64 the only writable
storage is the 32 KB of battery-backed SRAM declared in the ROM header, so
instead of teaching `SAVEGAME.CPP` about it, the SRAM became a filesystem:
libdragon lets a program register a prefix with a table of callbacks
(`attach_filesystem`), and newlib routes every `sram:/…` path to it — the
engine's user directory simply moved from `rom:/saves/` to `sram:/`. The
image is mirrored in RDRAM (header with magic and CRC32, then packed
records `size, name, data`); reads are served from the mirror, a write is
buffered per handle and, on `close()`, the whole image is rebuilt and DMA'd
to the cartridge at `0x08000000` with the PI domain-2 timings every SRAM
title programs. Name lookup is case-insensitive, because the engine probes
case variations of each path (it grew up on DOS). A blank or foreign part
fails the CRC and is formatted; a write that runs out of room is dropped
whole, so a failed save never leaves a truncated record for the load menu
to trip on.

The diet mattered more than the plumbing. A PC save is ~20 KB, 19,200 of
them the 160×120 thumbnail, and the PC engine stores the automatic
`current.lba` uncompressed. On N64 the thumbnail is drawn at 80×60 physical
pixels anyway (320×240 output), so that is what gets stored (4,800 bytes),
and `current.lba` goes through the same LZSS as the manual slots: a save is
now ~4 KB, and about six slots plus the resume file fit next to `lba2.cfg` —
which also moved into SRAM, so language and volume settings finally stick
(the first-boot default is English rather than the French dev config).
The one place the PC engine assumes a write cannot fail is the save menu;
it now checks the file afterwards and shows "Cartridge memory is full" in
the menu font. An input-free self-test (`DebugSaveTest` in the cfg) saves,
lists, reads back and fills the SRAM from a `DebugStartCube` boot, which
is how the layer was verified in Ares before the ROM went to the tester.

## Diagnostics kept in the tree

- `[renderprof]`/`[affprof]`: per-60-frame breakdown (terrain, object fill,
  dirty-box copy, present, cache writeback, logic).
- `[hangprof]`: any frame ≥80 ms with its phase split, HQR loads/evictions,
  `LoadCube` count/time and unaligned-access exceptions; `ChangeCube`
  timing; heap snapshot after each cube.
- `[scenechg]`: a checksum net around the cube-change zones for a rare
  heisenbug (Twinsen landing on a wall coming back from the cellar: the
  zone's `Info3` read as 2048 instead of 0). It distinguishes an emulator
  misread (`EMU-MISREAD`) from a memory mutation (`ZONE-MUTATED`); so far
  only a deterministic, harmless mutation on an exterior transition has
  been seen.
- `tools-n64/run-ares.ps1` redirects Ares' stdout (the ROM's ISViewer
  channel) to `ares_log.txt`: boot trace, engine logs, libdragon asserts
  with symbolic backtraces.
