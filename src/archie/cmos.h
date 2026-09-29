/*
 * cmos.h - PCF8583: 256 byte di RAM CMOS con orologio, sul bus I2C
 * pilotato dai pin C0 (SDA) e C1 (SCL) del registro di controllo dell'IOC.
 *
 * Nei primi 16 byte c'e' l'orologio (letto dall'ora dell'host), il resto
 * contiene la configurazione di RISC OS (*Configure), con la somma di
 * controllo nel byte che RISC OS si aspetta.
 */
#ifndef ARCHIE_CMOS_H
#define ARCHIE_CMOS_H

#include <stdint.h>

typedef struct Cmos {
    uint8_t ram[256];
    /* stato del protocollo I2C (privato) */
    int     scl, sda_in, sda_out;
    int     state, bit_count, reading;
    uint8_t shift, address;
    int     dirty;               /* modificata dal computer: da salvare */
} Cmos;

/* Carica la CMOS da 'path' (256 byte); se il file non c'e' o e' NULL,
   usa una configurazione predefinita valida per RISC OS 3.1. */
void cmos_init(Cmos *c, const char *path);
int  cmos_save(const Cmos *c, const char *path);

/* Chiamata a ogni scrittura del registro di controllo dell'IOC con i
   livelli di SCL e SDA scritti dal computer (1 = linea rilasciata).
   Ritorna il livello di SDA che il computer legge (bus open-collector:
   0 se il computer o la CMOS tirano la linea in basso). */
int  cmos_i2c(Cmos *c, int scl, int sda);

/* Ricalcola la somma di controllo di RISC OS dopo modifiche dall'host. */
void cmos_fix_checksum(Cmos *c);

#endif
