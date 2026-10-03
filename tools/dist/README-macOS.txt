ArchieEmu for macOS (Apple Silicon and Intel, macOS 11 or later)
================================================================

Drag both applications to the Applications folder.

ArchieEmu BASIC
---------------
The original BBC BASIC V running on an emulated 8 MHz ARM2. No ROM needed.
Your BASIC disc is the folder Documents/ArchieEmu/disc (created on first run
with the demos): try  CHAIN "Ray"   CHAIN "mandel"   CHAIN "Harmonograph"

  Escape       interrupt the program
  Cmd+T / F12  toggle 8 MHz ARM2 speed / turbo
  Cmd+C        copy the text selected with the mouse
  Cmd+V        paste a listing

SAVE "test" creates Documents/ArchieEmu/disc/test,ffb (the RISC OS file type
is a suffix). TEXTSAVE / TEXTLOAD use plain text listings.

ArchieEmu Archimedes
--------------------
Low-level Acorn machines running the real RISC OS ROMs: an Archimedes
A3000/A310 (ARM2) with Arthur and RISC OS up to 3.11, and a Risc PC (ARM610,
ARM710 or StrongARM, PC floppy, IDE hard disc, 16-bit sound) with RISC OS 3.5,
3.6 and 3.7.

THE RISC OS ROMS ARE NOT INCLUDED (they are copyrighted). Put your ROM images
in Documents/ArchieEmu/roms, named by version: ROM311 for RISC OS 3.11,
ROM350, ROM360, ROM370, ROM371 for the Risc PC. If there is none, the first
run asks for a RISC OS 3.11 ROM image and copies it there. At startup a dialog
asks which machine to start, with ROM, processor, RAM and VRAM; Machine >
Choose Another Machine... brings it back. The CMOS settings are saved next to
each ROM.

Risc PC: the StrongARM needs RISC OS 3.7, as on the real machine. Machine >
New Hard Disc Image creates a formatted hard disc (64-512 MB) that appears as
HardDisc4; Hard Disc Image attaches an existing .hdf. Archimedes games often
do not run on a Risc PC (as on the real one); applications usually do. Not
emulated yet: CD-ROM, network, serial port; RISC OS 3.8 does not boot.

HostFS: the folder Documents/ArchieEmu/HostFS is a disc in RISC OS, with its
own icon on the icon bar. Copy RISC OS software into it from the Mac, or save
into it from RISC OS: the files are shared both ways. File types are suffixes
in the host names, as in RPCEmu: "Game,ff8" is Absolute, "Prog,ffb" is BASIC.
A "!System" folder at the top of HostFS is registered when you open it.
On the Risc PC, HostFS is the boot disc: its !Boot loads the monitor
definition (Monitors.ArchieEmu) that the Display Manager needs to change the
screen mode. If your HostFS folder existed before this version, copy !Boot,feb
and Monitors from the app (Show Package Contents > Contents > Resources >
HostFS) into it, and with an older CMOS type once at the F12 command line:
*Configure FileSystem HostFS  and  *Configure Boot

Floppy images (.adf, or .hfe flux images that keep the original copy
protection) can go in Documents/ArchieEmu/ADF. Insert one from the
Machine menu, by dropping it on the window or by double-clicking it.

  Cmd+O / Cmd+Shift+O   insert a floppy in drive :0 / :1
  Cmd+E / Cmd+Shift+E   eject :0 / :1
  Cmd+T                 turbo
  Cmd+Shift+M           sound on/off
  Ctrl+Option or Cmd+Esc  free the mouse (click in the window to capture it)
  Cmd+Shift+R           reset

Mouse as on the Archimedes: left = Select, middle = Menu, right = Adjust. On a
trackpad or a two-button mouse, tick Machine > Right Button Is Menu.

Option works as AltGr for symbols and accented letters. On Apple keyboards
the function keys need fn: fn+F12 opens the RISC OS command line.
Desktop applications must be started by double-clicking them in the Filer.

Source code, Windows version and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE.txt). BBC BASIC V comes from RISC OS Open
under the Apache 2.0 licence.
