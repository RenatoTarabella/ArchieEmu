# CLAUDE.md — working notes for ArchieEmu

Context for anyone (human or AI) working on this repository. The user-facing
overview is in `README.md`; this file holds the conventions, test workflow and
lessons learned that are not obvious from the code.

## Layout

- `src/cpu` ARMv2a core (26-bit PC+PSR, banks, exceptions, S/N/I cycles, 3-stage
  prefetch pipeline) and, separately, the ARMv3 core for the Risc PC (`arm6.c`:
  ARM610/710, 26/32-bit modes, CP15 and MMU inside the core), `src/core` bus, `src/hle` + `src/riscos` RISC OS in HLE
  (kernel SWIs, VDU driver, HostFS, module loader), `src/machine` the BBC BASIC
  machine, `src/archie` the low-level Archimedes (MEMC1a, IOC, VIDC1a, keyboard,
  CMOS, WD1772), `src/riscpc` the Risc PC (IOMD, VIDC20; plan and status in
  `docs/riscpc-plan.md`), `src/frontend` Win32 and console front ends.
- Executables: `armwin` / `armbasic` (BASIC machine, window / console),
  `archie` (window for both the Archimedes and the Risc PC: without `--rom` a
  startup dialog, `splash_win32.c`, picks machine, ROM, CPU and memory and
  remembers them in `ArchieEmu.ini` next to the exe), `archie_boot` (headless Archimedes for
  diagnostics), `riscpc_boot` (headless Risc PC), `armemu`, `romdis` (`-3` for
  ARMv3), `modinfo`.
- `disc/` is the BASIC machine's HostFS disc (`name,ffb` = tokenised BASIC).
- `roms/` (RISC OS ROMs) and `ADF/` (floppy images) exist only locally and are
  **git-ignored: never commit or redistribute them**. The default ROM is
  `roms/1. Major/ROM311`; the CMOS is saved beside it as `ROM311.cmos`.
- Source comments and commit messages are in Italian; README and this file are
  in English.

## Build and test

Windows: MSVC 2022 through CMake (no gcc). macOS: see below.

```
cmake --build build --config Release
build\Release\test_arm2.exe   (also test_arm6, test_iomd, test_fdc82077, test_ide, test_hdformat, test_basic, test_memc, test_vidc, test_kbd, test_cmos, test_fdc, test_keys_es ROM)
.venv\Scripts\python tests\diff_unicorn.py        # ARM2 ALU oracle against Unicorn
.venv\Scripts\python tests\diff_unicorn_arm6.py   # ARMv3 core, 32-bit modes, against Unicorn (SA1100)
```

The linker fails if `archie.exe` / `armwin.exe` is still running: close it first.

Release packages: `tools/make_dist.ps1` (Windows portable zips, static CRT) and
`tools/make_dmg.sh` (macOS DMG, run on the Mac).

### macOS

Native Cocoa front ends (`armwin_mac.m`, `archie_mac.m`, `mac_keys.c`), built on
the Mac mini over SSH (`renatos-mini.fritz.box`, key in `~/.ssh/config`; cmake is
in `/opt/homebrew/bin`). Copy the tree with `tar` over `ssh` to `~/ArchieEmu`.

- GUI apps can only be launched from SSH (`open ...app`) while the user is logged
  in on the Mac's desktop (`stat -f%Su /dev/console` must not be `root`), and
  `screencapture` from SSH only sees the wallpaper: ask the user for a screenshot.
- `make_dmg.sh` signs with the Developer ID and notarizes with the `runebrace`
  notarytool profile; the keychain is unlocked from `~/.rb-keychain`, as for Runebrace.
- `[NSApp run]` never returns: cleanup (CMOS save) lives in `applicationWillTerminate`.

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
- Risc PC: `riscpc_boot [--rom "roms/1. Major/ROM350"] --ms 8000 --png out.png
  [--vram 0|1|2] [--ram MB] [--arm710] [--cmos file] [--floppy a.adf --floppy2 b.adf]
  [--hd disc.hdf] (`--create-hd disc.hdf MB` makes an ADFS-formatted image and exits,
  `--blank --create-hd` an empty one). `tools/mkhdf.py` does the same in Python
  and documents the new-map format; `--check` compares with images made by HForm.
  [--keys "..." --keys-at ms]`.
  Keys are PS/2 set 2 from a UK layout, typed from 10 s (the desktop is up at
  ~8 s); extra tokens `{MOUSE dx,dy}` (y up, RISC OS scales by 1.5),
  `{SELECT}` `{MENU}` `{ADJUST}`, `{ESC}` `{TAB}` and arrows. It lists every access to an unknown
  address; for debugging: `--trace-io`, `--watch-io lo hi`, `--trace-modes`,
  `--watch-low`, `--break pc`, `--ring N` (last instructions before the first
  abort), `--trace N --trace-at instr`, `--hist`. ROM350 reaches the desktop
  in about 8 s of emulated time.
