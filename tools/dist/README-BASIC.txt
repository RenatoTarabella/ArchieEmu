ArchieEmu - BBC BASIC V machine (portable, Windows 64-bit)
==========================================================

The original BBC BASIC V 1.87 module running on an emulated 8 MHz ARM2, with
RISC OS calls handled by the emulator. No installation and no ROM needed:
unzip anywhere and run armwin.exe.

Try the demos in the "disc" folder:  CHAIN "Ray"   CHAIN "mandel"
CHAIN "Harmonograph"

In the window
  Escape     interrupt the program
  Ctrl+V     paste a listing
  F12        toggle 8 MHz ARM2 speed / turbo

Options: --mhz 25 (an ARM3), --turbo, --disc <folder>, --mode <n>, --ram <MB>
Truecolour: MODE "X640 Y480 C16M" or MODE 49, then COLOUR r,g,b / GCOL r,g,b

Files
  The "disc" folder is the BASIC disc. The RISC OS file type is a suffix:
  SAVE "test" creates disc\test,ffb. TEXTSAVE / TEXTLOAD use plain text
  listings you can edit in Notepad. *CAT, *EX, *DELETE, *RENAME, *CDIR, *TYPE.

Source code, documentation and bug reports:
  https://github.com/RenatoTarabella/ArchieEmu

ArchieEmu is MIT licensed (LICENSE). BBC BASIC V comes from RISC OS Open and is
under the Apache 2.0 licence (third_party\riscos\LICENSE).
