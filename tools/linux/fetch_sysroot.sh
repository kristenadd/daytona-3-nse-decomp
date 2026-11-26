#!/bin/sh
# The Steam Runtime 3 ("sniper") SDK sysroot for cross-compiling the Linux
# (Steam Deck) build on a Mac (cmake/linux-x86_64.cmake): downloaded from
# Valve, unpacked to build/toolchains/sniper and made usable from outside a
# Linux system (absolute symbolic links made relative; the x86_64 loader link
# the linker scripts name). About 1.2 GB to download, 3.7 GB unpacked.
#
#   brew install llvm lld glslang
#   tools/linux/fetch_sysroot.sh
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
dest="$root/build/toolchains/sniper"
url="https://repo.steampowered.com/steamrt-images-sniper/snapshots/latest-public-stable/com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz"
mkdir -p "$root/build/toolchains"
if [ ! -d "$dest/usr" ]; then
    tarball="$root/build/toolchains/sniper-sdk.tar.gz"
    [ -f "$tarball" ] || curl -fL -o "$tarball" "$url"
    mkdir -p "$dest"
    # A few headers differ only in case (FLTK's): APFS keeps one of each; the port uses none.
    tar -xzf "$tarball" -C "$dest" 2>/dev/null || true
fi
python3 - "$dest" <<'PY'
import os, sys
root = sys.argv[1]
fixed = 0
for d, dirs, files in os.walk(root):
    for name in dirs + files:
        p = os.path.join(d, name)
        if not os.path.islink(p):
            continue
        target = os.readlink(p)
        if not target.startswith('/'):
            continue
        rel = os.path.relpath(os.path.join(root, target.lstrip('/')), d)
        os.remove(p)
        os.symlink(rel, p)
        fixed += 1
os.makedirs(os.path.join(root, 'lib64'), exist_ok=True)
loader = os.path.join(root, 'lib64', 'ld-linux-x86-64.so.2')
if not os.path.lexists(loader):
    os.symlink('../lib/x86_64-linux-gnu/ld-2.31.so', loader)
print('sysroot %s: %d absolute links made relative' % (root, fixed))
PY
