# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Wii U / devkitPPC + WUT toolchain wrapper. Layered on top of the official
# devkitPro CMake support so we get powerpc-eabi-gcc, the .rpx output rules,
# and the WUT search paths for free, then we add the LBA2-specific switches.

# Pull in the upstream devkitPro Wii U toolchain. It is shipped inside the
# `devkitpro/devkitppc` Docker image at this path (devkitPro pacman package
# `wiiu-cmake` keeps it here on native installs as well).
include($ENV{DEVKITPRO}/cmake/WiiU.cmake)

set(PLATFORM_WIIU TRUE)
set(LBA2_TARGET_WIIU TRUE)

add_definitions(
    -DLBA2_TARGET_WIIU
    -D__WIIU__
    -D__WUT__
)

# C++14 is required because the WUT headers use alignas / enum class.
# The LBA2 codebase is C++98-style, so we apply the same -fpermissive +
# -Wno-narrowing relaxation the Dreamcast port uses (legitimate U16/S16
# reinterprets in SINTAB and friends).
#
# -ffp-contract=off: GCC on PPC defaults to contracting a*b+c into fmadd(s),
# which skips the intermediate rounding that x86 SSE (mulss+addss) performs.
# The float 3D pipeline (MULMATF, ROTRALIF, LIROT3DF...) feeds the painter's
# depth sort with lrint()ed Z values; fused rounding shifted some of them by
# ±1 vs the original, flipping near-tie draw order (Twinsen's hair spheres
# popping in front of his face indoors). Keep every FP op individually
# rounded so PPC matches the x86 reference semantics.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-narrowing -fpermissive -ffp-contract=off")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffp-contract=off")

# Audio: native AX backend (LIB386/AIL/WIIU/) — software mix into two AX
# ring voices, OGG music via stb_vorbis. FMV: libsmacker (portable C), video
# streamed from VIDEO.HQR on SD via disk mode, audio through the AX FIFO.
set(SOUND_BACKEND  "wiiu"    CACHE STRING "Sound backend (Wii U: AX)"     FORCE)
set(MVIDEO_BACKEND "smacker" CACHE STRING "FMV backend (libsmacker)"      FORCE)
set(ENABLE_ASM     OFF       CACHE BOOL   "x86 ASM disabled on PPC"       FORCE)

# Tests are host-side (require objcopy and a runnable binary on the build host).
set(LBA2_BUILD_TESTS           OFF CACHE BOOL "No tests on cross-compile" FORCE)
set(LBA2_BUILD_ASM_EQUIV_TESTS OFF CACHE BOOL "No tests on cross-compile" FORCE)
