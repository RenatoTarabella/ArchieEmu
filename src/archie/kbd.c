/*
 * kbd.c - Tastiera dell'Archimedes (A300/A400/A3000), lato microcontrollore.
 *
 * Protocollo (A3000 TRM, driver A1 di RISC OS). Byte dal computer:
 *   HRST &FF  reset          RAK1 &FE / RAK2 &FD  conferme del reset
 *   RQPD &40|n richiesta dati (la tastiera risponde PDAT &E0|n)
 *   RQID &20  identita' (risposta KBID &80|id)
 *   PRST &21  reset del protocollo (nessun effetto visibile)
 *   RQMP &22  chiede la posizione del mouse (anche se ferma)
 *   LEDS &00-&07  bit 0 Caps Lock, bit 1 Num Lock, bit 2 Scroll Lock
 *   BACK &3F  conferma del primo byte di una coppia
 *   NACK &30 / SACK &31 / MACK &32 / SMAK &33  conferma del secondo byte e
 *             abilitazione della scansione: nessuna / tasti / mouse / entrambi
 * Byte dalla tastiera:
 *   KDDA &C0|n  tasto giu', KUDA &D0|n tasto su: due byte, prima la riga
 *               (4 bit alti del codice) poi la colonna (4 bit bassi)
 *   MDAT &00|v  dati del mouse: due byte, prima X poi Y, 7 bit con segno
 *
 * Ogni coppia va confermata: dopo il primo byte la tastiera aspetta BACK,
 * dopo il secondo una delle *ACK di scansione. Un byte inatteso mentre si
 * aspetta una conferma e' un errore di protocollo: la tastiera risponde
 * HRST e il computer deve rifare la sequenza di reset.
 *
 * Codici dei tasti (numeri "low level" dei PRM, riga<<4 | colonna):
 *   &00 Esc  &01-&0C F1-F12  &0D Print  &0E Scroll Lock  &0F Break
 *   &10 `  &11-&1A 1..9,0  &1B -  &1C =  &1D sterlina  &1E Backspace  &1F Insert
 *   &20 Home  &21 PgUp  &22 Num Lock  &23 KP/  &24 KP*  &25 KP#  &26 Tab
 *   &27-&30 Q W E R T Y U I O P  &31 [  &32 ]  &33 \  &34 Delete
 *   &35 Copy  &36 PgDn  &37-&39 KP7 KP8 KP9  &3A KP-  &3B Ctrl sinistro
 *   &3C-&44 A S D F G H J K L  &45 ;  &46 '  &47 Return
 *   &48-&4A KP4 KP5 KP6  &4B KP+  &4C Shift sinistro  (&4D assente su UK)
 *   &4E-&54 Z X C V B N M  &55 ,  &56 .  &57 /  &58 Shift destro  &59 Su
 *   &5A-&5C KP1 KP2 KP3  &5D Caps Lock  &5E Alt sinistro  &5F Spazio
 *   &60 Alt destro  &61 Ctrl destro  &62 Sinistra  &63 Giu'  &64 Destra
 *   &65 KP0  &66 KP.  &67 KP Enter
 *   &70 Select  &71 Menu  &72 Adjust (pulsanti del mouse)
 */
#include "kbd.h"
#include <string.h>

enum {
    HRST = 0xFF, RAK1 = 0xFE, RAK2 = 0xFD,
    RQID = 0x20, PRST = 0x21, RQMP = 0x22,
    NACK = 0x30, SACK = 0x31, MACK = 0x32, SMAK = 0x33, BACK = 0x3F,
    KBID = 0x80, KDDA = 0xC0, KUDA = 0xD0, PDAT = 0xE0
};

/* identita' della tastiera A300/A400/A3000 UK */
#define KBD_ID 0x01

enum {
    ST_WAIT_RAK1,   /* HRST spedito, si aspetta RAK1 */
    ST_WAIT_RAK2,   /* RAK1 spedito, si aspetta RAK2 */
    ST_WAIT_ENABLE, /* RAK2 spedito, si aspetta la prima *ACK di scansione */
    ST_IDLE,        /* funzionamento normale, nessuna coppia in volo */
    ST_WAIT_BACK,   /* primo byte di una coppia spedito */
    ST_WAIT_SACK    /* secondo byte spedito */
};

/* ------------------------------------------------------------------ */
/* code                                                               */
/* ------------------------------------------------------------------ */

