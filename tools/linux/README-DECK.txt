Daytona Championship USA for the Steam Deck (Linux, x86_64, Vulkan)
=====================================================================

1. Copy this folder (daytona-usa) anywhere in your home folder, for example
   /home/deck/Games/daytona-usa.

2. Let it find the game folders (the same ones TeknoParrot uses; nothing in
   them is changed). Any one of these:

   a. Links, in Dolphin (the file manager): open the folder that holds the
      game folders, drag a game folder onto daytona-usa/games in the side
      panel or another window, and choose "Link Here". Do it for each
      edition.

   b. Links, in Konsole:

        cd ~/Games/daytona-usa
        ln -s "/path/to/Daytona Championship USA New Season Edition (2019)[Sega PC][TP]" games/
        ln -s "/path/to/Daytona Championship USA (2017-12-21)[Sega PC][TP]" games/

      (Tip: type ln -s , drag the folder from Dolphin into Konsole to paste
      its path, then type games/ and press Enter.)

   c. No links: run it as  DCUSA_GAMES="/path/to/folder" ./run.sh , where
      that folder holds the game folders (it is searched a few levels down).

   A game on the microSD card works too: its folders are under
   /run/media/deck/<card name>/.

3. Try it in Desktop Mode first: double-click run.sh (or run ./run.sh in
   Konsole). With both editions present the edition picker opens: left /
   right on the stick or d-pad, A or Start to choose.

4. For Gaming Mode, add it to Steam (Desktop Mode):

   a. Double-click add-to-menu.sh (or run ./add-to-menu.sh in Konsole). It
      puts Daytona Championship USA in the application menu. Then in Steam:
      Games > Add a Non-Steam Game, tick Daytona Championship USA, Add
      Selected Programs.

   b. Or by hand: Games > Add a Non-Steam Game > Browse; change the file
      type at the bottom of the dialog to "All Files" and pick run.sh. If
      Steam still refuses it, add any program (dcrun), then in that
      shortcut's Properties set Target to the full path of run.sh, in
      quotes, and Start In to its folder.

   Leave its compatibility setting off: it is a native Linux program, not
   a Windows one.

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
