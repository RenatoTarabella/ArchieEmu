/*
 * fdc.c - Controller floppy WD1772 e unita' da 3,5" (vedi fdc.h).
 *
 * Modello del disco
 *   Le immagini sono semplici sequenze di settori. Per ogni traccia si
 *   ricostruisce una disposizione "logica" con la posizione (in byte
 *   dall'impulso di indice) di ogni campo ID e di ogni campo dati, come la
 *   scriverebbe il WD1772 in formattazione (gap presi dalle tabelle MAME):
 *     gap1 4E, poi per ogni settore 12x00 3xA1 FE C H R N CRC, 22x4E,
 *     12x00 3xA1 FB dati CRC, gap3 4E; il resto della traccia e' 4E.
 *   Il disco gira a 300 giri/min: 200 ms per giro, impulso di indice
 *   all'inizio di ogni giro (istanti multipli di 200 ms dall'accensione),
 *   largo 4 ms. In doppia densita' un byte dura 32 us (6250 byte per giro),
 *   in singola 64 us. Le immagini sono tutte in MFM: in singola densita'
 *   il controller non trova nessun ID (Record Not Found).
 *
 * HFE (HxC): flusso MFM grezzo, decodificato all'inserimento in tracce
 *   "custom" (vedi load_hfe); conserva le protezioni anticopia. Sola lettura.
 *
 * Formati riconosciuti (dalla dimensione del file)
 *   819200  D/E: 80 tracce, 2 facce, 5 settori da 1024 (0-4)
 *   655360  L:   80 tracce, 2 facce, 16 settori da 256 (0-15)
 *   327680  M:   80 tracce, 1 faccia, 16 settori da 256 (0-15)
 *   In tutti i casi il file e' "interlacciato": traccia 0 faccia 0,
 *   traccia 0 faccia 1, traccia 1 faccia 0... Per L e' la disposizione dei
 *   file .adl (MAME acorn_dsk.cpp, acorn_adfs_old_format::get_image_offset);
 *   la numerazione logica "sequenziale" dei settori di ADFS L e' affare del
 *   filing system, non dell'immagine. Il formato S (40 tracce) richiederebbe
 *   il doppio passo e non e' supportato; F (1,6 MB) e' ad alta densita'.
 *
 * Scrittura
 *   Le scritture modificano l'immagine in memoria (dirty). Il file viene
 *   riscritto: in fdc_eject, con fdc_flush e automaticamente quando il
 *   motore del WD1772 si spegne (9 giri dopo l'ultimo comando), cosi' una
 *   serie di scritture produce un solo salvataggio circa 1,8 s dopo.
 *   Il Write Track ricostruisce i settori dai byte scritti (F5 = A1 con
 *   preset del CRC, F6 = C2, F7 = due byte di CRC, F8-FB/FE segni); se la
 *   traccia risultante ha la geometria dell'immagine viene copiata nel
 *   file, altrimenti resta solo in memoria (traccia "custom", persa
 *   all'espulsione). Il segno di dato cancellato (F8) si conserva solo
 *   nelle tracce custom.
 *
 * Motore
 *   Il bit 7 di stato e' il motore interno del WD1772. Il disco gira (e
 *   produce impulsi di indice) se c'e' un'immagine nell'unita' selezionata
 *   e il motore e' acceso dal WD1772 oppure dal latch A (bit 5 attivo
 *   basso come nel collegamento MAME: mon_w(BIT(data,5)) con MON* attivo
 *   basso). Senza disco non arrivano impulsi di indice: come sul chip vero
 *   lo spin-up e la ricerca degli ID restano in attesa finche' il software
 *   non manda un Force Interrupt. Lo spegnimento dopo 9 giri e' invece
 *   calcolato sul tempo, anche senza disco.
 *
 * Tempi del WD1772 (datasheet): passo 6/12/2/3 ms per r1r0 = 00/01/10/11,
 * assestamento 15 ms (flag E e verifica), spin-up 6 impulsi di indice,
 * Record Not Found dopo 5 impulsi di indice senza l'ID cercato.
 */
#include "fdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REV         ARC_MS(200)
#define IDX_WIDTH   ARC_MS(4)
#define NEVER       (~(ArcTime)0)
#define SETTLE      ARC_MS(15)
#define MFM_LEN     6250
#define FM_LEN      3125
#define MAX_SECS    32

/* bit di stato */
#define ST_BUSY     0x01
#define ST_DRQ      0x02    /* tipo II/III */
#define ST_INDEX    0x02    /* tipo I */
#define ST_LOST     0x04    /* tipo II/III */
#define ST_TR00     0x04    /* tipo I */
#define ST_CRC      0x08
#define ST_RNF      0x10    /* tipo I: seek error */
#define ST_RTYPE    0x20    /* tipo II/III: dato cancellato */
#define ST_SPIN     0x20    /* tipo I: spin-up completato */
#define ST_WP       0x40
#define ST_MOTOR    0x80

enum {
    PH_IDLE,
    PH_SPINUP,          /* attesa del sesto impulso di indice */
    PH_SETTLE,          /* flag E dei comandi II/III */
    PH_STEP,            /* tipo I: fine del ritardo di passo */
    PH_VERIFY_SETTLE,
    PH_VERIFY_SEARCH,
    PH_SEARCH,          /* tipo II / Read Address: ricerca dell'ID */
    PH_READ_STREAM,     /* byte letti verso l'host (settore, ID, traccia) */
    PH_READ_CRC,        /* due byte di CRC dopo i dati */
    PH_NO_DAM,          /* ID trovato ma nessun campo dati */
    PH_WRITE_DRQ,       /* write sector: DRQ dopo 2 byte di gap2 */
    PH_WRITE_GATE,      /* write sector: il dato deve essere arrivato */
    PH_WRITE_STREAM,
    PH_WRITE_END,
    PH_RTRACK_INDEX,
    PH_WTRACK_INDEX,
    PH_WTRACK_STREAM,
    PH_WAIT_DRQ         /* comando finito, manca solo l'ultimo DRQ */
};

