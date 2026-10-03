/*
 * ide.c - Disco fisso IDE del Risc PC (vedi ide.h)
 */
#include "ide.h"
#include <string.h>

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DSC  0x10
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define ER_ABRT 0x04
#define ER_IDNF 0x10

void ide_init(IdeDisk *d)
{
    memset(d, 0, sizeof *d);
    ide_reset(d);
}

void ide_reset(IdeDisk *d)
{
    /* firma di un disco ATA dopo il reset */
    d->error = 0x01;
    d->count = 1;
    d->sector = 1;
    d->cyl_lo = d->cyl_hi = 0;
    d->drvhead = 0;
    d->status = d->fp ? ST_DRDY | ST_DSC : 0;
    d->remaining = 0;
    d->pos = d->len = 0;
    d->irq = 0;
    d->multiple = 0;
    d->cur_heads = d->heads;
    d->cur_spt = d->spt;
}

int ide_attach(IdeDisk *d, const char *path)
{
    ide_detach(d);
    FILE *fp = fopen(path, "r+b");
    int ro = 0;
    if (!fp) { fp = fopen(path, "rb"); ro = 1; }
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    if (n < 512 * 1024) { fclose(fp); return 0; }
    d->fp = fp;
    d->read_only = ro;
    d->sectors = (uint32_t)(n / 512);
    d->heads = 16;
    d->spt = 63;
    uint32_t c = d->sectors / (16 * 63);
    d->cyls = c > 16383 ? 16383 : (int)c;
    snprintf(d->path, sizeof d->path, "%s", path);
    ide_reset(d);
    return 1;
}

void ide_detach(IdeDisk *d)
{
    if (d->fp) fclose(d->fp);
    d->fp = NULL;
    d->path[0] = 0;
    d->sectors = 0;
    d->status = 0;
}

int ide_create_image(const char *path, uint32_t mb)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    static uint8_t zero[65536];
    int ok = 1;
    for (uint32_t k = 0; k < mb * 16 && ok; k++) ok = fwrite(zero, 1, sizeof zero, fp) == sizeof zero;
    return fclose(fp) == 0 && ok;
}

/* ------------------------------------------------------------------ */
/* indirizzi                                                          */
/* ------------------------------------------------------------------ */

static int slave(const IdeDisk *d) { return (d->drvhead >> 4) & 1; }

/* settore indicato dai registri; 0 se l'indirizzo non e' valido */
static int get_lba(const IdeDisk *d, uint32_t *lba)
{
    if (d->drvhead & 0x40) {
        *lba = (uint32_t)(d->drvhead & 15) << 24 | (uint32_t)d->cyl_hi << 16 | (uint32_t)d->cyl_lo << 8 | d->sector;
    } else {
        uint32_t c = (uint32_t)d->cyl_hi << 8 | d->cyl_lo, h = d->drvhead & 15, s = d->sector;
        if (!s || !d->cur_heads || !d->cur_spt || (int)h >= d->cur_heads || (int)s > d->cur_spt) return 0;
        *lba = (c * (uint32_t)d->cur_heads + h) * (uint32_t)d->cur_spt + s - 1;
    }
    return *lba < d->sectors;
}

/* dopo un trasferimento i registri indicano l'ultimo settore */
static void set_lba(IdeDisk *d, uint32_t lba)
{
    if (d->drvhead & 0x40) {
        d->sector = (uint8_t)lba;
        d->cyl_lo = (uint8_t)(lba >> 8);
        d->cyl_hi = (uint8_t)(lba >> 16);
        d->drvhead = (uint8_t)((d->drvhead & 0xF0) | ((lba >> 24) & 15));
    } else if (d->cur_heads && d->cur_spt) {
        uint32_t s = lba % (uint32_t)d->cur_spt, t = lba / (uint32_t)d->cur_spt;
        uint32_t h = t % (uint32_t)d->cur_heads, c = t / (uint32_t)d->cur_heads;
        d->sector = (uint8_t)(s + 1);
        d->cyl_lo = (uint8_t)c;
        d->cyl_hi = (uint8_t)(c >> 8);
        d->drvhead = (uint8_t)((d->drvhead & 0xF0) | h);
    }
}

static void fail(IdeDisk *d, uint8_t err)
{
    d->error = err;
    d->status = ST_DRDY | ST_DSC | ST_ERR;
    d->remaining = 0;
    d->irq = 1;
}

static void done(IdeDisk *d, int interrupt)
{
    d->error = 0;
    d->status = ST_DRDY | ST_DSC;
    if (interrupt) d->irq = 1;
}

