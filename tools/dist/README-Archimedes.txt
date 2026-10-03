ArchieEmu - Acorn Archimedes and Risc PC (portable, Windows 64-bit)
===================================================================

Low-level Acorn machines running the real RISC OS ROMs:
- an Archimedes A3000/A310 (ARM2, MEMC, IOC, VIDC, floppy, sound) with Arthur
  and RISC OS up to 3.11;
- a Risc PC (ARM610, ARM710 or StrongARM, IOMD, VIDC20, PS/2 keyboard, PC
  floppy, IDE hard disc, 8 and 16-bit sound) with RISC OS 3.5, 3.6 and 3.7.

THE RISC OS ROMS ARE NOT INCLUDED (they are copyrighted). Put your ROM images
in the "roms" folder (any subfolder), named by version, for example:

    roms\1. Major\ROM311     RISC OS 3.11 (Archimedes)
    roms\1. Major\ROM350     RISC OS 3.50 (Risc PC; also ROM360, ROM370, ROM371)

At startup a dialog asks which machine to start, with ROM, processor, RAM and
VRAM; the choice is remembered (ArchieEmu.ini) and Machine > Choose another
machine... brings the dialog back. archie.exe --rom path\to\ROM311 skips it.
The CMOS settings are saved next to each ROM (ROM311.cmos, ROM350.cmos...).

Risc PC: the StrongARM needs RISC OS 3.7, as on the real machine. Disc > New
hard disc image creates a formatted hard disc (64-512 MB) that appears as
HardDisc4; Disc > Hard disc image attaches an existing .hdf. The keyboard is a
UK PC keyboard; symbols are translated from your Windows layout. Archimedes
games often do not run on a Risc PC (as on the real one); applications usually
do. Not emulated yet: CD-ROM, network, serial port; RISC OS 3.8 does not boot.

HostFS: the "HostFS" folder next to archie.exe is a disc in RISC OS, with its
own icon on the icon bar. Copy RISC OS software into it from Windows, or save
into it from RISC OS: the files are shared both ways. File types are suffixes
in the host names, as in RPCEmu: "Game,ff8" is Absolute, "Prog,ffb" is BASIC.
A "!System" folder at the top of HostFS is registered when you open it (some
software needs one). To unpack a floppy image into HostFS, with the right types,
use tools/adfextract.py from the source code on GitHub.
On the Risc PC, HostFS is the boot disc: its !Boot loads the monitor
definition (Monitors.ArchieEmu) that the Display Manager needs to change the
screen mode. With a CMOS saved by an older version, type once at the F12
command line:  *Configure FileSystem HostFS  and  *Configure Boot

Floppy disc images (.adf, ADFS 800 KB, or .hfe flux images, which keep the
original copy protection; on the Risc PC also 1.6 MB and DOS discs) can go
in the "ADF" folder. Insert one from the Disc menu or by dropping the file on
the window, or from the command line:
  archie.exe --floppy game.adf --floppy2 disc2.adf

Commands are in the window's menu bar: Disc (floppies, hard disc), Machine
(Turbo, Sound, RAM, Reset, Choose another machine) and Mouse. No key is taken
away from RISC OS.
  Ctrl+Alt     pressed and released alone: free the mouse (click in the window
               to capture it again)
  Ctrl+Break   (Ctrl+Pause) reset, as on the real machines

Mouse as on the Archimedes: left = Select, middle = Menu, right = Adjust. For a
mouse without a middle button: Mouse > Right button is Menu (or --right-menu).

Desktop applications must be started by double-clicking them in the Filer:
from the F12 command line they stop with "Wimp is currently active".

Tested: Zarch, Pacmania, Elite, Genesis Professional 3.04.

Source code, documentation and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE).
