/*
 * ioc.c - IOC (vedi ioc.h)
 *
 * I timer contano all'indietro a 2 MHz dal valore di latch e quando
 * arrivano a zero ricaricano e (timer 0 e 1) chiedono un interrupt.
 * RISC OS programma il timer 0 a 20000: 100 interrupt al secondo.
 */
#include "ioc.h"
#include <string.h>

#define TIMER_TICK   12u                    /* unita' da 24 MHz per tick a 2 MHz */
#define KART_BYTE    ARC_US(352)            /* 11 bit a 31250 baud */

void ioc_init(Ioc *c, const IocHooks *hooks)
{
    memset(c, 0, sizeof *c);
    c->hooks = *hooks;
    ioc_reset(c, 0);
}

void ioc_reset(Ioc *c, ArcTime now)
{
    IocHooks h = c->hooks;
    memset(c, 0, sizeof *c);
    c->hooks = h;
    c->control = 0xFF;
    c->irqa = 0x80 | 0x10;                  /* forzato + accensione */
    c->irqb = 0x40;                          /* KART vuoto */
    c->fiq = 0x80;
    for (int i = 0; i < 4; i++) c->timer[i].start = now;
    c->rx_next = now;
}

/* ------------------------------------------------------------------ */
/* timer                                                              */
/* ------------------------------------------------------------------ */

static ArcTime timer_period(const IocTimer *t)
{
    return (ArcTime)(t->latch ? t->latch : 0x10000) * TIMER_TICK;
}

static void timers_update(Ioc *c, ArcTime now)
{
    for (int i = 0; i < 2; i++) {
        IocTimer *t = &c->timer[i];
        if (!t->running) continue;
        ArcTime p = timer_period(t);
        if (now >= t->start + p) {
            /* uno o piu' periodi trascorsi: l'interrupt resta pendente */
            ArcTime n = (now - t->start) / p;
            t->start += n * p;
            c->irqa |= i == 0 ? 0x20 : 0x40;
        }
    }
}

static uint16_t timer_value(const IocTimer *t, ArcTime now)
{
    if (!t->running) return t->latch;
    ArcTime p = timer_period(t);
    ArcTime elapsed = (now - t->start) % p;
    return (uint16_t)((p - elapsed) / TIMER_TICK);
}

/* ------------------------------------------------------------------ */
/* KART                                                               */
/* ------------------------------------------------------------------ */

static void kart_update(Ioc *c, ArcTime now)
{
    if (c->tx_busy && now >= c->tx_done) {
        c->tx_busy = 0;
        c->irqb |= 0x40;                     /* trasmettitore di nuovo vuoto */
        if (c->hooks.kart_to_keyboard) c->hooks.kart_to_keyboard(c->hooks.ctx, c->tx_byte);
        /* la tastiera risponde dopo che il byte e' arrivato */
        if (c->rx_next < now + ARC_US(100)) c->rx_next = now + ARC_US(100);
    }
    /* un nuovo byte dalla tastiera solo dopo che il computer ha letto il precedente */
    if (now >= c->rx_next && !(c->irqb & 0x80) && c->hooks.keyboard_to_kart) {
        uint8_t b;
        if (c->hooks.keyboard_to_kart(c->hooks.ctx, &b)) {
            c->kart_rx = b;
            c->irqb |= 0x80;
            c->rx_next = now + KART_BYTE;
        } else {
            c->rx_next = now + ARC_US(1000);  /* ricontrolla fra 1 ms */
        }
    }
}

void ioc_update(Ioc *c, ArcTime now)
{
    timers_update(c, now);
    kart_update(c, now);
}

ArcTime ioc_next_event(const Ioc *c, ArcTime now)
{
    ArcTime next = now + ARC_MS(10);
    for (int i = 0; i < 2; i++) {
        const IocTimer *t = &c->timer[i];
        if (!t->running) continue;
        ArcTime e = t->start + timer_period(t);
        if (e < next) next = e;
    }
    if (c->tx_busy && c->tx_done < next) next = c->tx_done;
    if (!(c->irqb & 0x80) && c->rx_next < next) next = c->rx_next;
    return next > now ? next : now + 1;
}

