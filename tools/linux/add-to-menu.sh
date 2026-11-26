#!/bin/sh
# Adds "Daytona Championship USA" to the desktop's application menu, pointing
# at run.sh in this folder. Steam's "Add a Non-Steam Game" lists the menu's
# programs, so it can then be ticked there (Steam's file browser does not
# accept a .sh file). Run it again if this folder moves.
dir="$(cd "$(dirname "$0")" && pwd)"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
icon="$dir/prepared/boot/title.png"
cat > "$apps/daytona-usa.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Daytona Championship USA
Comment=Native Linux build
Exec="$dir/run.sh"
Path=$dir
Icon=$icon
Terminal=false
Categories=Game;
DESKTOP
chmod +x "$dir/run.sh" "$dir/dcrun" "$apps/daytona-usa.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1
echo "Added $apps/daytona-usa.desktop"
echo "In Steam: Games > Add a Non-Steam Game, tick Daytona Championship USA, Add Selected Programs."
