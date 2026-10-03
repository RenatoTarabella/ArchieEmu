/*
 * riscpc.c - Montaggio della macchina Risc PC (vedi riscpc.h)
 */
#include "riscpc.h"
#include "../archie/hostfs.h"
#include "../archie/hostfs_module.h"
#include "../archie/podule.h"
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

static void build_ulaw(void);

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
    /* floppy: interrupt sull'IRQ B bit 4, richiesta di dato sul FIQ bit 0 */
    iomd_set_irqb_line(&m->iomd, 0x10, fdc82077_irq(&m->sio.fdc));
    iomd_set_irqb_line(&m->iomd, 0x02, ide_irq(&m->sio.ide));     /* IDE: IRQ B bit 1 (lo abilita ADFS) */
    if (fdc82077_drq(&m->sio.fdc)) m->iomd.fiq |= 0x01; else m->iomd.fiq &= (uint8_t)~0x01;
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

/* Un accesso al floppy puo' programmare un evento vicino (il prossimo byte
   dopo 16-32 us): la fetta di CPU in corso si accorcia fin li', altrimenti
   l'evento verrebbe visto solo alla fine della fetta. */
static void reschedule(RiscPc *m, ArcTime now)
{
    ArcTime e = fdc82077_next_event(&m->sio.fdc);
    if (e == ~(ArcTime)0 || !m->slice_stop) return;
    uint64_t at = m->cpu.cycles + (e > now ? (((e - now) << 16) + m->units_per_cycle_q16 - 1) / m->units_per_cycle_q16 : 1);
    if (at < m->slice_stop) m->slice_stop = at;
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
    } else if ((a & 0xFFFFF000u) == 0x03010000u) {
        /* a parola: il registro dati dell'IDE a 16 bit */
        v = size == 4 ? superio_read16(&m->sio, (a & 0xFFF) >> 2, now, &known)
                      : superio_read(&m->sio, (a & 0xFFF) >> 2, now, &known);
        update_lines(m);
        reschedule(m, now);
    } else if ((a & 0xFFFFF000u) == 0x03012000u || (a & 0xFFFFF000u) == 0x0302A000u) {
        /* DACK del floppy: il gestore FIQ di ADFS prende qui i byte; a
           &0302A000 con il terminal count (l'ultimo byte) */
        v = fdc82077_dack_read(&m->sio.fdc, (a & 0xFFFFF000u) == 0x0302A000u, now);
        known = 1;
        update_lines(m);
        reschedule(m, now);
    } else if ((a & 0xFFFF0000u) == 0x03310000u) {
        /* tasti del mouse, attivi bassi: bit 4 Adjust, 5 Menu, 6 Select */
        v = (uint32_t)(~m->mouse_buttons & 7u) << 4;
        known = 1;
    } else if ((a & 0xFFFF0000u) == 0x033C0000u) {
        /* schede: la 0 con il modulo HostFS, le altre leggono &FF (assenti) */
        v = podule_read(m->podule_rom, m->podule_size, a & 0xFFFF);
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
    } else if ((a & 0xFFFFF000u) == 0x03010000u) {
        if (size == 4) superio_write16(&m->sio, (a & 0xFFF) >> 2, (uint16_t)v, now, &known);
        else           superio_write(&m->sio, (a & 0xFFF) >> 2, (uint8_t)v, now, &known);
        update_lines(m);
        reschedule(m, now);
    } else if ((a & 0xFFFFF000u) == 0x03012000u || (a & 0xFFFFF000u) == 0x0302A000u) {
        fdc82077_dack_write(&m->sio.fdc, (uint8_t)v, (a & 0xFFFFF000u) == 0x0302A000u, now);
        known = 1;
        update_lines(m);
        reschedule(m, now);
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

/* mezze parole (LDRH/STRH dello StrongARM): memoria diretta, il dato dell'IDE */
static uint16_t bus_r16(void *ctx, uint32_t a, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a, &ro);
    if (p) return (uint16_t)(p[0] | p[1] << 8);
    if ((a & 0xFFFFF000u) == 0x03010000u) {
        int known;
        return superio_read16(&m->sio, (a & 0xFFF) >> 2, riscpc_now(m), &known);
    }
    return (uint16_t)(io_read(m, a & ~3u, 2) >> ((a & 2) * 8));
}

