#!/usr/bin/env python3
"""Tell whether an N64-LBA2 ROM carries the libdragon inthandler patch.

The port emulates unaligned memory accesses in its address-error handler
(LIB386/SYSTEM/N64_BACKEND.CPP): it decodes the faulting load/store and writes
the result into the saved register frame. Stock libdragon only reloads the
caller-saved GPRs on the way out of an exception, so a result written into a
callee-saved register (s0-s7) never reaches the register - the code resumes
with a stale value. docker/Dockerfile.n64 patches inthandler.S to reload
s0-s7 from the frame; the patch is eight `ld` instructions right after the
call to __onCriticalException, and that byte sequence is what this script
looks for.

Use it on any ROM already handed to a tester - the symptom (a wild pointer in
a spot that reads a packed structure: a save header, a scene zone) looks like
a game bug, not a toolchain one.

    python3 tools-n64/check_rom_unaligned_fix.py lba2.z64
"""

import sys

# ld s0,160(sp) ... ld s7,216(sp) - the patch's reload block, big-endian.
PATCH = bytes.fromhex("dfb000a0dfb100a8dfb200b0dfb300b8dfb400c0dfb500c8dfb600d0dfb700d8")


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2

    path = argv[1]
    with open(path, "rb") as f:
        data = f.read()

    at = data.find(PATCH)
    if at >= 0:
        print("PATCHED: %s carries the inthandler s0-s7 reload (offset 0x%x)" % (path, at))
        return 0

    print("NOT PATCHED: %s was built with a stock libdragon." % path)
    print("Emulated unaligned loads into s0-s7 are dropped in this build:")
    print("expect rare wild-pointer crashes (saves, scene changes). Rebuild the")
    print("toolchain image (docker/Dockerfile.n64) and the ROM.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
