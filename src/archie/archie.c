/*
 * archie.c - Montaggio della macchina Archimedes (vedi archie.h)
 *
 * Periferiche nell'area dell'IOC (&3200000 + banco << 16):
 *   banco 0  registri dell'IOC
 *   banco 1  WD1772 (&3310000)
 *   banco 4  schede di espansione (&33C0000): la 0 porta il modulo HostFS
 *   banco 5  latch del floppy e della stampante (&3350000):
 *            +&18 latch B, +&40 latch A, +&50 IOEB (assente)
 *   gli altri banchi (Econet, seriale) sono vuoti.
 */
#include "archie.h"
#include "podule.h"
#include "hostfs.h"
#include "hostfs_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* tempo                                                              */
/* ------------------------------------------------------------------ */

ArcTime archie_now(const Archie *a)
{
    return a->now + (((a->cpu.cycles - a->cycle_base) * a->units_per_cycle_q16) >> 16);
}

/* Durata di un tick CPU, tenendo conto dei cicli rubati dal DMA video:
   il MEMC legge lo schermo a gruppi di 16 byte (1 ciclo N + 3 S = 5 tick),
   e in quel tempo la CPU e' ferma. */
static void update_speed(Archie *a)
{
    a->now = archie_now(a);
    a->cycle_base = a->cpu.cycles;
    double busy = a->faithful ? a->dma_fraction : 0.0;
    if (busy > 0.9) busy = 0.9;
    a->units_per_cycle_q16 = (uint32_t)(24.0 / a->mhz / (1.0 - busy) * 65536.0 + 0.5);
    if (!a->units_per_cycle_q16) a->units_per_cycle_q16 = 1;
}

void archie_set_mhz(Archie *a, double mhz)
{
    if (mhz <= 0) mhz = 8;
    a->mhz = mhz;
    update_speed(a);
}

void archie_set_faithful(Archie *a, int on)
{
    a->faithful = on;
    Arm2 *c = &a->cpu;
    if (on) {
        /* MEMC a 8 MHz: S = 125 ns, N = 250 ns; la ROM non ha accessi
           sequenziali: ogni fetch dura il tempo d'accesso programmato */
        c->s_ticks = 1;
        c->n_ticks = 2;
        c->slow_base = 0x3400000u;
    } else {
        c->s_ticks = c->n_ticks = 1;
        c->slow_base = 0xFFFFFFFFu;
        c->slow_fetch_extra = 0;
    }
    update_speed(a);
}

/* tempo d'accesso della ROM alta scelto dal sistema (bit 6-7 del
   controllo del MEMC): 450, 325, 200, 200 ns = 4, 3, 2, 2 tick */
static void rom_speed(Archie *a)
{
    static const uint32_t ticks[4] = { 4, 3, 2, 2 };
    if (a->faithful) a->cpu.slow_fetch_extra = ticks[(a->memc.control >> 6) & 3] - 1;
}

static void dma_load(Archie *a)
{
    double f = 0;
    if (a->memc.video_dma) {
        VidcTiming t;
        vidc_timing(&a->vidc, &t);
        if (t.valid && t.frame_time) {
            double bytes = (double)t.width * t.height * (1 << t.log2bpp) / 8.0;
            double ticks_frame = (double)t.frame_time / 3.0;       /* tick a 8 MHz */
            f = bytes / 16.0 * 5.0 / ticks_frame;
        }
    }
    if (f != a->dma_fraction) { a->dma_fraction = f; update_speed(a); }
}

static void update_lines(Archie *a)
{
    ioc_set_fiq_line(&a->ioc, 0x01, fdc_drq(&a->fdc));
    ioc_set_fiq_line(&a->ioc, 0x02, fdc_intrq(&a->fdc));
    a->cpu.irq_line = ioc_irq(&a->ioc);
    a->cpu.fiq_line = ioc_fiq(&a->ioc);
}

/* ------------------------------------------------------------------ */
/* collegamenti dell'IOC                                              */
/* ------------------------------------------------------------------ */

static uint8_t ioc_control_write(void *ctx, uint8_t v)
{
    Archie *a = ctx;
    a->sda_in = cmos_i2c(&a->cmos, (v >> 1) & 1, v & 1);
    return 0;
}

