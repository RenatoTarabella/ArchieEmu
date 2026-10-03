/*
 * superio.c - Super I/O 82C711 del Risc PC (vedi superio.h)
 */
#include "superio.h"
#include <string.h>

void superio_init(SuperIo *s)
{
    memset(s, 0, sizeof *s);
    fdc82077_init(&s->fdc);
}

void superio_reset(SuperIo *s, ArcTime now)
{
    s->config_mode = s->config_keys = 0;
    s->fdc.dor = 0;
    fdc82077_reset(&s->fdc, now);
}

uint8_t superio_read(SuperIo *s, uint32_t port, ArcTime now, int *known)
{
    *known = 1;
    if (s->config_mode && port == 0x3F1) return s->config[s->config_index & 15];
    switch (port) {
    case 0x3F2: case 0x3F4: case 0x3F5: case 0x3F7:
        return fdc82077_read(&s->fdc, (int)(port & 7), now);
    case 0x391: return s->c710[s->c710_index & 15];
    case 0x3FF: return s->scratch[0];
    case 0x2FF: return s->scratch[1];
    case 0x3FD: return 0x60;                     /* seriale: trasmettitore vuoto */
    default:
        *known = 0;
        return 0xFF;
    }
}

void superio_write(SuperIo *s, uint32_t port, uint8_t v, ArcTime now, int *known)
{
    *known = 1;
    if (port == 0x3F0) {                         /* configurazione stile 37C665 */
        if (s->config_mode) {
            if (v == 0xAA) s->config_mode = 0;
            else s->config_index = v;
            return;
        }
        s->config_keys = v == 0x55 ? s->config_keys + 1 : 0;
        if (s->config_keys == 2) { s->config_mode = 1; s->config_keys = 0; }
        return;
    }
    if (port == 0x3F1 && s->config_mode) { s->config[s->config_index & 15] = v; return; }
    switch (port) {
    case 0x3F2: case 0x3F4: case 0x3F5: case 0x3F7:
        fdc82077_write(&s->fdc, (int)(port & 7), v, now);
        break;
    case 0x2FA: case 0x3FA: break;               /* sequenza d'accesso dell'82C710 */
    case 0x390: s->c710_index = v; break;
    case 0x391: s->c710[s->c710_index & 15] = v; break;
    case 0x3FF: s->scratch[0] = v; break;
    case 0x2FF: s->scratch[1] = v; break;
    default: *known = 0; break;
    }
}
