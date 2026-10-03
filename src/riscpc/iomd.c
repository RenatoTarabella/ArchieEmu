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

void iomd_reset(Iomd *m, ArcTime now)
{
    IomdHooks h = m->hooks;
    memset(m, 0, sizeof *m);
    m->hooks = h;
    m->iocr = 0xFF;
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

/* tastiera: i bit 6 e 7 dell'IRQ B seguono lo stato del canale */
static void kbd_lines(Iomd *m)
{
    int ena = (m->kbd_cr & 0x08) != 0;
    m->irqb = (uint8_t)((m->irqb & 0x3F) | (ena && !m->kbd_tx_busy ? 0x40 : 0)
                        | (ena && m->kbd_rx_full ? 0x80 : 0));
}

static void kbd_update(Iomd *m, ArcTime now)
{
    if (m->kbd_tx_busy && now >= m->kbd_tx_done) {
        m->kbd_tx_busy = 0;
        if (m->hooks.to_keyboard) m->hooks.to_keyboard(m->hooks.ctx, m->kbd_tx);
        if (m->kbd_rx_next < now + ARC_US(200)) m->kbd_rx_next = now + ARC_US(200);
    }
    /* un byte dalla tastiera solo dopo che il computer ha letto il precedente */
    if ((m->kbd_cr & 0x08) && !m->kbd_rx_full && now >= m->kbd_rx_next && m->hooks.from_keyboard) {
        uint8_t b;
        if (m->hooks.from_keyboard(m->hooks.ctx, &b)) {
            m->kbd_rx = b;
            m->kbd_rx_full = 1;
            m->kbd_rx_next = now + KBD_BYTE;
        } else {
            m->kbd_rx_next = now + ARC_US(1000);
        }
    }
    kbd_lines(m);
}

void iomd_update(Iomd *m, ArcTime now)
{
    kbd_update(m, now);
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
    if (m->kbd_tx_busy && m->kbd_tx_done < next) next = m->kbd_tx_done;
    if ((m->kbd_cr & 0x08) && !m->kbd_rx_full && m->kbd_rx_next < next) next = m->kbd_rx_next;
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
        m->kbd_rx_full = 0;
        kbd_lines(m);
        return m->kbd_rx;
    case 0x008:
    {
        /* bit 2: il bit di parita' (dispari) arrivato col byte */
        uint32_t ones = 0;
        for (uint8_t x = m->kbd_rx; x; x &= (uint8_t)(x - 1)) ones++;
        return (uint32_t)(m->kbd_cr | (m->kbd_tx_busy ? 0x40 : 0x80) | (m->kbd_rx_full ? 0x20 : 0)
                          | (ones & 1 ? 0 : 0x04) | 0x03);
    }
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
    case 0x094: return IOMD_ID & 0xFF;
    case 0x098: return IOMD_ID >> 8;
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
    case 0x1E0: return m->vidcr;
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
        if (off >= 0x100 && off < 0x1C0) return m->dma_regs[(off - 0x100) >> 2];
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
    case 0x004:                                    /* byte verso la tastiera */
        if (m->kbd_cr & 0x08) {
            m->kbd_tx = b;
            m->kbd_tx_busy = 1;
            m->kbd_tx_done = now + KBD_BYTE;
        }
        kbd_lines(m);
        break;
    case 0x008:
        m->kbd_cr = b & 0x08;
        if (!(b & 0x08)) { m->kbd_tx_busy = 0; m->kbd_rx_full = 0; }
        kbd_lines(m);
        break;
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
    default:
        if (off >= 0x100 && off < 0x1C0) { m->dma_regs[(off - 0x100) >> 2] = v; break; }
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
    return (m->irqa & m->irqa_mask) || (m->irqb & m->irqb_mask) || (m->dma_irq & m->dma_mask);
}

int iomd_fiq(const Iomd *m)
{
    return (m->fiq & m->fiq_mask) != 0;
}
