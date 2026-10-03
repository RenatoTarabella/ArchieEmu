/*
 * romlist.h - Riconoscimento delle ROM di RISC OS dal nome del file, per
 * le finestre iniziali di Windows e del Mac.
 */
#ifndef ROMLIST_H
#define ROMLIST_H

#include <stddef.h>

/* Versione dal nome (ROM311 -> 311), 0 se non e' una ROM riconosciuta;
   *riscpc = 1 per le ROM del Risc PC (3.50-3.80 per ARM6/7). */
int  romlist_version(const char *path, int *riscpc);
/* "Arthur 1.20", "RISC OS 3.11"... */
void romlist_name(int version, char *out, size_t size);

#endif