static uint8_t ioc_control_read(void *ctx)
{
    Archie *a = ctx;
    /* C0 = SDA della CMOS, gli altri pin in ingresso restano alti */
    return (uint8_t)(0x3E | (a->sda_in ? 1 : 0));
}

static void kart_to_keyboard(void *ctx, uint8_t b)   { kbd_rx(&((Archie *)ctx)->kbd, b); }
static int  keyboard_to_kart(void *ctx, uint8_t *b)  { return kbd_tx(&((Archie *)ctx)->kbd, b); }

/* ------------------------------------------------------------------ */
/* I/O                                                                */
/* ------------------------------------------------------------------ */

static uint32_t io_read(void *ctx, uint32_t addr, int is_byte)
{
    Archie *a = ctx;
    (void)is_byte;
    if (!(addr & 0x200000u)) return 0xFF;                /* MS0: niente */
    ArcTime now = archie_now(a);
    uint32_t bank = (addr >> 16) & 7, off = addr & 0xFFFF;
    uint32_t v = 0xFF;
    switch (bank) {
    case 0: v = ioc_read(&a->ioc, off, now); break;
    case 1:
        fdc_update(&a->fdc, now);
        v = fdc_read(&a->fdc, (int)((off >> 2) & 3), now);
        break;
    case 4: v = podule_read(a->podule_rom, a->podule_size, off); break;
    case 5:
        switch (off & 0xFC) {
        case 0x18: case 0x40: case 0x50: v = 0; break;
        case 0x70: v = 0x0F; break;
        default: v = 0xFF; break;
        }
        break;
    default: v = 0xFF; break;
    }
    update_lines(a);
    return v;
}

static void io_write(void *ctx, uint32_t addr, uint32_t value, int is_byte)
{
    Archie *a = ctx;
    if (!(addr & 0x200000u)) return;
    /* l'IOC prende il dato dalle linee D16-D23 (STRB replica il byte) */
    uint8_t v = is_byte ? (uint8_t)value : (uint8_t)(value >> 16);
    ArcTime now = archie_now(a);
    uint32_t bank = (addr >> 16) & 7, off = addr & 0xFFFF;
    switch (bank) {
    case 0: ioc_write(&a->ioc, off, v, now); break;
    case 1:
        fdc_update(&a->fdc, now);
        fdc_write(&a->fdc, (int)((off >> 2) & 3), v, now);
        break;
    case 5:
        switch (off & 0xFC) {
        case 0x18: a->latch_b = v; fdc_latch_b(&a->fdc, v); break;
        case 0x40:
            a->latch_a = v;
            fdc_latch_a(&a->fdc, v);
            if (a->latch_a_hook) a->latch_a_hook(a->hook_user, v, now);
            break;
        default: break;
        }
        break;
    default: break;
    }
    update_lines(a);
}

static void vidc_write_hook(void *ctx, uint32_t v)
{
    Archie *a = ctx;
    if ((v >> 24) == 0xC0) a->sound_sfr = v & 0xFF;    /* frequenza del suono */
    vidc_write(&a->vidc, v);
}

/* ------------------------------------------------------------------ */
/* audio                                                              */
/* ------------------------------------------------------------------ */

/* Il VIDC riceve dal MEMC byte in formato logaritmico (segno nel bit 0,
   "chord" nei bit 7-5, "point" nei bit 4-1) e li manda al DAC uno alla
   volta, ciascuno per SFR+2 us, girando sugli 8 canali stereo. */
static int16_t ulaw[256];
#define OUT_PERIOD (ARC_HZ / ARCHIE_AUDIO_HZ)      /* 500 unita' */

static void build_ulaw(void)
{
    for (int raw = 0; raw < 256; raw++) {
        int chord = raw >> 5, point = (raw & 0x1E) >> 1;
        int v = ((16 + point) << chord) - 16;
        ulaw[raw] = (int16_t)((raw & 1) ? -v * 8 : v * 8);
    }
}

/* guadagno dell'amplificatore dopo il DAC: il VIDC divide il tempo fra 8
   canali, e una nota a volume pieno arriva a circa meta' scala */
#define OUT_GAIN 2.0

