# Wii U port — development log (condensed)

A chronological summary of how the port came together. The recurring theme
is **endianness**: the engine reads most of its data structures by casting
raw little-endian file buffers, and the Wii U's PowerPC core is big-endian —
so nearly every subsystem worked only after its loader learned to swap.

## Boot and first light

- devkitPPC/WUT build glue: CMake toolchain file, Docker build script,
  `wuhbtool` packaging. First `lba2.wuhb` linked at 638 KB.
- OSScreen video bring-up with a boot-trace status line; ProcUI lifecycle.
- HQR resource loader fixed first (`HQF_Init` offset table, compressed-entry
  headers — `CompressMethod` read as LE32 instead of LE16 silently disabled
  LZ decompression and zeroed every decoded brick).
- Memory: the Wii U default heap gave tiny allocations where the engine
  expected a flat 32 MB pool — solved with a dedicated pool allocator.

## The endianness campaign

Fixed, roughly in the order the game forced us to:

- Scene loader (`LoadScene`): zones, brick tracks, patch lists.
- 3D bodies: header/group/point tables (`WiiU_SwapBodyHeader`, one-shot,
  idempotent across HQR cache reloads), plus texture tables.
- Grids and bricks (`ListCubeInfos`, block library, mask overflow in
  `CreateMaskGph`).
- Exterior islands: `.ILE`/`.OBL` payloads, `T_DECORS`, half-poly tables.
- Painter's-order sort keys (32-bit stores collapsing to 0/−1 broke interior
  draw order).
- Text banks, font GPM offset tables, dialogue ordering buffers.
- Save games (LZSS token order), holomap, `ANIM3DS.HQR` description table,
  particle flows, impact scripts.

Diagnosis pattern throughout: bounded `LogPrintf` tracing to the SD card,
memory canaries around suspect buffers, and Python decoders for the on-disk
formats (scene/impact/font dumps) to compare ground truth against what the
engine computed.

## Systems

- **Input:** VPAD backend with a unified keyboard+gamepad binding table and
  an in-game remap menu; GamePad touch panel with behaviour/weapon/spell
  shortcuts; on-screen keyboard for save names.
- **Audio:** native AX backend with software mixing — SFX, positional
  samples, OGG music via `stb_vorbis`, voice-overs.
- **FMV:** libsmacker in `SMK_MODE_DISK` streaming mode so the 76 MB intro
  plays from disk; audio through the AX FIFO.
- **Performance:** profiler pointed at read-for-ownership traffic on the
  DRAM bus during the frame blit; a `dcbz`-based store path took the game
  from 10–23 fps to 43–82 fps ("100% playable").

## Endgame

- Full playthrough on real hardware: both planets, mines, wagon, buggy,
  dinofly, ending credits.
- Final fixes after completion: `ListAnim3DS` swap (wrong animated object —
  a clam shell — appearing in the Sendell sphere room), `ScaleSprite`
  reimplemented (the opaque scaled-sprite blitter was a 1:1 stub, making the
  laser pistol's impact particles render at full magic-ball size), the
  UTF-8→CP850 converter made real (menu accents), and quit now returns to
  the system menu instead of a black screen.
