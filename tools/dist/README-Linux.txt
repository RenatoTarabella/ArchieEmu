ArchieEmu for Linux (x86-64, portable)
=====================================

Needs SDL2, which most desktops already have. If the program does not start:
  Debian/Ubuntu:  sudo apt install libsdl2-2.0-0
  Fedora:         sudo dnf install SDL2
  Arch:           sudo pacman -S sdl2
Built for glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35 and newer).
Unpack anywhere and run ./armwin or ./archie from the folder.

This is the first Linux version and it has not been tested on a Linux desktop
yet (only built and run without a screen). Please report problems with the
keyboard, the mouse, the menus or the file chooser on GitHub.

ArchieEmu BASIC (./armwin)
--------------------------
The original BBC BASIC V running on an emulated 8 MHz ARM2. No ROM needed.
The "disc" folder is your BASIC disc, with the demos:
  CHAIN "Ray"   CHAIN "mandel"   CHAIN "Harmonograph"   CHAIN "Juggler"

  Escape       interrupt the program
  F12          toggle 8 MHz ARM2 speed / turbo
  Ctrl+C       copy the text selected with the mouse (double click: a word)
  Ctrl+V       paste a listing (also the right button)

SAVE "test" creates disc/test,ffb (the RISC OS file type is a suffix).
TEXTSAVE / TEXTLOAD use plain text listings.

ArchieEmu Archimedes (./archie)
-------------------------------
Low-level Acorn machines running the real RISC OS ROMs: an Archimedes
A3000/A310 (ARM2) with Arthur and RISC OS up to 3.11, and a Risc PC (ARM610,
ARM710 or StrongARM, PC floppy, IDE hard disc, 16-bit sound) with RISC OS 3.5,
3.6 and 3.7.

THE RISC OS ROMS ARE NOT INCLUDED (they are copyrighted). Put your ROM images
in the "roms" folder (any subfolder), named by version: ROM311 for RISC OS
3.11, ROM350, ROM360, ROM370, ROM371 for the Risc PC. ~/Documents/ArchieEmu/roms
works too. At startup a dialog asks which machine to start, with ROM,
processor, RAM and VRAM; the choice is remembered in
~/.config/ArchieEmu/ArchieEmu.ini and Machine > Choose another machine...
brings the dialog back. The CMOS settings are saved next to each ROM.

The commands are in the menu bar at the top of the window: Disc (floppies,
hard disc), Machine (Turbo, Sound, RAM, Reset, Choose another machine, Quit)
and Mouse. Click the screen to capture the mouse; Ctrl+Alt pressed and
released alone frees it again, so the menu bar can be used. No key is taken
away from RISC OS. Right Alt works as AltGr.

Mouse as on the Archimedes: left = Select, middle = Menu, right = Adjust. For a
mouse without a middle button: Mouse > Right button is Menu.

HostFS: the "HostFS" folder is a disc in RISC OS, with its own icon on the
icon bar. Copy RISC OS software into it, or save into it from RISC OS: the
files are shared both ways. File types are suffixes in the host names, as in
RPCEmu: "Game,ff8" is Absolute, "Prog,ffb" is BASIC. On the Risc PC, HostFS is
the boot disc: its !Boot loads the monitor definition (Monitors.ArchieEmu)
that the Display Manager needs to change the screen mode.

Floppy images (.adf, or .hfe flux images that keep the original copy
protection; on the Risc PC also 1.6 MB and DOS discs) can go in the "ADF"
folder. Insert one from the Disc menu, by dropping it on the window (Shift for
drive :1), or from the command line:
  ./archie --floppy game.adf --floppy2 disc2.adf
The file chooser uses zenity or kdialog when installed, otherwise a list
inside the window.

Risc PC: the StrongARM needs RISC OS 3.7, as on the real machine. Disc > New
hard disc creates a formatted hard disc (64-512 MB) that appears as
HardDisc4. Archimedes games often do not run on a Risc PC (as on the real
one); applications usually do. Not emulated yet: CD-ROM, network, serial
port; RISC OS 3.8 does not boot.

Desktop applications must be started by double-clicking them in the Filer:
from the F12 command line they stop with "Wimp is currently active".

Source code, Windows and Mac versions, bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE). BBC BASIC V comes from RISC OS Open
under the Apache 2.0 licence.
