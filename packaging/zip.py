"""Zips a folder for the build workflow (build.yml): sorted entries, deflate, file modes kept.

    python zip.py <out.zip> <folder> [<folder inside the zip>]

With a third argument only that folder (inside <folder>) is zipped, under its own name.
"""
import os
import sys
import zipfile

out, root = sys.argv[1], sys.argv[2]
sub = sys.argv[3] if len(sys.argv) > 3 else ""
start = os.path.join(root, sub) if sub else root
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(start):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            arc = os.path.relpath(p, root).replace(os.sep, "/")
            info = zipfile.ZipInfo.from_file(p, arc)
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as fh:
                z.writestr(info, fh.read())
print(out, os.path.getsize(out))
