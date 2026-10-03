#!/usr/bin/env python3
"""Redirect a PE file's SETUPAPI/WINUSB imports to the bridge DLLs.

Rewrites two DLL names in the import table, in place and with names of the
same length:

    SETUPAPI.dll -> OSCSETUP.dll   (setupapi is a KnownDLL: never loaded from
                                    the application directory)
    WINUSB.DLL   -> OSCWUS.DLL     (Wine's own winusb.dll would be preferred)

Wine does not know the new names and they are not KnownDLLs, so the bridge
DLLs (oscsetup.dll, oscwus.dll) are loaded from the application directory.
Nothing else in the file changes (14 bytes in total for usbwave.dll).

The first time a file is patched, the original is saved as <file>.orig.
Patching an already patched file does nothing.

usage:
    patch_imports.py FILE...              patch
    patch_imports.py --check FILE...      show the state, change nothing
    patch_imports.py --restore FILE...    put <file>.orig back
"""
import os
import shutil
import struct
import sys
import tempfile

RENAMES = {
    "SETUPAPI.DLL": "OSCSETUP.dll",
    "WINUSB.DLL": "OSCWUS.DLL",
}
for _old, _new in RENAMES.items():
    assert len(_old) == len(_new), (_old, _new)
PATCHED_NAMES = {new.upper() for new in RENAMES.values()}


class PatchError(Exception):
    pass


def import_names(data):
    """Return [(file_offset, dll_name), ...] for every import descriptor."""
    if data[:2] != b"MZ":
        raise PatchError("not a PE file (no MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PatchError("not a PE file (no PE signature)")
    nsec, opt_size = struct.unpack_from("<H12xH", data, pe + 6)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        raise PatchError("not a 32-bit (PE32) image")
    imp_rva = struct.unpack_from("<I", data, opt + 96 + 8)[0]
    if not imp_rva:
        return []

    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def rva_to_offset(rva):
        for va, size, rawptr in sections:
            if va <= rva < va + size:
                return rva - va + rawptr
        raise PatchError("RVA 0x%x is not inside any section" % rva)

    names = []
    desc = rva_to_offset(imp_rva)
    while True:
        oft, _ts, _fwd, name_rva, ft = struct.unpack_from("<IIIII", data, desc)
        if not (oft or name_rva or ft):
            break
        off = rva_to_offset(name_rva)
        end = data.index(b"\0", off)
        names.append((off, data[off:end].decode("ascii")))
        desc += 20
    return names


def state(names):
    """'patched', 'original', 'partial' or 'none' (imports neither DLL)."""
    upper = {n.upper() for _, n in names}
    orig = [old for old in RENAMES if old in upper]
    patched = [new for new in PATCHED_NAMES if new in upper]
    if not orig and not patched:
        return "none"
    if not orig:
        return "patched"
    if not patched:
        return "original"
    return "partial"


def check(path):
    with open(path, "rb") as f:
        names = import_names(f.read())
    relevant = [n for _, n in names if n.upper() in RENAMES or n.upper() in PATCHED_NAMES]
    backup = " (backup: %s.orig)" % os.path.basename(path) if os.path.exists(path + ".orig") else ""
    print("%s: %s [%s]%s" % (path, state(names), ", ".join(relevant) or "-", backup))


def patch(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    names = import_names(data)
    st = state(names)
    if st == "none":
        raise PatchError("imports neither SETUPAPI.dll nor WINUSB.DLL - nothing to patch")
    if st == "patched":
        print("already patched: %s" % path)
        return

    backup = path + ".orig"
    if not os.path.exists(backup) and st == "original":
        shutil.copy2(path, backup)
        print("backup: %s" % backup)

    for off, name in names:
        new = RENAMES.get(name.upper())
        if new:
            data[off:off + len(new)] = new.encode("ascii")
            print("  %s -> %s (offset 0x%x)" % (name, new, off))

    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    shutil.copymode(path, tmp)
    os.replace(tmp, path)
    print("patched: %s" % path)


def restore(path):
    backup = path + ".orig"
    if not os.path.exists(backup):
        raise PatchError("no backup: %s" % backup)
    shutil.copy2(backup, path)
    print("restored: %s" % path)


def main(argv):
    action, files = patch, argv
    if argv and argv[0] in ("--check", "--restore"):
        action = check if argv[0] == "--check" else restore
        files = argv[1:]
    if not files or any(f.startswith("-") for f in files):
        sys.stderr.write(__doc__)
        return 2

    failed = False
    for path in files:
        try:
            action(path)
        except (OSError, PatchError, struct.error) as e:
            sys.stderr.write("error: %s: %s\n" % (path, e))
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
