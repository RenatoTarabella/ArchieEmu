/*
 * bus.h - Modulo bus: spazio di indirizzi a 26 bit (64 MB) diviso in
 * pagine da 4 KB. Ogni pagina punta a memoria dell'host (RAM/ROM, accesso
 * diretto) oppure a un dispositivo con le sue funzioni di lettura/scrittura.
 * Le pagine non mappate generano abort.
 */
#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stddef.h>
#include "../cpu/arm2.h"

#define BUS_ADDR_BITS  26
#define BUS_PAGE_BITS  12
#define BUS_PAGE_SIZE  (1u << BUS_PAGE_BITS)
#define BUS_NUM_PAGES  (1u << (BUS_ADDR_BITS - BUS_PAGE_BITS))

/* Un dispositivo riceve l'offset rispetto all'inizio della sua regione. */
typedef struct BusDevice {
    const char *name;
    void       *ctx;
    uint32_t  (*read32) (void *ctx, uint32_t offset);
    uint8_t   (*read8)  (void *ctx, uint32_t offset);   /* NULL: byte dalla parola */
    void      (*write32)(void *ctx, uint32_t offset, uint32_t v);
    void      (*write8) (void *ctx, uint32_t offset, uint8_t v);
} BusDevice;

typedef struct BusPage {
    uint8_t         *mem;       /* memoria diretta, o NULL                 */
    const BusDevice *dev;       /* dispositivo, o NULL                     */
    uint32_t         base;      /* inizio della regione del dispositivo    */
    uint8_t          readonly;  /* ROM: scritture ignorate                 */
} BusPage;

typedef struct Bus {
    BusPage pages[BUS_NUM_PAGES];
} Bus;

void bus_init(Bus *bus);
/* Mappa memoria dell'host. addr e size devono essere multipli di 4 KB. */
int  bus_map_memory(Bus *bus, uint32_t addr, uint32_t size, uint8_t *mem, int readonly);
int  bus_map_device(Bus *bus, uint32_t addr, uint32_t size, const BusDevice *dev);

uint32_t bus_read32 (Bus *bus, uint32_t addr, int *abort);
uint8_t  bus_read8  (Bus *bus, uint32_t addr, int *abort);
void     bus_write32(Bus *bus, uint32_t addr, uint32_t v, int *abort);
void     bus_write8 (Bus *bus, uint32_t addr, uint8_t v, int *abort);

/* Riempie un ArmBus che instrada gli accessi della CPU su questo bus. */
void bus_attach_cpu(Bus *bus, ArmBus *out);

#endif