static void bus_w16(void *ctx, uint32_t a, uint16_t v, int *abort)
{
    RiscPc *m = ctx;
    int ro;
    (void)abort;
    uint8_t *p = mem_at(m, a, &ro);
    if (p) { if (!ro) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); } return; }
    if ((a & 0xFFFFF000u) == 0x03010000u) {
        int known;
        superio_write16(&m->sio, (a & 0xFFF) >> 2, v, riscpc_now(m), &known);
        update_lines(m);
        return;
    }
    io_write(m, a & ~3u, v * 0x00010001u, 2);
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
    /* C2: la linea /DSKCHG dell'unita' selezionata, attiva bassa come sul
       cavo: 0 = disco assente o appena cambiato, 1 = disco presente (torna
       a 1 al primo passo della testina con il disco dentro) */
    const FdcDrive *d = &m->sio.fdc.media.drive[m->sio.fdc.dor & 3];
    int changed = !d->image || d->disc_changed;
    return (uint8_t)(0x3A | (changed ? 0 : 4) | (m->sda_in ? 1 : 0));
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

/* ------------------------------------------------------------------ */
/* HostFS                                                             */
/* ------------------------------------------------------------------ */

/* memoria logica di RISC OS: attraverso la MMU (permessi del supervisore) */
static uint8_t *logical(RiscPc *m, uint32_t va, int write)
{
    uint32_t pa;
    if (arm6_translate(&m->cpu, va, write, 0, &pa)) return NULL;
    return (uint8_t *)riscpc_phys(m, pa, 1);
}

static uint8_t hostfs_rd8(void *ctx, uint32_t a)
{
    uint8_t *p = logical(ctx, a, 0);
    return p ? *p : 0;
}

static void hostfs_wr8(void *ctx, uint32_t a, uint8_t v)
{
    RiscPc *m = ctx;
    uint8_t *p = logical(m, a, 1);
    int ro;
    (void)ro;
    if (p && (a >> 24) != 0) *p = v;            /* (la ROM a 0 non si scrive) */
    else if (p) *p = v;
}

/* le SWI riservate del modulo HostFS: il lavoro lo fa l'host */
static int riscpc_swi(Arm6 *cpu, uint32_t comment, void *user)
{
    RiscPc *m = user;
    uint32_t n = comment & ~0x20000u;            /* senza il bit X */
    if (!m->hostfs || n < ARC_HOSTFS_SWI || n >= ARC_HOSTFS_SWI + 8) return 0;
    HostFsRegs regs;
    memcpy(regs.r, cpu->r, sizeof regs.r);
    regs.v = (cpu->cpsr & ARM6_V) != 0;
    arc_hostfs_call(m->hostfs, &regs, (int)(n - ARC_HOSTFS_SWI));
    memcpy(cpu->r, regs.r, 15 * sizeof(uint32_t));
    cpu->cpsr = regs.v ? cpu->cpsr | ARM6_V : cpu->cpsr & ~ARM6_V;
    return 1;
}

static int hostfs_insert(void *ctx, int drive, const char *path)
{
    return riscpc_insert_floppy(ctx, drive, path);
}

int riscpc_insert_floppy(RiscPc *m, int drive, const char *path)
{
    return fdc_insert(&m->sio.fdc.media, drive, path);
}

void riscpc_eject_floppy(RiscPc *m, int drive)
{
    fdc_eject(&m->sio.fdc.media, drive);
}

int riscpc_attach_hd(RiscPc *m, const char *path)
{
    if (!ide_attach(&m->sio.ide, path)) return 0;
    /* IDEDiscs (CMOS fisica &C7, bit 6-7): senza, ADFS non cerca il disco */
    if (!(m->cmos.ram[0xC7] & 0xC0)) {
        m->cmos.ram[0xC7] |= 0x40;
        cmos_fix_checksum(&m->cmos);
        m->cmos.dirty = 1;
    }
    return 1;
}

void riscpc_detach_hd(RiscPc *m)
{
    ide_detach(&m->sio.ide);
    if (m->hostfs) arc_hostfs_close_all(m->hostfs);
    free(m->hostfs);
    free(m->podule_rom);
}

