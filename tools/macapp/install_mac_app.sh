#!/bin/zsh
# Builds "Daytona USA.app" from build/mac-host/dcrun and installs it in
# /Applications (or the folder given as the first argument).
#
# The app holds a copy of dcrun, the app icon and the boot screen's titles and
# font (made from the player's files, see README "Playing"); it reads the
# prepared caches and shaders from this checkout, so saves stay in the caches'
# container folders. With both editions built in and both caches there, it
# opens on the edition picker. The game folder is the sibling
# "Daytona Championship USA".
# Messages go to ~/Library/Logs/DaytonaUSA.log.
set -e
repo=${0:A:h:h:h}
dest=${1:-/Applications}
app="$dest/Daytona USA.app"
prepared=$repo/build/mac-host/prepared
content=${DCUSA_CONTENT:-${repo:h}/Daytona Championship USA}
[[ -x $repo/build/mac-host/dcrun ]] || { echo "build dcrun first (cmake --build --preset mac-host)"; exit 1 }
[[ -d $content ]] || { echo "no game folder at $content (set DCUSA_CONTENT)"; exit 1 }

work=$(mktemp -d)
trap 'rm -rf $work' EXIT
stage="$work/Daytona USA.app/Contents"
mkdir -p $stage/MacOS $stage/Resources

cp $repo/build/mac-host/dcrun $stage/MacOS/dcrun
mkdir -p $stage/Resources/boot
for f in title.png title2017.png font.ttf; do
    [[ -f $prepared/boot/$f ]] && cp $prepared/boot/$f $stage/Resources/boot/$f
done

# The icon: the iOS app's 1024 px image at the sizes an .icns holds.
icon=$repo/src/platform/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
iconset=$work/AppIcon.iconset
mkdir -p $iconset
for s in 16 32 128 256 512; do
    sips -z $s $s $icon --out $iconset/icon_${s}x${s}.png >/dev/null
    sips -z $((s * 2)) $((s * 2)) $icon --out $iconset/icon_${s}x${s}@2x.png >/dev/null
done
iconutil -c icns $iconset -o $stage/Resources/AppIcon.icns

cat > $stage/MacOS/DaytonaUSA <<EOF
#!/bin/zsh
# Starts the game: dcrun with this checkout's prepared cache and shaders.
here=\${0:A:h}
res=\$here/../Resources
export DCUSA_SHADERS="$prepared/shaders"
export DCUSA_BOOT_SCREEN=\${DCUSA_BOOT_SCREEN:-1}
# defaults write local.dcusa.mac ShowFPS -bool true: the frame rate and build, lower left.
[[ \$(defaults read local.dcusa.mac ShowFPS 2>/dev/null) == 1 ]] && export DCUSA_SHOW_FPS=\${DCUSA_SHOW_FPS:-1}
# The editions' titles (title.png, title2017.png): the picker and the boot screen.
export DCUSA_TITLE_DIR=\${DCUSA_TITLE_DIR:-\$res/boot}
[[ -f \$res/boot/font.ttf ]] && export DCUSA_BOOT_SCREEN_FONT=\${DCUSA_BOOT_SCREEN_FONT:-\$res/boot/font.ttf}
mkdir -p ~/Library/Logs
# From Finder the working directory is /, where the game's boot never finished.
cd "$repo"
exec \$here/dcrun --content "$content" "$prepared" >> ~/Library/Logs/DaytonaUSA.log 2>&1
EOF
chmod +x $stage/MacOS/DaytonaUSA

version=$(git -C $repo rev-parse --short HEAD 2>/dev/null || echo dev)
cat > $stage/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>Daytona USA</string>
    <key>CFBundleName</key><string>Daytona USA</string>
    <key>CFBundleExecutable</key><string>DaytonaUSA</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleIdentifier</key><string>local.dcusa.mac</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <!-- App Nap stretches the loader threads' sleeps and timers: the boot ran
         three times slower launched from Finder than from a shell. -->
    <key>NSAppSleepDisabled</key><true/>
    <key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
</dict>
</plist>
EOF
codesign --force --deep --sign - "$work/Daytona USA.app" >/dev/null 2>&1 || true

rm -rf "$app"
ditto "$work/Daytona USA.app" "$app"
touch "$app"   # Finder picks up the new icon
echo "installed $app ($version)"
