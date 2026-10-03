/*
 * riscpc.c - Montaggio della macchina Risc PC (vedi riscpc.h)
 */
#include "riscpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* tempo                                                              */
/* ------------------------------------------------------------------ */

ArcTime riscpc_now(const RiscPc *m)
{
    return m->now + (((m->cpu.cycles - m->cycle_base) * m->units_per_cycle_q16) >> 16);
}

static void update_speed(RiscPc *m)
{
    m->now = riscpc_now(m);
    m->cycle_base = m->cpu.cycles;
    m->units_per_cycle_q16 = (uint32_t)(24.0 / m->mhz * 65536.0 + 0.5);
}

void riscpc_set_mhz(RiscPc *m, double mhz)
{
    m->mhz = mhz > 0 ? mhz : 30;
    update_speed(m);
}

uint32_t riscpc_border_rgb(const RiscPc *m)
{
    uint32_t c = m->vidc.border;                      /* R nei bit bassi */
    return (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static void update_lines(RiscPc *m)
{
    m->cpu.irq_line = iomd_irq(&m->iomd);
    m->cpu.fiq_line = iomd_fiq(&m->iomd);
}

/* ------------------------------------------------------------------ */
/* memoria                                                            */
/* ------------------------------------------------------------------ */

/* memoria diretta per l'indirizzo fisico, o NULL; *ro = sola lettura */
static uint8_t *mem_at(RiscPc *m, uint32_t a, int *ro)
{
    *ro = 0;
    switch (a >> 24) {
    case 0x00:
        *ro = 1;
        return &m->rom[a & (m->rom_size - 1)];
    case 0x02:
        if (!m->vram_size) return NULL;
        /* 1 MB di VRAM e' largo 32 bit: col bus a 64 bit (VREFCR bit 6,
           2 MB) manca la meta' alta e la parola a +4 ricade su +0. E' cosi'
           che il kernel misura la VRAM. */
        if (m->vram_size == 0x100000u && (m->iomd.vrefcr & 0x40))
            a = ((a & ~7u) >> 1) | (a & 3);
        return &m->vram[a & (m->vram_size - 1)];
    case 0x10: case 0x11: case 0x12: case 0x13:        /* banco 0 della DRAM */
        return &m->ram[a & (m->ram_size - 1)];
    default:
        return NULL;
    }
}

const uint8_t *riscpc_phys(RiscPc *m, uint32_t addr, uint32_t len)
{
    int ro;
    uint8_t *p = mem_at(m, addr, &ro);
    if (!p || !len) return p;
    uint8_t *q = mem_at(m, addr + len - 1, &ro);
    return q == p + len - 1 ? p : NULL;               /* senza giri di indirizzo */
}

static void unknown(RiscPc *m, int write, uint32_t a, uint32_t v, int size)
{
    if (m->io_hook) m->io_hook(m->hook_user, write, a, v, size);
}

static uint32_t io_read(RiscPc *m, uint32_t a, int size)
{
    ArcTime now = riscpc_now(m);
    int known = 0;
    uint32_t v = 0;
    if ((a & 0xFFF00000u) == 0x03200000u) {
        v = iomd_read(&m->iomd, a & 0xFFFFF, now, &known);
        if (a & 0x1FF000u) known = 0;
        update_lines(m);
    } else if ((a & 0xFFFF0000u) == 0x03310000u) {
        /* tasti del mouse, attivi bassi: bit 4 Adjust, 5 Menu, 6 Select */
        v = (uint32_t)(~m->mouse_buttons & 7u) << 4;
        known = 1;
    } else if ((a & 0xFFFC0000u) == 0x033C0000u || (a & 0xF8000000u) == 0x08000000u) {
        /* schede di espansione assenti (anche nello spazio EASI): il bit 1 dell'identita' a 1 vuol dire "niente" */
        v = 0xFFFFFFFFu;
        known = 1;
    }
    if (!known) unknown(m, 0, a, 0, size);
    if (m->io_trace && a >= m->trace_lo && a <= m->trace_hi) m->io_trace(m->hook_user, 0, a, v, size);
    return v;
}

static void io_write(RiscPc *m, uint32_t a, uint32_t v, int size)
{
    ArcTime now = riscpc_now(m);
    int known = 0;
    if ((a & 0xFFF00000u) == 0x03200000u && !(a & 0x1FF000u)) {
        iomd_write(&m->iomd, a & 0xFFF, v, now, &known);
        update_lines(m);
    } else if ((a & 0xFFF00000u) == 0x03400000u) {
        vidc20_write(&m->vidc, v);
        known = 1;
    }
    if (!known) unknown(m, 1, a, v, size);
    if (m->io_trace && a >= m->trace_lo && a <= m->trace_hi) m->io_trace(m->hook_user, 1, a, v, size);
}

static uint32_t bus_r32(void *ctx, uint32_t a, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a & ~3u, &ro);
    if (p) { uint32_t v; memcpy(&v, p, 4); return v; }
    return io_read(m, a & ~3u, 4);
}

static uint8_t bus_r8(void *ctx, uint32_t a, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a, &ro);
    if (p) return *p;
    return (uint8_t)(io_read(m, a & ~3u, 1) >> ((a & 3) * 8));
}