/* ------------------------------------------------------------------ */
/* trasferimenti                                                      */
/* ------------------------------------------------------------------ */

static int load_block(IdeDisk *d)
{
    int n = d->remaining < d->block ? d->remaining : d->block;
    memset(d->buf, 0, (size_t)n * 512);
    if (fseek(d->fp, (long)d->lba * 512, SEEK_SET) != 0) return 0;
    size_t got = fread(d->buf, 1, (size_t)n * 512, d->fp);
    (void)got;                                  /* oltre la fine del file: zeri */
    d->pos = 0;
    d->len = n * 512;
    return 1;
}

static void start_transfer(IdeDisk *d, int writing, int block)
{
    uint32_t lba;
    if (!get_lba(d, &lba)) { fail(d, ER_IDNF); return; }
    d->remaining = d->count ? d->count : 256;
    if (lba + (uint32_t)d->remaining > d->sectors) { fail(d, ER_IDNF); return; }
    if (writing && d->read_only) { fail(d, ER_ABRT); return; }
    d->lba = lba;
    d->writing = writing;
    d->block = block;
    d->error = 0;
    if (writing) {
        int n = d->remaining < block ? d->remaining : block;
        d->pos = 0;
        d->len = n * 512;
        d->status = ST_DRDY | ST_DSC | ST_DRQ;   /* il primo blocco senza interrupt */
    } else {
        load_block(d);
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        d->irq = 1;
    }
}

/* fine di un blocco letto o scritto dalla CPU */
static void block_done(IdeDisk *d)
{
    int n = d->len / 512;
    if (d->writing) {
        if (fseek(d->fp, (long)d->lba * 512, SEEK_SET) != 0 ||
            fwrite(d->buf, 1, (size_t)d->len, d->fp) != (size_t)d->len) { fail(d, ER_ABRT); return; }
        fflush(d->fp);
    }
    d->lba += (uint32_t)n;
    d->remaining -= n;
    set_lba(d, d->lba - 1);
    if (d->remaining <= 0) {
        d->count = 0;
        done(d, d->writing);                    /* dopo la scrittura: interrupt */
        return;
    }
    d->count = (uint8_t)d->remaining;
    if (d->writing) {
        int m = d->remaining < d->block ? d->remaining : d->block;
        d->pos = 0;
        d->len = m * 512;
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
    } else {
        load_block(d);
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
    }
    d->irq = 1;
}

static void put_string(uint16_t *w, int first, int words, const char *s)
{
    for (int i = 0; i < words; i++) {
        char a = *s ? *s++ : ' ', b = *s ? *s++ : ' ';
        w[first + i] = (uint16_t)((uint8_t)a << 8 | (uint8_t)b);    /* byte scambiati come in ATA */
    }
}

static void identify(IdeDisk *d)
{
    uint16_t w[256];
    memset(w, 0, sizeof w);
    w[0] = 0x0040;                              /* disco fisso */
    w[1] = (uint16_t)d->cyls;
    w[3] = (uint16_t)d->heads;
    w[4] = (uint16_t)(512 * d->spt);
    w[5] = 512;
    w[6] = (uint16_t)d->spt;
    put_string(w, 10, 10, "ARCHIEEMU0001");
    w[20] = 3;                                  /* buffer a doppia porta */
    w[21] = 64;
    put_string(w, 23, 4, "1.0");
    put_string(w, 27, 20, "ArchieEmu IDE disc");
    w[47] = 0x8010;                             /* fino a 16 settori per blocco */
    w[49] = 0x0200;                             /* LBA */
    w[51] = 0x0200;
    w[53] = 1;
    uint32_t cur_c = d->cur_heads && d->cur_spt ? d->sectors / (uint32_t)(d->cur_heads * d->cur_spt) : 0;
    if (cur_c > 65535) cur_c = 65535;
    w[54] = (uint16_t)cur_c;
    w[55] = (uint16_t)d->cur_heads;
    w[56] = (uint16_t)d->cur_spt;
    uint32_t cap = cur_c * (uint32_t)d->cur_heads * (uint32_t)d->cur_spt;
    w[57] = (uint16_t)cap;
    w[58] = (uint16_t)(cap >> 16);
    if (d->multiple) w[59] = (uint16_t)(0x100 | d->multiple);
    w[60] = (uint16_t)d->sectors;
    w[61] = (uint16_t)(d->sectors >> 16);
    for (int i = 0; i < 256; i++) { d->buf[2 * i] = (uint8_t)w[i]; d->buf[2 * i + 1] = (uint8_t)(w[i] >> 8); }
    d->pos = 0;
    d->len = 512;
    d->remaining = 1;
    d->block = 1;
    d->writing = 0;
    d->lba = 0;
    d->error = 0;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    d->irq = 1;
}

