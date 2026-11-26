Daytona Championship USA for the Steam Deck (Linux, x86_64, Vulkan)
=====================================================================

1. Copy this folder (daytona-usa) anywhere in your home folder, for example
   /home/deck/Games/daytona-usa.

2. Put the game folders in daytona-usa/games, or link them there, for
   example (in Konsole, Desktop Mode):

     ln -s "/home/deck/Games/Daytona Championship USA New Season Edition (2019)[Sega PC][TP]" games/
     ln -s "/home/deck/Games/Daytona Championship USA (2017-12-21)[Sega PC][TP]" games/

   or set DCUSA_GAMES to the folder that holds them. These are the same
   folders TeknoParrot uses; nothing in them is changed.

3. Try it in Desktop Mode first: double-click run.sh (or run ./run.sh in
   Konsole). With both editions present the edition picker opens: left /
   right on the stick or d-pad, A or Start to choose.

4. For Gaming Mode: in Steam (Desktop Mode), Games > Add a Non-Steam Game,
   Browse to run.sh, Add. Leave its compatibility setting off (it is a
   native Linux program, not a Windows one).

Controls (Steam Input's default gamepad layout):
   Left stick          steer
   R2 / L2             gas / brake
   R1 / L1             shift up / down
   Y X B A             the four view buttons
   Start (menu)        start
   Select (view)       coin
   R3 (click R stick)  frame-rate counter on / off
   F11 / Alt+Return    full screen on / off (keyboard); Ctrl+Q quits

If something goes wrong, log.txt beside run.sh has this run's messages;
send it back. The game writes its saves and settings to
prepared/container (2019) and prepared/container-2017.
