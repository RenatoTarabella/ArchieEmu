/*
 * fdc82077.c - Controller floppy 82077 del Risc PC (vedi fdc82077.h)
 *
 * Fasi del 765: comando (byte di comando e parametri), esecuzione (byte dei
 * settori, uno per volta) e risultato (ST0 ST1 ST2 C H R N). Fuori dai
 * comandi, SEEK e RECALIBRATE lavorano in sottofondo e alla fine chiedono
 * un interrupt, che si chiude con SENSE INTERRUPT.
 *
 * Il disco: le posizioni dei settori nella traccia (in byte dall'indice)
 * vengono da archie/fdc.c; un giro dura 200 ms; senza disco o col motore
 * spento non passano impulsi di indice e la ricerca di un settore resta in
 * attesa come sul chip vero (RISC OS ha i suoi timeout).
 */
#include "fdc82077.h"
#include <string.h>

#define REV    ARC_MS(200)
#define NEVER  (~(ArcTime)0)

enum { PH_CMD, PH_PARAMS, PH_SEARCH, PH_EXEC_READ, PH_EXEC_WRITE, PH_EXEC_FORMAT, PH_RESULT };
enum { OP_NONE, OP_READ, OP_WRITE, OP_READID, OP_FORMAT };


static ArcTime byte_time(const Fdc82077 *f)
{
    switch (f->rate & 3) {
    case 0:  return ARC_US(16);                  /* 500 kbit/s */
    case 1:  return 640;                         /* 300 kbit/s: 26,7 us */
    case 2:  return ARC_US(32);                  /* 250 kbit/s */
    default: return ARC_US(8);                   /* 1 Mbit/s */
    }
}

static FdcDrive *drv(Fdc82077 *f) { return &f->media.drive[f->drive]; }

static int motor_on(const Fdc82077 *f, int d) { return (f->dor >> (4 + d)) & 1; }

/* il disco gira e la densita' corrisponde alla velocita' scelta */
static int readable(Fdc82077 *f)
{
    FdcDrive *d = drv(f);
    if (!f->present[f->drive] || !d->image || !motor_on(f, f->drive)) return 0;
    int rate = f->rate & 3;
    return d->hd ? rate == 0 : rate == 2 || rate == 1;
}

static int spinning(Fdc82077 *f)
{
    return f->present[f->drive] && drv(f)->image && motor_on(f, f->drive);
}

/* istante in cui il byte 'pos' della traccia passa sotto la testina, dopo 'now' */
static ArcTime when_at(const Fdc82077 *f, ArcTime now, int pos)
{
    ArcTime t = now - now % REV + (ArcTime)pos * byte_time(f);
    while (t <= now) t += REV;
    return t;
}

void fdc82077_init(Fdc82077 *f)
{
    memset(f, 0, sizeof *f);
    fdc_reset(&f->media);
    f->present[0] = f->present[1] = 1;
    fdc82077_reset(f, 0);
}

static void post_int(Fdc82077 *f, uint8_t st0)
{
    if (f->npending < FDC_DRIVES + 1) f->pending_int[f->npending++] = st0;
}

void fdc82077_reset(Fdc82077 *f, ArcTime now)
{
    (void)now;
    f->phase = PH_CMD;
    f->ncmd = f->nparams = f->nresult = f->rpos = 0;
    f->data_ready = 0;
    f->next = NEVER;
    f->npending = 0;
    f->irq = 0;
    f->op = OP_NONE;
    for (int d = 0; d < FDC_DRIVES; d++) f->seeking[d] = 0;
    if (!f->locked) memset(f->config, 0, sizeof f->config);
    /* dopo il reset: un interrupt di "polling" per ogni unita' */
    for (int d = 0; d < 4; d++) post_int(f, (uint8_t)(0xC0 | d));
}

/* ------------------------------------------------------------------ */
/* risultati                                                          */
/* ------------------------------------------------------------------ */

