ArchieEmu - Acorn Archimedes A3000/A310 (portable, Windows 64-bit)
==================================================================

A low-level Archimedes: the ARM2 runs the real RISC OS ROM and talks to the
emulated MEMC, IOC and VIDC chips, floppy drives, keyboard, mouse and sound.

THE RISC OS ROM IS NOT INCLUDED (it is copyrighted). Put your RISC OS 3.11 ROM
image, as a single 2 MB file, here:

    roms\1. Major\ROM311

or start the emulator with:  archie.exe --rom path\to\ROM311
The CMOS settings are saved next to the ROM (ROM311.cmos).

Floppy disc images (.adf, ADFS 800 KB) can go in the "ADF" folder. Insert one
with Ctrl+F9 or by dropping the file on the window, or from the command line:
  archie.exe --floppy game.adf --floppy2 disc2.adf

Keys
  Ctrl+F9          choose a floppy image for drive 0 (with Shift: drive 1)
  Ctrl+F8          eject
  Ctrl+F10         sound on/off
  Ctrl+F11         release the mouse (click in the window to capture it)
  Ctrl+F12         turbo
  Ctrl+Shift+F12   reset

Desktop applications must be started by double-clicking them in the Filer:
from the F12 command line they stop with "Wimp is currently active".

Tested: Zarch, Pacmania, Elite, Genesis Professional 3.04.

Source code, documentation and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE).