void riscpc_reset(RiscPc *m)
{
    if (m->hostfs) arc_hostfs_close_all(m->hostfs);
    iomd_reset(&m->iomd, m->now);
    superio_reset(&m->sio, m->now);
    ps2kbd_reset(&m->kbd);
    vidc20_reset(&m->vidc);
    arm6_reset(&m->cpu);
    m->frame_start = m->now;
    m->index_next = m->now - m->now % FDC82077_REV + FDC82077_REV;
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
    m->audio = calloc(RISCPC_AUDIO_FRAMES * 2, sizeof(int16_t));
    build_ulaw();
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

    ArmBus bus = { m, bus_r32, bus_r8, bus_w32, bus_w8, bus_r16, bus_w16 };
    arm6_init(&m->cpu, &bus, cfg->strongarm ? ARM6_ID_SA110 : cfg->arm710 ? ARM6_ID_ARM710 : ARM6_ID_ARM610);
    IomdHooks hooks = { m, lines_write, lines_read, to_keyboard, from_keyboard };
    superio_init(&m->sio);
    iomd_init(&m->iomd, &hooks);
    if (cfg->cmos_path) snprintf(m->cmos_path, sizeof m->cmos_path, "%s", cfg->cmos_path);
    cmos_init(&m->cmos, cfg->cmos_path);
    m->sda_in = 1;
    if (cfg->hostfs_dir && cfg->hostfs_dir[0]) {
        /* scheda 0 con il modulo HostFS: RISC OS lo carica all'avvio */
        m->podule_size = PODULE_WINDOW;
        m->podule_rom = malloc(m->podule_size);
        m->hostfs = malloc(sizeof *m->hostfs);
        if (!m->podule_rom || !m->hostfs ||
            !podule_build(m->podule_rom, m->podule_size, hostfs_module, sizeof hostfs_module, "ArchieEmu HostFS")) {
            snprintf(err, errsize, "impossibile preparare la scheda HostFS");
            return 0;
        }
        arc_hostfs_init_mem(m->hostfs, cfg->hostfs_dir, hostfs_rd8, hostfs_wr8, m);
        m->hostfs->insert = hostfs_insert;
        m->hostfs->insert_ctx = m;
        m->cpu.swi_hook = riscpc_swi;
        m->cpu.swi_user = m;
    }
    /* lo StrongARM vero va a 202 MHz ma col bus a 16 MHz; senza le cache e la
       banda della memoria emulate, 100 MHz equivalenti (che stanno nel tempo reale) */
    m->mhz = cfg->mhz > 0 ? cfg->mhz : cfg->strongarm ? 100 : cfg->arm710 ? 40 : 30;
    update_speed(m);
    riscpc_reset(m);
    return 1;
}

void riscpc_destroy(RiscPc *m)
{
    for (int d = 0; d < FDC_DRIVES; d++) fdc_eject(&m->sio.fdc.media, d);
    ide_detach(&m->sio.ide);
    if (m->cmos_path[0] && m->cmos.dirty) cmos_save(&m->cmos, m->cmos_path);
    free(m->rom);
    free(m->ram);
    free(m->vram);
    free(m->audio);
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

/* Il VIDC20 suona i byte in formato logaritmico come il VIDC1 (segno nel
   bit 0, "chord" nei bit 7-5, "point" nei bit 4-1), uno ogni SFR + 2 us,
   girando sugli 8 canali stereo (il canale e' l'indirizzo del byte & 7). */
static int16_t ulaw[256];
#define OUT_PERIOD (ARC_HZ / RISCPC_AUDIO_HZ)       /* 500 unita' */
#define OUT_GAIN   2.0

static void build_ulaw(void)
{
    for (int raw = 0; raw < 256; raw++) {
        int chord = raw >> 5, point = (raw & 0x1E) >> 1;
        int v = ((16 + point) << chord) - 16;
        ulaw[raw] = (int16_t)((raw & 1) ? -v * 8 : v * 8);
    }
}

static void emit_sample(RiscPc *m)
{
    double l = m->acc_l / OUT_PERIOD * OUT_GAIN, r = m->acc_r / OUT_PERIOD * OUT_GAIN;
    l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
    r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    uint32_t i = m->audio_w % RISCPC_AUDIO_FRAMES;
    m->audio[2 * i] = (int16_t)l;
    m->audio[2 * i + 1] = (int16_t)r;
    m->audio_w++;
    if (m->audio_w - m->audio_r > RISCPC_AUDIO_FRAMES) m->audio_r = m->audio_w - RISCPC_AUDIO_FRAMES;
    m->acc_l = m->acc_r = 0;
}

/* aggiunge il valore (l, r) per l'intervallo [t0, t1) all'uscita a 48 kHz */
static void mix(RiscPc *m, ArcTime t0, ArcTime t1, double l, double r)
{
    while (t0 < t1) {
        ArcTime end = m->out_t + OUT_PERIOD;
        ArcTime seg = t1 < end ? t1 : end;
        double w = (double)(seg - t0);
        m->acc_l += l * w;
        m->acc_r += r * w;
        t0 = seg;
        if (seg == end) { emit_sample(m); m->out_t = end; }
    }
}

uint32_t riscpc_audio_read(RiscPc *m, int16_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && m->audio_r != m->audio_w) {
        uint32_t i = m->audio_r % RISCPC_AUDIO_FRAMES;
        out[2 * n] = m->audio[2 * i];
        out[2 * n + 1] = m->audio[2 * i + 1];
        m->audio_r++;
        n++;
    }
    return n;
}