static void bus_w32(void *ctx, uint32_t a, uint32_t v, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a & ~3u, &ro);
    if (p) { if (!ro) memcpy(p, &v, 4); return; }
    io_write(m, a & ~3u, v, 4);
}

static void bus_w8(void *ctx, uint32_t a, uint8_t v, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a, &ro);
    if (p) { if (!ro) *p = v; return; }
    /* STRB: il byte e' replicato su tutte le linee del bus */
    io_write(m, a, v * 0x01010101u, 1);
}

/* ------------------------------------------------------------------ */
/* CMOS sulle linee dell'IOMD                                         */
/* ------------------------------------------------------------------ */

static void lines_write(void *ctx, uint8_t v)
{
    RiscPc *m = ctx;
    m->sda_in = cmos_i2c(&m->cmos, (v >> 1) & 1, v & 1);
}

static uint8_t lines_read(void *ctx)
{
    RiscPc *m = ctx;
    return (uint8_t)(0x3E | (m->sda_in ? 1 : 0));
}

/* ------------------------------------------------------------------ */
/* tastiera e mouse                                                   */
/* ------------------------------------------------------------------ */

static void to_keyboard(void *ctx, uint8_t b)    { ps2kbd_rx(&((RiscPc *)ctx)->kbd, b); }
static int  from_keyboard(void *ctx, uint8_t *b) { return ps2kbd_tx(&((RiscPc *)ctx)->kbd, b); }

void riscpc_key(RiscPc *m, uint32_t code, int down)
{
    ps2kbd_key(&m->kbd, code, down);
}

/* contatori a quadratura dell'IOMD: RISC OS ne legge la differenza */
void riscpc_mouse_move(RiscPc *m, int dx, int dy)
{
    m->iomd.mouse_x = (uint16_t)(m->iomd.mouse_x + dx);
    m->iomd.mouse_y = (uint16_t)(m->iomd.mouse_y + dy);
}

void riscpc_mouse_buttons(RiscPc *m, int buttons)
{
    m->mouse_buttons = buttons & 7;
}

/* ------------------------------------------------------------------ */
/* creazione                                                          */
/* ------------------------------------------------------------------ */

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *d = malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *size = (uint32_t)n;
    return d;
}

void riscpc_reset(RiscPc *m)
{
    iomd_reset(&m->iomd, m->now);
    ps2kbd_reset(&m->kbd);
    vidc20_reset(&m->vidc);
    arm6_reset(&m->cpu);
    m->frame_start = m->now;
    m->flyback = 0;
    update_lines(m);
}

int riscpc_create(RiscPc *m, const RiscPcConfig *cfg, char *err, size_t errsize)
{
    memset(m, 0, sizeof *m);
    uint32_t mb = cfg->ram_mb ? cfg->ram_mb : 16;
    if (mb < 4 || mb > 64 || (mb & (mb - 1))) mb = 16;
    m->ram_size = mb << 20;
    m->vram_size = (cfg->vram_mb <= 2 ? cfg->vram_mb : 1) << 20;
    m->ram = calloc(1, m->ram_size);
    m->vram = m->vram_size ? calloc(1, m->vram_size) : NULL;

    uint32_t n = 0;
    uint8_t *raw = load_file(cfg->rom_path, &n);
    if (!m->ram || !raw) { snprintf(err, errsize, "impossibile leggere la ROM %s", cfg->rom_path); free(raw); return 0; }
    uint32_t size = 1;
    while (size * 2 <= n) size *= 2;
    if (size < 0x200000u || size > 0x800000u) {
        snprintf(err, errsize, "%s: dimensione %u non valida per un Risc PC (2-8 MB)", cfg->rom_path, n);
        free(raw);
        return 0;
    }
    m->rom = raw;
    m->rom_size = size;

    ArmBus bus = { m, bus_r32, bus_r8, bus_w32, bus_w8 };
    arm6_init(&m->cpu, &bus, cfg->arm710 ? ARM6_ID_ARM710 : ARM6_ID_ARM610);
    IomdHooks hooks = { m, lines_write, lines_read, to_keyboard, from_keyboard };
    iomd_init(&m->iomd, &hooks);
    if (cfg->cmos_path) snprintf(m->cmos_path, sizeof m->cmos_path, "%s", cfg->cmos_path);
    cmos_init(&m->cmos, cfg->cmos_path);
    m->sda_in = 1;
    m->mhz = cfg->mhz > 0 ? cfg->mhz : (cfg->arm710 ? 40 : 30);
    update_speed(m);
    riscpc_reset(m);
    return 1;
}

