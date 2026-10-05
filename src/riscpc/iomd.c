/*
 * iomd.c - IOMD (vedi iomd.h)
 *
 * Interrupt e timer funzionano come sull'IOC (timer a 2 MHz che contano
 * all'indietro e ricaricano). I registri che non conosciamo ancora tornano
 * con *known = 0, cosi' la macchina li puo' segnalare.
 */
#include "iomd.h"
#include <string.h>

#define TIMER_TICK 12u                     /* unita' da 24 MHz per tick a 2 MHz */
#define KBD_BYTE   ARC_US(1000)            /* 11 bit a ~11 kHz */

void iomd_init(Iomd *m, const IomdHooks *hooks)
{
    memset(m, 0, sizeof *m);
    m->hooks = *hooks;
    iomd_reset(m, 0);
}

void iomd_set_type(Iomd *m, int type) { m->type = type; }

void iomd_reset(Iomd *m, ArcTime now)
{
    IomdHooks h = m->hooks;
    int type = m->type;
    memset(m, 0, sizeof *m);
    m->hooks = h;
    m->type = type;
    m->iocr = 0xFF;
    if (type != IOMD_RISCPC) {
        /* nEVENT2 deve leggere 1: a 0 RISC OS si crede sul portatile
           (Stork) e cerca il suo microcontrollore della batteria */
        m->irqd = 0x18;
        m->vrefcr = 1;
        m->atod[0] = 0x0F;
        m->romcr[0] = m->romcr[1] = 0x40;
    }
    m->irqa = 0x80 | 0x10;                 /* forzato + accensione */
    m->irqb = 0;
    m->fiq = 0x80;
    for (int i = 0; i < 2; i++) m->timer[i].start = now;
}

/* ------------------------------------------------------------------ */
/* timer                                                              */
/* ------------------------------------------------------------------ */

static ArcTime timer_period(const IomdTimer *t)
{
    return (ArcTime)(t->latch ? t->latch : 0x10000) * TIMER_TICK;
}

/* PS/2: la tastiera sui bit 6 e 7 dell'IRQ B, il mouse sui bit 1 e 0
   dell'IRQ D (trasmettitore vuoto, byte ricevuto) */
static void ps2_lines(Iomd *m)
{
    const Ps2Port *k = &m->ps2[0], *ms = &m->ps2[1];
    int ena = (k->cr & 0x08) != 0;
    m->irqb = (uint8_t)((m->irqb & 0x3F) | (ena && !k->tx_busy ? 0x40 : 0) | (ena && k->rx_full ? 0x80 : 0));
    ena = (ms->cr & 0x08) != 0;
    m->irqd = (uint8_t)((m->irqd & ~3) | (ena && !ms->tx_busy ? 0x02 : 0) | (ena && ms->rx_full ? 0x01 : 0));
}

static void ps2_update(Iomd *m, int n, ArcTime now)
{
    Ps2Port *p = &m->ps2[n];
    void (*to)(void *, uint8_t) = n ? m->hooks.to_mouse : m->hooks.to_keyboard;
    int (*from)(void *, uint8_t *) = n ? m->hooks.from_mouse : m->hooks.from_keyboard;
    if (p->tx_busy && now >= p->tx_done) {
        p->tx_busy = 0;
        if (to) to(m->hooks.ctx, p->tx);
        if (p->rx_next < now + ARC_US(200)) p->rx_next = now + ARC_US(200);
    }
    /* un byte dal dispositivo solo dopo che il computer ha letto il precedente */
    if ((p->cr & 0x08) && !p->rx_full && now >= p->rx_next && from) {
        uint8_t b;
        if (from(m->hooks.ctx, &b)) {
            p->rx = b;
            p->rx_full = 1;
            p->rx_next = now + KBD_BYTE;
        } else {
            p->rx_next = now + ARC_US(1000);
        }
    }
}

static uint32_t ps2_read_cr(const Ps2Port *p)
{
    uint8_t par = 1;                            /* parita' dispari */
    for (uint8_t x = p->rx; x; x >>= 1) par ^= x & 1;
    return (uint32_t)((p->cr & 0x08) | (p->tx_busy ? 0x40 : 0x80) | (p->rx_full ? 0x20 : 0) | (par ? 0x04 : 0) | 0x03);
}

static void ps2_write_data(Iomd *m, int n, uint8_t b, ArcTime now)
{
    Ps2Port *p = &m->ps2[n];
    if (p->cr & 0x08) {
        p->tx = b;
        p->tx_busy = 1;
        p->tx_done = now + KBD_BYTE;
    }
    ps2_lines(m);
}

