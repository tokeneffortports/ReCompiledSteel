RECOMPILED STEEL
================
Real Steel, recompiled: a native PC port of the Xbox 360 game.

This is Real Steel (Xbox 360, 2011) recompiled to run natively on your
computer. It contains no game data: you need your own copy of the game.

SETUP
-----
1. Copy your Real Steel files from the Xbox 360 to this PC. On the console
   they are under  Content\0000000000000000\584111E0\
       000D0000\...   the game (one file, about 200 MB)
       000B0000\...   the title update (required)
       00000002\...   the add-on packs (optional)
   Get them off with FTP from a console running Aurora or FSD, or with a PC
   tool that reads Xbox 360 storage (Party Buffalo or Xplorer360 for a hard
   drive, USBXTAFGUI for a USB stick; Windows cannot read a console-
   formatted stick by itself). The setup finds the right files in whatever
   folder you copied them to.
2. Windows: run RealSteelSetup.exe in this folder.
   Linux / Steam Deck: run realsteel-setup (in Desktop Mode on the Deck).
   Point it at the folder you copied and press Install.
3. Windows: start realsteel.exe.   Linux: start.sh
   Steam Deck: add start.sh to Steam as a non-Steam game and launch it from
   Gaming Mode.

The game opens windowed at 1280x720 (F11 switches to fullscreen). Settings
are in the game's own menus: HELP AND OPTIONS > GAME OPTIONS, then LB / RB
(or the tabs above the panel) for display, graphics, audio, controls and
online play. F1 (or clicking both sticks) opens the same settings as an
overlay anywhere in the game.
Saves are in Documents\realsteel (Windows) or ~/.local/share/realsteel
(Linux). Settings are saved in realsteel_settings.cfg beside the game.

CONTROLS (keyboard defaults, change them on the settings page)
--------
  Move: W A S D      Punches: J K      Guard / dodge: Shift, Ctrl
  Special / taunt: U I      Camera: arrow keys      Confirm: Space
  Back: Backspace    Menu: Enter    Settings: F1    Quit: main menu > QUIT GAME

MULTIPLAYER
-----------
The MULTIPLAYER menu finds other players through a lobby server: enter its
address on the panel next to the menu (or Settings > Game > Lobby server).
Whoever runs the server you play on gives you the address; to run one for
your friends, see LOBBY_SERVER.md and tools/lobby/realsteel_lobby.py (one
Python file). Without a server, matches are found on the local network, or
by typing the host's public address (the host forwards the UDP port shown).
Both players need the same add-ons and mods; the panel next to the MULTIPLAYER
menu says which mods are missing when a match is hidden.

EXTRAS
------
F12 saves a screenshot to the screenshots folder. Every launch copies your
saves to backups\ next to them (newest five kept). The settings page has a
Cheats tab (game speed, cash) and a Language choice (English / Japanese).

MODS
----
Drop a robot folder into mods/ and start the game: see MODDING.md. Delete
the folder to remove it again.

FIRST RUN ON WINDOWS
--------------------
Windows shows "Windows protected your PC" for an unsigned download like this
one: click "More info", then "Run anyway". The Visual C++ runtime files the
game needs are in this folder, nothing else has to be installed.

PROBLEMS
--------
rex_latest.log and rs_native_print.log beside the game say what happened.
Source code, updates, issues and credits:
https://github.com/tokeneffortports/ReCompiledSteel
ReCompiled Steel is free software under the GPL-3.0 (LICENSE); the
licences of the components it is built on are in the licenses folder. The
game itself stays Yuke's and DreamWorks'.
