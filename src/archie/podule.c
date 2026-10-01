/*
 * podule.c - Scheda di espansione emulata (vedi podule.h)
 */
#include "podule.h"
#include <string.h>

static void put_entry(uint8_t *p, uint8_t type, uint32_t size, uint32_t start)
{
    p[0] = type;
    p[1] = (uint8_t)size; p[2] = (uint8_t)(size >> 8); p[3] = (uint8_t)(size >> 16);
    p[4] = (uint8_t)start; p[5] = (uint8_t)(start >> 8); p[6] = (uint8_t)(start >> 16); p[7] = (uint8_t)(start >> 24);
}

int podule_build(uint8_t *rom, uint32_t size, const uint8_t *module, uint32_t module_size, const char *description)
{
    uint32_t desc_len = (uint32_t)strlen(description) + 1;
    uint32_t dir = 16, data = dir + 3 * 8;                /* due voci e il terminatore */
    uint32_t mod_at = (data + desc_len + 3) & ~3u;
    if (mod_at + module_size > size || mod_at + module_size > PODULE_WINDOW) return 0;
    memset(rom, 0, size);
    /* intestazione estesa: byte 0 = 0 (niente IRQ/FIQ), byte 1 bit 0 = c'e' la
       directory; prodotto e costruttore non li guarda nessuno */
    rom[0] = 0x00;
    rom[1] = 0x01;
    rom[3] = 0xEA; rom[4] = 0x00;                         /* prodotto */
    rom[5] = 0xEA; rom[6] = 0x00;                         /* costruttore */
    put_entry(rom + dir, 0xF5, desc_len, data);
    put_entry(rom + dir + 8, 0x81, module_size, mod_at);
    memcpy(rom + data, description, desc_len);
    memcpy(rom + mod_at, module, module_size);
    return 1;
}

uint8_t podule_read(const uint8_t *rom, uint32_t rom_size, uint32_t offset)
{
    uint32_t slot = (offset >> 14) & 3, k = (offset & 0x3FFF) >> 2;
    /* le altre schede sono assenti: il bit 1 dell'identita' a 1 vuol dire "niente" */
    if (slot != 0 || !rom || k >= rom_size) return 0xFF;
    return rom[k];
}