static void result(Fdc82077 *f, const uint8_t *r, int n, int interrupt)
{
    memcpy(f->result, r, (size_t)n);
    f->nresult = n;
    f->rpos = 0;
    f->phase = n ? PH_RESULT : PH_CMD;
    f->data_ready = 0;
    f->next = NEVER;
    if (interrupt) f->irq = 1;
    f->op = OP_NONE;
}

static void rw_result(Fdc82077 *f, int st0, int st1, int st2)
{
    uint8_t r[7] = { (uint8_t)(st0 | f->head << 2 | f->drive), (uint8_t)st1, (uint8_t)st2, f->c, f->h, f->r, f->n };
    result(f, r, 7, 1);
}

/* ------------------------------------------------------------------ */
/* ricerca dei settori                                                */
/* ------------------------------------------------------------------ */


/* prossimo settore (c,h,r,n) dopo 'now'; arrivo all'inizio dei dati */
static void search_sector(Fdc82077 *f, ArcTime now)
{
    f->phase = PH_SEARCH;
    f->found = -1;
    if (!spinning(f)) { f->next = NEVER; return; }        /* niente indice: si aspetta */
    if (!readable(f)) {                                    /* densita' sbagliata: nessun ID */
        f->st1 = 0x01;
        f->found = -2;
        f->next = now + 2 * REV;
        return;
    }
    f->nsecs = fdc_track_sectors(drv(f), f->pcn[f->drive], f->head, f->secs, 64);
    ArcTime best = NEVER;
    for (int i = 0; i < f->nsecs; i++) {
        const FdcSectorView *s = &f->secs[i];
        if (s->id[0] != f->c || s->id[1] != f->h || s->id[2] != f->r || s->id[3] != f->n || !s->id_crc_ok) continue;
        ArcTime t = when_at(f, now, s->pos + 45);
        if (t < best) { best = t; f->found = i; }
    }
    if (f->found < 0) {                                       /* due giri senza trovarlo */
        f->st1 = f->nsecs ? 0x04 : 0x01;
        f->found = -2;
        f->next = now + 2 * REV;
        return;
    }
    f->next = best;
}

static void start_transfer(Fdc82077 *f)
{
    const FdcSectorView *s = &f->secs[f->found];
    f->len = f->n ? 128 << (f->n & 7) : f->cmd[8];
    if (f->len > s->size) f->len = s->size;
    if (f->op == OP_READ) {
        if (s->data) memcpy(f->buf, s->data, (size_t)f->len);
        else memset(f->buf, 0, (size_t)f->len);
        f->phase = PH_EXEC_READ;
    } else {
        f->phase = PH_EXEC_WRITE;
    }
    f->pos = 0;
    f->data_ready = 0;
}

/* fine di un settore: il prossimo, o la fine del comando */
static void sector_done(Fdc82077 *f, ArcTime now)
{
    const FdcSectorView *s = &f->secs[f->found];
    if (f->op == OP_WRITE) {
        if (s->data) {
            memcpy(s->data, f->buf, (size_t)f->len);
            drv(f)->dirty = 1;
        }
    } else {
        if (!s->data_crc_ok) { rw_result(f, 0x40, 0x20, 0x20); return; }
        if (s->deleted != ((f->cmd[0] & 0x1F) == 0x0C)) {       /* segno di dato diverso */
            rw_result(f, 0x40, 0, 0x40);
            return;
        }
    }
    if (f->tc) {
        /* terminal count: fine normale, il risultato indica il settore dopo */
        if (f->r == f->eot) {
            f->r = 1;
            if (f->mt && f->head == 0) { f->head = 1; f->h ^= 1; }
            else { f->c++; if (f->mt) f->h ^= 1; }
        } else {
            f->r++;
        }
        rw_result(f, 0, 0, 0);
        return;
    }
    if (f->r == f->eot) {
        if (f->mt && f->head == 0) {                     /* multitraccia: si passa alla faccia 1 */
            f->head = 1;
            f->h ^= 1;
            f->r = 1;
        } else {
            /* fine del cilindro senza TC: terminazione "anormale" con EN, come
               la usa chi non ha il DMA */
            f->r = 1;
            f->c++;
            if (f->mt) f->h ^= 1;
            rw_result(f, 0x40, 0x80, 0);
            return;
        }
    } else {
        f->r++;
    }
    search_sector(f, now);
}