typedef struct FdcSec {
    uint8_t  id[4];         /* C H R N */
    int      id_am;         /* posizione del byte FE */
    int      data_am;       /* posizione del byte FB/F8, -1 = assente */
    int      size;
    int      id_crc_ok, data_crc_ok, deleted;
    uint8_t *data;
} FdcSec;

typedef struct FdcTrack {
    int    count, fm;
    FdcSec sec[MAX_SECS];
} FdcTrack;

struct FdcCustomTrack {
    FdcTrack t;
    uint8_t  data[MAX_SECS][1024];
};

/* ------------------------------------------------------------------ */
/* utilita'                                                            */
/* ------------------------------------------------------------------ */

static uint16_t crc_byte(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)(b << 8);
    for (int i = 0; i < 8; i++)
        crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    return crc;
}

static uint16_t crc_block(uint16_t crc, const uint8_t *p, int n)
{
    while (n-- > 0) crc = crc_byte(crc, *p++);
    return crc;
}

/* CRC di un campo che inizia con il segno 'mark' (3 x A1 prima in MFM) */
static uint16_t field_crc(int fm, uint8_t mark, const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    if (!fm) {
        crc = crc_byte(crc, 0xA1);
        crc = crc_byte(crc, 0xA1);
        crc = crc_byte(crc, 0xA1);
    }
    crc = crc_byte(crc, mark);
    return crc_block(crc, p, n);
}

static ArcTime next_index(ArcTime t) { return (t / REV + 1) * REV; }

static ArcTime byte_time(const Fdc *f) { return f->dden ? ARC_US(64) : ARC_US(32); }

static int track_len(const Fdc *f) { return f->dden ? FM_LEN : MFM_LEN; }

static FdcDrive *sel_drive(Fdc *f)
{
    return f->selected >= 0 ? &f->drive[f->selected] : NULL;
}

static const FdcDrive *sel_drive_c(const Fdc *f)
{
    return f->selected >= 0 ? &f->drive[f->selected] : NULL;
}

static int spinning(const Fdc *f)
{
    const FdcDrive *d = sel_drive_c(f);
    return d && d->image && ((f->status & ST_MOTOR) || f->motor);
}

static int size_code(int size)
{
    return size == 128 ? 0 : size == 256 ? 1 : size == 512 ? 2 : 3;
}

static void std_gaps(const FdcDrive *d, int *gap1, int *gap3)
{
    if (d->sector_size == 1024) { *gap1 = 32; *gap3 = 90; }
    else if (d->sides == 2)     { *gap1 = 42; *gap3 = 57; }
    else                        { *gap1 = 60; *gap3 = 43; }
}

static uint8_t *sector_ptr(FdcDrive *d, int cyl, int head, int idx)
{
    uint32_t off = (uint32_t)(((cyl * d->sides + head) * d->sectors + idx) * d->sector_size);
    return d->image + off;
}

/* Disposizione della traccia 'cyl' (posizione della testina), faccia 'head'. */
static void track_at(FdcDrive *d, int cyl, int head, FdcTrack *t)
{
    t->count = 0;
    t->fm = 0;
    if (!d || !d->image || cyl < 0 || cyl >= FDC_MAX_CYL || head < 0 || head > 1) return;
    struct FdcCustomTrack *c = d->custom[cyl * 2 + head];
    if (c) {
        *t = c->t;
        for (int i = 0; i < t->count; i++) t->sec[i].data = c->data[i];
        return;
    }
    if (cyl >= d->tracks || head >= d->sides) return;
    int gap1, gap3, pos;
    std_gaps(d, &gap1, &gap3);
    pos = gap1;
    for (int i = 0; i < d->sectors; i++) {
        FdcSec *s = &t->sec[i];
        s->id[0] = (uint8_t)cyl;
        s->id[1] = (uint8_t)head;
        s->id[2] = (uint8_t)(d->first_sector + i);
        s->id[3] = (uint8_t)size_code(d->sector_size);
        s->id_am = pos + 15;
        s->data_am = s->id_am + 44;
        s->size = d->sector_size;
        s->id_crc_ok = s->data_crc_ok = 1;
        s->deleted = 0;
        s->data = sector_ptr(d, cyl, head, i);
        pos = s->data_am + 1 + s->size + 2 + gap3;
    }
    t->count = d->sectors;
}

/* Traccia sotto la testina dell'unita' selezionata, come la vede il WD1772
   (solo doppia densita': i dischi ad alta densita' non li legge). */
static void get_track(Fdc *f, FdcTrack *t)
{
    FdcDrive *d = sel_drive(f);
    t->count = 0;
    t->fm = 0;
    if (!d || d->hd) return;
    track_at(d, d->track, f->side, t);
}

int fdc_track_sectors(FdcDrive *d, int cyl, int head, FdcSectorView *out, int max)
{
    static FdcTrack t;
    track_at(d, cyl, head, &t);
    int n = t.count < max ? t.count : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].id, t.sec[i].id, 4);
        out[i].size = t.sec[i].size;
        out[i].data = t.sec[i].data_am >= 0 ? t.sec[i].data : NULL;
        out[i].id_crc_ok = t.sec[i].id_crc_ok;
        out[i].data_crc_ok = t.sec[i].data_crc_ok;
        out[i].deleted = t.sec[i].deleted;
        out[i].pos = t.sec[i].id_am;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* immagini                                                            */
/* ------------------------------------------------------------------ */

static void free_custom(FdcDrive *d)
{
    for (int i = 0; i < FDC_MAX_CYL * 2; i++) {
        free(d->custom[i]);
        d->custom[i] = NULL;
    }
}

int fdc_flush(Fdc *f, int drive)
{
    if (drive < 0 || drive >= FDC_DRIVES) return 0;
    FdcDrive *d = &f->drive[drive];
    if (!d->image || !d->dirty) return 1;
    FILE *fp = fopen(d->path, "r+b");
    if (!fp) fp = fopen(d->path, "wb");
    if (!fp) return 0;
    size_t n = fwrite(d->image, 1, d->size, fp);
    int ok = fclose(fp) == 0 && n == d->size;
    if (ok) d->dirty = 0;
    return ok;
}

