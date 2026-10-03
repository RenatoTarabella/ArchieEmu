/*
 * superio.h - Super I/O del Risc PC (82C711): controller floppy, seriale,
 * parallela e IDE, alle porte del PC moltiplicate per 4 da &03010000.
 *
 * RISC OS lo riconosce cosi': prova la sequenza di configurazione dell'SMC
 * 37C665 (&55 &55 a &3F0, identita' all'indice &0D); se l'identita' non e'
 * &65 programma l'82C710/711 (&55 a &2FA, &AA &36 &E4 a &3FA, &1B a &2FA,
 * poi registri &390/&391). Qui si accettano entrambe e il chip risponde da
 * 82C711. Il controller floppy (fdc82077.c) sta a &3F0-&3F7, il disco IDE
 * (ide.c) a &1F0-&1F7 e &3F6; della seriale
 * per ora solo il registro scratch e lo stato del trasmettitore.
 */
#ifndef RISCPC_SUPERIO_H
#define RISCPC_SUPERIO_H

#include <stdint.h>
#include "fdc82077.h"
#include "ide.h"

typedef struct SuperIo {
    Fdc82077 fdc;
    IdeDisk  ide;
    int      config_mode;        /* sequenza &55 &55 del 37C665 ricevuta */
    int      config_keys;
    uint8_t  config_index, config[16];
    uint8_t  c710_index, c710[16];
    uint8_t  scratch[2];         /* &3FF e &2FF */
} SuperIo;

void    superio_init(SuperIo *s);
void    superio_reset(SuperIo *s, ArcTime now);
/* port: la porta del PC (offset / 4); *known = 0 se nessuno risponde */
uint8_t superio_read(SuperIo *s, uint32_t port, ArcTime now, int *known);
void    superio_write(SuperIo *s, uint32_t port, uint8_t v, ArcTime now, int *known);
/* registro dati dell'IDE (&1F0), a 16 bit */
uint16_t superio_read16(SuperIo *s, uint32_t port, ArcTime now, int *known);
void    superio_write16(SuperIo *s, uint32_t port, uint16_t v, ArcTime now, int *known);

#endif
