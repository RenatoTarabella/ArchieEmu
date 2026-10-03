/*
 * hdformat.h - Immagini di disco fisso IDE gia' formattate ADFS ("new map")
 * per il Risc PC, identiche a quelle che lascia HForm 2.23 su RISC OS 3.5
 * (inizializzazione "I"). La spiegazione del formato e il confronto con
 * HForm sono in tools/mkhdf.py, che fa la stessa cosa in Python.
 */
#ifndef RISCPC_HDFORMAT_H
#define RISCPC_HDFORMAT_H

#include <stdint.h>

/* Geometria proposta dall'emulatore: 16 testine, 63 settori da 512 byte. */
#define HDF_HEADS 16
#define HDF_SPT   63

typedef struct HdfParams {
    int idlen, log2bpmb, nzones, zone_spare;
} HdfParams;

/* I parametri della mappa per un disco di 'mb' MB (quelli di HForm). */
void hdf_params(uint32_t mb, HdfParams *p);

/* Crea 'path' di mb MB, formattato, col nome dato (al massimo 10 caratteri)
   e il disc ID (0 = a caso). 0 = errore. */
int  hdf_create(const char *path, uint32_t mb, const char *name, uint16_t disc_id);

/* Come hdf_create ma con parametri dati (per i test). */
int  hdf_create_params(const char *path, uint32_t mb, const char *name, uint16_t disc_id,
                       const HdfParams *p);

#endif