void fdc_eject(Fdc *f, int drive)
{
    if (drive < 0 || drive >= FDC_DRIVES) return;
    FdcDrive *d = &f->drive[drive];
    if (!d->image) return;
    fdc_flush(f, drive);
    free(d->image);
    free_custom(d);
    d->image = NULL;
    d->size = 0;
    d->dirty = 0;
    d->path[0] = 0;
    d->disc_changed = 1;
}

/* ------------------------------------------------------------------ */
/* immagini HFE (HxC Floppy Emulator, versione 1)                      */
/* ------------------------------------------------------------------ */

/* L'HFE registra il flusso magnetico, non i settori: si conservano cosi'
   le protezioni anticopia (settori con CRC sbagliato, dati cancellati,
   settori che sconfinano nel successivo, numeri di traccia "sbagliati").
   Intestazione da 512 byte: "HXCPICFE", tracce (+9), facce (+10), velocita'
   in kbit/s (+12), tabella delle tracce (+18, in blocchi da 512). Per ogni
   traccia offset (blocchi) e lunghezza; i dati vanno a blocchi da 512 byte,
   256 della faccia 0 e 256 della faccia 1. Ogni byte porta 8 celle MFM, la
   prima nel bit 0. Si cerca il sincronismo 4489 (A1 con un impulso di clock
   mancante) e si decodifica un byte ogni 16 celle; i settori si ricostruiscono
   come in apply_written_track, come li vedrebbe il WD1772. Solo lettura. */

/* celle di una faccia -> byte decodificati (mk = sincronismo A1); posizioni in byte */
static int hfe_decode(const uint8_t *cells, int nbytes, uint8_t *raw, uint8_t *mk, int max)
{
    uint32_t sr = 0, word = 0;
    int aligned = 0, cnt = 0, n = 0;
    for (int i = 0; i < nbytes * 8 && n < max; i++) {
        uint32_t bit = (cells[i >> 3] >> (i & 7)) & 1;
        sr = ((sr << 1) | bit) & 0xFFFF;
        if (sr == 0x4489) {
            raw[n] = 0xA1;
            mk[n++] = 1;
            aligned = 1;
            cnt = 0;
            word = 0;
            continue;
        }
        if (!aligned) continue;
        word = ((word << 1) | bit) & 0xFFFF;
        if (++cnt == 16) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) v = (uint8_t)(v << 1 | ((word >> (14 - 2 * k)) & 1));
            raw[n] = v;
            mk[n++] = 0;
            cnt = 0;
        }
    }
    return n;
}

static int is_mark(const uint8_t *raw, const uint8_t *mk, int i, int fm, uint8_t lo, uint8_t hi);

/* settori di una traccia decodificata; scale = posizioni in byte da 32 us */
static struct FdcCustomTrack *hfe_track(const uint8_t *raw, const uint8_t *mk, int len, double scale)
{
    struct FdcCustomTrack *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    for (int i = 0; i < len && c->t.count < MAX_SECS; i++) {
        if (!is_mark(raw, mk, i, 0, 0xFE, 0xFE) || i + 7 > len) continue;
        FdcSec *s = &c->t.sec[c->t.count];
        memcpy(s->id, raw + i + 1, 4);
        s->id_am = (int)(i * scale);
        s->id_crc_ok = field_crc(0, 0xFE, s->id, 4) == (uint16_t)(raw[i + 5] << 8 | raw[i + 6]);
        s->size = 128 << (s->id[3] & 3);
        s->data_am = -1;
        for (int j = i + 7; j < i + 7 + 43 && j < len; j++) {
            if (!is_mark(raw, mk, j, 0, 0xF8, 0xFB)) continue;
            s->data_am = (int)(j * scale);
            s->deleted = raw[j] <= 0xF9;
            /* il WD1772 legge 'size' byte qualunque cosa ci sia dopo (anche
               il settore seguente): cosi' fanno certe protezioni */
            int avail = len - (j + 1);
            int n = avail < s->size ? avail : s->size;
            memcpy(c->data[c->t.count], raw + j + 1, (size_t)n);
            s->data_crc_ok = n == s->size && j + 3 + s->size <= len &&
                field_crc(0, raw[j], raw + j + 1, s->size) == (uint16_t)(raw[j + 1 + s->size] << 8 | raw[j + 2 + s->size]);
            break;
        }
        c->t.count++;
        i += 6;
    }
    return c;
}

static int load_hfe(FdcDrive *d, const uint8_t *file, long n)
{
    if (n < 512 || memcmp(file, "HXCPICFE", 8)) return 0;
    int tracks = file[9], sides = file[10];
    int rate = file[12] | file[13] << 8;
    long table = (long)(file[18] | file[19] << 8) * 512;
    if (tracks < 1 || sides < 1 || sides > 2 || table + tracks * 4 > n) return 0;
    if (tracks > FDC_MAX_CYL) tracks = FDC_MAX_CYL;
    double scale = rate > 0 ? 250.0 / rate : 1.0;       /* posizioni in byte a 250 kbit/s */
    for (int t = 0; t < tracks; t++) {
        const uint8_t *e = file + table + t * 4;
        long off = (long)(e[0] | e[1] << 8) * 512, len = e[2] | e[3] << 8;
        if (off + len > n) len = n - off > 0 ? n - off : 0;
        int half = (int)(len / 2);
        uint8_t *cells = malloc((size_t)half + 1), *raw = malloc((size_t)half), *mk = malloc((size_t)half);
        if (!cells || !raw || !mk) { free(cells); free(raw); free(mk); return 0; }
        for (int side = 0; side < sides; side++) {
            int k = 0;
            for (long b = 0; b < len && k < half; b += 512)
                for (int i = 0; i < 256 && k < half; i++) cells[k++] = file[off + b + side * 256 + i];
            int nraw = hfe_decode(cells, k, raw, mk, half);
            d->custom[t * 2 + side] = hfe_track(raw, mk, nraw, scale);
        }
        free(cells);
        free(raw);
        free(mk);
    }
    d->tracks = tracks;
    d->sides = sides;
    d->sectors = 5;
    d->sector_size = 1024;
    return 1;
}

