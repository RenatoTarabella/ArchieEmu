/*
 * splash_win32.h - Finestra iniziale: quale macchina avviare.
 *
 * Archimedes (ARM2, ROM fino a RISC OS 3.11) o Risc PC (ARM610/ARM710,
 * RISC OS 3.50-3.80), con la ROM scelta fra quelle trovate nella cartella
 * roms, la RAM e (Risc PC) la VRAM. L'ultima scelta si ricorda in
 * ArchieEmu.ini accanto all'eseguibile.
 */
#ifndef SPLASH_WIN32_H
#define SPLASH_WIN32_H

#include <windows.h>

enum { CPU_ARM2 = 0, CPU_ARM610 = 1, CPU_ARM710 = 2 };

typedef struct MachineChoice {
    int  riscpc;
    char rom[MAX_PATH];
    char rom_name[64];        /* "RISC OS 3.50" */
    int  cpu;
    int  ram_mb;
    int  vram_mb;
} MachineChoice;

/* Versione di RISC OS dal nome del file (ROM311 -> 311), 0 se non e' una ROM
   riconosciuta; *riscpc = 1 per le ROM del Risc PC (3.50 e successive). */
int  splash_rom_version(const char *path, int *riscpc);
/* Nome leggibile: "Arthur 1.20", "RISC OS 3.11"... */
void splash_rom_name(int version, char *out, size_t size);

/* Mostra la finestra; ritorna 0 se l'utente rinuncia. */
int  splash_choose(HINSTANCE inst, MachineChoice *choice);

/* impostazioni in ArchieEmu.ini, sezione [Machine] */
void splash_get(const char *key, char *out, unsigned size);
void splash_set(const char *key, const char *value);

#endif
