/*
 * ps2mouse.c - Mouse PS/2 (vedi ps2mouse.h)
 */
#include "ps2mouse.h"
#include <string.h>

static void put(Ps2Mouse *m, uint8_t b)
{
    if (m->out_w - m->out_r >= PS2M_QUEUE) return;
    m->out[m->out_w++ % PS2M_QUEUE] = b;
}

void ps2mouse_reset(Ps2Mouse *m)
{
    int buttons = m->buttons;
    memset(m, 0, sizeof *m);
    m->buttons = m->sent_buttons = buttons;
    m->rate = 100;
    m->resolution = 2;
}

void ps2mouse_rx(Ps2Mouse *m, uint8_t b)
{
    if (m->pending) {                                   /* argomento */
        if (m->pending == 0xF3) m->rate = b;
        if (m->pending == 0xE8) m->resolution = b & 3;
        m->pending = 0;
        put(m, 0xFA);
        return;
    }
    switch (b) {
    case 0xFF:                                          /* reset e autotest */
        m->out_r = m->out_w = 0;
        m->enabled = 0;
        m->dx = m->dy = 0;
        put(m, 0xFA); put(m, 0xAA); put(m, 0x00);
        break;
    case 0xFE: put(m, m->last); break;
    case 0xF2: put(m, 0xFA); put(m, 0x00); break;       /* identita': mouse standard */
    case 0xF3: case 0xE8: m->pending = b; put(m, 0xFA); break;
    case 0xF4: m->enabled = 1; put(m, 0xFA); break;
    case 0xF5: m->enabled = 0; put(m, 0xFA); break;
    case 0xF6: m->rate = 100; m->resolution = 2; put(m, 0xFA); break;
    case 0xE9:                                          /* stato */
        put(m, 0xFA);
        put(m, (uint8_t)((m->enabled ? 0x20 : 0) | (m->buttons & 7)));
        put(m, m->resolution);
        put(m, m->rate);
        break;
    default: put(m, 0xFA); break;                       /* E6 E7 EA F0 ... */
    }
}

/* un pacchetto se c'e' qualcosa da dire e la coda e' vuota */
static void packet(Ps2Mouse *m)
{
    int dx, dy;
    if (!m->enabled || m->out_r != m->out_w) return;
    if (!m->dx && !m->dy && m->buttons == m->sent_buttons) return;
    dx = m->dx < -255 ? -255 : m->dx > 255 ? 255 : m->dx;
    dy = m->dy < -255 ? -255 : m->dy > 255 ? 255 : m->dy;
    m->dx -= dx;
    m->dy -= dy;
    m->sent_buttons = m->buttons;
    put(m, (uint8_t)(0x08 | (m->buttons & 7) | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
    put(m, (uint8_t)dx);
    put(m, (uint8_t)dy);
}

int ps2mouse_tx(Ps2Mouse *m, uint8_t *b)
{
    packet(m);
    if (m->out_r == m->out_w) return 0;
    *b = m->out[m->out_r++ % PS2M_QUEUE];
    m->last = *b;
    return 1;
}

void ps2mouse_move(Ps2Mouse *m, int dx, int dy)
{
    m->dx += dx;
    m->dy += dy;
}

void ps2mouse_buttons(Ps2Mouse *m, int rb)
{
    /* Select = sinistro, Menu = centrale, Adjust = destro */
    m->buttons = (rb & 4 ? 1 : 0) | (rb & 1 ? 2 : 0) | (rb & 2 ? 4 : 0);
}
