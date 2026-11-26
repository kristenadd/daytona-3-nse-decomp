#!/bin/sh
# Daytona Championship USA on Linux / the Steam Deck. Put (or link) the game
# folders in games/ beside this script, or set DCUSA_GAMES to the folder that
# holds them. With both editions there, the edition picker opens first.
# What the game writes goes to prepared/container (2019) and
# prepared/container-2017; this run's messages to log.txt.
dir="$(cd "$(dirname "$0")" && pwd)"
games="${DCUSA_GAMES:-$dir/games}"
export DCUSA_FULLSCREEN="${DCUSA_FULLSCREEN:-1}"
export DCUSA_SHOW_FPS="${DCUSA_SHOW_FPS:-1}"
cd "$dir"
exec "$dir/dcrun" --content "$games" "$@" "$dir/prepared" > "$dir/log.txt" 2>&1
