/*
 * ioc.h - IOC (Input/Output Controller) dell'Archimedes.
 *
 * Registri (offset in byte, banco 0 dell'area IOC):
 *   &00 controllo (pin C0-C5, IF, IR)   &04 KART (seriale della tastiera)
 *   &10-&18 IRQ A: stato, richiesta/azzeramento, maschera
 *   &20-&28 IRQ B       &30-&38 FIQ
 *   &40+16*n timer n: latch basso, latch alto, go, lettura
 * Bit dell'IRQ A: 7 forzato, 6 timer 1, 5 timer 0, 4 accensione (POR),
 *   3 flyback verticale (IR), 2 IF, 1-0 IL7/IL6.
 * IRQ B: 7 KART pieno, 6 KART vuoto, 5-0 IL5-IL0 (1 = fine buffer audio).
 * FIQ: 7 forzato, 6 IL0, 2 FL, 1 FH1 (floppy INTRQ), 0 FH0 (floppy DRQ).
 */
#ifndef ARCHIE_IOC_H
#define ARCHIE_IOC_H

#include <stdint.h>
#include "archie_time.h"

typedef struct IocTimer {
    uint16_t latch;              /* valore di ricarica scritto con "go" */
    uint16_t in_lo, in_hi;       /* registri di latch in scrittura */
    uint16_t out;                /* valore letto dopo il comando "latch" */
    ArcTime  start;              /* istante dell'ultima ricarica */
    int      running;
} IocTimer;

typedef struct IocHooks {
    void *ctx;
    /* il computer ha scritto il registro di controllo (bit 0 SDA, 1 SCL...);
       ritorna i livelli dei pin in ingresso (1 = alto) */
    uint8_t (*control_write)(void *ctx, uint8_t value);
    uint8_t (*control_read)(void *ctx);
    /* byte arrivato alla tastiera dalla linea seriale */
    void    (*kart_to_keyboard)(void *ctx, uint8_t byte);
    /* la tastiera ha un byte da spedire? */
    int     (*keyboard_to_kart)(void *ctx, uint8_t *byte);
} IocHooks;

typedef struct Ioc {
    uint8_t  control;            /* ultimo valore scritto */
    uint8_t  irqa, irqa_mask, irqb, irqb_mask, fiq, fiq_mask;
    IocTimer timer[4];
    int      ir_level, if_level;

    /* KART: un byte in trasmissione verso la tastiera, uno in ricezione */
    uint8_t  kart_rx;
    int      tx_busy;
    uint8_t  tx_byte;
    ArcTime  tx_done;
    ArcTime  rx_next;            /* prossimo istante utile per ricevere */

    IocHooks hooks;
} Ioc;

void    ioc_init(Ioc *c, const IocHooks *hooks);
void    ioc_reset(Ioc *c, ArcTime now);
uint8_t ioc_read(Ioc *c, uint32_t offset, ArcTime now);
void    ioc_write(Ioc *c, uint32_t offset, uint8_t value, ArcTime now);

/* Avanza timer e seriale fino a 'now'. */
void    ioc_update(Ioc *c, ArcTime now);
ArcTime ioc_next_event(const Ioc *c, ArcTime now);

/* Linee in ingresso */
void    ioc_set_ir(Ioc *c, int level);           /* flyback verticale */
void    ioc_set_irqb_line(Ioc *c, uint8_t bit, int level);
void    ioc_set_fiq_line(Ioc *c, uint8_t bit, int level);

int     ioc_irq(const Ioc *c);
int     ioc_fiq(const Ioc *c);

#endif
