/*
 * archie_keys.h - Dai messaggi di tastiera di Windows ai tasti dell'Archimedes.
 *
 * Lettere, cifre senza modificatori, frecce, tasti funzione e modificatori
 * vanno per posizione (li usano i giochi). I simboli seguono il carattere
 * che Windows produce con la disposizione attiva (spagnola, italiana...):
 * si premono sull'Archimedes UK i tasti che danno quel carattere; le lettere
 * accentate si compongono con Alt + codice sul tastierino, come in RISC OS.
 *
 * AltGr arriva da Windows come Ctrl sinistro + Alt destro con lo stesso
 * istante: il Ctrl viene trattenuto per un attimo e scartato se segue AltGr.
 */
#ifndef ARCHIE_KEYS_H
#define ARCHIE_KEYS_H

#include <stdint.h>
#include "archie/kbd.h"

typedef struct ArchieKeys {
    Kbd     *kbd;
    int      shift_held;              /* Shift inoltrati alla macchina */
    int      altgr_held;
    int      ctrl_pending;            /* Ctrl sinistro in attesa di capire se e' AltGr */
    uint32_t ctrl_time;
    int      ctrl_fake;               /* il Ctrl premuto era quello di AltGr */
    int      ctrl_sent;
    int      dead_pending;
    int      char_expected;
    uint8_t  suppressed[256];
    int      trace;                   /* stampa i tasti inviati (debug) */

    /* RISC OS guarda la tastiera a ogni centesimo: un tasto premuto e
       rilasciato piu' in fretta puo' sfuggire. Gli eventi generati dalla
       traduzione dei caratteri escono quindi uno ogni KEYS_GAP_MS. */
    uint8_t  out_code[256], out_down[256];
    int      out_head, out_tail;
    uint32_t out_next;
    uint32_t now;
} ArchieKeys;

#define KEYS_GAP_MS 40

void keys_init(ArchieKeys *k, Kbd *kbd);
/* WM_KEYDOWN/WM_KEYUP (anche SYS): vk, flag 'extended', ripetizione, istante (ms) */
void keys_key(ArchieKeys *k, int vk, int extended, int down, int repeat, uint32_t time);
/* WM_CHAR / WM_DEADCHAR */
void keys_char(ArchieKeys *k, unsigned ch);
void keys_deadchar(ArchieKeys *k);
/* da chiamare ogni frame: un Ctrl trattenuto troppo a lungo e' un Ctrl vero */
void keys_tick(ArchieKeys *k, uint32_t time);
/* eventi ancora in coda (per chi deve aspettare che siano usciti) */
int  keys_pending(const ArchieKeys *k);

#endif
