/*
 * cmos.c - PCF8583 (orologio + 240 byte di RAM) sul bus I2C dell'IOC.
 *
 * Il chip risponde all'indirizzo &A0 (scrittura) / &A1 (lettura). Una
 * scrittura manda l'indirizzo interno e poi i dati; una lettura parte
 * dall'indirizzo interno corrente. L'indirizzo avanza a ogni byte e
 * ritorna a 0 dopo 255.
 *
 * Indirizzi fisici e logici. RISC OS non usa gli indirizzi del chip ma
 * quelli "logici" di OS_Byte 161/162 (Hdr:CMOS), sfalsati in modo che i
 * byte piu' usati stiano sopra il blocco dell'orologio:
 *     fisico = ((logico + &30) MOD &F0) + &10
 *   logico &00-&BF -> fisico &40-&FF
 *   logico &C0-&EF -> fisico &10-&3F
 * Il byte logico &EF (CheckSumCMOS, fisico &3F) e' la somma di controllo:
 * seme CMOSxseed = 1 piu' tutti i byte logici &00-&EE, modulo 256
 * (ValChecksum/MakeChecksum in s/PMF/i2cutils del kernel). I registri
 * fisici &00-&0F (orologio) non entrano nella somma. RISC OS 3.1 verifica
 * la somma all'accensione e, se e' sbagliata, riporta la CMOS ai valori
 * di fabbrica.
 *
 * Registri dell'orologio (fisici): 0 controllo, 1 centesimi, 2 secondi,
 * 3 minuti, 4 ore (24 h), 5 anno mod 4 (bit 6-7) e giorno, 6 giorno della
 * settimana (bit 5-7) e mese; tutto in BCD. L'anno intero lo tiene RISC OS
 * nella RAM (YearCMOS, logico &80-&81) e lo allinea ai 2 bit del chip.
 */
#include "cmos.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DEV_ADDR   0xA0
#define LOGICAL_CHECKSUM 0xEF
#define CHECKSUM_SEED    1

enum { ST_IDLE, ST_DEV, ST_WORD, ST_WRITE, ST_READ };

static int phys(int logical) { return ((logical + 0x30) % 0xF0) + 0x10; }

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* ------------------------------------------------------------------ */
/* orologio e somma di controllo                                      */
/* ------------------------------------------------------------------ */

/* Carica nei registri 1-6 l'ora locale dell'host (RISC OS tiene l'ora
   locale nell'orologio, con l'ora legale segnata a parte). */
static void latch_clock(Cmos *c)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return;
    c->ram[1] = 0;
    c->ram[2] = bcd(t->tm_sec > 59 ? 59 : t->tm_sec);
    c->ram[3] = bcd(t->tm_min);
    c->ram[4] = bcd(t->tm_hour);
    c->ram[5] = (uint8_t)(((t->tm_year + 1900) & 3) << 6 | bcd(t->tm_mday));
    c->ram[6] = (uint8_t)(t->tm_wday << 5 | bcd(t->tm_mon + 1));
}

static uint8_t checksum(const Cmos *c)
{
    unsigned sum = CHECKSUM_SEED;
    for (int l = 0; l < LOGICAL_CHECKSUM; l++) sum += c->ram[phys(l)];
    return (uint8_t)sum;
}

void cmos_fix_checksum(Cmos *c)
{
    c->ram[phys(LOGICAL_CHECKSUM)] = checksum(c);
}

/* L'anno completo sta in YearCMOS (binario: logico &80 = anno mod 100,
   &81 = secolo); lo si allinea all'host, altrimenti RISC OS lo ricava dai
   soli 2 bit del chip partendo da un valore vecchio. */
static void sync_year(Cmos *c)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return;
    int y = t->tm_year + 1900;
    c->ram[phys(0x80)] = (uint8_t)(y % 100);
    c->ram[phys(0x81)] = (uint8_t)(y / 100);
}

