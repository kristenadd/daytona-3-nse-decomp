#!/usr/bin/env python3
"""Make a slim copy of one edition's install that leans on another's.

Both editions are about 5 GB, and about 1.3 GB of the 2017 files are the same
bytes as the 2019 files at the same place. The slim copy keeps the edition's
folder tree, only the files that differ from the base (or that the base does
not have), and dcusa-overlay.txt in its root: "base <id>", then the shared
files' relative paths. Next to the base's game folder it works as a full
install: the game reads a shared file from the base (src/system/content.hpp,
src/recomp/host_files.hpp). For the iPhone or iPad, copy the slim folder
beside the full 2019 folder instead of the full 2017 folder.

    tools/overlay/make_overlay.py \\
        --edition "../Daytona Championship USA/Daytona Championship USA (2017-12-21)[Sega PC][TP]" \\
        --base "../Daytona Championship USA/Daytona Championship USA New Season Edition (2019)[Sega PC][TP]" \\
        --base-id 2019 --out "build/overlay"

--edition and --base may be an edition's top folder or its install root (the
folder that holds Daytona/). The output is <out>/<edition top folder name>;
files are cloned (APFS), so the copy costs no space on the Mac. Nothing is
read from or written to the game folders except reading.
"""
import argparse, hashlib, os, shutil, subprocess, sys


def install_root(top):
    """The folder holding Daytona/ (with rom/ and nrt/), searched a few levels down."""
    for d, subdirs, _ in os.walk(top):
        if os.path.relpath(d, top).count(os.sep) > 3:
            subdirs[:] = []
            continue
        for s in subdirs:
            game = os.path.join(d, s)
            if os.path.isdir(os.path.join(game, 'rom')) and os.path.isdir(os.path.join(game, 'nrt')):
                return d
    sys.exit('no game folder (Daytona/ with rom/ and nrt/) under %s' % top)


def files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root).replace(os.sep, '/').lower()] = p
    return out


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.digest()


def clone(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if subprocess.run(['cp', '-c', src, dst], stderr=subprocess.DEVNULL).returncode != 0:
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--edition', required=True, help='the edition to slim (top folder or install root)')
    ap.add_argument('--base', required=True, help='the edition it leans on (top folder or install root)')
    ap.add_argument('--base-id', required=True, choices=['2017', '2019'], help="the base's edition")
    ap.add_argument('--out', required=True, help='output folder (the slim copy goes inside it)')
    a = ap.parse_args()
    ed_root, base_root = install_root(a.edition), install_root(a.base)
    out = os.path.join(a.out, os.path.basename(os.path.normpath(a.edition)))
    if os.path.exists(out):
        sys.exit('%s exists; remove it first' % out)
    # The install root inside the top folder (the same place, for 2017).
    out_root = os.path.join(out, os.path.relpath(ed_root, a.edition))
    base = files(base_root)
    shared, kept, saved, total = [], 0, 0, 0
    for d, _, fs in os.walk(ed_root):
        rel_dir = os.path.relpath(d, ed_root)
        os.makedirs(os.path.join(out_root, rel_dir), exist_ok=True)  # the whole folder tree, empty or not
        for f in fs:
            src = os.path.join(d, f)
            rel = os.path.normpath(os.path.join(rel_dir, f)).replace(os.sep, '/')
            size = os.path.getsize(src)
            total += size
            other = base.get(rel.lower())
            if other and os.path.getsize(other) == size and digest(other) == digest(src):
                shared.append(rel)
                saved += size
            else:
                clone(src, os.path.join(out_root, rel))
                kept += size
    with open(os.path.join(out_root, 'dcusa-overlay.txt'), 'w') as f:
        f.write('# Files shared with the %s install beside this one (tools/overlay/make_overlay.py).\n' % a.base_id)
        f.write('base %s\n' % a.base_id)
        f.write('\n'.join(sorted(shared)) + '\n')
    g = 1 << 30
    print('%s: %.2f GB kept, %.2f GB (%d files) read from the %s install, of %.2f GB'
          % (out, kept / g, saved / g, len(shared), a.base_id, total / g))


if __name__ == '__main__':
    main()