static void command(IdeDisk *d, uint8_t c)
{
    d->irq = 0;
    if (slave(d) || !d->fp) return;             /* nessun disco: nessuna risposta */
    switch (c) {
    case 0x20: case 0x21: start_transfer(d, 0, 1); break;               /* READ SECTORS */
    case 0x30: case 0x31: start_transfer(d, 1, 1); break;               /* WRITE SECTORS */
    case 0xC4: case 0xC5:                                               /* READ/WRITE MULTIPLE */
        if (!d->multiple) { fail(d, ER_ABRT); break; }
        start_transfer(d, c == 0xC5, d->multiple);
        break;
    case 0x40: case 0x41: {                                             /* READ VERIFY */
        uint32_t lba;
        int n = d->count ? d->count : 256;
        if (!get_lba(d, &lba) || lba + (uint32_t)n > d->sectors) { fail(d, ER_IDNF); break; }
        set_lba(d, lba + (uint32_t)n - 1);
        done(d, 1);
        break;
    }
    case 0x91:                                                          /* INITIALIZE DEVICE PARAMETERS */
        d->cur_heads = (d->drvhead & 15) + 1;
        d->cur_spt = d->count;
        done(d, 1);
        break;
    case 0xC6:                                                          /* SET MULTIPLE */
        if (d->count > 16 || (d->count & (d->count - 1))) { fail(d, ER_ABRT); break; }
        d->multiple = d->count;
        done(d, 1);
        break;
    case 0xEC: identify(d); break;
    case 0x90:                                                          /* DIAGNOSTICS */
        d->error = 0x01;
        d->status = ST_DRDY | ST_DSC;
        d->irq = 1;
        break;
    case 0xEF: done(d, 1); break;                                       /* SET FEATURES */
    case 0xE5: case 0x98: d->count = 0xFF; done(d, 1); break;           /* CHECK POWER MODE */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: case 0xE7:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
        done(d, 1);                                                     /* standby, idle, flush */
        break;
    default:
        if ((c & 0xF0) == 0x10 || (c & 0xF0) == 0x70) { done(d, 1); break; }   /* RECALIBRATE, SEEK */
        fail(d, ER_ABRT);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* registri                                                           */
/* ------------------------------------------------------------------ */

uint8_t ide_read(IdeDisk *d, int reg)
{
    if (slave(d) && reg != 6) return 0;         /* lo slave non c'e' */
    switch (reg) {
    case 0: return (uint8_t)ide_read_data(d);
    case 1: return d->error;
    case 2: return d->count;
    case 3: return d->sector;
    case 4: return d->cyl_lo;
    case 5: return d->cyl_hi;
    case 6: return d->drvhead | 0xA0;
    case 7: d->irq = 0; return d->status;
    case 8: return d->status;                   /* stato alternativo */
    default: return 0xFF;
    }
}

void ide_write(IdeDisk *d, int reg, uint8_t v)
{
    switch (reg) {
    case 0: ide_write_data(d, v); break;
    case 1: d->feature = v; break;
    case 2: d->count = v; break;
    case 3: d->sector = v; break;
    case 4: d->cyl_lo = v; break;
    case 5: d->cyl_hi = v; break;
    case 6: d->drvhead = v; break;
    case 7: command(d, v); break;
    case 8:
        if ((v & 4) && !(d->control & 4)) {     /* reset software */
            d->status = ST_BSY;
        } else if (!(v & 4) && (d->control & 4)) {
            ide_reset(d);
        }
        d->control = v;
        break;
    default: break;
    }
}

uint16_t ide_read_data(IdeDisk *d)
{
    if (slave(d) || !(d->status & ST_DRQ) || d->writing) return 0xFFFF;
    uint16_t v = (uint16_t)(d->buf[d->pos] | d->buf[d->pos + 1] << 8);
    d->pos += 2;
    if (d->pos >= d->len) block_done(d);
    return v;
}

void ide_write_data(IdeDisk *d, uint16_t v)
{
    if (slave(d) || !(d->status & ST_DRQ) || !d->writing) return;
    d->buf[d->pos] = (uint8_t)v;
    d->buf[d->pos + 1] = (uint8_t)(v >> 8);
    d->pos += 2;
    if (d->pos >= d->len) block_done(d);
}

int ide_irq(const IdeDisk *d)
{
    return d->irq && !(d->control & 2);
}