int fdc_insert(Fdc *f, int drive, const char *path)
{
    if (drive < 0 || drive >= FDC_DRIVES || !path) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char sig[8] = "";
    if (fread(sig, 1, 8, fp) == 8 && !memcmp(sig, "HXCPICFE", 8)) {
        uint8_t *file = malloc((size_t)n);
        fseek(fp, 0, SEEK_SET);
        int ok = file && fread(file, 1, (size_t)n, fp) == (size_t)n;
        fclose(fp);
        if (!ok) { free(file); return 0; }
        fdc_eject(f, drive);
        FdcDrive *d = &f->drive[drive];
        ok = load_hfe(d, file, n);
        free(file);
        if (!ok) { free_custom(d); return 0; }
        d->image = calloc(1, 1);                       /* c'e' un disco; i settori stanno nelle tracce */
        d->size = 0;
        d->write_protect = 1;
        d->first_sector = 0;
        d->hd = 0;
        d->dirty = 0;
        d->disc_changed = 1;
        snprintf(d->path, sizeof d->path, "%s", path);
        return 1;
    }
    fseek(fp, 0, SEEK_SET);
    int tracks, sides, sectors, ssize;
    long full = n;
    /* alcune immagini D/E in circolazione sono troncate di una o due tracce
       (es. 814080 byte): si completano con zeri */
    if (n < 819200 && n >= 819200 - 2 * 10240) full = 819200;
    int first = 0, hd = 0;
    switch (full) {
    case 819200:  tracks = 80; sides = 2; sectors = 5;  ssize = 1024; break;
    case 655360:  tracks = 80; sides = 2; sectors = 16; ssize = 256;  break;
    case 327680:  tracks = 80; sides = 1; sectors = 16; ssize = 256;  break;
    case 1638400: tracks = 80; sides = 2; sectors = 10; ssize = 1024; hd = 1; break;     /* ADFS F */
    case 737280:  tracks = 80; sides = 2; sectors = 9;  ssize = 512; first = 1; break;   /* DOS 720 KB */
    case 1474560: tracks = 80; sides = 2; sectors = 18; ssize = 512; first = 1; hd = 1; break; /* DOS 1,44 MB */
    default: fclose(fp); return 0;
    }
    uint8_t *img = calloc(1, (size_t)full);
    if (!img || fread(img, 1, (size_t)n, fp) != (size_t)n) {
        free(img);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    n = full;
    fdc_eject(f, drive);
    FdcDrive *d = &f->drive[drive];
    FILE *wp = fopen(path, "r+b");
    d->write_protect = wp == NULL;
    if (wp) fclose(wp);
    d->image = img;
    d->size = (uint32_t)n;
    d->tracks = tracks;
    d->sides = sides;
    d->sectors = sectors;
    d->sector_size = ssize;
    d->first_sector = first;
    d->hd = hd;
    d->dirty = 0;
    d->disc_changed = 1;
    snprintf(d->path, sizeof d->path, "%s", path);
    return 1;
}

/* ------------------------------------------------------------------ */
/* stato e linee                                                       */
/* ------------------------------------------------------------------ */

static void master_reset(Fdc *f)
{
    f->command = 0x03;
    f->status = 0;
    f->sector = 1;
    f->phase = PH_IDLE;
    f->next_event = NEVER;
    f->drq = f->intrq = 0;
    f->intrq_cond = 0;
    f->type1 = 1;
    f->spin_flag = 0;
    f->motor_off_at = NEVER;
    f->idx_next = NEVER;
}

void fdc_reset(Fdc *f)
{
    /* le unita' e le immagini restano; si azzera solo il controller */
    if (f->selected < -1 || f->selected >= FDC_DRIVES) f->selected = -1;
    f->track = 0;
    f->data = 0;
    f->direction = 1;
    f->reset = 0;
    master_reset(f);
}

int fdc_drq(const Fdc *f) { return f->drq; }
int fdc_intrq(const Fdc *f) { return f->intrq; }

int fdc_disc_changed(const Fdc *f)
{
    const FdcDrive *d = sel_drive_c(f);
    return d && d->disc_changed ? 0 : 1;
}

void fdc_latch_a(Fdc *f, uint8_t value)
{
    f->selected = -1;
    for (int i = 0; i < FDC_DRIVES; i++)
        if (!(value & (1 << i))) { f->selected = i; break; }
    f->side = (value & 0x10) ? 0 : 1;
    f->motor = (value & 0x20) ? 0 : 1;
}

void fdc_latch_b(Fdc *f, uint8_t value)
{
    f->dden = (value >> 1) & 1;
    int in_reset = !(value & 0x08);
    if (in_reset && !f->reset) master_reset(f);
    f->reset = in_reset;
}

/* ------------------------------------------------------------------ */
/* fine comando                                                        */
/* ------------------------------------------------------------------ */

static void finish(Fdc *f, ArcTime t)
{
    f->phase = PH_IDLE;
    f->next_event = NEVER;
    f->status &= (uint8_t)~ST_BUSY;
    f->intrq = 1;
    f->motor_off_at = next_index(t) + 8 * REV;
}

static void end_command(Fdc *f, ArcTime t)
{
    if (f->drq && (f->status & ST_LOST)) f->drq = 0;
    if (f->drq) {
        /* BUSY e INTRQ aspettano che l'host prenda l'ultimo byte */
        f->phase = PH_WAIT_DRQ;
        f->next_event = NEVER;
        return;
    }
    finish(f, t);
}

/* il byte successivo e' pronto: se il precedente non e' stato preso, lost data */
static void set_drq(Fdc *f)
{
    if (f->drq) f->status |= ST_LOST;
    else f->drq = 1;
}

/* ------------------------------------------------------------------ */
/* ricerca degli ID                                                    */
/* ------------------------------------------------------------------ */

enum { MATCH_ANY, MATCH_TRACK, MATCH_SECTOR };

/*
 * Cerca, a partire da 'from', il primo campo ID che soddisfa il criterio.
 * Ritorna l'indice del settore e in *base l'istante dell'impulso di indice
 * del giro in cui si trova; -1 se non lo trova entro 5 impulsi di indice
 * (*base = istante della rinuncia, NEVER se il disco non gira).
 */
static int search_id(Fdc *f, ArcTime from, int mode, ArcTime *base)
{
    if (!spinning(f)) { *base = NEVER; return -1; }
    FdcTrack t;
    get_track(f, &t);
    ArcTime bt = byte_time(f);
    ArcTime limit = next_index(from) + 4 * REV;
    if (t.fm != f->dden) t.count = 0;
    for (ArcTime rev = (from / REV) * REV; rev <= limit; rev += REV) {
        for (int i = 0; i < t.count; i++) {
            const FdcSec *s = &t.sec[i];
            ArcTime start = rev + (ArcTime)(s->id_am - (t.fm ? 1 : 3)) * bt;
            ArcTime end = rev + (ArcTime)(s->id_am + 7) * bt;
            if (start < from) continue;
            if (end > limit) { *base = limit; return -1; }
            if (mode == MATCH_ANY) { *base = rev; return i; }
            if (s->id[0] != f->track) continue;
            if (mode == MATCH_SECTOR && s->id[2] != f->sector) continue;
            if (!s->id_crc_ok) { f->status |= ST_CRC; continue; }
            *base = rev;
            return i;
        }
    }
    *base = limit;
    return -1;
}

/* ------------------------------------------------------------------ */
/* comandi                                                             */
/* ------------------------------------------------------------------ */

static void step_pulse(Fdc *f)
{
    FdcDrive *d = sel_drive(f);
    if (!d) return;
    if (f->direction > 0) { if (d->track < FDC_MAX_CYL - 1) d->track++; }
    else if (d->track > 0) d->track--;
    if (d->image) d->disc_changed = 0;
}

static int tr00(const Fdc *f)
{
    const FdcDrive *d = sel_drive_c(f);
    return d && d->track == 0;
}

static const ArcTime step_rate[4] = { ARC_MS(6), ARC_MS(12), ARC_MS(2), ARC_MS(3) };

static void start_search(Fdc *f, ArcTime t, int mode, int phase)
{
    ArcTime base;
    f->sec_idx = search_id(f, t, mode, &base);
    f->t0 = base;
    f->phase = phase;
    if (f->sec_idx < 0) { f->next_event = base; return; }
    /* evento alla fine del campo ID (per Read Address al primo byte) */
    FdcTrack tr;
    get_track(f, &tr);
    int at = phase == PH_SEARCH && (f->command & 0xF0) == 0xC0 ? tr.sec[f->sec_idx].id_am + 2
                                                                : tr.sec[f->sec_idx].id_am + 7;
    f->next_event = base + (ArcTime)at * byte_time(f);
}

/* tipo I: un passo del ciclo restore/seek/step */
static void type1_step(Fdc *f, ArcTime t)
{
    uint8_t c = f->command;
    int done = 0;
    if (c < 0x10) {                                 /* Restore */
        if (tr00(f)) { f->track = 0; done = 1; }
        else if (f->steps >= 255) {
            if (c & 0x04) f->status |= ST_RNF;
            f->track = 0;                           /* il registro vale comunque 0 */
            finish(f, t);
            return;
        } else f->direction = -1;
    } else if (c < 0x20) {                          /* Seek */
        if (f->track == f->data) done = 1;
        else {
            f->direction = f->data > f->track ? 1 : -1;
            f->track = (uint8_t)(f->track + f->direction);
        }
    } else {                                        /* Step, Step-in, Step-out */
        if (f->steps > 0) done = 1;
        else if (c & 0x10) f->track = (uint8_t)(f->track + f->direction);
    }
    if (done) {
        if (c & 0x04) {
            f->phase = PH_VERIFY_SETTLE;
            f->next_event = t + SETTLE;
        } else finish(f, t);
        return;
    }
    step_pulse(f);
    f->steps++;
    f->phase = PH_STEP;
    f->next_event = t + step_rate[c & 3];
}

/* tipo II/III dopo spin-up e assestamento */
static void type23_begin(Fdc *f, ArcTime t)
{
    uint8_t c = f->command;
    FdcDrive *d = sel_drive(f);
    int write = (c & 0xE0) == 0xA0 || (c & 0xF0) == 0xF0;
    if (write && d && d->image && d->write_protect) {
        f->status |= ST_WP;
        finish(f, t);
        return;
    }
    switch (c & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0:
        start_search(f, t, MATCH_SECTOR, PH_SEARCH);
        break;
    case 0xC0:
        start_search(f, t, MATCH_ANY, PH_SEARCH);
        break;
    case 0xE0:
        f->phase = PH_RTRACK_INDEX;
        f->next_event = spinning(f) ? next_index(t) : NEVER;
        break;
    case 0xF0:
        f->drq = 1;
        f->phase = PH_WTRACK_INDEX;
        f->next_event = spinning(f) ? next_index(t) : NEVER;
        break;
    }
}

static void after_spinup(Fdc *f, ArcTime t)
{
    if (f->type1 && f->spin_flag) f->status |= ST_SPIN;
    if (f->type1) {
        f->steps = 0;
        type1_step(f, t);
    } else if (f->command & 0x04) {
        f->phase = PH_SETTLE;
        f->next_event = t + SETTLE;
    } else type23_begin(f, t);
}

static void force_interrupt(Fdc *f, uint8_t c, ArcTime now)
{
    if (f->status & ST_BUSY) {
        f->phase = PH_IDLE;
        f->next_event = NEVER;
        f->status &= (uint8_t)~ST_BUSY;
        f->drq = 0;
    } else {
        f->type1 = 1;
        f->drq = 0;
    }
    f->intrq_cond = c & 0x0F;
    if (c & 0x08) f->intrq = 1;
    f->idx_next = (c & 0x04) ? next_index(now) : NEVER;
    /* come in MAME: il WD1772 accende il motore anche con il Force Interrupt */
    f->status |= ST_MOTOR;
    f->motor_off_at = next_index(now) + 8 * REV;
}

static void command(Fdc *f, uint8_t c, ArcTime now)
{
    f->intrq = 0;
    if ((c & 0xF0) == 0xD0) { force_interrupt(f, c, now); return; }
    if (f->status & ST_BUSY) return;                /* ignorato durante un comando */
    f->command = c;
    f->intrq_cond = 0;
    f->idx_next = NEVER;
    f->motor_off_at = NEVER;
    f->drq = 0;
    f->type1 = c < 0x80;
    f->status = (uint8_t)((f->status & ST_MOTOR) | ST_BUSY);
    if (c >= 0x40 && c < 0x60) f->direction = 1;
    else if (c >= 0x60 && c < 0x80) f->direction = -1;
    /* spin-up: con h = 0 e motore spento, 6 giri */
    int motor_was_on = f->status & ST_MOTOR;
    f->status |= ST_MOTOR;
    f->spin_flag = !(motor_was_on && (c & 0x08));
    if (!motor_was_on && !(c & 0x08)) {
        f->phase = PH_SPINUP;
        f->next_event = spinning(f) ? next_index(now) + 5 * REV : NEVER;
        return;
    }
    after_spinup(f, now);
}

/* ------------------------------------------------------------------ */
/* traccia grezza (Read Track) e interpretazione del Write Track        */
/* ------------------------------------------------------------------ */

static void put_field(uint8_t *raw, int len, int fm, int am, uint8_t mark,
                      const uint8_t *p, int n, int crc_ok)
{
    int pre = fm ? 6 : 12;
    int sync = fm ? 0 : 3;
    int pos = am - sync - pre;
    for (int i = 0; i < pre; i++, pos++) if (pos >= 0 && pos < len) raw[pos] = 0x00;
    for (int i = 0; i < sync; i++, pos++) if (pos >= 0 && pos < len) raw[pos] = 0xA1;
    if (pos < len) raw[pos++] = mark;
    for (int i = 0; i < n && pos < len; i++) raw[pos++] = p[i];
    uint16_t crc = field_crc(fm, mark, p, n);
    if (!crc_ok) crc ^= 0x5555;
    if (pos < len) raw[pos++] = (uint8_t)(crc >> 8);
    if (pos < len) raw[pos++] = (uint8_t)crc;
}

static int build_raw_track(Fdc *f, uint8_t *raw)
{
    int len = track_len(f);
    FdcTrack t;
    get_track(f, &t);
    if (t.fm != f->dden) {
        memset(raw, 0xFF, (size_t)len);             /* nessun dato leggibile */
        return len;
    }
    memset(raw, t.fm ? 0xFF : 0x4E, (size_t)len);
    for (int i = 0; i < t.count; i++) {
        const FdcSec *s = &t.sec[i];
        put_field(raw, len, t.fm, s->id_am, 0xFE, s->id, 4, s->id_crc_ok);
        if (s->data_am >= 0)
            put_field(raw, len, t.fm, s->data_am, s->deleted ? 0xF8 : 0xFB,
                      s->data, s->size, s->data_crc_ok);
    }
    return len;
}

/* il marcatore precede il byte di segno: A1 (MFM) o nulla (FM) */
static int is_mark(const uint8_t *raw, const uint8_t *mk, int i, int fm, uint8_t lo, uint8_t hi)
{
    if (raw[i] < lo || raw[i] > hi) return 0;
    if (fm) return mk[i];
    return i > 0 && mk[i - 1] && raw[i - 1] == 0xA1 && !mk[i];
}

static void store_track(FdcDrive *d, int cyl, int side, struct FdcCustomTrack *c);

/* Ricostruisce i settori dai byte scritti da Write Track e li applica. */
static void apply_written_track(Fdc *f, const uint8_t *raw, const uint8_t *mk, int len)
{
    FdcDrive *d = sel_drive(f);
    if (!d || !d->image) return;
    int fm = f->dden;
    struct FdcCustomTrack *c = calloc(1, sizeof *c);
    if (!c) return;
    c->t.fm = fm;
    for (int i = 0; i < len && c->t.count < MAX_SECS; i++) {
        if (!is_mark(raw, mk, i, fm, 0xFE, 0xFE) || i + 7 > len) continue;
        FdcSec *s = &c->t.sec[c->t.count];
        memcpy(s->id, raw + i + 1, 4);
        s->id_am = i;
        s->id_crc_ok = field_crc(fm, 0xFE, s->id, 4) == (uint16_t)(raw[i + 5] << 8 | raw[i + 6]);
        s->size = 128 << (s->id[3] & 3);
        s->data_am = -1;
        int lim = i + 7 + (fm ? 30 : 43);
        for (int j = i + 7; j < lim && j < len; j++) {
            if (!is_mark(raw, mk, j, fm, 0xF8, 0xFB)) continue;
            if (j + 1 + s->size + 2 > len) break;
            s->data_am = j;
            s->deleted = raw[j] <= 0xF9;
            memcpy(c->data[c->t.count], raw + j + 1, (size_t)s->size);
            uint16_t crc = field_crc(fm, raw[j], raw + j + 1, s->size);
            s->data_crc_ok = crc == (uint16_t)(raw[j + 1 + s->size] << 8 | raw[j + 2 + s->size]);
            break;
        }
        c->t.count++;
        i = (s->data_am >= 0 ? s->data_am + s->size : i + 6);
    }

    store_track(d, d->track, f->side, c);
}

/* Una traccia riscritta: se ha la geometria dell'immagine va nel file,
   altrimenti resta in memoria come traccia "custom". c viene preso. */
static void store_track(FdcDrive *d, int cyl, int side, struct FdcCustomTrack *c)
{
    int fm = c->t.fm;
    int slot = cyl * 2 + side;
    int standard = !fm && cyl < d->tracks && side < d->sides && c->t.count == d->sectors;
    uint32_t seen = 0;
    for (int i = 0; standard && i < c->t.count; i++) {
        const FdcSec *s = &c->t.sec[i];
        int r = s->id[2] - d->first_sector;
        if (!s->id_crc_ok || s->data_am < 0 || !s->data_crc_ok || s->deleted
            || s->id[0] != cyl || s->id[1] != side || s->size != d->sector_size
            || r < 0 || r >= d->sectors || (seen & (1u << r)))
            standard = 0;
        else seen |= 1u << r;
    }
    free(d->custom[slot]);
    d->custom[slot] = NULL;
    if (standard) {
        for (int i = 0; i < c->t.count; i++) {
            const FdcSec *s = &c->t.sec[i];
            memcpy(sector_ptr(d, cyl, side, s->id[2] - d->first_sector), c->data[i], (size_t)d->sector_size);
        }
        d->dirty = 1;
        free(c);
    } else {
        d->custom[slot] = c;
    }
}

void fdc_format_track(FdcDrive *d, int cyl, int head, const uint8_t (*ids)[4], int n, uint8_t fill)
{
    if (!d || !d->image || cyl < 0 || cyl >= FDC_MAX_CYL || head < 0 || head > 1) return;
    struct FdcCustomTrack *c = calloc(1, sizeof *c);
    if (!c) return;
    int pos = 80;
    for (int i = 0; i < n && i < MAX_SECS; i++) {
        FdcSec *s = &c->t.sec[i];
        memcpy(s->id, ids[i], 4);
        s->size = 128 << (s->id[3] & 3);
        s->id_am = pos;
        s->data_am = pos + 44;
        s->id_crc_ok = s->data_crc_ok = 1;
        memset(c->data[i], fill, sizeof c->data[i]);
        pos = s->data_am + s->size + 60;
        c->t.count++;
    }
    store_track(d, cyl, head, c);
}

/* ------------------------------------------------------------------ */
/* eventi                                                              */
/* ------------------------------------------------------------------ */

static void read_stream_done(Fdc *f, ArcTime t)
{
    uint8_t c = f->command & 0xF0;
    if (c == 0xC0) {                                /* Read Address */
        f->sector = f->buffer[0];
        end_command(f, t);
    } else if (c == 0xE0) {                         /* Read Track */
        end_command(f, t);
    } else {                                        /* Read Sector: CRC */
        f->phase = PH_READ_CRC;
        f->next_event = t + 2 * byte_time(f);
    }
}

static void next_sector_or_end(Fdc *f, ArcTime t)
{
    if (f->command & 0x10) {
        f->sector++;
        start_search(f, t, MATCH_SECTOR, PH_SEARCH);
    } else end_command(f, t);
}

static void wtrack_byte(Fdc *f, ArcTime t)
{
    ArcTime bt = byte_time(f);
    int len = track_len(f);
    uint8_t *raw = f->buffer, *mk = f->buffer + 8192;
    uint8_t b = f->data;
    if (f->drq) { f->status |= ST_LOST; b = 0; }
    int fm = f->dden, n = 1, p = f->pos;
    if (b == 0xF7) {
        raw[p] = (uint8_t)(f->crc >> 8);
        if (p + 1 < len) { raw[p + 1] = (uint8_t)f->crc; n = 2; }
        f->prev_mark = 0;
    } else if (!fm && b == 0xF5) {
        if (!f->prev_mark) f->crc = 0xFFFF;
        raw[p] = 0xA1; mk[p] = 1;
        f->crc = crc_byte(f->crc, 0xA1);
        f->prev_mark = 1;
    } else if (!fm && b == 0xF6) {
        raw[p] = 0xC2; mk[p] = 1;
        f->prev_mark = 0;
    } else if (fm && ((b >= 0xF8 && b <= 0xFB) || b == 0xFE || b == 0xFC)) {
        raw[p] = b; mk[p] = 1;
        f->crc = crc_byte(0xFFFF, b);
        f->prev_mark = 0;
    } else {
        raw[p] = b;
        f->crc = crc_byte(f->crc, b);
        f->prev_mark = 0;
    }
    f->pos += n;
    f->drq = 1;
    if (f->pos >= len) {
        f->phase = PH_WTRACK_STREAM;
        f->pos = len;
        f->next_event = f->t0 + (ArcTime)len * bt;   /* impulso di indice successivo */
        return;
    }
    f->next_event = t + (ArcTime)n * bt;
}

static void process_event(Fdc *f)
{
    ArcTime t = f->next_event;
    ArcTime bt = byte_time(f);
    FdcTrack tr;

    switch (f->phase) {
    case PH_SPINUP:
        after_spinup(f, t);
        break;

    case PH_SETTLE:
        type23_begin(f, t);
        break;

    case PH_STEP:
        type1_step(f, t);
        break;

    case PH_VERIFY_SETTLE:
        start_search(f, t, MATCH_TRACK, PH_VERIFY_SEARCH);
        break;

    case PH_VERIFY_SEARCH:
        if (f->sec_idx < 0) f->status |= ST_RNF;
        else f->status &= (uint8_t)~ST_CRC;
        finish(f, t);
        break;

    case PH_SEARCH:
        if (f->sec_idx < 0) {
            f->status |= ST_RNF;
            end_command(f, t);
            break;
        }
        get_track(f, &tr);
        {
            const FdcSec *s = &tr.sec[f->sec_idx];
            uint8_t c = f->command & 0xF0;
            if (c == 0xC0) {
                uint16_t crc = field_crc(tr.fm, 0xFE, s->id, 4);
                if (!s->id_crc_ok) { crc ^= 0x5555; f->status |= ST_CRC; }
                memcpy(f->buffer, s->id, 4);
                f->buffer[4] = (uint8_t)(crc >> 8);
                f->buffer[5] = (uint8_t)crc;
                f->pos = 0;
                f->len = 6;
                f->phase = PH_READ_STREAM;
                process_event(f);                   /* primo byte gia' pronto */
                break;
            }
            if (c == 0x80 || c == 0x90) {
                if (s->data_am < 0) {
                    f->phase = PH_NO_DAM;
                    f->next_event = t + (ArcTime)(tr.fm ? 30 : 43) * bt;
                    break;
                }
                memcpy(f->buffer, s->data, (size_t)s->size);
                f->pos = 0;
                f->len = s->size;
                if (s->deleted) f->status |= ST_RTYPE;
                else f->status &= (uint8_t)~ST_RTYPE;
                f->multi = s->data_crc_ok;          /* esito del CRC dei dati */
                f->phase = PH_READ_STREAM;
                f->next_event = f->t0 + (ArcTime)(s->data_am + 2) * bt;
                break;
            }
            /* Write Sector */
            f->len = s->size;
            f->phase = PH_WRITE_DRQ;
            f->next_event = t + 2 * bt;
        }
        break;

    case PH_NO_DAM:
        f->status |= ST_RNF;
        end_command(f, t);
        break;

    case PH_READ_STREAM:
        f->data = f->buffer[f->pos++];
        set_drq(f);
        if (f->pos >= f->len) read_stream_done(f, t);
        else f->next_event = t + bt;
        break;

    case PH_READ_CRC:
        if (!f->multi) { f->status |= ST_CRC; end_command(f, t); break; }
        next_sector_or_end(f, t);
        break;

    case PH_WRITE_DRQ:
        f->drq = 1;
        f->phase = PH_WRITE_GATE;
        f->next_event = t + (ArcTime)(f->dden ? 9 : 20) * bt;   /* 11/22 byte dal CRC */
        break;

    case PH_WRITE_GATE:
        if (f->drq) {
            f->status |= ST_LOST;
            f->drq = 0;
            finish(f, t);
            break;
        }
        /* 12x00 3xA1 (6x00 in FM) e segno, poi i dati */
        f->pos = 0;
        f->phase = PH_WRITE_STREAM;
        f->next_event = t + (ArcTime)(f->dden ? 7 : 16) * bt;
        break;

    case PH_WRITE_STREAM: {
        uint8_t b = f->data;
        if (f->drq) { f->status |= ST_LOST; b = 0; }
        f->buffer[f->pos++] = b;
        if (f->pos < f->len) {
            f->drq = 1;
            f->next_event = t + bt;
        } else {
            f->phase = PH_WRITE_END;
            f->next_event = t + 3 * bt;             /* CRC e un byte FF */
        }
        break;
    }

    case PH_WRITE_END: {
        FdcDrive *d = sel_drive(f);
        get_track(f, &tr);
        if (d && f->sec_idx < tr.count) {
            FdcSec *s = &tr.sec[f->sec_idx];
            memcpy(s->data, f->buffer, (size_t)s->size);
            struct FdcCustomTrack *c = d->custom[d->track * 2 + f->side];
            if (c) {
                FdcSec *cs = &c->t.sec[f->sec_idx];
                cs->data_am = cs->id_am + (tr.fm ? 24 : 44);
                cs->deleted = f->command & 1;
                cs->data_crc_ok = 1;
            } else d->dirty = 1;
        }
        next_sector_or_end(f, t);
        break;
    }

    case PH_RTRACK_INDEX:
        f->len = build_raw_track(f, f->buffer);
        f->pos = 0;
        f->phase = PH_READ_STREAM;
        f->next_event = t + bt;
        break;

    case PH_WTRACK_INDEX:
        if (f->drq) {
            f->status |= ST_LOST;
            f->drq = 0;
            finish(f, t);
            break;
        }
        memset(f->buffer, 0x4E, 8192);
        memset(f->buffer + 8192, 0, 8192);
        f->pos = 0;
        f->t0 = t;
        f->crc = 0xFFFF;
        f->prev_mark = 0;
        f->phase = PH_WTRACK_STREAM;
        wtrack_byte(f, t);
        break;

    case PH_WTRACK_STREAM:
        if (f->pos >= track_len(f)) {
            apply_written_track(f, f->buffer, f->buffer + 8192, track_len(f));
            f->drq = 0;
            finish(f, t);
        } else wtrack_byte(f, t);
        break;

    default:
        f->next_event = NEVER;
        break;
    }
}

static ArcTime idle_event(const Fdc *f)
{
    ArcTime e = NEVER;
    if (f->phase == PH_IDLE && (f->status & ST_MOTOR)) e = f->motor_off_at;
    if ((f->intrq_cond & 0x04) && f->idx_next < e) e = f->idx_next;
    return e;
}

ArcTime fdc_next_event(const Fdc *f)
{
    ArcTime e = idle_event(f);
    if (f->phase != PH_IDLE && f->phase != PH_WAIT_DRQ && f->next_event < e) e = f->next_event;
    return e;
}

void fdc_update(Fdc *f, ArcTime now)
{
    for (;;) {
        ArcTime ev = f->phase != PH_IDLE && f->phase != PH_WAIT_DRQ ? f->next_event : NEVER;
        ArcTime ie = idle_event(f);
        if (ev <= ie) {
            if (ev > now) break;
            process_event(f);
        } else {
            if (ie > now) break;
            if ((f->intrq_cond & 0x04) && f->idx_next == ie) {
                if (spinning(f)) f->intrq = 1;
                f->idx_next += REV;
            } else {
                /* 9 giri senza comandi: motore spento e salvataggio */
                f->status &= (uint8_t)~ST_MOTOR;
                f->motor_off_at = NEVER;
                for (int i = 0; i < FDC_DRIVES; i++) fdc_flush(f, i);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* registri                                                            */
/* ------------------------------------------------------------------ */

static void data_taken(Fdc *f, ArcTime now)
{
    f->drq = 0;
    if (f->phase == PH_WAIT_DRQ) finish(f, now);
}

uint8_t fdc_read(Fdc *f, int reg, ArcTime now)
{
    if (f->reset) return 0;
    fdc_update(f, now);
    switch (reg & 3) {
    case 0: {
        if (!(f->intrq_cond & 0x08)) f->intrq = 0;
        uint8_t s = f->status;
        if (f->type1) {
            const FdcDrive *d = sel_drive_c(f);
            s &= (uint8_t)(ST_MOTOR | ST_SPIN | ST_RNF | ST_CRC | ST_BUSY);
            if (d && d->image && d->write_protect) s |= ST_WP;
            if (tr00(f)) s |= ST_TR00;
            if (spinning(f) && now % REV < IDX_WIDTH) s |= ST_INDEX;
        } else {
            s &= (uint8_t)~ST_DRQ;
            if (f->drq) s |= ST_DRQ;
        }
        return s;
    }
    case 1: return f->track;
    case 2: return f->sector;
    default:
        data_taken(f, now);
        return f->data;
    }
}

void fdc_write(Fdc *f, int reg, uint8_t value, ArcTime now)
{
    if (f->reset) return;
    fdc_update(f, now);
    switch (reg & 3) {
    case 0: command(f, value, now); break;
    case 1: f->track = value; break;
    case 2: f->sector = value; break;
    default:
        f->data = value;
        data_taken(f, now);
        break;
    }
}
