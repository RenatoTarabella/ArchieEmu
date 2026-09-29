/*
 * kbd.h - Tastiera e mouse dell'Archimedes (A300/A400/A3000), protocollo
 * seriale verso il KART dell'IOC.
 *
 * Il modulo e' solo la logica del microcontrollore della tastiera: riceve
 * i byte che il computer trasmette (kbd_rx) e mette in coda quelli da
 * rispondere (kbd_tx). I tempi della linea seriale li gestisce l'IOC.
 * Include il reset (HRST, RAK1, RAK2), le conferme (BACK, NACK, SACK,
 * SMAK), i tasti premuti/rilasciati (KDDA/KUDA) e i dati del mouse.
 */
#ifndef ARCHIE_KBD_H
#define ARCHIE_KBD_H

#include <stdint.h>

#define KBD_QUEUE 64

typedef struct Kbd {
    int     state;               /* stato del protocollo (privato) */
    uint8_t queue[KBD_QUEUE];    /* byte pronti da spedire al computer */
    int     q_head, q_tail;
    uint8_t keys_down[16];       /* bitmap riga/colonna dei tasti premuti */
    uint8_t pending_keys[32][2]; /* eventi di tasto non ancora spediti: codice, giu' */
    int     pk_head, pk_tail;
    int     mouse_dx, mouse_dy;  /* movimento accumulato */
    int     scan_keys, scan_mouse;
    uint8_t leds;
    int     awaiting_ack;        /* privato: richiesta RQMP in sospeso */
    uint8_t last_sent[2];        /* privato */
} Kbd;

void kbd_reset(Kbd *k);
/* Byte trasmesso dal computer alla tastiera. */
void kbd_rx(Kbd *k, uint8_t byte);
/* Se c'e' un byte da spedire al computer lo mette in *byte e ritorna 1. */
int  kbd_tx(Kbd *k, uint8_t *byte);

/* Eventi dal frontend. 'code' e' il codice riga/colonna dell'Archimedes
   (riga nei 4 bit alti, colonna nei 4 bassi); i pulsanti del mouse sono
   i codici &70 (sinistro/Select), &71 (centrale/Menu), &72 (destro/Adjust). */
void kbd_key(Kbd *k, int code, int down);
void kbd_mouse_move(Kbd *k, int dx, int dy);

/* Tasto di Windows (virtual key, flag 'extended' di WM_KEYDOWN) ->
   codice Archimedes, -1 se non ha corrispondenza. */
int  kbd_code_from_vk(int vk, int extended);

#endif
