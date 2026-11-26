GT Racing: Motor Academy HD - PC port 1.0.1 (Windows, x86)
==========================================================

A static recompilation of the iPad game (free edition, version 1.3.4b) that runs on Windows.

No game files come with this folder: no art, sound, menus or data of the game.  You need your own copy of the
iPad game.


Setting up
----------
1. Copy the contents of the GTRacingFreemiumHD.app folder from your own copy of the iPad game into the empty
   GTRacingFreemiumHD.app folder here (its note, PUT THE GAME HERE.txt, says what goes in).
2. Start gtracing.exe.

Updating from 1.0.0: unpack this zip to a new folder, put the game in it as above, then copy home\, gtracing.ini,
patch\ and add-on\ over from the 1.0.0 folder.  Saves and settings are unchanged.

The folder then looks like this:
    gtracing.exe
    SDL3.dll
    gtracing.ini
    GTRacingFreemiumHD.app\      (your copy of the game; empty in the zip)
    patch\                       (optional: the PC menus, see below; empty in the zip)
    add-on\                      (optional: extra content, see below; empty in the zip)
    home\                        (made at the first start: your save)
    IMPORTANT.TXT                (the link to the notes and downloads)

Each empty folder holds a short note on what goes in it.  The game never reads the notes.


Optional folders: patch\ and add-on\
------------------------------------
Two folders next to gtracing.exe add to the game.  The game runs without them.
    patch\       the PC menus: Options > Display, the keys page in Options > Controls, and the Exit and Name
                 tiles
    add-on\      extra content, one folder per add-on: more cars (Dodge, Ferrari, Hyundai), the paid edition's
                 extra licence tests and its 44 kHz audio

The notes and downloads for both are at:
    http://tiny.cc/gtracingpc

An add-on is turned off by deleting (or moving out) its folder in add-on\.  A save that holds an add-on car still
loads without that add-on: the car is hidden until the folder is back.


Settings
--------
gtracing.ini holds the settings: the picture (size, screen shape, full screen, anti-aliasing and more) and the
mouse pointer's size.  The defaults: the iPad 3's art, drawn at your monitor's
resolution and shape, full screen, 4x anti-aliasing.  A value the game cannot read as a number takes its
default, and a number outside a setting's range takes the nearest allowed value.

Alt+Enter switches between full screen and a window.  A keyboard, a mouse and gamepads all work.

{PC_MENUS}


Files the game writes
---------------------
home\            your save.  Copy this folder to keep a backup.
gtracing.log     what happened in the last run, for bug reports.
gtracing.dmp     only after a crash, for bug reports.

What the log and the crash dump never hold: the names of your folders, your Windows user name, your PC's name
and your public internet address.  Each is written as a tag (%USERPROFILE%, %EXE%, %USER%, %PC%, %IP%, ...) or
blanked out, so both files are safe to send with a bug report.  To write no log at all, add GT_LOG=0 to
gtracing.ini.

The game connects to the internet only for two things.  Multiplayer (Local, Bluetooth and Online with room
codes) talks to the other players' PCs, and to your own router to open a port.  And the Red Bull leaderboard
asks its server for the best times when you open that page (at most once a minute) and sends it your best
lap: the time, the name you go by and a random player number the game made; never your Windows user name, your
PC's name or your folders.  To keep the leaderboard offline (this PC and the friends you race in multiplayer
only), add this line to gtracing.ini:
    GT_CLOUD_BOARD=0
