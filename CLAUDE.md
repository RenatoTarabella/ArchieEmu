# CLAUDE.md — working notes for ArchieEmu

Context for anyone (human or AI) working on this repository. The user-facing
overview is in `README.md`; this file holds the conventions, test workflow and
lessons learned that are not obvious from the code.

## Layout

- `src/cpu` ARMv2a core (26-bit PC+PSR, banks, exceptions, S/N/I cycles, 3-stage
  prefetch pipeline), `src/core` bus, `src/hle` + `src/riscos` RISC OS in HLE
  (kernel SWIs, VDU driver, HostFS, module loader), `src/machine` the BBC BASIC
  machine, `src/archie` the low-level Archimedes (MEMC1a, IOC, VIDC1a, keyboard,
  CMOS, WD1772), `src/frontend` Win32 and console front ends.
- Executables: `armwin` / `armbasic` (BASIC machine, window / console),
  `archie` (Archimedes window), `archie_boot` (headless Archimedes for
  diagnostics), `armemu`, `romdis`, `modinfo`.
- `disc/` is the BASIC machine's HostFS disc (`name,ffb` = tokenised BASIC).
- `roms/` (RISC OS ROMs) and `ADF/` (floppy images) exist only locally and are
  **git-ignored: never commit or redistribute them**. The default ROM is
  `roms/1. Major/ROM311`; the CMOS is saved beside it as `ROM311.cmos`.
- Source comments and commit messages are in Italian; README and this file are
  in English.

## Build and test

Windows only, no gcc: MSVC 2022 through CMake.

```
cmake --build build --config Release
build\Release\test_arm2.exe   (also test_basic, test_memc, test_vidc, test_kbd, test_cmos, test_fdc, test_keys_es)
.venv\Scripts\python tests\diff_unicorn.py     # ALU oracle against Unicorn
```

The linker fails if `archie.exe` / `armwin.exe` is still running: close it first.

**Test from the command line before handing anything over**; don't make the user
type long test lines. The tools for that:

- BASIC machine: pipe a script into `armbasic` (it reads lines as if typed):
  `armbasic --disc disc --png out.png --stats < script.txt`. `--stats` prints
  instructions, cycles and the time on a real 8 MHz ARM2. The PNG is the final
  framebuffer, so end with a `GET` or leave the program waiting for input.
  Write scripts to a file (Write tool), not through shell heredocs, when they
  contain backslashes or `%`.
- Archimedes: `archie_boot --rom "roms/1. Major/ROM311" --ms 60000 --floppy a.adf
  --floppy2 b.adf --png out.png --keys "{F12}run :0.!App{ENTER}"`. Key tokens:
  `{F12}` `{ENTER}` `{VKxx}` `{SVKxx}` `{ALTnnn}` `{RESET}` `{WAIT}` (5 s); typing
  starts at 15 s of emulated time. Other options: `--ram --cmos --trace-vectors
  --hist --trace-abort --wav`. It prints the POST report.
- Wimp applications must be started from the desktop: `*run` from the F12 command
  line fails with "Wimp is currently active". Use `filer_run :0.!App` instead.
  `filer_opendir` opens a directory window.
- `tools/mkadfs.py` builds ADFS D images from a directory, zip (RISC OS extra
  field; Implode via 7-Zip) or Spark/Arc archive.

## Timing model (don't break it)

- 8 MHz MEMC: N cycle = 2 ticks, ROM fetch = 3 ticks (325 ns as programmed by
  RISC OS), video DMA steals bandwidth (16% in 80 KB modes).
- The BASIC machine's HLE VDU costs (`COST_*` in `src/riscos/vdu.c`) and the
  per-SWI costs in `kernel.c` were calibrated against real RISC OS 3.11 running
  the same BASIC benchmarks (FOR/NEXT, VDU, GCOL, RECTANGLE) to within a few
  centiseconds. Re-check those benchmarks after changing them.
- Sound: VIDC log format, one byte every SFR+2 µs, 8 stereo channels, resampled
  to 48 kHz. Verified: `SOUND 1,-15,89,50` gives about 435 Hz.

## Lessons learned

- **The ARM2 pipeline matters**: Elite's protection rewrites the next
  instruction and relies on it being already fetched.
- MEMC CAM writes encode everything in the address: the CPU must pass the full
  address to the bus. The LDRT/STRT TRANS signal is needed for the POST's
  protection test.
- RISC OS samples the keyboard every centisecond and drops keys pressed and
  released faster: the Win32 front end queues translated key events 40 ms
  apart. RISC OS 3.11 has no Spanish layout, so symbols are translated per
  character from the Windows layout, accented letters via Alt+keypad; AltGr
  arrives as a fake LCtrl and AltGr+4 (~) is a dead key.
- The CMOS always configures at least two floppy drives (with one, `:1` gives
  "Bad drive").
- BBC BASIC traps met while writing demos: a `FOR` loop always runs its body at
  least once (`FOR j%=1 TO 0` runs once: use `WHILE`); in the inline assembler a
  `:` separates statements even inside a `;` comment; `USR` passes A%–H% in
  R0–R7, so don't use those variables for anything else in the same program.
- Fixed-point kernels need exact refinement in BASIC at sphere edges; a false hit
  gives a non-unit normal and the specular power overflows ("Number too big").

## Tested software

Zarch (Play It Again Sam 2: run `!Superior`, press 1), Pacmania, Elite (from the
desktop), Genesis Professional 3.04 (disc 1 in :0, disc 2 with
`!GenLib` in :1; `!System`, `!Scrap` and `!GenLib` must be seen by the Filer
before `!Genesis` starts).

## Ideas for later

Swappable ROMs for the BASIC machine (Forth, ...), hard disc and CD-ROM images
(two `.iso` files are waiting in `ADF/`), ARMv3/v4 and Risc PC hardware for
RISC OS 5.
