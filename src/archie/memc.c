/*
 * memc.c - MEMC1a (vedi memc.h)
 *
 * La CAM si programma con scritture nell'area &3800000: il DATO e'
 * ignorato, pagina logica, pagina fisica e protezione sono codificate
 * nell'INDIRIZZO. Lo stesso per i registri del MEMC in &3600000.
 * Formato dell'indirizzo per la CAM (bit 22-0):
 *    4K  page: 1LLL LLLL LLLL LLAA MPPP PPPP
 *    8K  page: 1LLL LLLL LLLM LLAA MPPP PPPP
 *    16K page: 1LLL LLLL LLxM LLAA MPPP PPPP
 *    32K page: 1LLL LLLL LxxM LLAA MPPP PPPP
 * L pagina logica (i bit 11-10 sono i piu' significativi), A protezione,
 * P pagina fisica, codificata diversamente per ogni dimensione di pagina.
 */
#include "memc.h"
#include <string.h>

#define PAGE4K_SHIFT 12

static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* tabella veloce                                                     */
/* ------------------------------------------------------------------ */

static void map_fast(Memc *m, int logical)
{
    int sub = 1 << (m->page_shift - PAGE4K_SHIFT);
    int base = logical * sub;
    int phys = m->cam_phys[logical];
    for (int i = 0; i < sub && base + i < MEMC_LOGICAL_PAGES; i++) {
        uint32_t off = phys >= 0 ? ((uint32_t)phys << m->page_shift) + ((uint32_t)i << PAGE4K_SHIFT) : 0;
        m->fast[base + i] = (phys >= 0 && off < m->ram_size) ? m->ram + off : NULL;
        m->fast_ppl[base + i] = m->cam_ppl[logical];
    }
}

static void rebuild_fast(Memc *m)
{
    memset(m->fast, 0, sizeof m->fast);
    int pages = 1 << (25 - m->page_shift);
    for (int l = 0; l < pages; l++)
        if (m->cam_phys[l] >= 0) map_fast(m, l);
}

/* ------------------------------------------------------------------ */
/* reset e collegamento                                               */
/* ------------------------------------------------------------------ */

void memc_reset(Memc *m)
{
    for (int i = 0; i < MEMC_LOGICAL_PAGES; i++) { m->cam_phys[i] = -1; m->cam_ppl[i] = 0; }
    m->page_shift = 12;
    m->rom_latched = 1;
    m->os_mode = m->video_dma = m->sound_dma = 0;
    m->control = 0;
    m->vinit = m->vstart = m->vend = m->cinit = 0;
    m->sstart = m->send = m->sptr = 0;
    m->cursor_enabled = 0;
    rebuild_fast(m);
}

void memc_init(Memc *m, uint8_t *ram, uint32_t ram_size, const uint8_t *rom, uint32_t rom_size,
               const Arm2 *cpu, const MemcIo *io)
{
    memset(m, 0, sizeof *m);
    m->ram = ram;
    m->ram_size = ram_size;
    m->rom = rom;
    m->rom_size = rom_size;
    m->cpu = cpu;
    m->io = *io;
    memc_reset(m);
}

/* modo utente, oppure accesso LDRT/STRT (segnale TRANS della CPU) */
static int user_mode(const Memc *m) { return (m->cpu->r[15] & 3) == ARM_MODE_USR || m->cpu->trans_user; }

static int allowed(const Memc *m, uint8_t ppl, int write)
{
    if (!user_mode(m)) return 1;
    switch (ppl & 3) {
    case 0:  return 1;
    case 1:  return m->os_mode || !write;
    default: return m->os_mode && !write;
    }
}

int32_t memc_translate(const Memc *m, uint32_t logical)
{
    if (logical >= 0x2000000u) return -1;
    const uint8_t *p = m->fast[logical >> PAGE4K_SHIFT];
    return p ? (int32_t)(p - m->ram) + (int32_t)(logical & 0xFFF) : -1;
}

/* ------------------------------------------------------------------ */
/* registri e CAM                                                     */
/* ------------------------------------------------------------------ */