static void sound_update(RiscPc *m, ArcTime now)
{
    static const float left[8] = { 9, 18, 15, 12, 9, 6, 3, 0 };   /* su 18, come sul VIDC1 */
    ArcTime period = ARC_US((m->vidc.sound_freq & 0xFF) + 2);
    if (!iomd_sound_active(&m->iomd)) {
        /* silenzio, senza rincorrere le pause lunghe */
        if (m->audio) {
            if (m->out_t + OUT_PERIOD * 4 < now) m->out_t = now - OUT_PERIOD * 4;
            mix(m, m->out_t, now, 0, 0);
        }
        m->snd_next = now + sound_block(m);
        return;
    }
    if (m->snd_next + ARC_MS(100) < now) m->snd_next = now - ARC_MS(100);
    if (m->out_t + ARC_MS(100) < m->snd_next) m->out_t = m->snd_next;
    while (m->snd_next <= now) {
        uint32_t addr;
        if (!iomd_sound_next(&m->iomd, &addr)) { m->snd_next = now + sound_block(m); break; }
        const uint8_t *p = riscpc_phys(m, addr, 16);
        ArcTime t = m->snd_next;
        if (m->vidc.sound_ctrl & 2) {
            /* 16 bit (RISC OS 3.6/3.7 con "SoundSystem 16bit", controllo &03):
               frame stereo lineari di 4 byte (prima il destro, poi il
               sinistro: con *Stereo 1 -127 il suono e' tutto nel secondo),
               stesso ritmo dei byte, quindi un frame dura 4 periodi */
            for (int k = 0; k < 16 && m->audio; k += 4, t += 4 * period) {
                double r = p ? (int16_t)(p[k] | p[k + 1] << 8) : 0;
                double l = p ? (int16_t)(p[k + 2] | p[k + 3] << 8) : 0;
                mix(m, t, t + 4 * period, l / OUT_GAIN, r / OUT_GAIN);
            }
        } else for (int k = 0; k < 16; k++, t += period) {
            if (!m->audio) continue;
            int ch = (int)((addr + (uint32_t)k) & 7);
            double v = p ? ulaw[p[k]] : 0, pl = left[m->vidc.stereo[ch] & 7] / 18.0;
            mix(m, t, t + period, v * pl, v * (1.0 - pl));
        }
        m->snd_next += sound_block(m);
    }
}

/* Durata del frame e inizio del flyback (fine del display) dai registri
   del VIDC20; prima che RISC OS li programmi, 50 Hz. */
static void frame_times(const RiscPc *m, ArcTime *frame, ArcTime *flyback_at)
{
    Vidc20Timing t;
    if (vidc20_timing(&m->vidc, &t) && t.frame_time > ARC_MS(5) && t.frame_time < ARC_MS(100)) {
        *frame = t.frame_time;
        *flyback_at = t.flyback_at < t.frame_time ? t.flyback_at : t.frame_time - ARC_US(500);
    } else {
        *frame = ARC_MS(20);
        *flyback_at = ARC_MS(18);
    }
}

static void video_update(RiscPc *m, ArcTime now)
{
    ArcTime frame, fb;
    frame_times(m, &frame, &fb);
    while (now >= m->frame_start + frame) {
        m->frame_start += frame;
        m->frames++;
        if (m->flyback) { m->flyback = 0; iomd_set_flyback(&m->iomd, 0); }
    }
    int fly = now >= m->frame_start + fb;
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
        fdc82077_update(&m->sio.fdc, now);
        /* impulso di indice del floppy (IRQ A bit 2): ADFS li conta per
           capire se nell'unita' c'e' un disco che gira */
        while (now >= m->index_next) {
            if (fdc82077_spinning(&m->sio.fdc)) m->iomd.irqa |= 0x04;
            m->index_next += FDC82077_REV;
        }
        sound_update(m, now);
        video_update(m, now);
        update_lines(m);
        if (now >= target) break;

        ArcTime next = target;
        ArcTime e = iomd_next_event(&m->iomd, now);  if (e < next) next = e;
        {
            ArcTime frame, fb;
            frame_times(m, &frame, &fb);
            e = m->flyback ? m->frame_start + frame : m->frame_start + fb;
            if (e < next) next = e;
        }
        e = fdc82077_next_event(&m->sio.fdc);         if (e > now && e < next) next = e;
        if (m->index_next < next) next = m->index_next;
        if (iomd_sound_active(&m->iomd) && m->snd_next < next) next = m->snd_next > now ? m->snd_next : now + 1;

        uint64_t cycles = (((next - now) << 16) + m->units_per_cycle_q16 - 1) / m->units_per_cycle_q16;
        if (!cycles) cycles = 1;
        m->slice_stop = m->cpu.cycles + cycles;
        while (m->cpu.cycles < m->slice_stop && !m->cpu.halted) arm6_step(&m->cpu);
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
