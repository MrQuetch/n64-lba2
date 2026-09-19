// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Endian helpers for the Wii U port. The LBA2 retail data files (.HQR, .OBL,
// .ILE, save format, etc.) were authored on x86 in little-endian. The Wii U
// CPU (Espresso, PPC750-derived) is big-endian, so any 4- or 2-byte field
// read directly via fread() comes out byte-swapped.
//
// LBA2_LE32 / LBA2_LE16 are no-ops on little-endian targets (x86, DC SH4 in
// userland LE mode, ARM little-endian) and call __builtin_bswap on Wii U.
// Apply them on every multi-byte field loaded from disk before using its value.
//
// Coverage so far:
//   - LIB386/SYSTEM/HQFILE.CPP : HQF_Init, HQF_NbRes (offset table + COMPRESSED_HEADER)
//   - (extend as endian audit progresses through HQRMEM, HQRMLOAD, save format, etc.)

#pragma once

#include <stddef.h> // size_t
#include <stdint.h> // uintptr_t

#ifdef LBA2_TARGET_WIIU
#  define LBA2_LE32(x)  ((U32)__builtin_bswap32((U32)(x)))
#  define LBA2_LE16(x)  ((U16)__builtin_bswap16((U16)(x)))
#else
#  define LBA2_LE32(x)  (x)
#  define LBA2_LE16(x)  (x)
#endif

#include <SYSTEM/ADELINE_TYPES.H> // U16/U32 for the inline readers
#include <string.h>               // memcpy (unaligned-safe readers below)

// Unaligned-safe LE readers. The bare `LBA2_LE16(*(U16 *)p)` pattern works on
// PPC (tolerates unaligned integer loads) but traps on MIPS (N64): use these
// for byte-stream cursors. GCC lowers the memcpy to lwl/lwr pairs on VR4300,
// so there is no function-call or trap overhead on any platform.
static inline U16 LBA2_LE16_UA(const void *p) {
    U16 v;
    memcpy(&v, p, 2);
    return LBA2_LE16(v);
}
static inline U32 LBA2_LE32_UA(const void *p) {
    U32 v;
    memcpy(&v, p, 4);
    return LBA2_LE32(v);
}

#ifdef LBA2_TARGET_WIIU
// In-place byte-swap of the leading U32 offset table embedded in HQR sub-records
// like RESS_FILE3D. The first offset itself encodes the table length: if there
// are N entries, offsets are dense and tab[0] == N*4 (since the body of the
// first entry begins right after the table). Returns the entry count, or 0 if
// the first offset looks implausible (caller can decide what to do).
//
// The body bytes of each sub-entry are NOT touched here; callers that read
// multi-byte fields out of the body must still swap on read. This helper only
// fixes the offset lookup so that LoadFile3D(i) returns a sane pointer.
extern "C" inline unsigned int WiiU_SwapHqrSubOffsetTable(unsigned char *buf) {
    unsigned int *tab = (unsigned int *)buf;
    unsigned int first = LBA2_LE32(tab[0]);
    if (first == 0 || (first & 3u) != 0) {
        return 0;
    }
    unsigned int nrec = first / 4u;
    if (nrec > 8192u) { // sanity cap; LBA2 sub-records are far smaller
        return 0;
    }
    for (unsigned int i = 0; i < nrec; i++) {
        tab[i] = LBA2_LE32(tab[i]);
    }
    return nrec;
}

// One-shot swap of a plain U16/S16 LE array (e.g. the RESS *_GPC sprite ZV
// tables: 8 S16 per sprite = hotX, hotY, dX, dY + ZV box). NOT idempotent —
// call exactly once, right after the LoadMalloc_HQR that filled the buffer.
extern "C" inline void WiiU_SwapU16Array(void *buf, unsigned int bytes) {
    unsigned short *p = (unsigned short *)buf;
    for (unsigned int i = 0; i < bytes / 2u; i++) {
        p[i] = LBA2_LE16(p[i]);
    }
}

// One-shot swap of a plain U32/S32 LE array (e.g. the .ILE cube records:
// INF S32 infos, DOB T_DECORS = 12 S32 each, GRD T_HALF_POLY = 1 U32 each).
// NOT idempotent — gate on HQR_Flag (TRUE only when HQR_Get just loaded the
// bloc from disk; cached hits return already-swapped data).
extern "C" inline void WiiU_SwapU32Array(void *buf, unsigned int bytes) {
    unsigned int *p = (unsigned int *)buf;
    for (unsigned int i = 0; i < bytes / 4u; i++) {
        p[i] = LBA2_LE32(p[i]);
    }
}