static void write_register(Memc *m, uint32_t addr)
{
    uint32_t value = ((addr >> 2) & 0x7FFF) * 16;
    switch ((addr >> 17) & 7) {
    case 0: m->vinit = value; break;
    case 1: m->vstart = value; break;
    case 2: m->vend = value; break;
    case 3: m->cinit = value; m->cursor_enabled = 1; break;
    case 4: m->sstart = value; m->sound_reg = 4; if (m->io.sound_changed) m->io.sound_changed(m->io.ctx); break;
    case 5: m->send = value; break;
    case 6: m->sptr = m->sstart; m->sound_reg = 6; if (m->io.sound_changed) m->io.sound_changed(m->io.ctx); break;
    default: {                                        /* registro di controllo */
        int shift = 12 + (int)((addr >> 2) & 3);
        m->control = addr & 0x3FFC;
        m->video_dma = (addr >> 10) & 1;
        m->sound_dma = (addr >> 11) & 1;
        m->os_mode = (addr >> 12) & 1;
        if (!m->video_dma) m->cursor_enabled = 0;
        if (shift != m->page_shift) { m->page_shift = shift; rebuild_fast(m); }
        m->sound_reg = 7;
        if (m->io.sound_changed) m->io.sound_changed(m->io.ctx);
        break;
    }
    }
}

static void write_cam(Memc *m, uint32_t a)
{
    int phys = 0, logical = 0;
    switch (m->page_shift) {
    case 12:
        phys = (int)(a & 0x7F);
        logical = (int)(((a >> 12) & 0x7FF) | ((a >> 10) & 3) << 11);
        break;
    case 13:
        phys = (int)(((a >> 1) & 0x3F) | (a & 1) << 6);
        logical = (int)(((a >> 13) & 0x3FF) | ((a >> 10) & 3) << 10);
        break;
    case 14:
        phys = (int)(((a >> 2) & 0x1F) | (a & 3) << 5);
        logical = (int)(((a >> 14) & 0x1FF) | ((a >> 10) & 3) << 9);
        break;
    default:
        phys = (int)(((a >> 3) & 0xF) | (a & 1) << 4 | ((a >> 1) & 1) << 6 | ((a >> 2) & 1) << 5);
        logical = (int)(((a >> 15) & 0xFF) | ((a >> 10) & 3) << 8);
        break;
    }
    m->rom_latched = 0;
    /* una pagina fisica compare in un solo posto: toglie le mappature precedenti */
    int pages = 1 << (25 - m->page_shift);
    for (int l = 0; l < pages; l++) {
        if (m->cam_phys[l] == phys && l != logical) {
            m->cam_phys[l] = -1;
            map_fast(m, l);
        }
    }
    m->cam_phys[logical] = (int16_t)phys;
    m->cam_ppl[logical] = (uint8_t)((a >> 8) & 3);
    map_fast(m, logical);
}

/* ------------------------------------------------------------------ */
/* accessi                                                            */
/* ------------------------------------------------------------------ */

static uint32_t rom_word(const Memc *m, uint32_t off)
{
    return m->rom_size ? rd32le(m->rom + (off % m->rom_size & ~3u)) : 0;
}

/* area alta (>= &2000000): RAM fisica e I/O solo in supervisore; le ROM
   si leggono in ogni modo (i moduli come il BASIC girano in modo utente) */
static uint32_t high_read(Memc *m, uint32_t addr, int is_byte, int *abort)
{
    if (addr < 0x3400000u && user_mode(m)) { m->aborts++; *abort = 1; return 0; }
    m->rom_latched = 0;
    if (addr < 0x3000000u) {
        uint32_t off = (addr - 0x2000000u) % m->ram_size;
        return rd32le(m->ram + (off & ~3u));
    }
    if (addr < 0x3400000u) return m->io.io_read(m->io.ctx, addr, is_byte);
    if (addr < 0x3800000u) return 0;                   /* ROM bassa: assente */
    return rom_word(m, addr - 0x3800000u);
}

uint32_t memc_read32(Memc *m, uint32_t addr, int *abort)
{
    addr &= 0x3FFFFFCu;
    if (addr >= 0x2000000u) return high_read(m, addr, 0, abort);
    if (m->rom_latched) return rom_word(m, addr);
    uint32_t page = addr >> PAGE4K_SHIFT;
    const uint8_t *p = m->fast[page];
    if (!p || !allowed(m, m->fast_ppl[page], 0)) { m->aborts++; *abort = 1; return 0; }
    return rd32le(p + (addr & 0xFFF));
}