static void emit_sample(Archie *a)
{
    double l = a->acc_l / OUT_PERIOD * OUT_GAIN, r = a->acc_r / OUT_PERIOD * OUT_GAIN;
    l = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
    r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    uint32_t i = a->audio_w % ARCHIE_AUDIO_FRAMES;
    a->audio[2 * i] = (int16_t)l;
    a->audio[2 * i + 1] = (int16_t)r;
    a->audio_w++;
    if (a->audio_w - a->audio_r > ARCHIE_AUDIO_FRAMES) a->audio_r = a->audio_w - ARCHIE_AUDIO_FRAMES;
    a->acc_l = a->acc_r = 0;
}

/* aggiunge il valore (l, r) per l'intervallo [t0, t1) all'uscita a 48 kHz */
static void mix(Archie *a, ArcTime t0, ArcTime t1, double l, double r)
{
    while (t0 < t1) {
        ArcTime end = a->out_t + OUT_PERIOD;
        ArcTime seg = t1 < end ? t1 : end;
        double w = (double)(seg - t0);
        a->acc_l += l * w;
        a->acc_r += r * w;
        t0 = seg;
        if (seg == end) { emit_sample(a); a->out_t = end; }
    }
}

static void sound_update(Archie *a, ArcTime now)
{
    if (!a->audio) return;
    if (!a->memc.sound_dma || a->sound_sfr + 2 < 3) {
        /* silenzio */
        if (a->out_t + OUT_PERIOD * 4 < now) a->out_t = now - OUT_PERIOD * 4;   /* non rincorre pause lunghe */
        mix(a, a->out_t, now, 0, 0);
        a->snd_next = now;
        return;
    }
    ArcTime period = ARC_US(a->sound_sfr + 2);
    if (a->snd_next + ARC_MS(100) < now) a->snd_next = now - ARC_MS(100);
    if (a->out_t + ARC_MS(100) < a->snd_next) a->out_t = a->snd_next;
    static const float left[8] = { 9, 18, 15, 12, 9, 6, 3, 0 };   /* su 18 */
    while (a->snd_next + period <= now) {
        uint8_t b = a->ram[a->snd_ptr % a->ram_size];
        int ch = (int)(a->snd_ptr & 7);
        double v = ulaw[b], pl = left[a->vidc.stereo[ch] & 7] / 18.0;
        mix(a, a->snd_next, a->snd_next + period, v * pl, v * (1.0 - pl));
        a->snd_next += period;
        a->snd_ptr++;
        /* fine del buffer: si passa al prossimo e si chiede a RISC OS di riempirne un altro */
        if (a->snd_ptr >= a->snd_end + 16) {
            a->snd_ptr = a->memc.sstart;
            a->snd_end = a->memc.send;
            ioc_set_irqb_line(&a->ioc, 0x02, 1);
        }
    }
}

static void sound_changed(void *ctx)
{
    Archie *a = ctx;
    ArcTime now = archie_now(a);
    sound_update(a, now);
    switch (a->memc.sound_reg) {
    case 4:                                       /* nuovo buffer: richiesta soddisfatta */
        ioc_set_irqb_line(&a->ioc, 0x02, 0);
        break;
    case 6:                                       /* ripartenza del puntatore */
        a->snd_ptr = a->memc.sstart;
        a->snd_end = a->memc.send;
        ioc_set_irqb_line(&a->ioc, 0x02, 1);
        break;
    default:                                      /* controllo: DMA acceso o spento */
        if (a->memc.sound_dma && a->snd_end == 0) {
            a->snd_ptr = a->memc.sstart;
            a->snd_end = a->memc.send;
        }
        a->snd_next = now;
        break;
    }
    update_lines(a);
}

uint32_t archie_audio_read(Archie *a, int16_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && a->audio_r != a->audio_w) {
        uint32_t i = a->audio_r % ARCHIE_AUDIO_FRAMES;
        out[2 * n] = a->audio[2 * i];
        out[2 * n + 1] = a->audio[2 * i + 1];
        a->audio_r++;
        n++;
    }
    return n;
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

static int insert_floppy(void *ctx, int drive, const char *path)
{
    return fdc_insert(&((Archie *)ctx)->fdc, drive, path);
}