/* ------------------------------------------------------------------ */
/* comandi                                                            */
/* ------------------------------------------------------------------ */

static int param_count(uint8_t c)
{
    switch (c & 0x1F) {
    case 0x03: return 2;                         /* SPECIFY */
    case 0x04: return 1;                         /* SENSE DRIVE STATUS */
    case 0x05: case 0x06: case 0x09: case 0x0C: return 8;
    case 0x07: return 1;                         /* RECALIBRATE */
    case 0x08: return 0;                         /* SENSE INTERRUPT */
    case 0x0A: return 1;                         /* READ ID */
    case 0x0D: return 5;                         /* FORMAT TRACK */
    case 0x0E: return 0;                         /* DUMPREG */
    case 0x0F: return 2;                         /* SEEK */
    case 0x10: return 0;                         /* VERSION */
    case 0x12: return 1;                         /* PERPENDICULAR MODE */
    case 0x13: return 3;                         /* CONFIGURE */
    case 0x14: return 0;                         /* LOCK */
    default:   return -1;
    }
}

static void start_seek(Fdc82077 *f, int d, int target, ArcTime now)
{
    int steps = target - f->pcn[d];
    if (steps < 0) steps = -steps;
    /* passo da SPECIFY (SRT, a 500 kbit/s: 16 - SRT ms) piu' assestamento */
    int srt = 16 - (f->specify[0] >> 4);
    f->seek_target[d] = target;
    f->seeking[d] = 1;
    f->seek_done[d] = now + (ArcTime)steps * ARC_MS(srt ? srt : 16) + ARC_MS(15);
    if (steps && f->present[d] && f->media.drive[d].image) f->media.drive[d].disc_changed = 0;
}

