#!/usr/bin/env python3
"""Writes a reproducible zip: make_zip.py OUT.zip BASEDIR NAME EPOCH

Packs the tree BASEDIR/NAME as NAME/... with sorted entries, the timestamp of EPOCH (seconds, as
SOURCE_DATE_EPOCH, clamped to the zip minimum of 1980), fixed permissions and no extra fields, so the
same tree gives the same bytes (for one zlib version). Standard library only.
"""
import os
import sys
import time
import zipfile


def main():
    out, base, name, epoch = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    stamp = time.gmtime(max(epoch, 315532800))[:6]
    root = os.path.join(base, name)
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for filename in sorted(filenames):
            path = os.path.join(dirpath, filename)
            arc = os.path.relpath(path, base).replace(os.sep, "/")
            entries.append((arc, path))
    entries.sort()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, path in entries:
            info = zipfile.ZipInfo(arc, stamp)
            executable = arc.endswith((".exe", ".dll"))
            info.external_attr = (0o755 if executable else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            with open(path, "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)


if __name__ == "__main__":
    main()
