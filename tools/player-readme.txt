LEGO Racers 2 - native port
===========================

  !! UNOFFICIAL FAN PROJECT !!
  This is not affiliated with, sponsored, endorsed or supported by the
  LEGO Group, Attention to Detail or any other rights holder of LEGO
  Racers 2. LEGO is a trademark of the LEGO Group. No game files are
  included: you need your own copy of the original CD.

Newer versions: https://github.com/Spaggel/lr2/releases

This runs LEGO Racers 2 (2001) as a native program for Windows, macOS and
Linux: the game's code was recompiled, and Direct3D 8, DirectInput and
DirectSound are replaced with SDL3 and OpenGL. It adds native resolution
and widescreen, savestates, modern gamepad support and HD textures.
Nothing needs to be installed.

The game's own files (levels, cars, music, movies) are not included: you
need your own LEGO Racers 2 CD, or a disc image (.iso) made from it.


Setting up
----------

1. Unpack the download into a folder of its own, for example in
   Documents. The game keeps its files in a "game" folder next to itself.

   macOS - IMPORTANT: the game will not work until you run one command,
   because it is not signed by Apple. Open Terminal (Applications >
   Utilities > Terminal), type the following with a space at the end,
   but do not press Enter yet:

       xattr -cr

   Then drag the LEGO Racers 2 app from the unpacked folder into the
   Terminal window (this types its location) and press Enter. The full
   command looks like:

       xattr -cr LEGO\ Racers\ 2.app

   Do this again if you download the game again.

2. Start the game: lr2.exe on Windows, LEGO Racers 2 on macOS, lr2 on
   Linux.

3. The first time, a window asks for your CD. Drag one of these into it:
   - your disc image (.iso),
   - the CD itself (its drive icon), or
   - a folder with a copy of everything on the CD.
   Or click the window and choose the .iso. If you put the .iso next to
   the game before starting it, it is used automatically.

   The files are copied into the "game" folder (about 500 MB, under a
   minute); after that the CD or .iso is no longer needed and the game
   starts by itself.

Windows may say "Windows protected your PC" because lr2.exe is not signed:
click "More info" and then "Run anyway". On Windows on ARM the game needs
OpenGL 3.3; if it does not start, install the "OpenCL, OpenGL, and Vulkan
Compatibility Pack" from the Microsoft Store.

macOS: if it says the app "is damaged", "cannot be opened" or "cannot
be verified", or the game cannot save, you skipped the xattr command in
step 1: run it, then open the app again.

The HD texture pack is built in. To play with the original textures, start
the game with --no-hd (see below).


Playing
-------

The game starts full screen at your screen's native resolution. To play
in a window, start it with --windowed (on Windows: make a shortcut to
lr2.exe and add --windowed to its target, for example
"C:\Games\LR2-windows-x64\lr2.exe" --windowed).

Keyboard: the arrow keys steer, accelerate (Up) and brake (Down); Enter
confirms and Esc goes back. All controls can be changed under Game Options.

Controller: connect it before starting the game (the game only looks for
controllers when it starts). Xbox, PlayStation, Switch Pro and most other
pads work; buttons are named by their Xbox position. The first controller
drives player 1, a second one player 2, and the keyboard keeps working
alongside.

    Racing                              Menus
    Steer            left stick, d-pad  Move           left stick, d-pad
    Accelerate       RT                 Confirm        A
    Brake / reverse  LT                 Back           B
    Hard brake       B                  Pause, resume  Start
    Fire weapon      A
    Brick boost      X
    Talk             Y
    Look behind      LB or click the right stick
    Next target      RB or d-pad down
    Previous target  d-pad up
    Pause            Start

This layout applies while the player's controls are set to Keyboard in the
game's options, which is the default. Choosing Joystick there switches to
the game's own button setup.

Savestates capture the whole game, mid-race included:

    Save to the current slot    F5    hold Back/View, press LB
    Load the current slot       F9    hold Back/View, press RB
    Previous / next slot        F6 / F7

There are 10 slots (0-9); the current slot is 0 when the game starts.


Files
-----

Saved games, settings and savestates are in game/save. If something goes
wrong, game/save/lr2.log has the program's log. To copy the game files
again (from another disc image, say), start the game with --setup.

Command-line options (--help lists them):

    --windowed       run in a window instead of full screen
    --setup [ISO]    copy the game files from your CD again
    --no-hd          use the original textures
    --hd DIR         use an HD texture pack from another folder
    --save DIR       where saves and settings go


Credits
-------

LEGO Racers 2 was made by Attention to Detail and LEGO Software. LEGO is a
trademark of the LEGO Group, which does not sponsor or endorse this port.
The HD textures are "Higher resolution textures" by dorpz, from
https://rockraidersunited.com. The game uses SDL3 and zlib; CREDITS.txt
lists everyone whose work this builds on, and the LICENSES folder holds
the license texts.
