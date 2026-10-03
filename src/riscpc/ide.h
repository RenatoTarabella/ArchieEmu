/*
 * ide.h - Disco fisso IDE (ATA) del Risc PC, nel Super I/O 82C711.
 *
 * Porte (del PC): &1F0 dato (16 bit), &1F1 errore/feature, &1F2 numero di
 * settori, &1F3 settore (LBA 0-7), &1F4/&1F5 cilindro (LBA 8-23), &1F6
 * unita'/testina (bit 4 slave, bit 6 LBA, bit 0-3 testina o LBA 24-27),
 * &1F7 stato/comando; &3F6 stato alternativo / controllo (bit 1 niente
 * interrupt, bit 2 reset).
 * Solo il master; l'immagine (.hdf) e' la sequenza dei settori da 512 byte,
 * senza intestazione. Comandi: IDENTIFY, READ/WRITE SECTORS e MULTIPLE,
 * VERIFY, INITIALIZE DEVICE PARAMETERS (la geometria che ADFS si aspetta),
 * SET MULTIPLE, RECALIBRATE, SEEK, DIAGNOSTICS, SET FEATURES e quelli di
 * risparmio energetico. I comandi finiscono subito; l'interrupt resta alto
 * finche' non si legge lo stato.
 */
#ifndef RISCPC_IDE_H
#define RISCPC_IDE_H

#include <stdint.h>
#include <stdio.h>

typedef struct IdeDisk {
    FILE    *fp;
    char     path[512];
    uint32_t sectors;            /* capacita' in settori da 512 byte */
    int      read_only;
    int      cyls, heads, spt;   /* geometria predefinita (IDENTIFY) */
    int      cur_heads, cur_spt; /* quella scelta con INITIALIZE DEVICE PARAMETERS */

    uint8_t  error, feature, count, sector, cyl_lo, cyl_hi, drvhead, status, control;
    int      multiple;           /* settori per blocco di READ/WRITE MULTIPLE */
    int      irq;

    /* trasferimento in corso */
    uint8_t  buf[512 * 16];
    int      pos, len;           /* byte nel buffer */
    int      remaining;          /* settori ancora da trasferire */
    int      writing;
    uint32_t lba;
    int      block;              /* settori per blocco (1 o multiple) */
} IdeDisk;

void     ide_init(IdeDisk *d);
/* apre un'immagine .hdf (0 = errore); la scrittura va subito nel file */
int      ide_attach(IdeDisk *d, const char *path);
void     ide_detach(IdeDisk *d);
/* crea un'immagine vuota di 'mb' megabyte (0 = errore) */
int      ide_create_image(const char *path, uint32_t mb);
void     ide_reset(IdeDisk *d);

/* reg: 0-7 = &1F0-&1F7, 8 = &3F6 */
uint8_t  ide_read(IdeDisk *d, int reg);
void     ide_write(IdeDisk *d, int reg, uint8_t v);
uint16_t ide_read_data(IdeDisk *d);
void     ide_write_data(IdeDisk *d, uint16_t v);
int      ide_irq(const IdeDisk *d);

#endif