static void put(Kbd *k, uint8_t b)
{
    int next = (k->q_tail + 1) % KBD_QUEUE;
    if (next == k->q_head) return;          /* coda piena: non succede col protocollo a coppie */
    k->queue[k->q_tail] = b;
    k->q_tail = next;
}

static int key_is_down(const Kbd *k, int code)
{
    return (k->keys_down[code >> 3] >> (code & 7)) & 1;
}

static int push_event(Kbd *k, int code, int down)
{
    int next = (k->pk_tail + 1) % 32;
    if (next == k->pk_head) return 0;
    k->pending_keys[k->pk_tail][0] = (uint8_t)code;
    k->pending_keys[k->pk_tail][1] = (uint8_t)down;
    k->pk_tail = next;
    return 1;
}

/* Errore di protocollo o HRST dal computer: si ricomincia dal reset. */
static void protocol_reset(Kbd *k)
{
    k->q_head = k->q_tail = 0;
    k->scan_keys = k->scan_mouse = 0;
    k->awaiting_ack = 0;
    put(k, HRST);
    k->state = ST_WAIT_RAK1;
}

/* Fine della sequenza di reset. Il micro vero azzera il suo stato e alla
   prima scansione vede come "appena premuti" i tasti gia' giu': cosi'
   RISC OS riconosce R, T, Delete e Copy tenuti all'accensione. */
static void enter_normal(Kbd *k)
{
    k->pk_head = k->pk_tail = 0;
    for (int code = 0; code < 128; code++)
        if (key_is_down(k, code)) push_event(k, code, 1);
    k->state = ST_IDLE;
}

static void set_scan(Kbd *k, uint8_t ack)
{
    k->scan_keys  = (ack == SACK || ack == SMAK);
    k->scan_mouse = (ack == MACK || ack == SMAK);
}

static int clamp7(int v) { return v < -64 ? -64 : v > 63 ? 63 : v; }

/* Con la linea libera, avvia la prossima coppia: prima i tasti, poi il mouse. */
static void pump(Kbd *k)
{
    if (k->state != ST_IDLE || k->q_head != k->q_tail) return;

    if (k->scan_keys && k->pk_head != k->pk_tail) {
        uint8_t code = k->pending_keys[k->pk_head][0];
        uint8_t base = k->pending_keys[k->pk_head][1] ? KDDA : KUDA;
        k->pk_head = (k->pk_head + 1) % 32;
        k->last_sent[0] = (uint8_t)(base | (code >> 4));
        k->last_sent[1] = (uint8_t)(base | (code & 15));
    } else if ((k->scan_mouse && (k->mouse_dx || k->mouse_dy)) || k->awaiting_ack) {
        /* awaiting_ack: richiesta RQMP da soddisfare anche col mouse fermo */
        int dx = clamp7(k->mouse_dx), dy = clamp7(k->mouse_dy);
        k->mouse_dx -= dx;
        k->mouse_dy -= dy;
        k->last_sent[0] = (uint8_t)(dx & 0x7F);
        k->last_sent[1] = (uint8_t)(dy & 0x7F);
        k->awaiting_ack = 0;
    } else {
        return;
    }
    put(k, k->last_sent[0]);
    k->state = ST_WAIT_BACK;
}

/* ------------------------------------------------------------------ */
/* interfaccia                                                        */
/* ------------------------------------------------------------------ */

void kbd_reset(Kbd *k)
{
    memset(k, 0, sizeof *k);
    /* All'accensione la tastiera spedisce HRST, ma il computer e' ancora
       in reset e lo perde: si parte quindi come se l'avesse gia' spedito,
       in attesa del RAK1 (o dell'HRST con cui il sistema operativo inizia). */
    k->state = ST_WAIT_RAK1;
}

