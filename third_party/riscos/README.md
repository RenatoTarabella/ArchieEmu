# RISC OS components

- `BASIC`: BBC BASIC V 1.87 module (13 August 2025) from RISC OS Open's
  "System Resources" package (`PlingSystem.zip`, `!System/310/Modules/BASIC`), built
  for RISC OS 3.10 and therefore able to run on ARM2/ARM3. It is compressed with
  modsqz; the emulator expands it on load (`src/riscos/module.c`).
- `src/riscos/font.h` is generated from `Kernel/s/vdu/vdufontl1` in the RISC OS 5 sources.

Both are © Acorn Computers / RISC OS Developments and distributed under the
Apache 2.0 licence (see `LICENSE`).
