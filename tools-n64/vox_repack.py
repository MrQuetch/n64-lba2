#!/usr/bin/env python3
# LBA2 N64 voice repack.
#
# The retail VOX files are HQR archives of RIFF/IMA-ADPCM voice clips, ~83 MB
# per language — far too big for the ROM. This tool, run at build time:
#   1. extracts every clip (LZSS-decompressing method-1 entries, restoring the
#      'R' of "RIFF" that the packer overwrote with the FlagNextVoc byte) to a
#      temp .wav, so build-n64.sh can re-encode it to a small Opus .wav64;
#   2. writes a *placeholder* VOX HQR with the SAME entry layout (so the engine's
#      voice-index math and FlagNextVoc chaining are untouched) where each entry
#      is just [FlagNextVoc byte][wav64 basename '\0']. At runtime the N64 audio
#      backend sees userhandle==SPEAK_SAMPLE, reads that basename out of BufSpeak
#      and streams rom:/vox/<basename>.wav64 on the dedicated voice channel.
#
# HQR header: U32 offsets[slotcount]; offsets[0] == header size == slotcount*4
# == entry 0's file offset. Entry: [U32 size][U32 sizelzss][U16 method] payload.
import struct, sys, os, glob


def expand_lz(src, out_size, min_bloc=2):
    # Port of ExpandLZ (LIB386/SYSTEM/LZ.CPP): LZSS, flag byte gates 8 ops.
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
                length = (lo & 0x0F) + min_bloc + 1
                start = di - off - 1
                for k in range(length):
                    if di >= out_size:
                        break
                    dst[di] = dst[start + k]; di += 1
            flags >>= 1
    return bytes(dst)


def repack(voxpath, wavdir, voxoutdir):
    data = open(voxpath, 'rb').read()
    stem = os.path.splitext(os.path.basename(voxpath))[0].lower()  # en_000
    first = struct.unpack('<I', data[0:4])[0]
    slotcount = first // 4
    offs = [struct.unpack('<I', data[i * 4:i * 4 + 4])[0] for i in range(slotcount)]

    header_size = slotcount * 4
    new_offsets = [0] * slotcount
    blob = bytearray()
    pos = header_size
    n_clips = 0
    for i, o in enumerate(offs):
        if o == 0 or o + 10 > len(data):
            continue
        size, sizelzss, method = struct.unpack('<IIH', data[o:o + 10])
        if size <= 0 or size > 4_000_000:
            continue
        p = o + 10
        if method == 0:
            if p + size > len(data):
                continue
            payload = bytearray(data[p:p + size])
        else:
            if p + sizelzss > len(data):
                continue
            payload = bytearray(expand_lz(data[p:p + sizelzss], size))
        flagnext = payload[0]
        payload[0] = 0x52  # 'R' -> valid RIFF for ffmpeg
        base = f"{stem}_{i:04d}"
        open(os.path.join(wavdir, base + ".wav"), 'wb').write(payload)
        # placeholder entry: [flagnext][basename\0]
        ph = bytes([flagnext]) + base.encode('ascii') + b'\0'
        new_offsets[i] = pos
        entry = struct.pack('<IIH', len(ph), len(ph), 0) + ph
        blob += entry
        pos += len(entry)
        n_clips += 1

    # offsets[0] must stay the header size (== entry 0 position) for MaxVoice.
    new_offsets[0] = header_size
    out = bytearray()
    for o in new_offsets:
        out += struct.pack('<I', o)
    out += blob
    open(os.path.join(voxoutdir, stem + ".vox"), 'wb').write(out)
    return n_clips, len(out)


if __name__ == '__main__':
    voxdir, wavdir, voxoutdir, prefix = sys.argv[1:5]
    os.makedirs(wavdir, exist_ok=True)
    os.makedirs(voxoutdir, exist_ok=True)
    total = 0
    for f in sorted(glob.glob(os.path.join(voxdir, prefix + '_*.VOX'))):
        c, sz = repack(f, wavdir, voxoutdir)
        total += c
        print(f"  {os.path.basename(f)}: {c} clips, placeholder {sz} B")
    print(f"[vox_repack] {total} clips extracted, placeholders in {voxoutdir}")