void kbd_rx(Kbd *k, uint8_t b)
{
    if (b == HRST) {
        protocol_reset(k);
        return;
    }

    switch (k->state) {
    case ST_WAIT_RAK1:
        if (b == RAK1) { put(k, RAK1); k->state = ST_WAIT_RAK2; }
        else protocol_reset(k);
        return;
    case ST_WAIT_RAK2:
        if (b == RAK2) { put(k, RAK2); k->state = ST_WAIT_ENABLE; }
        else protocol_reset(k);
        return;
    default:
        break;
    }

    /* comandi accettati in qualunque momento dopo il reset */
    if (b <= 0x07) { k->leds = b; return; }
    if (b == RQID) { put(k, KBID | KBD_ID); return; }
    if ((b & 0xF0) == 0x40) { put(k, (uint8_t)(PDAT | (b & 15))); return; }
    if (b == PRST) return;
    if (b == RQMP) { k->awaiting_ack = 1; pump(k); return; }

    switch (k->state) {
    case ST_WAIT_ENABLE:
        if (b >= NACK && b <= SMAK) { set_scan(k, b); enter_normal(k); pump(k); }
        else if (b != BACK) protocol_reset(k);
        break;
    case ST_IDLE:
        /* una *ACK a riposo cambia solo l'abilitazione della scansione */
        if (b >= NACK && b <= SMAK) { set_scan(k, b); pump(k); }
        else if (b != BACK) protocol_reset(k);
        break;
    case ST_WAIT_BACK:
        if (b == BACK) { put(k, k->last_sent[1]); k->state = ST_WAIT_SACK; }
        else protocol_reset(k);
        break;
    case ST_WAIT_SACK:
        if (b >= NACK && b <= SMAK) { set_scan(k, b); k->state = ST_IDLE; pump(k); }
        else protocol_reset(k);
        break;
    default:
        protocol_reset(k);
        break;
    }
}

int kbd_tx(Kbd *k, uint8_t *byte)
{
    pump(k);
    if (k->q_head == k->q_tail) return 0;
    *byte = k->queue[k->q_head];
    k->q_head = (k->q_head + 1) % KBD_QUEUE;
    return 1;
}

void kbd_key(Kbd *k, int code, int down)
{
    if (code < 0 || code > 0x7F) return;
    down = down != 0;
    if (key_is_down(k, code) == down) return;   /* autorepeat dell'host */
    /* Durante il reset la mappa si aggiorna senza eventi: enter_normal
       riporta poi i tasti ancora premuti. */
    if (k->state >= ST_IDLE && !push_event(k, code, down)) return;
    if (down) k->keys_down[code >> 3] |= (uint8_t)(1u << (code & 7));
    else      k->keys_down[code >> 3] &= (uint8_t)~(1u << (code & 7));
}

void kbd_mouse_move(Kbd *k, int dx, int dy)
{
    /* il limite evita overflow se il computer non legge mai il mouse */
    k->mouse_dx += dx;
    k->mouse_dy += dy;
    if (k->mouse_dx >  4096) k->mouse_dx =  4096;
    if (k->mouse_dx < -4096) k->mouse_dx = -4096;
    if (k->mouse_dy >  4096) k->mouse_dy =  4096;
    if (k->mouse_dy < -4096) k->mouse_dy = -4096;
}

/* ------------------------------------------------------------------ */
/* tasti di Windows                                                   */
/* ------------------------------------------------------------------ */

/* Virtual key (disposizione UK) -> codice Archimedes. I tasti che Windows
   distingue solo col flag 'extended' sono gestiti in kbd_code_from_vk. */
