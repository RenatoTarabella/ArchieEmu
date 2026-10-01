/*
 * podule.h - Scheda di espansione emulata: una ROM con moduli RISC OS.
 *
 * Il Podule Manager di RISC OS 3.11 legge la scheda N a &33C0000 + N*&4000
 * (spazio dell'IOC, banco 4, a qualunque velocita'): il byte k della ROM sta
 * nei bit 0-7 della parola a base + 4*k, quindi la finestra diretta e' di
 * 4 KB. Intestazione "estesa" (byte 0 = 0) e directory dei blocchi da +16:
 * per ogni voce tipo (&81 = modulo, &F5 = descrizione), dimensione su 3 byte
 * e inizio; un tipo 0 chiude. All'avvio il kernel copia i moduli in RMA e
 * li inizializza (se in ROM non ce n'e' uno con lo stesso nome e versione
 * piu' alta).
 */
#ifndef PODULE_H
#define PODULE_H

#include <stdint.h>

#define PODULE_WINDOW 4096u            /* byte leggibili senza loader */

/* costruisce la ROM: un modulo e una descrizione; 0 se non ci sta */
int podule_build(uint8_t *rom, uint32_t size, const uint8_t *module, uint32_t module_size,
                 const char *description);

/* lettura dallo spazio delle schede (offset nel banco 4 dell'IOC) */
uint8_t podule_read(const uint8_t *rom, uint32_t rom_size, uint32_t offset);

#endif
