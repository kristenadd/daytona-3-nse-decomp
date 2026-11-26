#!/bin/sh
# The Steam Deck package: the Linux build (build/linux-deck/dcrun), the SDL3
# library from the Steam Runtime sysroot, the prepared caches, the Vulkan
# shaders and the title images, and run.sh. Nothing from the game folder is
# in it except what the player's own Mac prepared (caches, translated
# shaders, title images); the game folders themselves stay on the Deck.
#
#   tools/linux/package.sh [prepared folder] [out]
#   (defaults: build/mac-host/prepared, build/deck)
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
prepared="${1:-$root/build/mac-host/prepared}"
out="${2:-$root/build/deck}"
pkg="$out/daytona-usa"
sysroot="$root/build/toolchains/sniper"
rm -rf "$pkg"
mkdir -p "$pkg/lib" "$pkg/prepared/shaders/msl" "$pkg/games"
cp "$root/build/linux-deck/dcrun" "$pkg/"
cp -L "$sysroot/usr/lib/x86_64-linux-gnu/libSDL3.so.0" "$pkg/lib/"
for c in DCUSA2019.cache DCUSA2017.cache; do
    [ -f "$prepared/$c" ] && cp "$prepared/$c" "$pkg/prepared/"
done
# The Vulkan modules and their bindings only (no HLSL, no MSL).
find "$prepared/shaders/msl" -name '*.spv' -exec cp {} "$pkg/prepared/shaders/msl/" \;
find "$prepared/shaders/msl" -name '*.refl' -exec cp {} "$pkg/prepared/shaders/msl/" \;
[ -d "$prepared/boot" ] && cp -R "$prepared/boot" "$pkg/prepared/boot"
cp "$root/tools/linux/run.sh" "$pkg/run.sh"
cp "$root/tools/linux/add-to-menu.sh" "$pkg/add-to-menu.sh"
cp "$root/tools/linux/README-DECK.txt" "$pkg/README.txt"
chmod +x "$pkg/run.sh" "$pkg/dcrun" "$pkg/add-to-menu.sh"
(cd "$out" && tar -czf daytona-usa-deck.tar.gz daytona-usa)
echo "$out/daytona-usa-deck.tar.gz: $(du -h "$out/daytona-usa-deck.tar.gz" | cut -f1)"
echo "shaders: $(ls "$pkg/prepared/shaders/msl" | grep -c '\.spv$')"