static void ps2_write_cr(Iomd *m, int n, uint8_t b)
{
    Ps2Port *p = &m->ps2[n];
    p->cr = b & 0x08;
    if (!(b & 0x08)) { p->tx_busy = 0; p->rx_full = 0; }
    ps2_lines(m);
}

void iomd_update(Iomd *m, ArcTime now)
{
    ps2_update(m, 0, now);
    if (m->type != IOMD_RISCPC) ps2_update(m, 1, now);
    ps2_lines(m);
    for (int i = 0; i < 2; i++) {
        IomdTimer *t = &m->timer[i];
        if (!t->running) continue;
        ArcTime p = timer_period(t);
        if (now >= t->start + p) {
            t->start += (now - t->start) / p * p;
            m->irqa |= i == 0 ? 0x20 : 0x40;
        }
    }
}

ArcTime iomd_next_event(const Iomd *m, ArcTime now)
{
    ArcTime next = now + ARC_MS(10);
    for (int i = 0; i < 2; i++) {
        const IomdTimer *t = &m->timer[i];
        if (!t->running) continue;
        ArcTime e = t->start + timer_period(t);
        if (e < next) next = e;
    }
    for (int n = 0; n < 2; n++) {
        const Ps2Port *p = &m->ps2[n];
        if (p->tx_busy && p->tx_done < next) next = p->tx_done;
        if ((p->cr & 0x08) && !p->rx_full && p->rx_next < next) next = p->rx_next;
    }
    return next > now ? next : now + 1;
}

static uint16_t timer_value(const IomdTimer *t, ArcTime now)
{
    if (!t->running) return t->latch;
    ArcTime p = timer_period(t);
    return (uint16_t)((p - (now - t->start) % p) / TIMER_TICK);
}

/* ------------------------------------------------------------------ */
/* DMA del suono                                                      */
/* ------------------------------------------------------------------ */

static uint32_t sd_status(const Iomd *m)
{
    int empty = !m->sd_valid[0] + !m->sd_valid[1];
    return (uint32_t)(m->sd_buf | (empty ? 2 : 0) | (empty == 2 ? 4 : 0));
}

/* bit 4 dell'interrupt dei DMA: il canale del suono vuole un buffer */
static void sd_irq(Iomd *m)
{
    if ((m->sd_cr & 0x20) && !m->sd_stopped && (sd_status(m) & 2)) m->dma_irq |= 0x10;
    else m->dma_irq &= (uint8_t)~0x10;
}

static void sd_clear(Iomd *m)
{
    m->sd_valid[0] = m->sd_valid[1] = 0;
    m->sd_buf = 0;
    m->sd_stopped = 0;
}

int iomd_sound_active(const Iomd *m)
{
    return (m->sd_cr & 0x20) && !m->sd_stopped && (m->sd_valid[0] || m->sd_valid[1]);
}

int iomd_sound_next(Iomd *m, uint32_t *addr)
{
    if (!iomd_sound_active(m)) return 0;
    /* buffer corrente vuoto e l'altro pronto: il DMA passa a quello */
    if (!m->sd_valid[m->sd_buf]) m->sd_buf ^= 1;
    int b = m->sd_buf;
    uint32_t inc = m->sd_cr & 0x1F ? m->sd_cr & 0x1F : 16;
    *addr = m->sd_cur[b];
    /* finito il buffer (trasferito l'indirizzo END): si passa all'altro */
    if ((m->sd_cur[b] & 0xFFF) >= (m->sd_end[b] & 0xFFF)) {
        m->sd_valid[b] = 0;
        if (m->sd_end[b] & 0x80000000u) m->sd_stopped = 1;
        m->sd_buf = b ^ 1;
    }
    m->sd_cur[b] += inc;
    sd_irq(m);
    return 1;
}

/* ------------------------------------------------------------------ */
/* registri                                                           */
/* ------------------------------------------------------------------ */

