/*
 * armbus.h - Interfaccia tra un core ARM e la macchina: i quattro accessi
 * alla memoria, comuni all'ARM2 (indirizzi a 26 bit) e all'ARM6/7
 * (indirizzi fisici a 32 bit, dopo la MMU).
 */
#ifndef ARMBUS_H
#define ARMBUS_H

#include <stdint.h>

/* Se il dispositivo vuole segnalare un abort mette *abort = 1. */
typedef struct ArmBus {
    void    *ctx;
    uint32_t (*read32) (void *ctx, uint32_t addr, int *abort);
    uint8_t  (*read8)  (void *ctx, uint32_t addr, int *abort);
    void     (*write32)(void *ctx, uint32_t addr, uint32_t v, int *abort);
    void     (*write8) (void *ctx, uint32_t addr, uint8_t v, int *abort);
} ArmBus;

#endif
