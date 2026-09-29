# Componenti di RISC OS

- `BASIC`: modulo BBC BASIC V 1.87 (13 agosto 2025) dal pacchetto "System Resources"
  di RISC OS Open (`PlingSystem.zip`, `!System/310/Modules/BASIC`), compilato per
  RISC OS 3.10 e quindi eseguibile su ARM2/ARM3. È compresso con modsqz; l'emulatore
  lo espande al caricamento (`src/riscos/module.c`).
- `src/riscos/font.h` è generato da `Kernel/s/vdu/vdufontl1` dei sorgenti di RISC OS 5.

Entrambi sono © Acorn Computers / RISC OS Developments e distribuiti sotto licenza
Apache 2.0 (vedi `LICENSE`).