/* le SWI riservate del modulo HostFS: il lavoro lo fa l'host */
static int archie_swi(Arm2 *cpu, uint32_t comment, void *user)
{
    Archie *a = user;
    uint32_t n = comment & ~0x20000u;                 /* senza il bit X */
    if (!a->hostfs || n < ARC_HOSTFS_SWI || n >= ARC_HOSTFS_SWI + 8) return 0;
    arc_hostfs_entry(a->hostfs, cpu, (int)(n - ARC_HOSTFS_SWI));
    return 1;
}

void archie_reset(Archie *a)
{
    if (a->hostfs) arc_hostfs_close_all(a->hostfs);
    memc_reset(&a->memc);
    ioc_reset(&a->ioc, a->now);
    vidc_reset(&a->vidc);
    kbd_reset(&a->kbd);
    fdc_reset(&a->fdc);
    arm2_reset(&a->cpu);
    a->frame_start = a->now;
    a->flyback = 0;
    a->snd_ptr = a->snd_end = 0;
    a->snd_next = a->out_t = a->now;
    update_lines(a);
}

/* cambia la RAM (1, 2 o 4 MB) e riavvia: RISC OS la misura all'accensione */
int archie_set_ram(Archie *a, uint32_t mb)
{
    if (mb != 1 && mb != 2 && mb != 4) return 0;
    uint8_t *ram = calloc(1, mb << 20);
    if (!ram) return 0;
    free(a->ram);
    a->ram = ram;
    a->ram_size = mb << 20;
    a->memc.ram = ram;
    a->memc.ram_size = a->ram_size;
    archie_reset(a);
    return 1;
}

int archie_create(Archie *a, const ArchieConfig *cfg, char *err, size_t errsize)
{
    memset(a, 0, sizeof *a);
    uint32_t mb = cfg->ram_mb ? cfg->ram_mb : 4;
    if (mb != 1 && mb != 2 && mb != 4) mb = 4;
    a->ram_size = mb << 20;
    a->ram = calloc(1, a->ram_size);
    a->audio = calloc(ARCHIE_AUDIO_FRAMES * 2, sizeof(int16_t));
    build_ulaw();

    uint32_t n = 0;
    uint8_t *raw = load_file(cfg->rom_path, &n);
    if (!a->ram || !raw) { snprintf(err, errsize, "impossibile leggere la ROM %s", cfg->rom_path); free(raw); return 0; }
    /* la dimensione utile e' la potenza di due piu' vicina (alcuni dump hanno un byte in piu') */
    uint32_t size = 1;
    while (size * 2 <= n) size *= 2;
    if (size < 0x80000u || size > 0x200000u) {
        snprintf(err, errsize, "%s: dimensione %u non valida per un Archimedes (512 KB - 2 MB)", cfg->rom_path, n);
        free(raw);
        return 0;
    }
    a->rom = raw;
    a->rom_size = size;

    MemcIo io = { a, io_read, io_write, vidc_write_hook, sound_changed };
    memc_init(&a->memc, a->ram, a->ram_size, a->rom, a->rom_size, &a->cpu, &io);
    ArmBus bus;
    memc_attach_cpu(&a->memc, &bus);
    arm2_init(&a->cpu, &bus);

    IocHooks hooks = { a, ioc_control_write, ioc_control_read, kart_to_keyboard, keyboard_to_kart };
    ioc_init(&a->ioc, &hooks);
    vidc_reset(&a->vidc);
    kbd_reset(&a->kbd);
    if (cfg->cmos_path) snprintf(a->cmos_path, sizeof a->cmos_path, "%s", cfg->cmos_path);
    cmos_init(&a->cmos, cfg->cmos_path);
    a->sda_in = 1;
    fdc_reset(&a->fdc);
    for (int d = 0; d < 2; d++) {
        if (cfg->floppy[d] && !fdc_insert(&a->fdc, d, cfg->floppy[d])) {
            snprintf(err, errsize, "immagine floppy non valida: %s", cfg->floppy[d]);
            return 0;
        }
    }
    if (cfg->hostfs_dir && cfg->hostfs_dir[0]) {
        /* scheda 0 con il modulo HostFS: RISC OS lo carica all'avvio */
        a->podule_size = PODULE_WINDOW;
        a->podule_rom = malloc(a->podule_size);
        a->hostfs = malloc(sizeof *a->hostfs);
        if (!a->podule_rom || !a->hostfs ||
            !podule_build(a->podule_rom, a->podule_size, hostfs_module, sizeof hostfs_module, "ArchieEmu HostFS")) {
            snprintf(err, errsize, "impossibile preparare la scheda HostFS");
            return 0;
        }
        arc_hostfs_init(a->hostfs, cfg->hostfs_dir, &a->memc);
        a->hostfs->insert = insert_floppy;
        a->hostfs->insert_ctx = a;
        a->cpu.swi_hook = archie_swi;
        a->cpu.swi_user = a;
    }
    a->mhz = cfg->mhz > 0 ? cfg->mhz : 8;
    archie_set_faithful(a, 1);
    archie_reset(a);
    return 1;
}