- Wimp applications must be started from the desktop: `*run` from the F12 command
  line fails with "Wimp is currently active". Use `filer_run :0.!App` instead.
  `filer_opendir` opens a directory window.
- `tools/mkadfs.py` builds ADFS D images from a directory, zip (RISC OS extra
  field; Implode via 7-Zip) or Spark/Arc archive. `tools/adfextract.py` does the
  reverse (L, D, E formats, zip/Spark/ArcFS archives, CD ISOs) into a HostFS
  folder, opening nested archives. The floppy drives also take HFE v1 flux
  images (decoded to "custom" tracks at insert time, read only).
- `archie_boot --keys` types with a UK keyboard: `"` comes out as `@` and `~` is
  lost (use `{ALT126}`); prompts that flush the keyboard buffer (e.g. `*Copy`'s
  confirmation) swallow keys typed ahead, so put `{WAIT}` before the answer.
- HostFS: `--hostfs dir` (default `HostFS` next to the exe / `Documents/ArchieEmu/HostFS`).
  `ARCHIE_HOSTFS_TRACE=1` logs every FileSwitch call with registers.

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
- ADFS D/L images: every object must start on a 1 KB sector. ADFS answers
  "Bad parameters" otherwise, and with the floppy cache on (ADFSBuffers > 0) the
  error is lost and FileCore polls forever: it looks like an FDC hang but isn't.
- Podules (from the ROM's Podule Manager 1.26): card N at `&33C0000 + N*&4000`,
  byte k in D0-D7 of the word at base + 4k, so only 4 KB are readable without a
  loader; extended ID needs byte0 & &F8 == 0; chunk directory from +16 (type &81
  = module, &F5 = description, 0 ends). The kernel loads &81 modules at boot.
- FileSwitch 2.08 (RISC OS 3.11), as HostFS uses it: absolute names (`$`,
  `$.dir.file`); buffered opens (R2 = &400) get GetBytes/PutBytes by block and
  offset; *Cat/*Ex/*Info come through Func 14/15; "not found" is Open R1 = 0 or
  File 5 R0 = 0 (also for names with wildcards, never an error); errors are V set
  with R0 -> block. R12 on entry is the value passed in R3 to OS_FSControl 12.
- Unicorn as an oracle: it does not rotate unaligned LDRs (ARM2/ARM6 do), it
  reuses stale translated blocks after a mode change (call `uc.ctl_flush_tb()`
  per case), and with `count=1` it doesn't complete exception returns (MOVS PC
  in a privileged mode keeps the mode and only sets the flags).
- Emulated devices that schedule an event in response to a CPU access (the next
  floppy byte 16 us later) must shorten the CPU slice in progress
  (`reschedule` in `riscpc.c`), or the event is only seen at the end of the
  slice: the Risc PC floppy DMA ran 10-20 times too slow and ADFS timed out.
- Risc PC floppy (ADFS on RISC OS 3.5): DMA through FIQ, data at `&03012000`,
  last byte with terminal count at `&0302A000`; index pulses on IRQA bit 2
  (needed to see a disc); /DSKCHG on IOCR bit 2. Details in docs/riscpc-plan.md.
- Fixed-point kernels need exact refinement in BASIC at sphere edges; a false hit
  gives a non-unit normal and the specular power overflows ("Number too big").

## Tested software

Zarch (Play It Again Sam 2: run `!Superior`, press 1), Pacmania, Elite (from the
desktop), Genesis Professional 3.04 (disc 1 in :0, disc 2 with
`!GenLib` in :1; `!System`, `!Scrap` and `!GenLib` must be seen by the Filer
before `!Genesis` starts).

## Ideas for later

Risc PC for RISC OS 3.5 (plan in `docs/riscpc-plan.md`), swappable ROMs for the
BASIC machine (Forth, ...), hard disc and CD-ROM images
(two `.iso` files are waiting in `ADF/`), ARMv3/v4 and Risc PC hardware for
RISC OS 5.
