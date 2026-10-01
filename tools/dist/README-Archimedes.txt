ArchieEmu - Acorn Archimedes A3000/A310 (portable, Windows 64-bit)
==================================================================

A low-level Archimedes: the ARM2 runs the real RISC OS ROM and talks to the
emulated MEMC, IOC and VIDC chips, floppy drives, keyboard, mouse and sound.

THE RISC OS ROM IS NOT INCLUDED (it is copyrighted). Put your RISC OS 3.11 ROM
image, as a single 2 MB file, here:

    roms\1. Major\ROM311

or start the emulator with:  archie.exe --rom path\to\ROM311
The CMOS settings are saved next to the ROM (ROM311.cmos).

HostFS: the "HostFS" folder next to archie.exe is a disc in RISC OS, with its
own icon on the icon bar. Copy RISC OS software into it from Windows, or save
into it from RISC OS: the files are shared both ways. File types are suffixes
in the host names, as in RPCEmu: "Game,ff8" is Absolute, "Prog,ffb" is BASIC.
A "!System" folder at the top of HostFS is registered when you open it (some
software needs one). To unpack a floppy image into HostFS, with the right types,
use tools/adfextract.py from the source code on GitHub.

Floppy disc images (.adf, ADFS 800 KB, or .hfe flux images, which keep the
original copy protection) can go in the "ADF" folder. Insert one
with Ctrl+F9 or by dropping the file on the window, or from the command line:
  archie.exe --floppy game.adf --floppy2 disc2.adf

Commands are in the window's menu bar: Disc (insert, eject), Machine (Turbo,
Sound, Reset) and Mouse. No key is taken away from RISC OS.
  Ctrl+Alt     pressed and released alone: free the mouse (click in the window
               to capture it again)
  Ctrl+Break   (Ctrl+Pause) reset, as on the Archimedes

Mouse as on the Archimedes: left = Select, middle = Menu, right = Adjust. For a
mouse without a middle button: Mouse > Right button is Menu (or --right-menu).

Desktop applications must be started by double-clicking them in the Filer:
from the F12 command line they stop with "Wimp is currently active".

Tested: Zarch, Pacmania, Elite, Genesis Professional 3.04.

Source code, documentation and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE).
