#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Reads the retail VIDEO.HQR (the Smacker movies) and RESS.HQR entry 48
# (RESS_ACFLIST, the movie name list the engine indexes into: list position N
# == VIDEO.HQR entry N). Used by fmv-probe.sh to pull one movie out of the
# archive; also useful on its own to see what is in there.
#
#   python3 video_hqr.py list    <VIDEO.HQR> [RESS.HQR]
#   python3 video_hqr.py extract <VIDEO.HQR> <RESS.HQR> <NAME|INDEX> <out.smk>
#
# HQR layout (same as tools-n64/vox_repack.py): U32 offsets[slotcount], where
# offsets[0] == slotcount*4; each entry is [U32 size][U32 sizelzss][U16 method]
# followed by the payload. The movies are stored uncompressed (method 0).
import struct, sys


def expand_lz(src, out_size, min_bloc=2):
    # Port of ExpandLZ (LIB386/SYSTEM/LZ.CPP): LZSS, one flag byte per 8 ops.
    dst = bytearray(out_size)
    di = si = 0
    n = len(src)
    while di < out_size and si < n:
        flags = src[si]; si += 1
        for _ in range(8):
            if di >= out_size or si >= n:
                break
            if flags & 1:
                dst[di] = src[si]; di += 1; si += 1
            else:
                lo = src[si]; hi = src[si + 1]; si += 2
                off = (hi << 4) | (lo >> 4)
                length = (lo & 0x0F) + min_bloc
                start = di - off - 1
                for k in range(length):
                    if di >= out_size:
                        break
                    dst[di] = dst[start + k]; di += 1
            flags >>= 1
    return bytes(dst)


def offsets(path):
    with open(path, 'rb') as f:
        first = struct.unpack('<I', f.read(4))[0]
        slots = first // 4
        f.seek(0)
        raw = f.read(slots * 4)
    return [struct.unpack('<I', raw[i * 4:i * 4 + 4])[0] for i in range(slots)]


def read_entry(path, idx):
    off = offsets(path)[idx]
    with open(path, 'rb') as f:
        f.seek(off)
        size, lzsize, method = struct.unpack('<IIH', f.read(10))
        return f.read(size) if method == 0 else expand_lz(f.read(lzsize), size)


def acf_names(ress_path):
    # RESS_ACFLIST == 48 (SOURCES/COMMON.H): whitespace-separated "NAME.SMK".
    text = read_entry(ress_path, 48).decode('latin-1')
    return [w.strip() for w in text.split() if w.strip()]


def smk_info(path, off):
    """(width, height, frames, fps) from a Smacker header at `off`, or None."""
    with open(path, 'rb') as f:
        f.seek(off)
        raw = f.read(10)
        if len(raw) < 10:
            return None
        hdr = f.read(32)
    if hdr[:3] != b'SMK':
        return None
    w, h, frames, rate = struct.unpack('<IIIi', hdr[4:20])
    # Smacker frame rate: 0 -> 10 fps, >0 -> 1000/rate, <0 -> 100000/-rate.
    fps = 10.0 if rate == 0 else (1000.0 / rate if rate > 0 else 100000.0 / -rate)
    return w, h, frames, fps


def resolve(names, key):
    if key.isdigit():
        return int(key)
    want = key.upper()
    if not want.endswith('.SMK'):
        want += '.SMK'
    for i, n in enumerate(names):
        if n.upper() == want:
            return i
    raise SystemExit(f"movie '{key}' is not in the ACF list")


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__ or "usage: video_hqr.py list|extract ...")
    cmd, video = argv[1], argv[2]
    if cmd == 'list':
        names = acf_names(argv[3]) if len(argv) > 3 else []
        offs = offsets(video)
        total_s = total_b = 0
        print(f"{'idx':>3}  {'name':14s} {'size':>9s}  {'res':>9s} {'frames':>7s} {'fps':>5s}  dur")
        for i, o in enumerate(offs):
            info = smk_info(video, o) if o else None
            if not info:
                continue
            w, h, frames, fps = info
            with open(video, 'rb') as f:
                f.seek(o)
                size = struct.unpack('<I', f.read(4))[0]
            secs = frames / fps
            total_s += secs; total_b += size
            name = names[i] if i < len(names) else '?'
            print(f"{i:3d}  {name:14s} {size/1048576:8.1f}M  {w:4d}x{h:<4d} {frames:7d} {fps:5.1f}  "
                  f"{int(secs // 60)}:{int(secs % 60):02d}")
        print(f"\ntotal: {total_s/60:.1f} min, {total_b/1048576:.1f} MB")
    elif cmd == 'extract':
        ress, key, out = argv[3], argv[4], argv[5]
        idx = resolve(acf_names(ress), key)
        with open(out, 'wb') as f:
            f.write(read_entry(video, idx))
        print(f"[fmv-probe] extracted entry {idx} -> {out}")
    else:
        raise SystemExit(f"unknown command '{cmd}'")


if __name__ == '__main__':
    main(sys.argv)
