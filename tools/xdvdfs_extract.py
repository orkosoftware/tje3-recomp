"""Extract an Xbox XDVDFS (XISO) image and write a SHA-1 manifest.

Usage: python -I xdvdfs_extract.py <image.iso> <out_dir> <manifest.tsv>

Also validates the filesystem: every directory tree is walked, every file
extent must lie within the image, and overlapping extents are reported.
"""
import hashlib
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
ATTR_DIR = 0x10


def read_dir(img, sector, size):
    img.seek(sector * SECTOR)
    return img.read(size)


def walk_tree(buf, off, out, seen):
    # Directory entries form a binary tree linked by dword offsets.
    stack = [off]
    while stack:
        o = stack.pop()
        if o in seen or o + 14 > len(buf):
            continue
        seen.add(o)
        left, right, start, size, attr, nlen = struct.unpack_from("<HHIIBB", buf, o)
        if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
            continue  # padding / empty directory
        name = buf[o + 14:o + 14 + nlen].decode("latin-1")
        out.append((name, start, size, attr))
        if left not in (0, 0xFFFF):
            stack.append(left * 4)
        if right not in (0, 0xFFFF):
            stack.append(right * 4)


def main():
    iso, out_dir, manifest = sys.argv[1:4]
    img_size = os.path.getsize(iso)
    img = open(iso, "rb")
    img.seek(32 * SECTOR)
    vd = img.read(SECTOR)
    if vd[:20] != MAGIC or vd[0x7EC:0x800] != MAGIC:
        sys.exit("not an XDVDFS image (volume descriptor magic missing)")
    root_sector, root_size = struct.unpack_from("<II", vd, 20)

    files, extents, errors = [], [], []
    dirs = [("", root_sector, root_size)]
    while dirs:
        path, sec, size = dirs.pop()
        os.makedirs(os.path.join(out_dir, path), exist_ok=True)
        if size == 0:
            continue
        entries = []
        walk_tree(read_dir(img, sec, size), 0, entries, set())
        for name, start, fsize, attr in entries:
            rel = os.path.join(path, name) if path else name
            if attr & ATTR_DIR:
                dirs.append((rel, start, fsize))
                continue
            end = start * SECTOR + fsize
            if end > img_size:
                errors.append(f"extent past end of image: {rel}")
                continue
            files.append((rel, start, fsize))
            if fsize:
                extents.append((start * SECTOR, end, rel))

    extents.sort()
    for (a0, a1, an), (b0, b1, bn) in zip(extents, extents[1:]):
        if b0 < a1:
            errors.append(f"overlapping extents: {an} / {bn}")

    total = 0
    with open(manifest, "w", encoding="utf-8", newline="\n") as mf:
        mf.write("sha1\tsize\tsector\tpath\n")
        for rel, start, fsize in sorted(files, key=lambda f: f[0].lower()):
            h = hashlib.sha1()
            img.seek(start * SECTOR)
            dst = os.path.join(out_dir, rel)
            with open(dst, "wb") as o:
                left = fsize
                while left:
                    chunk = img.read(min(left, 1 << 22))
                    if not chunk:
                        errors.append(f"short read: {rel}")
                        break
                    h.update(chunk)
                    o.write(chunk)
                    left -= len(chunk)
            total += fsize
            mf.write(f"{h.hexdigest()}\t{fsize}\t{start}\t{rel.replace(os.sep, '/')}\n")

    print(f"files: {len(files)}  bytes: {total}")
    for e in errors:
        print("ERROR:", e)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