/* ------------------------------------------------------------------ */
/* registri                                                           */
/* ------------------------------------------------------------------ */

uint8_t ioc_read(Ioc *c, uint32_t offset, ArcTime now)
{
    ioc_update(c, now);
    switch ((offset >> 2) & 0x1F) {
    case 0x00: {
        uint8_t in = c->hooks.control_read ? c->hooks.control_read(c->hooks.ctx) : 0x3F;
        return (uint8_t)((c->control & in & 0x3F) | (c->if_level ? 0x40 : 0) | (c->ir_level ? 0x80 : 0));
    }
    case 0x01:
        c->irqb &= (uint8_t)~0x80;
        return c->kart_rx;
    case 0x04: return c->irqa;
    case 0x05: return c->irqa & c->irqa_mask;
    case 0x06: return c->irqa_mask;
    case 0x08: return c->irqb;
    case 0x09: return c->irqb & c->irqb_mask;
    case 0x0A: return c->irqb_mask;
    case 0x0C: return c->fiq;
    case 0x0D: return c->fiq & c->fiq_mask;
    case 0x0E: return c->fiq_mask;
    default: {
        int reg = (int)((offset >> 2) & 0x1F);
        if (reg >= 0x10) {
            IocTimer *t = &c->timer[(reg - 0x10) >> 2];
            switch (reg & 3) {
            case 0: return (uint8_t)t->out;
            case 1: return (uint8_t)(t->out >> 8);
            default: return 0;
            }
        }
        return 0;
    }
    }
}

void ioc_write(Ioc *c, uint32_t offset, uint8_t v, ArcTime now)
{
    ioc_update(c, now);
    int reg = (int)((offset >> 2) & 0x1F);
    switch (reg) {
    case 0x00:
        c->control = v;
        if (c->hooks.control_write) c->hooks.control_write(c->hooks.ctx, v);
        break;
    case 0x01:
        c->irqb &= (uint8_t)~0x40;
        c->tx_byte = v;
        c->tx_busy = 1;
        c->tx_done = now + KART_BYTE;
        break;
    case 0x05: c->irqa &= (uint8_t)~(v & 0x7C); break;   /* azzera le richieste a fronte */
    case 0x06: c->irqa_mask = v; break;
    case 0x0A: c->irqb_mask = v; break;
    case 0x0E: c->fiq_mask = v; break;
    default:
        if (reg >= 0x10) {
            IocTimer *t = &c->timer[(reg - 0x10) >> 2];
            switch (reg & 3) {
            case 0: t->in_lo = v; break;
            case 1: t->in_hi = v; break;
            case 2:                                          /* go: ricarica */
                t->latch = (uint16_t)(t->in_hi << 8 | t->in_lo);
                t->start = now;
                t->running = 1;
                break;
            default:                                         /* latch: fotografa il conteggio */
                t->out = timer_value(t, now);
                break;
            }
        }
        break;
    }
}

void ioc_set_ir(Ioc *c, int level)
{
    if (level && !c->ir_level) c->irqa |= 0x08;   /* fronte di salita: inizio del flyback */
    c->ir_level = level;
}

void ioc_set_irqb_line(Ioc *c, uint8_t bit, int level)
{
    if (level) c->irqb |= bit; else c->irqb &= (uint8_t)~bit;
}

void ioc_set_fiq_line(Ioc *c, uint8_t bit, int level)
{
    if (level) c->fiq |= bit; else c->fiq &= (uint8_t)~bit;
}

int ioc_irq(const Ioc *c)
{
    return (c->irqa & c->irqa_mask) || (c->irqb & c->irqb_mask);
}

int ioc_fiq(const Ioc *c)
{
    return (c->fiq & c->fiq_mask) != 0;
}
