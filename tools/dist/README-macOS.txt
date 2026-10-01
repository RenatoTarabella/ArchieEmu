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
A low-level Acorn Archimedes running the real RISC OS ROM.

THE RISC OS ROM IS NOT INCLUDED (it is copyrighted). On first run you are asked
for your RISC OS 3.11 ROM image (a single 2 MB file): it is copied to
Documents/ArchieEmu/roms/ROM311, and the CMOS settings are saved next to it.

Floppy images (.adf) can go in Documents/ArchieEmu/ADF. Insert one from the
Machine menu, by dropping it on the window or by double-clicking it.

  Cmd+O / Cmd+Shift+O   insert a floppy in drive :0 / :1
  Cmd+E / Cmd+Shift+E   eject :0 / :1
  Cmd+T                 turbo
  Cmd+Shift+M           sound on/off
  Cmd+Esc               release the mouse (click in the window to capture it)
  Cmd+Shift+R           reset

Option works as AltGr for symbols and accented letters. On Apple keyboards
the function keys need fn: fn+F12 opens the RISC OS command line.
Desktop applications must be started by double-clicking them in the Filer.

Source code, Windows version and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE.txt). BBC BASIC V comes from RISC OS Open
under the Apache 2.0 licence.