static void execute(Fdc82077 *f, ArcTime now)
{
    uint8_t c = f->cmd[0];
    f->drive = f->cmd[1] & 3;
    f->head = (f->cmd[1] >> 2) & 1;
    switch (c & 0x1F) {
    case 0x03:                                   /* SPECIFY */
        f->specify[0] = f->cmd[1];
        f->specify[1] = f->cmd[2];
        f->nondma = f->cmd[2] & 1;
        result(f, NULL, 0, 0);
        break;
    case 0x04: {                                 /* SENSE DRIVE STATUS */
        FdcDrive *d = drv(f);
        uint8_t st3 = (uint8_t)((d->image && d->write_protect ? 0x40 : 0) | 0x20 |
                                (f->pcn[f->drive] == 0 ? 0x10 : 0) | 0x08 | f->head << 2 | f->drive);
        result(f, &st3, 1, 0);
        break;
    }
    case 0x07:                                   /* RECALIBRATE */
        start_seek(f, f->drive, 0, now);
        result(f, NULL, 0, 0);
        break;
    case 0x0F: {                                 /* SEEK (anche relativo: bit 7) */
        int target = f->cmd[2];
        if (c & 0x80) target = f->pcn[f->drive] + ((c & 0x40) ? -target : target);
        if (target < 0) target = 0;
        start_seek(f, f->drive, target, now);
        result(f, NULL, 0, 0);
        break;
    }
    case 0x08:                                   /* SENSE INTERRUPT */
        if (f->npending) {
            uint8_t st0 = f->pending_int[0];
            memmove(f->pending_int, f->pending_int + 1, (size_t)--f->npending);
            uint8_t r[2] = { st0, (uint8_t)f->pcn[st0 & 3] };
            result(f, r, 2, 0);
            f->irq = 0;
        } else {
            uint8_t r = 0x80;
            result(f, &r, 1, 0);
        }
        break;
    case 0x05: case 0x06: case 0x09: case 0x0C: /* WRITE / READ DATA (anche "deleted") */
        f->mt = (c >> 7) & 1;
        f->skip = (c >> 5) & 1;
        f->c = f->cmd[2]; f->h = f->cmd[3]; f->r = f->cmd[4]; f->n = f->cmd[5]; f->eot = f->cmd[6];
        f->st0 = f->st1 = f->st2 = 0;
        f->op = (c & 0x1F) == 0x05 || (c & 0x1F) == 0x09 ? OP_WRITE : OP_READ;
        f->tc = 0;
        if (f->op == OP_WRITE && drv(f)->image && drv(f)->write_protect) {
            rw_result(f, 0x40, 0x02, 0);
            break;
        }
        search_sector(f, now);
        break;
    case 0x0A:                                   /* READ ID: il prossimo ID che passa */
        f->op = OP_READID;
        f->phase = PH_SEARCH;
        f->found = -1;
        if (!spinning(f)) { f->next = NEVER; break; }
        f->nsecs = readable(f) ? fdc_track_sectors(drv(f), f->pcn[f->drive], f->head, f->secs, 64) : 0;
        if (!f->nsecs) { f->st1 = 0x01; f->found = -2; f->next = now + 2 * REV; break; }
        {
            ArcTime best = NEVER;
            for (int i = 0; i < f->nsecs; i++) {
                ArcTime t = when_at(f, now, f->secs[i].pos + 7);
                if (f->secs[i].id_crc_ok && t < best) { best = t; f->found = i; }
            }
            f->next = best;
        }
        break;
    case 0x0D:                                   /* FORMAT TRACK */
        f->op = OP_FORMAT;
        f->tc = 0;
        f->n = f->cmd[2];
        f->fmt_sectors = f->cmd[3] > 32 ? 32 : f->cmd[3];
        f->fmt_fill = f->cmd[5];
        f->fmt_count = 0;
        f->pos = 0;
        if (drv(f)->image && drv(f)->write_protect) { rw_result(f, 0x40, 0x02, 0); break; }
        f->phase = PH_SEARCH;
        f->found = -3;                              /* si parte dall'indice */
        f->next = spinning(f) ? when_at(f, now, 0) : NEVER;
        break;
    case 0x0E: {                                 /* DUMPREG */
        uint8_t r[10] = { (uint8_t)f->pcn[0], (uint8_t)f->pcn[1], (uint8_t)f->pcn[2], (uint8_t)f->pcn[3],
                          f->specify[0], f->specify[1], f->eot, (uint8_t)(f->locked << 7),
                          f->config[1], f->config[2] };
        result(f, r, 10, 0);
        break;
    }
    case 0x10: { uint8_t r = 0x90; result(f, &r, 1, 0); break; }      /* VERSION: 82077 */
    case 0x12: result(f, NULL, 0, 0); break;                           /* PERPENDICULAR */
    case 0x13:                                                         /* CONFIGURE */
        memcpy(f->config, f->cmd + 1, 3);
        result(f, NULL, 0, 0);
        break;
    case 0x14: {                                                       /* LOCK */
        f->locked = (c >> 7) & 1;
        uint8_t r = (uint8_t)(f->locked << 4);
        result(f, &r, 1, 0);
        break;
    }
    default: { uint8_t r = 0x80; result(f, &r, 1, 0); break; }        /* comando non valido */
    }
}

/* ------------------------------------------------------------------ */
/* eventi                                                             */
/* ------------------------------------------------------------------ */