/* ------------------------------------------------------------------ */
/* configurazione predefinita                                         */
/* ------------------------------------------------------------------ */

/* Coppie logico/valore. La base e' la tabella di fabbrica della ROM di
   RISC OS 3.10/3.11 (quella che il kernel scrive con un reset Delete-
   accensione, estratta dalla ROM); le righe marcate * sono cambiate per
   un A3000/A310 con monitor multisync e un solo floppy. */
static const uint8_t default_cmos[][2] = {
    { 0x01, 0xFE },   /* NetFSIDCMOS: file server 0.254 */
    { 0x03, 0xEB },   /* NetPSIDCMOS: print server 0.235 */
    { 0x05, 0x08 },   /* FileLangCMOS: filing system 8 = ADFS */
    { 0x0A, 0x1B },   /* * MODETVCMOS: bit 0-3 modo (27 & 15), bit 4 TV interlace (TV 0,1) */
    { 0x0B, 0x50 },   /* StartCMOS: drive 0, NoCaps, NoDir */
    { 0x0C, 0x20 },   /* KeyDelCMOS: 32 cs */
    { 0x0D, 0x08 },   /* KeyRepCMOS: 8 cs */
    { 0x0E, 0x0A },   /* PigCMOS: carattere ignorato dalla stampante */
    { 0x0F, 0x2C },   /* PSITCMOS: 9600 baud, stampante 1 */
    { 0x10, 0x80 },   /* DBTBCMOS: formato seriale 4, NoBoot, Scroll, beep quiet */
    { 0x11, 0x02 },   /* NetFilerCMOS */
    { 0x1C, 0x01 },   /* FileSwitchCMOS: tronca i nomi */
    { 0x84, 0x04 },   /* PrintSoundCMOS */
    { 0x85, 0x06 },   /* * VduCMOS: MonitorType 1 (multisync, bit 2-6), bit 1 = bit 4 del modo, sync separato */
    { 0x86, 0x08 },   /* FontCMOS: 32K */
    { 0x87, 0x02 },   /* * ADFSDrivesCMOS: 2 floppy, nessun disco fisso */
    { 0x88, 0xFF },   /* ADFSStepDelayCMOS */
    { 0x89, 0x01 },   /* ADFSFileCacheCMOS: dimensione automatica */
    { 0x8F, 0x05 },   /* * ScreenSizeCMOS: 5 pagine = 160K con 4 MB (pagine da 32K) */
    { 0x91, 0x08 },   /* SysHeapCMOS */
    { 0x92, 0x00 },   /* RMASizeCMOS */
    { 0x93, 0x00 },   /* SpriteSizeCMOS */
    { 0x94, 0xF0 },   /* SoundCMOS: altoparlante acceso, volume 7, voce 1 */
    { 0xB9, 0x0A },   /* LanguageCMOS: modulo 10 della ROM = Desktop */
    { 0xC2, 0x03 },   /* MouseStepCMOS */
    { 0xC3, 0x10 },   /* SystemSpeedCMOS (senza il bit 2 "ultimo reset CMOS") */
    { 0xC4, 27 },     /* * WimpModeCMOS: 640x480 16 colori */
    { 0xC5, 0x6F },   /* WimpFlagsCMOS */
    { 0xC6, 0x40 },   /* DesktopCMOS: Filer verbose */
};

static void load_defaults(Cmos *c)
{
    memset(c->ram, 0, sizeof c->ram);
    for (size_t i = 0; i < sizeof default_cmos / sizeof default_cmos[0]; i++)
        c->ram[phys(default_cmos[i][0])] = default_cmos[i][1];
}

/* ------------------------------------------------------------------ */
/* interfaccia                                                        */
/* ------------------------------------------------------------------ */