uint32_t iomd_read(Iomd *m, uint32_t off, ArcTime now, int *known)
{
    iomd_update(m, now);
    *known = 1;
    off &= 0x1FC;
    switch (off) {
    case 0x000: {
        uint8_t in = m->hooks.lines_read ? m->hooks.lines_read(m->hooks.ctx) : 0x3F;
        return (uint32_t)((m->iocr & in & 0x3F) | (m->flyback ? 0x80 : 0));
    }
    case 0x004:
        m->ps2[0].rx_full = 0;
        ps2_lines(m);
        return m->ps2[0].rx;
    case 0x008: return ps2_read_cr(&m->ps2[0]);  /* bit 2: la parita' (dispari) del byte */
    case 0x010: return m->irqa;
    case 0x014: return m->irqa & m->irqa_mask;
    case 0x018: return m->irqa_mask;
    case 0x020: return m->irqb;
    case 0x024: return m->irqb & m->irqb_mask;
    case 0x028: return m->irqb_mask;
    case 0x030: return m->fiq;
    case 0x034: return m->fiq & m->fiq_mask;
    case 0x038: return m->fiq_mask;
    case 0x040: case 0x050: return m->timer[(off >> 4) & 1].out & 0xFF;
    case 0x044: case 0x054: return m->timer[(off >> 4) & 1].out >> 8;
    case 0x080: return m->romcr[0];
    case 0x084: return m->romcr[1];
    case 0x088: return m->dramcr;
    case 0x08C: return m->vrefcr;
    case 0x090: return m->fsize;
    case 0x094: return m->type == IOMD_ARM7500FE ? 0x7C : m->type == IOMD_ARM7500 ? 0x98 : 0xE7;
    case 0x098: return m->type == IOMD_ARM7500FE ? 0xAA : m->type == IOMD_ARM7500 ? 0x5B : 0xD4;
    case 0x09C: return 0;                          /* versione */
    case 0x0A0: return m->mouse_x;
    case 0x0A4: return m->mouse_y;
    case 0x0C0: return m->iotcr;
    case 0x0C4: return m->ectcr;
    case 0x0C8: return m->astcr;
    case 0x1C0: return m->curscur;
    case 0x1C4: return m->cursinit;
    case 0x1D0: return m->vidcur;
    case 0x1D4: return m->vidend;
    case 0x1D8: return m->vidstart;
    case 0x1DC: return m->vidinit;
    case 0x1E0: return m->type == IOMD_RISCPC ? m->vidcr : (m->vidcr & 0xA0) | 0x50;   /* video nella DRAM */
    case 0x180: return m->sd_cur[0];
    case 0x184: return m->sd_end[0];
    case 0x188: return m->sd_cur[1];
    case 0x18C: return m->sd_end[1];
    case 0x190: return m->sd_cr;
    case 0x194: return sd_status(m);
    case 0x1F0: return m->dma_irq;
    case 0x1F4: return m->dma_irq & m->dma_mask;
    case 0x1F8: return m->dma_mask;
    default:
        if (m->type != IOMD_RISCPC) {
            switch (off) {
            case 0x00C: return 0;                  /* IOLINES: bit 0 = 0, monitor VGA */
            case 0x01C: return m->susmode;
            case 0x03C: return m->clkctl;
            case 0x060: case 0x064: return 0;      /* IRQ C: nessuna linea attiva */
            case 0x068: return m->irqc_mask;
            case 0x06C: return m->vidmux;
            case 0x070: return m->irqd;
            case 0x074: return m->irqd & m->irqd_mask;
            case 0x078: return m->irqd_mask;
            case 0x0A8:
                m->ps2[1].rx_full = 0;
                ps2_lines(m);
                return m->ps2[1].rx;
            case 0x0AC: return ps2_read_cr(&m->ps2[1]);
            case 0x0CC: return m->astcr;
            case 0x0D0: return m->dramwid;
            case 0x0D4: return m->selfref;
            case 0x1C8: return m->vidcurb;
            case 0x1E8: return m->vidinitb;
            default:
                if (off >= 0x0E0 && off <= 0x0F8) return m->atod[(off - 0x0E0) >> 2];
                break;
            }
        } else if (off >= 0x100 && off < 0x1C0) return m->dma_regs[(off - 0x100) >> 2];
        *known = 0;
        return 0;
    }
}