void fdc82077_update(Fdc82077 *f, ArcTime now)
{
    for (int d = 0; d < FDC_DRIVES; d++) {
        if (!f->seeking[d] || now < f->seek_done[d]) continue;
        f->seeking[d] = 0;
        if (!f->present[d] && f->seek_target[d] == 0) {
            post_int(f, (uint8_t)(0x70 | d));              /* nessuna traccia 0: equipment check */
        } else {
            f->pcn[d] = f->seek_target[d];
            f->media.drive[d].track = f->pcn[d];
            post_int(f, (uint8_t)(0x20 | d));
        }
        f->irq = 1;
    }
    while (f->next != NEVER && now >= f->next) {
        ArcTime t = f->next;
        switch (f->phase) {
        case PH_SEARCH:
            if (f->found == -2) { rw_result(f, 0x40, f->st1, 0); break; }
            if (f->op == OP_READID) {
                const FdcSectorView *s = &f->secs[f->found];
                f->c = s->id[0]; f->h = s->id[1]; f->r = s->id[2]; f->n = s->id[3];
                rw_result(f, 0, 0, 0);
                break;
            }
            if (f->op == OP_FORMAT) {                       /* all'indice: si chiedono gli ID */
                f->phase = PH_EXEC_FORMAT;
                f->data_ready = 1;
                f->next = NEVER;
                break;
            }
            start_transfer(f);
            if (f->phase == PH_EXEC_READ) { f->data_ready = 1; f->next = NEVER; }
            else                          { f->data_ready = 1; f->next = NEVER; }
            break;
        case PH_EXEC_READ:
            if (f->pos >= f->len) { sector_done(f, t); break; }
            f->data_ready = 1;
            f->next = NEVER;
            break;
        case PH_EXEC_WRITE:
            if (f->pos >= f->len) { sector_done(f, t); break; }
            f->data_ready = 1;
            f->next = NEVER;
            break;
        case PH_EXEC_FORMAT:
            f->data_ready = 1;
            f->next = NEVER;
            break;
        default:
            f->next = NEVER;
            break;
        }
    }
}

ArcTime fdc82077_next_event(const Fdc82077 *f)
{
    ArcTime next = f->next;
    for (int d = 0; d < FDC_DRIVES; d++)
        if (f->seeking[d] && f->seek_done[d] < next) next = f->seek_done[d];
    return next;
}

/* il prossimo byte del settore dopo uno preso o dato dalla CPU */
static void byte_taken(Fdc82077 *f, ArcTime now)
{
    f->data_ready = 0;
    f->next = now + byte_time(f);
}

static void format_byte(Fdc82077 *f, uint8_t v, ArcTime now)
{
    f->fmt_ids[f->fmt_count][f->pos++] = v;
    if (f->pos < 4) { byte_taken(f, now); return; }
    f->pos = 0;
    if (++f->fmt_count < f->fmt_sectors) { byte_taken(f, now); return; }
    fdc_format_track(drv(f), f->pcn[f->drive], f->head, (const uint8_t (*)[4])f->fmt_ids,
                     f->fmt_sectors, f->fmt_fill);
    const uint8_t *last = f->fmt_ids[f->fmt_sectors - 1];
    f->c = last[0]; f->h = last[1]; f->r = last[2]; f->n = last[3];
    rw_result(f, 0, 0, 0);
}

uint8_t fdc82077_dack_read(Fdc82077 *f, int tc, ArcTime now)
{
    fdc82077_update(f, now);
    if (f->phase != PH_EXEC_READ || !f->data_ready) return 0xFF;
    uint8_t v = f->buf[f->pos++];
    if (tc) { f->tc = 1; f->pos = f->len; }      /* il resto del settore non si trasferisce */
    byte_taken(f, now);
    return v;
}

void fdc82077_dack_write(Fdc82077 *f, uint8_t v, int tc, ArcTime now)
{
    fdc82077_update(f, now);
    if (!f->data_ready) return;
    if (f->phase == PH_EXEC_WRITE) {
        f->buf[f->pos++] = v;
        if (tc) {                                /* il resto del settore si riempie di zeri */
            f->tc = 1;
            memset(f->buf + f->pos, 0, (size_t)(f->len - f->pos));
            f->pos = f->len;
        }
        byte_taken(f, now);
    } else if (f->phase == PH_EXEC_FORMAT) {
        if (tc) f->tc = 1;
        format_byte(f, v, now);
    }
}

