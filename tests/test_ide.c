/*
 * test_ide.c - Test del disco IDE del Risc PC: IDENTIFY, lettura e
 * scrittura in CHS e LBA, INITIALIZE DEVICE PARAMETERS, READ/WRITE
 * MULTIPLE, errori, slave assente, interrupt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ide.h"

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b) do { uint32_t va_ = (uint32_t)(a), vb_ = (uint32_t)(b); checks++; \
    if (va_ != vb_) { failures++; fprintf(stderr, "FALLITO %s:%d: %s = %08X, atteso %08X\n", \
        __FILE__, __LINE__, #a, va_, vb_); } } while (0)

static IdeDisk d;

static void regs(int count, int sector, int cyl, int dh)
{
    ide_write(&d, 2, (uint8_t)count);
    ide_write(&d, 3, (uint8_t)sector);
    ide_write(&d, 4, (uint8_t)cyl);
    ide_write(&d, 5, (uint8_t)(cyl >> 8));
    ide_write(&d, 6, (uint8_t)dh);
}

int main(void)
{
    char path[512];
    const char *tmp = getenv("TEMP");
    snprintf(path, sizeof path, "%s/test_ide.hdf", tmp ? tmp : ".");
    CHECK_EQ(ide_create_image(path, 2), 1);
    ide_init(&d);
    CHECK_EQ(ide_attach(&d, path), 1);
    CHECK_EQ(d.sectors, 4096);

    /* IDENTIFY */
    ide_write(&d, 6, 0xA0);
    ide_write(&d, 7, 0xEC);
    CHECK_EQ(ide_irq(&d), 1);
    CHECK_EQ(ide_read(&d, 7) & 0x08, 0x08);         /* DRQ; leggere lo stato toglie l'interrupt */
    CHECK_EQ(ide_irq(&d), 0);
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = ide_read_data(&d);
    CHECK_EQ(id[0], 0x0040);
    CHECK_EQ(id[3], 16);
    CHECK_EQ(id[6], 63);
    CHECK_EQ(id[60], 4096);
    CHECK_EQ(id[27], 'A' << 8 | 'r');               /* modello, byte scambiati */
    CHECK_EQ(ide_read(&d, 7) & 0x08, 0);

    /* scrittura LBA di due settori, poi rilettura */
    regs(2, 10, 0, 0xE0);
    ide_write(&d, 7, 0x30);
    CHECK_EQ(ide_irq(&d), 0);                       /* il primo blocco senza interrupt */
    for (int i = 0; i < 512; i++) ide_write_data(&d, (uint16_t)(0x1000 + i));
    CHECK_EQ(ide_irq(&d), 1);
    CHECK_EQ(ide_read(&d, 7), 0x50);                /* fine: pronto, niente DRQ */
    CHECK_EQ(ide_read(&d, 3), 11);                  /* registri sull'ultimo settore */

    regs(1, 11, 0, 0xE0);
    ide_write(&d, 7, 0x20);
    for (int i = 0; i < 255; i++) ide_read_data(&d);
    CHECK_EQ(ide_read_data(&d), 0x1000 + 511);

    /* CHS con la geometria di INITIALIZE DEVICE PARAMETERS: 4 testine, 8 settori */
    regs(8, 0, 0, 0xA3);
    ide_write(&d, 7, 0x91);
    CHECK_EQ(ide_read(&d, 7), 0x50);
    regs(1, 4, 0, 0xA1);                            /* C0 H1 S4 = LBA 1*8 + 3 = 11 */
    ide_write(&d, 7, 0x20);
    CHECK_EQ(ide_read_data(&d), 0x1000 + 256);
    for (int i = 1; i < 256; i++) ide_read_data(&d);
    regs(1, 9, 0, 0xA0);                            /* settore 9 > 8: errore */
    ide_write(&d, 7, 0x20);
    CHECK_EQ(ide_read(&d, 7), 0x51);
    CHECK_EQ(ide_read(&d, 1), 0x10);                /* IDNF */

    /* READ MULTIPLE: senza SET MULTIPLE e' un errore, poi blocchi da 2 settori */
    regs(2, 10, 0, 0xE0);
    ide_write(&d, 7, 0xC4);
    CHECK_EQ(ide_read(&d, 1), 0x04);                /* ABRT */
    regs(2, 0, 0, 0xE0);
    ide_write(&d, 7, 0xC6);
    regs(2, 10, 0, 0xE0);
    ide_write(&d, 7, 0xC4);
    for (int i = 0; i < 511; i++) ide_read_data(&d);
    CHECK_EQ(ide_read_data(&d), 0x1000 + 511);       /* un solo blocco: niente interrupt in mezzo */
    CHECK_EQ(ide_read(&d, 7), 0x50);

    /* oltre la fine del disco */
    regs(2, 0xFF, 0x0F, 0xE0);
    ide_write(&d, 7, 0x20);
    CHECK_EQ(ide_read(&d, 1), 0x10);

    /* lo slave non c'e': i registri leggono 0 e i comandi non fanno nulla */
    ide_write(&d, 6, 0xB0);
    CHECK_EQ(ide_read(&d, 7), 0);
    ide_write(&d, 7, 0xEC);
    CHECK_EQ(ide_irq(&d), 0);
    ide_write(&d, 6, 0xA0);

    /* nIEN: niente interrupt verso la macchina */
    ide_write(&d, 8, 0x02);
    ide_write(&d, 7, 0xEC);
    CHECK_EQ(ide_irq(&d), 0);
    ide_write(&d, 8, 0x00);
    CHECK_EQ(ide_irq(&d), 1);

    /* reset software: firma del disco */
    ide_write(&d, 8, 0x04);
    CHECK_EQ(ide_read(&d, 8) & 0x80, 0x80);
    ide_write(&d, 8, 0x00);
    CHECK_EQ(ide_read(&d, 7), 0x50);
    CHECK_EQ(ide_read(&d, 2), 1);

    ide_detach(&d);
    remove(path);
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