static const struct { uint8_t vk, code; } vk_table[] = {
    { 0x1B, 0x00 },                                     /* Esc */
    { 0x70, 0x01 }, { 0x71, 0x02 }, { 0x72, 0x03 }, { 0x73, 0x04 },
    { 0x74, 0x05 }, { 0x75, 0x06 }, { 0x76, 0x07 }, { 0x77, 0x08 },
    { 0x78, 0x09 }, { 0x79, 0x0A }, { 0x7A, 0x0B }, { 0x7B, 0x0C },
    { 0x2C, 0x0D },                                     /* Print Screen */
    { 0x91, 0x0E },                                     /* Scroll Lock */
    { 0x13, 0x0F }, { 0x03, 0x0F },                     /* Pause, Ctrl+Pause -> Break */
    { 0xDF, 0x10 },                                     /* ` (VK_OEM_8 su UK) */
    { '1', 0x11 }, { '2', 0x12 }, { '3', 0x13 }, { '4', 0x14 }, { '5', 0x15 },
    { '6', 0x16 }, { '7', 0x17 }, { '8', 0x18 }, { '9', 0x19 }, { '0', 0x1A },
    { 0xBD, 0x1B }, { 0xBB, 0x1C },                     /* - = */
    { 0x08, 0x1E },                                     /* Backspace */
    { 0x09, 0x26 },                                     /* Tab */
    { 'Q', 0x27 }, { 'W', 0x28 }, { 'E', 0x29 }, { 'R', 0x2A }, { 'T', 0x2B },
    { 'Y', 0x2C }, { 'U', 0x2D }, { 'I', 0x2E }, { 'O', 0x2F }, { 'P', 0x30 },
    { 0xDB, 0x31 }, { 0xDD, 0x32 },                     /* [ ] */
    { 0xDC, 0x33 }, { 0xE2, 0x33 },                     /* \ (OEM_5, OEM_102) */
    { 0x14, 0x5D },                                     /* Caps Lock */
    { 'A', 0x3C }, { 'S', 0x3D }, { 'D', 0x3E }, { 'F', 0x3F }, { 'G', 0x40 },
    { 'H', 0x41 }, { 'J', 0x42 }, { 'K', 0x43 }, { 'L', 0x44 },
    { 0xBA, 0x45 }, { 0xC0, 0x46 },                     /* ; ' (VK_OEM_3 su UK) */
    { 0xDE, 0x1D },                                     /* # ~ (OEM_7) -> sterlina */
    { 'Z', 0x4E }, { 'X', 0x4F }, { 'C', 0x50 }, { 'V', 0x51 }, { 'B', 0x52 },
    { 'N', 0x53 }, { 'M', 0x54 },
    { 0xBC, 0x55 }, { 0xBE, 0x56 }, { 0xBF, 0x57 },     /* , . / */
    { 0x20, 0x5F },                                     /* Spazio */
    { 0xA0, 0x4C }, { 0xA1, 0x58 },                     /* Shift sx, dx */
    { 0xA2, 0x3B }, { 0xA3, 0x61 },                     /* Ctrl sx, dx */
    { 0xA4, 0x5E }, { 0xA5, 0x60 },                     /* Alt sx, dx */
    { 0x90, 0x22 },                                     /* Num Lock */
    { 0x6A, 0x24 }, { 0x6D, 0x3A }, { 0x6B, 0x4B },     /* KP * - + */
    { 0x67, 0x37 }, { 0x68, 0x38 }, { 0x69, 0x39 },     /* KP 7 8 9 */
    { 0x64, 0x48 }, { 0x65, 0x49 }, { 0x66, 0x4A },     /* KP 4 5 6 */
    { 0x61, 0x5A }, { 0x62, 0x5B }, { 0x63, 0x5C },     /* KP 1 2 3 */
    { 0x60, 0x65 }, { 0x6E, 0x66 },                     /* KP 0 . */
};

int kbd_code_from_vk(int vk, int extended)
{
    /* Stesso virtual key per due tasti: il flag 'extended' sceglie. Senza
       NumLock il tastierino arriva come frecce/Home/... non estese. */
    switch (vk) {
    case 0x10: return 0x4C;                              /* Shift generico: sinistro */
    case 0x11: return extended ? 0x61 : 0x3B;            /* Ctrl */
    case 0x12: return extended ? 0x60 : 0x5E;            /* Alt */
    case 0x0D: return extended ? 0x67 : 0x47;            /* KP Enter / Return */
    case 0x6F: return 0x23;                              /* KP / */
    case 0x2D: return extended ? 0x1F : 0x65;            /* Insert / KP0 */
    case 0x24: return extended ? 0x20 : 0x37;            /* Home / KP7 */
    case 0x21: return extended ? 0x21 : 0x39;            /* PgUp / KP9 */
    case 0x2E: return extended ? 0x34 : 0x66;            /* Delete / KP. */
    case 0x23: return extended ? 0x35 : 0x5A;            /* End = Copy / KP1 */
    case 0x22: return extended ? 0x36 : 0x5C;            /* PgDn / KP3 */
    case 0x26: return extended ? 0x59 : 0x38;            /* Su / KP8 */
    case 0x28: return extended ? 0x63 : 0x5B;            /* Giu' / KP2 */
    case 0x25: return extended ? 0x62 : 0x48;            /* Sinistra / KP4 */
    case 0x27: return extended ? 0x64 : 0x4A;            /* Destra / KP6 */
    case 0x0C: return 0x49;                              /* KP5 senza NumLock */
    default: break;
    }
    for (size_t i = 0; i < sizeof vk_table / sizeof vk_table[0]; i++)
        if (vk_table[i].vk == vk) return vk_table[i].code;
    return -1;
}