uint8_t memc_read8(Memc *m, uint32_t addr, int *abort)
{
    addr &= 0x3FFFFFFu;
    if (addr < 0x2000000u && !m->rom_latched) {
        uint32_t page = addr >> PAGE4K_SHIFT;
        const uint8_t *p = m->fast[page];
        if (!p || !allowed(m, m->fast_ppl[page], 0)) { m->aborts++; *abort = 1; return 0; }
        return p[addr & 0xFFF];
    }
    if (addr >= 0x3000000u && addr < 0x3400000u) {
        if (user_mode(m)) { m->aborts++; *abort = 1; return 0; }
        m->rom_latched = 0;
        return (uint8_t)m->io.io_read(m->io.ctx, addr, 1);
    }
    uint32_t w = addr >= 0x2000000u ? high_read(m, addr & ~3u, 1, abort) : rom_word(m, addr & ~3u);
    return (uint8_t)(w >> ((addr & 3) * 8));
}

static void high_write(Memc *m, uint32_t addr, uint32_t v, int is_byte, int *abort)
{
    if (user_mode(m)) { m->aborts++; *abort = 1; return; }
    m->rom_latched = 0;
    if (addr < 0x3000000u) {
        uint32_t off = (addr - 0x2000000u) % m->ram_size;
        if (is_byte) m->ram[off] = (uint8_t)v;
        else wr32le(m->ram + (off & ~3u), v);
        return;
    }
    if (addr < 0x3400000u) { m->io.io_write(m->io.ctx, addr, v, is_byte); return; }
    if (addr < 0x3600000u) {
        /* STRB mette il byte su tutte e quattro le linee del bus dati */
        if (is_byte) v = (v & 0xFF) * 0x01010101u;
        m->io.vidc_write(m->io.ctx, v);
        return;
    }
    if (addr < 0x3800000u) { write_register(m, addr); return; }
    write_cam(m, addr);
}

void memc_write32(Memc *m, uint32_t addr, uint32_t v, int *abort)
{
    /* i bit 1-0 servono alla CAM (pagina fisica): si tolgono solo per la RAM */
    addr &= 0x3FFFFFFu;
    if (addr >= 0x2000000u) { high_write(m, addr, v, 0, abort); return; }
    addr &= ~3u;
    if (m->rom_latched) return;
    uint32_t page = addr >> PAGE4K_SHIFT;
    uint8_t *p = m->fast[page];
    if (!p || !allowed(m, m->fast_ppl[page], 1)) { m->aborts++; *abort = 1; return; }
    wr32le(p + (addr & 0xFFF), v);
}

void memc_write8(Memc *m, uint32_t addr, uint8_t v, int *abort)
{
    addr &= 0x3FFFFFFu;
    if (addr >= 0x2000000u) { high_write(m, addr, v, 1, abort); return; }
    if (m->rom_latched) return;
    uint32_t page = addr >> PAGE4K_SHIFT;
    uint8_t *p = m->fast[page];
    if (!p || !allowed(m, m->fast_ppl[page], 1)) { m->aborts++; *abort = 1; return; }
    p[addr & 0xFFF] = v;
}

static uint32_t cpu_r32(void *c, uint32_t a, int *ab)            { return memc_read32((Memc *)c, a, ab); }
static uint8_t  cpu_r8 (void *c, uint32_t a, int *ab)            { return memc_read8((Memc *)c, a, ab); }
static void     cpu_w32(void *c, uint32_t a, uint32_t v, int *ab) { memc_write32((Memc *)c, a, v, ab); }
static void     cpu_w8 (void *c, uint32_t a, uint8_t v, int *ab)  { memc_write8((Memc *)c, a, v, ab); }

void memc_attach_cpu(Memc *m, ArmBus *bus)
{
    bus->ctx = m;
    bus->read32 = cpu_r32;
    bus->read8 = cpu_r8;
    bus->write32 = cpu_w32;
    bus->write8 = cpu_w8;
    bus->read16 = NULL;
    bus->write16 = NULL;
}
