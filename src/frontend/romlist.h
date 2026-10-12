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
/* Versione dal contenuto: il messaggio d'avvio ("RISC OS 3.71 (...)",
   "Arthur 1.20 (...)") nei primi 512 KB; 0 se non c'e' o la ROM non e' tra
   quelle che l'emulatore avvia (oltre la 3.80). Serve per le ROM con un nome
   qualsiasi (quelle scaricate da 4corn.co.uk, ad esempio). */
int  romlist_identify(const char *path, int *riscpc);
/* Prima il contenuto, poi il nome. */
int  romlist_detect(const char *path, int *riscpc);
/* "Arthur 1.20", "RISC OS 3.11"... */
void romlist_name(int version, char *out, size_t size);

#endif