// T_BODY_HEADER + T_OBJ_GROUPE + vertex/normal one-shot swap. Body file is
// LE-encoded on disk; on PPC every S32 count/offset and every S16 vertex
// coordinate becomes huge unless byte-swapped, which makes ObjectDisplay's
// vertex transform loop run for millions of iterations or crash on garbage
// pointers. Idempotent — uses SizeHeader as sentinel: post-swap it's the
// struct's actual size in bytes (~96-104), pre-swap it's >= 0x0100 because
// of the byte-flipped layout. Call once when a body is first looked up.
//
// T_BODY_HEADER (LIB386/H/OBJECT/AFF_OBJ.H), each row is one S32 unless noted:
//   u32[0]=Info     u32[1]={SizeHeader S16, Dummy S16}
//   u32[2..7]  = XMin XMax YMin YMax ZMin ZMax  (bounding box)
//   u32[8..9]  = NbGroupes  OffGroupes
//   u32[10..11]= NbPoints   OffPoints
//   u32[12..13]= NbNormales OffNormales
//   u32[14..15]= NbNormFaces OffNormFaces
//   u32[16..17]= NbPolys    OffPolys
//   u32[18..19]= NbLines    OffLines
//   u32[20..21]= NbSpheres  OffSpheres
//   u32[22..23]= NbTextures OffTextures
//
// After the header swap, also swap:
//   - T_OBJ_GROUPE table at OffGroupes (NbGroupes × 4 U16)
//   - TYPE_VT16 point array at OffPoints (NbPoints × 4 U16 = X,Y,Z,Grp)
//   - TYPE_VT16 normales at OffNormales (NbNormales × 4 U16)
//   - TYPE_VT16 normFaces at OffNormFaces (NbNormFaces × 4 U16)
//
// Polys/lines/spheres/textures bodies are not swapped here — first frame
// only needs transform+lighting; we'll extend coverage when those surface.
extern "C" inline void WiiU_SwapBodyHeader(void *body) {
    if (!body || body == (void *)(uintptr_t)-1) return;
    unsigned char *p = (unsigned char *)body;
    unsigned short *sh16 = (unsigned short *)(p + 4); // SizeHeader
    if (*sh16 < 0x0100) {
        // Likely already swapped (host order, small value).
        return;
    }
    unsigned int *u32 = (unsigned int *)p;
    u32[0] = LBA2_LE32(u32[0]);                 // Info
    sh16[0] = LBA2_LE16(sh16[0]);               // SizeHeader
    sh16[1] = LBA2_LE16(sh16[1]);               // Dummy
    // 22x S32 starting at offset 8: XMin..OffTextures (bbox 6 + 8 Nb/Off
    // pairs). The header ends exactly at byte 96 == OffGroupes. Swapping 24
    // here over-ran 2 U32s into group[0]'s descriptor: LE32 + the later
    // per-U16 group swap left its fields pairwise EXCHANGED, so the root
    // group read {OrgPoint=-1(!), NbPts=0} instead of {OrgGroupe=-1,
    // OrgPoint=0, NbPts=2} — root points never transformed, every child
    // group translated off stale Obj_ListRotatedPoints ("body Y flies").
    for (int i = 2; i < 2 + 22; i++) {
        u32[i] = LBA2_LE32(u32[i]);
    }

    // off-range check is just "past the header" — groupes can start at
    // offset 96 (right after a 96-byte header alignment), so don't gate on
    // anything tighter than that.
    auto swap_u16_array = [](unsigned char *base, int off, int count4) {
        if (count4 <= 0 || off < 8) return;
        unsigned short *a = (unsigned short *)(base + off);
        for (int i = 0; i < count4; i++) a[i] = LBA2_LE16(a[i]);
    };

    int nbGroupes   = (int)u32[8];
    int offGroupes  = (int)u32[9];
    int nbPoints    = (int)u32[10];
    int offPoints   = (int)u32[11];
    int nbNormales  = (int)u32[12];
    int offNormales = (int)u32[13];
    int nbNormFaces = (int)u32[14];
    int offNormFaces= (int)u32[15];

    if (nbGroupes > 0 && nbGroupes < 1024)   swap_u16_array(p, offGroupes,   nbGroupes   * 4);
    if (nbPoints  > 0 && nbPoints  < 8192)   swap_u16_array(p, offPoints,    nbPoints    * 4);
    if (nbNormales> 0 && nbNormales< 8192)   swap_u16_array(p, offNormales,  nbNormales  * 4);
    if (nbNormFaces>0 && nbNormFaces<8192)   swap_u16_array(p, offNormFaces, nbNormFaces * 4);

    // ──── Polys ───────────────────────────────────────────────────────────
    // Walk T_POLY_HEADER groups from OffPolys until OffLines. Per group:
    //   * Swap header: U16 TypePoly, U16 NbPoly, U32 OffNextType
    //   * Determine per-poly size from (typePoly & 0xFF) + flags
    //     (MASK_QUADRILATERE=1<<15, MASK_ENVIRONMENT=1<<14, POLY_DITHER_TABLE=7)
    //   * Swap NbPoly × poly_size_in_U16 entries (all poly struct fields are U16)
    //
    // T_BODY_HEADER index reminder: Nb/Off PAIRS — Nb is the even index, Off
    // the odd one. u32[16]=NbPolys, u32[17]=OffPolys, etc.
    /* int nbPolys = (int)u32[16];  // not currently needed for the walk */
    int offPolys   = (int)u32[17];
    int nbLines    = (int)u32[18];
    int offLines   = (int)u32[19];
    int nbSpheres  = (int)u32[20];
    int offSpheres = (int)u32[21];

    if (offPolys >= 8 && offLines > offPolys && offLines < 1024*1024) {
        unsigned char *cur = p + offPolys;
        unsigned char *end = p + offLines;
        int safety = 0;
        while (cur + 8 <= end && safety < 4096) {
            unsigned short *hdr = (unsigned short *)cur;
            unsigned int *off32 = (unsigned int *)cur;
            unsigned short typePoly = LBA2_LE16(hdr[0]);
            unsigned short nbPoly   = LBA2_LE16(hdr[1]);
            hdr[0] = typePoly;
            hdr[1] = nbPoly;
            off32[1] = LBA2_LE32(off32[1]);  // OffNextType
            cur += 8;

            // POLY_DITHER_TABLE = 7
            const unsigned MASK_QUAD = 1u << 15;
            const unsigned MASK_ENV  = 1u << 14;
            unsigned typeIdx = typePoly & 0xFFu;
            int isQuad = (typePoly & MASK_QUAD) != 0;
            int isEnv  = (typePoly & MASK_ENV)  != 0;
            int polyU16;  // # of U16 fields per poly entry
            if (isQuad) {
                if (isEnv)                    polyU16 = 8;   // STRUC_POLY4_ENV
                else if (typeIdx <= 7)        polyU16 = 6;   // STRUC_POLY4_LIGHT
                else                          polyU16 = 16;  // STRUC_POLY4_TEXTURE
            } else {
                if (isEnv)                    polyU16 = 8;   // STRUC_POLY3_ENV
                else if (typeIdx <= 7)        polyU16 = 6;   // STRUC_POLY3_LIGHT
                else                          polyU16 = 12;  // STRUC_POLY3_TEXTURE
            }

            int totalU16 = (int)nbPoly * polyU16;
            if (totalU16 > 0 && cur + (size_t)totalU16 * 2 <= end) {
                unsigned short *pd = (unsigned short *)cur;
                for (int i = 0; i < totalU16; ++i) pd[i] = LBA2_LE16(pd[i]);
                cur += (size_t)totalU16 * 2;
            } else {
                // Malformed or end-of-list — bail out.
                break;
            }
            safety++;
        }
    }

    // ──── Lines (T_OBJ_LINE = 4× U16 each) ────────────────────────────────
    if (nbLines > 0 && nbLines < 4096 && offLines >= 8) {
        swap_u16_array(p, offLines, nbLines * 4);
    }

    // ──── Spheres (T_OBJ_SPHERE = 4× U16 each) ────────────────────────────
    if (nbSpheres > 0 && nbSpheres < 4096 && offSpheres >= 8) {
        swap_u16_array(p, offSpheres, nbSpheres * 4);
    }

    // ──── Textures (NbTextures × U32 handle table) ────────────────────────
    // PtrTextures[HandleText] entries are consumed as full U32 values:
    // RepMask = info >> 16, texel base = info & 0xFFFF (AFF_OBJ.CPP). Left
    // unswapped the bytes come out reversed → wrong texture region/mask
    // (garbled shirt texture with otherwise correct geometry).
    int nbTextures  = (int)u32[22];
    int offTextures = (int)u32[23];
    if (nbTextures > 0 && nbTextures < 1024 && offTextures >= 8) {
        unsigned int *t = (unsigned int *)(p + offTextures);
        for (int i = 0; i < nbTextures; i++) t[i] = LBA2_LE32(t[i]);
    }
}
#endif