void riscpc_destroy(RiscPc *m)
{
    if (m->cmos_path[0] && m->cmos.dirty) cmos_save(&m->cmos, m->cmos_path);
    free(m->rom);
    free(m->ram);
    free(m->vram);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ */
/* esecuzione                                                         */
/* ------------------------------------------------------------------ */

/* Il DMA del suono: il VIDC20 consuma un byte ogni SFR + 2 microsecondi
   e l'IOMD gli passa blocchi da 16 byte. Per ora i dati non si suonano:
   conta il ritmo (il POST misura quanto dura un buffer). */
static ArcTime sound_block(const RiscPc *m)
{
    return ARC_US(16 * ((m->vidc.sound_freq & 0xFF) + 2));
}

static void sound_update(RiscPc *m, ArcTime now)
{
    if (!iomd_sound_active(&m->iomd)) { m->snd_next = now + sound_block(m); return; }
    while (m->snd_next <= now) {
        uint32_t addr;
        if (!iomd_sound_next(&m->iomd, &addr)) { m->snd_next = now + sound_block(m); break; }
        m->snd_next += sound_block(m);
    }
}

/* per ora un frame fisso a 50 Hz con il flyback negli ultimi 2 ms */
#define FRAME   ARC_MS(20)
#define FLYBACK ARC_MS(18)

static void video_update(RiscPc *m, ArcTime now)
{
    while (now >= m->frame_start + FRAME) {
        m->frame_start += FRAME;
        m->frames++;
        if (m->flyback) { m->flyback = 0; iomd_set_flyback(&m->iomd, 0); }
    }
    int fly = now >= m->frame_start + FLYBACK;
    if (fly != m->flyback) { m->flyback = fly; iomd_set_flyback(&m->iomd, fly); }
}

void riscpc_run(RiscPc *m, ArcTime duration)
{
    ArcTime target = riscpc_now(m) + duration;
    while (!m->cpu.halted) {
        ArcTime now = riscpc_now(m);
        m->now = now;
        m->cycle_base = m->cpu.cycles;
        iomd_update(&m->iomd, now);
        sound_update(m, now);
        video_update(m, now);
        update_lines(m);
        if (now >= target) break;

        ArcTime next = target;
        ArcTime e = iomd_next_event(&m->iomd, now);  if (e < next) next = e;
        e = m->flyback ? m->frame_start + FRAME : m->frame_start + FLYBACK;
        if (e < next) next = e;
        if (iomd_sound_active(&m->iomd) && m->snd_next < next) next = m->snd_next > now ? m->snd_next : now + 1;

        uint64_t cycles = (((next - now) << 16) + m->units_per_cycle_q16 - 1) / m->units_per_cycle_q16;
        if (!cycles) cycles = 1;
        uint64_t stop = m->cpu.cycles + cycles;
        while (m->cpu.cycles < stop && !m->cpu.halted) arm6_step(&m->cpu);
    }
}

/* Lo schermo come lo legge il DMA video: da VIDINIT, e arrivato a VIDEND
   si riparte da VIDSTART (RISC OS fa scorrere il testo cosi'). Si copia
   in un buffer contiguo e il VIDC20 disegna da li'. */
typedef struct ScreenCopy { const uint8_t *data; uint32_t size; } ScreenCopy;

static const uint8_t *render_mem(void *ctx, uint32_t addr, uint32_t len)
{
    const ScreenCopy *sc = ctx;
    return addr + len <= sc->size ? sc->data + addr : NULL;
}

void riscpc_render(RiscPc *m, uint32_t *out, int stride, int *w, int *h)
{
    static uint8_t *copy;
    static uint32_t copy_size;
    int vw, vh;
    vidc20_size(&m->vidc, &vw, &vh);
    uint32_t bytes = (uint32_t)vw * (uint32_t)vh * (1u << vidc20_log2bpp(&m->vidc)) / 8;
    if (bytes > copy_size) {
        free(copy);
        copy = malloc(bytes);
        copy_size = copy ? bytes : 0;
    }
    ScreenCopy sc = { copy, 0 };
    /* VIDEND e' l'inizio dell'ultimo trasferimento: dalla VRAM mezza riga
       (&800 col bus a 64 bit, &400 a 32 bit: bit 3 e 2 di VIDCR), dalla
       DRAM 16 byte. Cosi' VIDEND + trasferimento = area dello schermo. */
    uint32_t xfer = (m->iomd.vidcr & 8) ? 0x800 : (m->iomd.vidcr & 4) ? 0x400 : 16;
    uint32_t start = m->iomd.vidstart, end = m->iomd.vidend + xfer, addr = m->iomd.vidinit;
    int wrap = end > start && addr >= start && addr < end;
    while (copy && sc.size < bytes) {
        uint32_t n = bytes - sc.size;
        if (wrap && addr + n > end) n = end - addr;
        /* a pezzi di al massimo 4 KB: la memoria fisica non e' per forza contigua */
        uint32_t page = 0x1000 - (addr & 0xFFF);
        if (n > page) n = page;
        const uint8_t *p = riscpc_phys(m, addr, n);
        if (p) memcpy(copy + sc.size, p, n);
        else   memset(copy + sc.size, 0, n);
        sc.size += n;
        addr += n;
        if (wrap && addr >= end) addr = start;
    }
    vidc20_render(&m->vidc, render_mem, &sc, 0, out, stride, w, h);
    int ch = vidc20_cursor_height(&m->vidc);
    if (ch) vidc20_draw_cursor(&m->vidc, riscpc_phys(m, m->iomd.cursinit, (uint32_t)ch * 8), out, stride, *w, *h);
}