uint8_t fdc82077_read(Fdc82077 *f, int port, ArcTime now)
{
    fdc82077_update(f, now);
    switch (port & 7) {
    case 2: return f->dor;
    case 4: {                                    /* MSR */
        uint8_t msr = 0;
        for (int d = 0; d < FDC_DRIVES; d++) if (f->seeking[d]) msr |= (uint8_t)(1 << d);
        switch (f->phase) {
        case PH_CMD:    msr |= 0x80; break;
        case PH_PARAMS: msr |= 0x90; break;
        case PH_RESULT: msr |= 0xD0; break;
        case PH_SEARCH: msr |= 0x10; break;
        case PH_EXEC_READ:
            msr |= (uint8_t)(0x10 | (f->nondma ? 0x20 : 0) | (f->data_ready ? 0xC0 : 0));
            break;
        default:
            msr |= (uint8_t)(0x10 | (f->nondma ? 0x20 : 0) | (f->data_ready ? 0x80 : 0));
            break;
        }
        return msr;
    }
    case 5:                                      /* dati */
        if (f->phase == PH_RESULT) {
            f->irq = 0;
            uint8_t v = f->result[f->rpos++];
            if (f->rpos >= f->nresult) f->phase = PH_CMD;
            return v;
        }
        if (f->phase == PH_EXEC_READ && f->data_ready) {
            uint8_t v = f->buf[f->pos++];
            byte_taken(f, now);
            return v;
        }
        return 0xFF;
    case 7: {                                    /* DIR: disco cambiato */
        const FdcDrive *d = &f->media.drive[f->dor & 3];
        return (uint8_t)((!d->image || d->disc_changed) ? 0x80 : 0);
    }
    default:
        return 0xFF;
    }
}

void fdc82077_write(Fdc82077 *f, int port, uint8_t v, ArcTime now)
{
    fdc82077_update(f, now);
    switch (port & 7) {
    case 2: {                                    /* DOR */
        uint8_t old = f->dor;
        f->dor = v;
        if (!(v & 4)) { f->phase = PH_CMD; f->next = NEVER; f->irq = 0; f->npending = 0; }
        else if (!(old & 4)) fdc82077_reset(f, now);
        break;
    }
    case 4:                                      /* DSR */
        f->rate = v & 3;
        if (v & 0x80) fdc82077_reset(f, now);
        break;
    case 7:                                      /* CCR */
        f->rate = v & 3;
        break;
    case 5:                                      /* dati */
        if (f->phase == PH_CMD) {
            int n = param_count(v);
            f->cmd[0] = v;
            f->ncmd = 1;
            if (n < 0) { f->nparams = 0; execute(f, now); break; }
            f->nparams = n;
            if (n == 0) execute(f, now);
            else f->phase = PH_PARAMS;
        } else if (f->phase == PH_PARAMS) {
            f->cmd[f->ncmd++] = v;
            if (f->ncmd > f->nparams) execute(f, now);
        } else if (f->phase == PH_EXEC_WRITE && f->data_ready) {
            f->buf[f->pos++] = v;
            byte_taken(f, now);
        } else if (f->phase == PH_EXEC_FORMAT && f->data_ready) {
            format_byte(f, v, now);
        }
        break;
    default:
        break;
    }
}

/* interrupt: fine di una ricerca, risultato pronto, o (senza DMA) un byte da
   scambiare; con il bit 3 del DOR a zero le linee restano basse */
int fdc82077_irq(const Fdc82077 *f)
{
    if (!(f->dor & 8)) return 0;
    if (f->irq || f->npending) return 1;
    return f->nondma && f->data_ready &&
           (f->phase == PH_EXEC_READ || f->phase == PH_EXEC_WRITE || f->phase == PH_EXEC_FORMAT);
}

int fdc82077_spinning(const Fdc82077 *f)
{
    int d = f->dor & 3;
    return (f->dor & 4) && f->present[d] && f->media.drive[d].image && motor_on(f, d);
}

int fdc82077_drq(const Fdc82077 *f)
{
    if (!(f->dor & 8) || f->nondma) return 0;
    return f->data_ready && (f->phase == PH_EXEC_READ || f->phase == PH_EXEC_WRITE || f->phase == PH_EXEC_FORMAT);
}
