#!/bin/sh
# One-shot: convert the WiiU-era `LBA2_LE16(*(U16 *)p)` swap pattern (traps on
# MIPS when p is unaligned) to the memcpy-based LBA2_LE16_UA/LBA2_LE32_UA
# helpers from wiiu_endian.h. DISKFUNC.CPP excluded (has its own N64 branch).
set -e
cd "$(dirname "$0")/.."

for f in SOURCES/FICHE.CPP SOURCES/GERELIFE.CPP SOURCES/GERETRAK.CPP \
         SOURCES/GRILLE.CPP SOURCES/HOLOBODY.CPP SOURCES/IMPACT.CPP \
         SOURCES/POF.CPP; do
    sed -i \
        -e 's/LBA2_LE16(\*(U16 \*)/LBA2_LE16_UA((const void *)/g' \
        -e 's/LBA2_LE16(\*(S16 \*)/LBA2_LE16_UA((const void *)/g' \
        -e 's/LBA2_LE32(\*(U32 \*)/LBA2_LE32_UA((const void *)/g' \
        -e 's/LBA2_LE32(\*(S32 \*)/LBA2_LE32_UA((const void *)/g' \
        "$f"
done

echo "leftover bare-cast swap sites (expect only DISKFUNC's own):"
grep -rn 'LBA2_LE[13][62](\*' SOURCES/ || true