void iomd_write(Iomd *m, uint32_t off, uint32_t v, ArcTime now, int *known)
{
    iomd_update(m, now);
    *known = 1;
    off &= 0x1FC;
    uint8_t b = (uint8_t)v;
    switch (off) {
    case 0x000:
        m->iocr = b;
        if (m->hooks.lines_write) m->hooks.lines_write(m->hooks.ctx, b);
        break;
    case 0x004: ps2_write_data(m, 0, b, now); break;   /* byte verso la tastiera */
    case 0x008: ps2_write_cr(m, 0, b); break;
    case 0x014: m->irqa &= (uint8_t)~(b & 0x7C); break;
    case 0x018: m->irqa_mask = b; break;
    case 0x028: m->irqb_mask = b; break;
    case 0x038: m->fiq_mask = b; break;
    case 0x040: case 0x050: m->timer[(off >> 4) & 1].in_lo = b; break;
    case 0x044: case 0x054: m->timer[(off >> 4) & 1].in_hi = b; break;
    case 0x048: case 0x058: {
        IomdTimer *t = &m->timer[(off >> 4) & 1];
        t->latch = (uint16_t)(t->in_hi << 8 | t->in_lo);
        t->start = now;
        t->running = 1;
        break;
    }
    case 0x04C: case 0x05C: {
        IomdTimer *t = &m->timer[(off >> 4) & 1];
        t->out = timer_value(t, now);
        break;
    }
    case 0x080: m->romcr[0] = b; break;
    case 0x084: m->romcr[1] = b; break;
    case 0x088: m->dramcr = b; break;
    case 0x08C: m->vrefcr = b; break;
    case 0x090: m->fsize = b; break;
    case 0x0A0: m->mouse_x = (uint16_t)v; break;
    case 0x0A4: m->mouse_y = (uint16_t)v; break;
    case 0x0C0: m->iotcr = b; break;
    case 0x0C4: m->ectcr = b; break;
    case 0x0C8: m->astcr = b; break;
    case 0x1C0: m->curscur = v; break;
    case 0x1C4: m->cursinit = v; break;
    case 0x1D0: m->vidcur = v; break;
    case 0x1D4: m->vidend = v; break;
    case 0x1D8: m->vidstart = v; break;
    case 0x1DC: m->vidinit = v; break;
    case 0x1E0: m->vidcr = v; break;
    case 0x180: case 0x188: m->sd_cur[(off >> 3) & 1] = v; break;
    case 0x184: case 0x18C:                        /* END: il buffer e' pronto */
        m->sd_end[(off >> 3) & 1] = v;
        m->sd_valid[(off >> 3) & 1] = 1;
        m->sd_stopped = 0;
        sd_irq(m);
        break;
    case 0x190:
        if (b & 0x80) sd_clear(m);
        m->sd_cr = b & 0x7F;
        sd_irq(m);
        break;
    case 0x1F8: m->dma_mask = b; break;
    case 0x1F4: break;                             /* (RISC OS ci scrive per uscire dal modo ROM) */
    default:
        if (m->type != IOMD_RISCPC) {
            switch (off) {
            case 0x00C: case 0x02C: return;        /* IOLINES, STOPMODE */
            case 0x01C: m->susmode = b; return;
            case 0x03C: m->clkctl = b; return;
            case 0x064: return;                    /* IRQ C: azzeramento */
            case 0x068: m->irqc_mask = b; return;
            case 0x06C: m->vidmux = b; return;
            case 0x074: return;                    /* IRQ D: livelli, niente da azzerare */
            case 0x078: m->irqd_mask = b; return;
            case 0x0A8: ps2_write_data(m, 1, b, now); return;
            case 0x0AC: ps2_write_cr(m, 1, b); return;
            case 0x0CC: m->astcr = b; return;
            case 0x0D0: m->dramwid = b; return;
            case 0x0D4: m->selfref = b; return;
            case 0x1C8: m->vidcurb = v; return;
            case 0x1E8: m->vidinitb = v; return;
            default:
                if (off >= 0x0E0 && off <= 0x0F8) { m->atod[(off - 0x0E0) >> 2] = b; return; }
                break;
            }
        } else if (off >= 0x100 && off < 0x1C0) { m->dma_regs[(off - 0x100) >> 2] = v; break; }
        *known = 0;
        break;
    }
}

void iomd_set_flyback(Iomd *m, int level)
{
    if (level && !m->flyback) m->irqa |= 0x08;
    m->flyback = level;
}

void iomd_set_irqb_line(Iomd *m, uint8_t bit, int level)
{
    if (level) m->irqb |= bit; else m->irqb &= (uint8_t)~bit;
}

int iomd_irq(const Iomd *m)
{
    return (m->irqa & m->irqa_mask) || (m->irqb & m->irqb_mask) || (m->dma_irq & m->dma_mask) ||
           (m->type != IOMD_RISCPC && (m->irqd & m->irqd_mask));
}

int iomd_fiq(const Iomd *m)
{
    return (m->fiq & m->fiq_mask) != 0;
}
