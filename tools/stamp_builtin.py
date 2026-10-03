#!/usr/bin/env python3
"""Mark a PE DLL as a "Wine builtin DLL".

Writes the same 32-byte signature that `winebuild --builtin` writes: the
string "Wine builtin DLL" zero-padded to 32 bytes, right after the 64-byte
DOS header (Wine's loader checks it in server/mapping.c). Only modules with
this signature get a Unix library bound to them.

Doing it here avoids a build dependency on winebuild (wine-devel).

usage: stamp_builtin.py FILE.dll
"""
import struct
import sys

SIGNATURE = b"Wine builtin DLL".ljust(32, b"\0")
OFFSET = 0x40  # sizeof(IMAGE_DOS_HEADER)


def main(argv):
    if len(argv) != 1:
        sys.stderr.write(__doc__)
        return 2
    path = argv[0]
    with open(path, "r+b") as f:
        data = f.read(OFFSET + len(SIGNATURE))
        if data[:2] != b"MZ":
            sys.stderr.write("error: %s is not a PE file\n" % path)
            return 1
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if e_lfanew < OFFSET + len(SIGNATURE):
            sys.stderr.write("error: %s has no room for the signature (e_lfanew=0x%x)\n"
                             % (path, e_lfanew))
            return 1
        f.seek(OFFSET)
        f.write(SIGNATURE)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