void cmos_init(Cmos *c, const char *path)
{
    memset(c, 0, sizeof *c);
    int loaded = 0;
    if (path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            loaded = fread(c->ram, 1, sizeof c->ram, f) == sizeof c->ram;
            fclose(f);
        }
    }
    if (!loaded) load_defaults(c);
    else if (c->ram[phys(LOGICAL_CHECKSUM)] != checksum(c)) {
        /* file corrotto o scritto a mano: meglio i valori di fabbrica che
           lasciare che RISC OS cancelli tutto, anche la stazione Econet */
        load_defaults(c);
    }
    /* l'emulatore ha sempre due unita' floppy: con una sola configurata
       RISC OS risponde "Bad drive" a :1 (CMOS salvate prima di questa modifica) */
    if ((c->ram[phys(0x87)] & 7) < 2) c->ram[phys(0x87)] = (uint8_t)((c->ram[phys(0x87)] & ~7) | 2);
    sync_year(c);
    cmos_fix_checksum(c);
    latch_clock(c);
    c->scl = c->sda_in = c->sda_out = 1;
    c->state = ST_IDLE;
}

int cmos_save(const Cmos *c, const char *path)
{
    if (!path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(c->ram, 1, sizeof c->ram, f) == sizeof c->ram;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* Byte completo ricevuto dal computer (dopo l'ottavo fronte di discesa
   di SCL): ritorna 1 se il chip conferma con ACK. */
static int receive_byte(Cmos *c)
{
    switch (c->state) {
    case ST_DEV:
        if ((c->shift & 0xFE) != DEV_ADDR) { c->state = ST_IDLE; return 0; }
        if (c->shift & 1) { latch_clock(c); c->state = ST_READ; }
        else c->state = ST_WORD;
        return 1;
    case ST_WORD:
        c->address = c->shift;
        c->state = ST_WRITE;
        return 1;
    case ST_WRITE:
        /* i registri 1-6 seguono sempre l'ora dell'host: le scritture si
           perdono alla prossima lettura */
        if (c->ram[c->address] != c->shift && c->address >= 16) c->dirty = 1;
        c->ram[c->address] = c->shift;
        c->address++;
        return 1;
    default:
        return 0;
    }
}

int cmos_i2c(Cmos *c, int scl, int sda)
{
    scl = scl != 0;
    sda = sda != 0;
    int bus = sda & c->sda_out;

    if (c->scl && scl) {
        /* SDA che cambia con SCL alto: START o STOP */
        if (c->sda_in && !bus) {
            c->state = ST_DEV;
            c->bit_count = 0;
            c->reading = 0;
            c->sda_out = 1;
        } else if (!c->sda_in && bus) {
            c->state = ST_IDLE;
            c->sda_out = 1;
        }
    } else if (!c->scl && scl && c->state != ST_IDLE) {
        /* fronte di salita: il ricevente campiona */
        c->bit_count++;
        if (c->bit_count <= 8) {
            if (!c->reading) c->shift = (uint8_t)(c->shift << 1 | bus);
        } else if (c->reading && bus) {
            c->state = ST_IDLE;               /* NACK del computer: fine lettura */
        }
    } else if (c->scl && !scl && c->state != ST_IDLE) {
        /* fronte di discesa: il trasmittente cambia SDA */
        if (c->bit_count == 8) {
            if (c->reading) {
                c->sda_out = 1;               /* il computer mette l'ACK */
                c->address++;
            } else c->sda_out = receive_byte(c) ? 0 : 1;
        } else if (c->bit_count >= 9) {
            c->sda_out = 1;
            c->bit_count = 0;
            if (c->state == ST_READ) {
                c->reading = 1;
                c->shift = c->ram[c->address];
                c->sda_out = c->shift >> 7;
            } else {
                c->reading = 0;
            }
        } else if (c->reading) {
            c->sda_out = (c->shift >> (7 - c->bit_count)) & 1;
        }
    }

    if (c->state == ST_IDLE) c->sda_out = 1;
    c->scl = scl;
    c->sda_in = sda & c->sda_out;
    return c->sda_in;
}
