/*
 * bus.c - Modulo bus (vedi bus.h). Memoria little-endian come sull'Archimedes.
 */
#include "bus.h"
#include <string.h>

void bus_init(Bus *bus)
{
    memset(bus, 0, sizeof *bus);
}

static int check_region(uint32_t addr, uint32_t size)
{
    if ((addr | size) & (BUS_PAGE_SIZE - 1)) return 0;
    if (size == 0 || addr + size > (1u << BUS_ADDR_BITS) || addr + size < addr) return 0;
    return 1;
}

int bus_map_memory(Bus *bus, uint32_t addr, uint32_t size, uint8_t *mem, int readonly)
{
    if (!check_region(addr, size)) return 0;
    for (uint32_t off = 0; off < size; off += BUS_PAGE_SIZE) {
        BusPage *p = &bus->pages[(addr + off) >> BUS_PAGE_BITS];
        p->mem = mem + off;
        p->dev = NULL;
        p->base = addr;
        p->readonly = (uint8_t)(readonly != 0);
    }
    return 1;
}

int bus_map_device(Bus *bus, uint32_t addr, uint32_t size, const BusDevice *dev)
{
    if (!check_region(addr, size)) return 0;
    for (uint32_t off = 0; off < size; off += BUS_PAGE_SIZE) {
        BusPage *p = &bus->pages[(addr + off) >> BUS_PAGE_BITS];
        p->mem = NULL;
        p->dev = dev;
        p->base = addr;
        p->readonly = 0;
    }
    return 1;
}

static inline const BusPage *page_of(const Bus *bus, uint32_t addr)
{
    return &bus->pages[(addr & ((1u << BUS_ADDR_BITS) - 1)) >> BUS_PAGE_BITS];
}

uint32_t bus_read32(Bus *bus, uint32_t addr, int *abort)
{
    const BusPage *p = page_of(bus, addr);
    uint32_t off = addr & (BUS_PAGE_SIZE - 1) & ~3u;
    if (p->mem) {
        const uint8_t *m = p->mem + off;
        return (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    }
    if (p->dev && p->dev->read32) return p->dev->read32(p->dev->ctx, (addr & ~3u) - p->base);
    *abort = 1;
    return 0;
}

uint8_t bus_read8(Bus *bus, uint32_t addr, int *abort)
{
    const BusPage *p = page_of(bus, addr);
    if (p->mem) return p->mem[addr & (BUS_PAGE_SIZE - 1)];
    if (p->dev) {
        if (p->dev->read8) return p->dev->read8(p->dev->ctx, addr - p->base);
        if (p->dev->read32)
            return (uint8_t)(p->dev->read32(p->dev->ctx, (addr & ~3u) - p->base) >> ((addr & 3) * 8));
    }
    *abort = 1;
    return 0;
}

void bus_write32(Bus *bus, uint32_t addr, uint32_t v, int *abort)
{
    const BusPage *p = page_of(bus, addr);
    if (p->mem) {
        if (p->readonly) return;
        uint8_t *m = p->mem + (addr & (BUS_PAGE_SIZE - 1) & ~3u);
        m[0] = (uint8_t)v; m[1] = (uint8_t)(v >> 8); m[2] = (uint8_t)(v >> 16); m[3] = (uint8_t)(v >> 24);
        return;
    }
    if (p->dev) {
        if (p->dev->write32) p->dev->write32(p->dev->ctx, (addr & ~3u) - p->base, v);
        return;
    }
    *abort = 1;
}

void bus_write8(Bus *bus, uint32_t addr, uint8_t v, int *abort)
{
    const BusPage *p = page_of(bus, addr);
    if (p->mem) {
        if (!p->readonly) p->mem[addr & (BUS_PAGE_SIZE - 1)] = v;
        return;
    }
    if (p->dev) {
        if (p->dev->write8) p->dev->write8(p->dev->ctx, addr - p->base, v);
        return;
    }
    *abort = 1;
}

/* adattatori verso l'interfaccia della CPU */
static uint32_t cpu_r32(void *c, uint32_t a, int *ab)            { return bus_read32((Bus *)c, a, ab); }
static uint8_t  cpu_r8 (void *c, uint32_t a, int *ab)            { return bus_read8((Bus *)c, a, ab); }
static void     cpu_w32(void *c, uint32_t a, uint32_t v, int *ab) { bus_write32((Bus *)c, a, v, ab); }
static void     cpu_w8 (void *c, uint32_t a, uint8_t v, int *ab)  { bus_write8((Bus *)c, a, v, ab); }

void bus_attach_cpu(Bus *bus, ArmBus *out)
{
    out->ctx = bus;
    out->read32 = cpu_r32;
    out->read8 = cpu_r8;
    out->write32 = cpu_w32;
    out->write8 = cpu_w8;
    out->read16 = NULL;
    out->write16 = NULL;
}