void archie_destroy(Archie *a)
{
    if (a->cmos_path[0] && a->cmos.dirty) cmos_save(&a->cmos, a->cmos_path);
    for (int d = 0; d < FDC_DRIVES; d++) fdc_eject(&a->fdc, d);
    if (a->hostfs) arc_hostfs_close_all(a->hostfs);
    free(a->hostfs);
    free(a->podule_rom);
    free(a->ram);
    free(a->rom);
    free(a->audio);
    memset(a, 0, sizeof *a);
}

/* ------------------------------------------------------------------ */
/* esecuzione                                                         */
/* ------------------------------------------------------------------ */

/* geometria del frame: prima che RISC OS programmi il VIDC, 50 Hz */
static void frame_times(const Archie *a, ArcTime *frame, ArcTime *flyback_at)
{
    VidcTiming t;
    vidc_timing(&a->vidc, &t);
    if (t.valid && t.frame_time > ARC_MS(5) && t.frame_time < ARC_MS(100)) {
        *frame = t.frame_time;
        *flyback_at = (ArcTime)t.display_end * t.line_time;
        if (*flyback_at >= *frame) *flyback_at = *frame - ARC_US(500);
    } else {
        *frame = ARC_MS(20);
        *flyback_at = ARC_MS(18);
    }
}

static void video_update(Archie *a, ArcTime now)
{
    ArcTime frame, fb;
    frame_times(a, &frame, &fb);
    while (now >= a->frame_start + frame) {
        a->frame_start += frame;
        a->frames++;
        dma_load(a);
        rom_speed(a);
        if (a->flyback) { a->flyback = 0; ioc_set_ir(&a->ioc, 0); }
    }
    int fly = now >= a->frame_start + fb;
    if (fly != a->flyback) { a->flyback = fly; ioc_set_ir(&a->ioc, fly); }
}

static ArcTime video_next_event(const Archie *a)
{
    ArcTime frame, fb;
    frame_times(a, &frame, &fb);
    return a->flyback ? a->frame_start + frame : a->frame_start + fb;
}


void archie_run(Archie *a, ArcTime duration)
{
    ArcTime target = archie_now(a) + duration;
    while (!a->cpu.halted) {
        ArcTime now = archie_now(a);
        a->now = now;
        a->cycle_base = a->cpu.cycles;

        ioc_update(&a->ioc, now);
        fdc_update(&a->fdc, now);
        video_update(a, now);
        sound_update(a, now);
        update_lines(a);
        if (now >= target) break;

        ArcTime next = target;
        ArcTime e = ioc_next_event(&a->ioc, now);  if (e < next) next = e;
        e = fdc_next_event(&a->fdc);                if (e > now && e < next) next = e;
        e = video_next_event(a);                    if (e < next) next = e;
        /* il DMA audio: aggiornato ogni ~0,5 ms per dare IL1 in tempo */
        if (a->memc.sound_dma && now + ARC_US(500) < next) next = now + ARC_US(500);

        uint64_t cycles = (((next - now) << 16) + a->units_per_cycle_q16 - 1) / a->units_per_cycle_q16;
        if (!cycles) cycles = 1;
        uint64_t stop = a->cpu.cycles + cycles;
        while (a->cpu.cycles < stop && !a->cpu.halted) arm2_step(&a->cpu);
    }
}

void archie_render(Archie *a, uint32_t *out, int stride, int *w, int *h)
{
    const Memc *m = &a->memc;
    vidc_render(&a->vidc, a->ram, a->ram_size, m->vinit, m->vstart, m->vend,
                m->cinit, m->cursor_enabled, out, stride, w, h);
}
