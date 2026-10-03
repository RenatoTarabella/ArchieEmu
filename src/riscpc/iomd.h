/*
 * iomd.h - IOMD del Risc PC: interrupt, timer, linee I/O (I2C della CMOS),
 * tastiera PS/2, contatori del mouse, controllo di ROM e DRAM e i canali
 * DMA (video, cursore, suono, I/O).
 *
 * Registri a parola da &03200000 (si usano i bit bassi del dato):
 *   &000 IOCR  linee C0-C5, bit 7 flyback      &004 KBDDAT  &008 KBDCR
 *   &010-&018 IRQ A: stato, richiesta/azzeramento, maschera
 *   &020-&028 IRQ B       &030-&038 FIQ
 *   &040-&04C timer 0: basso, alto, go, latch    &050-&05C timer 1
 *   &080 ROMCR0 &084 ROMCR1 &088 DRAMCR &08C VREFCR &090 FSIZE
 *   &094 ID0 &098 ID1 &09C VERSION  &0A0 MOUSEX &0A4 MOUSEY
 *   &0C0 IOTCR &0C4 ECTCR &0C8 ASTCR
 *   &100-&17C DMA di I/O  &180-&1BC DMA del suono
 *   &1C0 CURSCUR &1C4 CURSINIT  &1D0 VIDCUR &1D4 VIDEND &1D8 VIDSTART
 *   &1DC VIDINIT &1E0 VIDCR     &1F0 DMAST &1F4 DMARQ &1F8 DMAMSK
 * Bit dell'IRQ A come sull'IOC: 7 forzato, 6 timer 1, 5 timer 0,
 *   4 accensione, 3 flyback, 2 indice del floppy, 0 stampante.
 * IRQ B: 7 tastiera: byte ricevuto, 6 tastiera: trasmettitore vuoto, 5 schede,
 *   4 floppy, 2 seriale, 1 IDE (abilitato da ADFS), 0 FIQ delle schede.
 */
#ifndef RISCPC_IOMD_H
#define RISCPC_IOMD_H

#include <stdint.h>
#include "../archie/archie_time.h"

#define IOMD_ID 0xD4E7u

typedef struct IomdTimer {
    uint16_t latch, in_lo, in_hi, out;
    ArcTime  start;
    int      running;
} IomdTimer;

typedef struct IomdHooks {
    void *ctx;
    /* scrittura delle linee C0-C5 (I2C della CMOS su C0/C1);
       ritorna i livelli letti sui pin (1 = alto) */
    void    (*lines_write)(void *ctx, uint8_t value);
    uint8_t (*lines_read)(void *ctx);
    /* tastiera PS/2: byte arrivato alla tastiera; la tastiera ne ha uno? */
    void    (*to_keyboard)(void *ctx, uint8_t byte);
    int     (*from_keyboard)(void *ctx, uint8_t *byte);
} IomdHooks;

typedef struct Iomd {
    uint8_t   iocr;
    uint8_t   irqa, irqa_mask, irqb, irqb_mask, fiq, fiq_mask;
    uint8_t   dma_irq, dma_mask;
    IomdTimer timer[2];
    int       flyback;

    /* tastiera: KBDCR bit 7 trasmettitore vuoto, 6 occupato, 5 byte
       ricevuto, 3 abilitata, 2 parita' del byte ricevuto, 1-0 linee di
       clock e dati */
    uint8_t   kbd_cr;
    uint8_t   kbd_rx, kbd_tx;
    int       kbd_rx_full, kbd_tx_busy;
    ArcTime   kbd_tx_done, kbd_rx_next;
    uint32_t  romcr[2], dramcr, vrefcr, fsize, iotcr, ectcr, astcr;
    uint16_t  mouse_x, mouse_y;

    /* DMA video e cursore (indirizzi fisici) */
    uint32_t  curscur, cursinit, vidcur, vidend, vidstart, vidinit, vidcr;
    /* DMA del suono, canale 0: due buffer a ping-pong. CUR e' l'indirizzo
       fisico, END i 12 bit bassi dell'ultimo trasferimento (un buffer non
       attraversa una pagina da 4 KB) piu' il bit 31 di stop. Controllo:
       bit 7 azzera, bit 5 abilita, bit 4-0 incremento (16 byte). Stato:
       bit 0 il buffer su cui lavora il DMA (la CPU riempie l'altro),
       1 interrupt (un buffer da riempire), 2 overrun (tutti e due vuoti). */
    uint32_t  sd_cur[2], sd_end[2], sd_cr;
    int       sd_valid[2], sd_buf, sd_stopped;
    /* DMA di I/O e canale 1 del suono: per ora solo i registri */
    uint32_t  dma_regs[0x30];

    IomdHooks hooks;
} Iomd;

void     iomd_init(Iomd *m, const IomdHooks *hooks);
void     iomd_reset(Iomd *m, ArcTime now);
uint32_t iomd_read(Iomd *m, uint32_t offset, ArcTime now, int *known);
void     iomd_write(Iomd *m, uint32_t offset, uint32_t value, ArcTime now, int *known);

void     iomd_update(Iomd *m, ArcTime now);
ArcTime  iomd_next_event(const Iomd *m, ArcTime now);

/* Il DMA del suono chiede il prossimo blocco di 16 byte: ritorna 1 e
   l'indirizzo fisico, o 0 se il canale e' fermo o senza buffer. */
int      iomd_sound_next(Iomd *m, uint32_t *addr);
int      iomd_sound_active(const Iomd *m);

void     iomd_set_flyback(Iomd *m, int level);
void     iomd_set_irqb_line(Iomd *m, uint8_t bit, int level);

int      iomd_irq(const Iomd *m);
int      iomd_fiq(const Iomd *m);

#endif
