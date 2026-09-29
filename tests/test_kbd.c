/*
 * test_kbd.c - Test del micro della tastiera: sequenza di reset, coppie di
 * byte con le conferme, errori di protocollo, mouse, richieste e tabella
 * dei tasti di Windows.
 */
#include <stdio.h>
#include <string.h>
#include "kbd.h"

static int failures = 0, checks = 0;

#define CHECK(cond_) do { checks++; if (!(cond_)) { failures++; \
    printf("  FALLITO %s:%d: %s\n", __FILE__, __LINE__, #cond_); } } while (0)
#define CHECK_EQ(a, b) do { int va_ = (int)(a), vb_ = (int)(b); checks++; \
    if (va_ != vb_) { failures++; printf("  FALLITO %s:%d: %s = %d (&%02X), atteso %d (&%02X)\n", \
    __FILE__, __LINE__, #a, va_, va_, vb_, vb_); } } while (0)

enum {
    HRST = 0xFF, RAK1 = 0xFE, RAK2 = 0xFD, RQID = 0x20, PRST = 0x21, RQMP = 0x22,
    NACK = 0x30, SACK = 0x31, MACK = 0x32, SMAK = 0x33, BACK = 0x3F
};

/* prossimo byte della tastiera, -1 se non ce n'e' */
static int next(Kbd *k)
{
    uint8_t b;
    return kbd_tx(k, &b) ? b : -1;
}

/* reset completo come lo fa RISC OS, con l'abilitazione 'ack' */
static void handshake(Kbd *k, uint8_t ack)
{
    kbd_rx(k, HRST); CHECK_EQ(next(k), HRST);
    kbd_rx(k, RAK1); CHECK_EQ(next(k), RAK1);
    kbd_rx(k, RAK2); CHECK_EQ(next(k), RAK2);
    kbd_rx(k, ack);
}

static void test_reset_sequence(void)
{
    Kbd k;
    kbd_reset(&k);
    CHECK_EQ(next(&k), -1);                 /* all'accensione non parla */
    handshake(&k, SMAK);
    CHECK(k.scan_keys && k.scan_mouse);
    CHECK_EQ(next(&k), -1);

    /* byte sbagliato durante il reset: HRST e si riparte */
    kbd_rx(&k, HRST); CHECK_EQ(next(&k), HRST);
    kbd_rx(&k, RAK2); CHECK_EQ(next(&k), HRST);
    kbd_rx(&k, RAK1); CHECK_EQ(next(&k), RAK1);
    kbd_rx(&k, RAK1); CHECK_EQ(next(&k), HRST);
    CHECK(!k.scan_keys && !k.scan_mouse);

    /* senza abilitazione i tasti restano in attesa */
    handshake(&k, NACK);
    kbd_key(&k, 0x2A, 1);
    CHECK_EQ(next(&k), -1);
    kbd_rx(&k, SACK);
    CHECK_EQ(next(&k), 0xC2);
}

static void test_key_pairs(void)
{
    Kbd k;
    kbd_reset(&k);
    handshake(&k, SMAK);

    kbd_key(&k, 0x47, 1);                    /* Return giu' */
    kbd_key(&k, 0x47, 1);                    /* autorepeat dell'host: ignorato */
    kbd_key(&k, 0x3C, 1);                    /* A giu' */
    CHECK_EQ(next(&k), 0xC4);                /* KDDA | riga 4 */
    CHECK_EQ(next(&k), -1);                  /* aspetta BACK */
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xC7);                /* KDDA | colonna 7 */
    CHECK_EQ(next(&k), -1);                  /* aspetta SMAK */
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), 0xC3);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xCC);
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), -1);

    kbd_key(&k, 0x47, 0);                    /* Return su */
    kbd_key(&k, 0x47, 0);                    /* doppio rilascio: ignorato */
    CHECK_EQ(next(&k), 0xD4);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xD7);
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), -1);

    /* pulsante Select del mouse: codice &70 */
    kbd_key(&k, 0x70, 1);
    CHECK_EQ(next(&k), 0xC7);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xC0);
    kbd_rx(&k, SACK);                        /* SACK: tasti si', mouse no */
    CHECK(k.scan_keys && !k.scan_mouse);
}

static void test_protocol_errors(void)
{
    Kbd k;
    kbd_reset(&k);
    handshake(&k, SMAK);

    kbd_key(&k, 0x11, 1);
    CHECK_EQ(next(&k), 0xC1);
    kbd_rx(&k, SMAK);                        /* doveva essere BACK */
    CHECK_EQ(next(&k), HRST);
    CHECK(!k.scan_keys);
    kbd_rx(&k, RAK1); CHECK_EQ(next(&k), RAK1);
    kbd_rx(&k, RAK2); CHECK_EQ(next(&k), RAK2);
    kbd_rx(&k, SMAK);
    /* il tasto e' ancora giu': viene riportato dopo il reset */
    CHECK_EQ(next(&k), 0xC1);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xC1);
    kbd_rx(&k, BACK);                        /* doveva essere una *ACK */
    CHECK_EQ(next(&k), HRST);
}

static void test_keys_held_at_reset(void)
{
    /* Delete tenuto all'accensione: RISC OS lo deve vedere */
    Kbd k;
    kbd_reset(&k);
    kbd_key(&k, 0x34, 1);
    CHECK_EQ(next(&k), -1);
    handshake(&k, SMAK);
    CHECK_EQ(next(&k), 0xC3);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xC4);
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), -1);
}

static void test_mouse(void)
{
    Kbd k;
    kbd_reset(&k);
    handshake(&k, SACK);
    kbd_mouse_move(&k, 5, -3);
    CHECK_EQ(next(&k), -1);                  /* mouse non abilitato */
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), 0x05);                /* X */
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0x7D);                /* Y = -3 su 7 bit */
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), -1);

    /* movimento grande: spezzato in passi da 7 bit */
    kbd_mouse_move(&k, 100, -100);
    CHECK_EQ(next(&k), 0x3F);                /* +63 */
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0x40);                /* -64 */
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), 37);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), (-36) & 0x7F);
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), -1);

    /* i tasti hanno la precedenza sul mouse */
    kbd_mouse_move(&k, 1, 0);
    kbd_key(&k, 0x5F, 1);
    CHECK_EQ(next(&k), 0xC5);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0xCF);
    kbd_rx(&k, SMAK);
    CHECK_EQ(next(&k), 0x01);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0x00);
    kbd_rx(&k, SMAK);

    /* RQMP: posizione anche col mouse fermo e la scansione spenta */
    kbd_rx(&k, NACK);
    kbd_rx(&k, RQMP);
    CHECK_EQ(next(&k), 0x00);
    kbd_rx(&k, BACK);
    CHECK_EQ(next(&k), 0x00);
    kbd_rx(&k, NACK);
    CHECK_EQ(next(&k), -1);
}

static void test_requests(void)
{
    Kbd k;
    kbd_reset(&k);
    handshake(&k, SMAK);
    kbd_rx(&k, RQID);
    CHECK_EQ(next(&k), 0x81);                /* KBID: tastiera A300/A3000 UK */
    kbd_rx(&k, 0x45);                        /* RQPD con dato 5 */
    CHECK_EQ(next(&k), 0xE5);                /* PDAT */
    kbd_rx(&k, 0x05);                        /* LEDS: Caps + Scroll */
    CHECK_EQ(k.leds, 5);
    kbd_rx(&k, PRST);
    CHECK_EQ(next(&k), -1);
    CHECK(k.scan_keys && k.scan_mouse);
}

static void test_vk_table(void)
{
    CHECK_EQ(kbd_code_from_vk(0x1B, 0), 0x00);   /* Esc */
    CHECK_EQ(kbd_code_from_vk(0x70, 0), 0x01);   /* F1 */
    CHECK_EQ(kbd_code_from_vk(0x7B, 0), 0x0C);   /* F12 */
    CHECK_EQ(kbd_code_from_vk(0x13, 0), 0x0F);   /* Pause = Break */
    CHECK_EQ(kbd_code_from_vk('1', 0), 0x11);
    CHECK_EQ(kbd_code_from_vk('0', 0), 0x1A);
    CHECK_EQ(kbd_code_from_vk('Q', 0), 0x27);
    CHECK_EQ(kbd_code_from_vk('R', 0), 0x2A);
    CHECK_EQ(kbd_code_from_vk('T', 0), 0x2B);
    CHECK_EQ(kbd_code_from_vk('A', 0), 0x3C);
    CHECK_EQ(kbd_code_from_vk('Z', 0), 0x4E);
    CHECK_EQ(kbd_code_from_vk('M', 0), 0x54);
    CHECK_EQ(kbd_code_from_vk(0x0D, 0), 0x47);   /* Return */
    CHECK_EQ(kbd_code_from_vk(0x0D, 1), 0x67);   /* Enter del tastierino */
    CHECK_EQ(kbd_code_from_vk(0x2E, 1), 0x34);   /* Delete */
    CHECK_EQ(kbd_code_from_vk(0x23, 1), 0x35);   /* End = Copy */
    CHECK_EQ(kbd_code_from_vk(0x08, 0), 0x1E);   /* Backspace */
    CHECK_EQ(kbd_code_from_vk(0x11, 0), 0x3B);   /* Ctrl sinistro */
    CHECK_EQ(kbd_code_from_vk(0x11, 1), 0x61);   /* Ctrl destro */
    CHECK_EQ(kbd_code_from_vk(0x12, 1), 0x60);   /* Alt destro */
    CHECK_EQ(kbd_code_from_vk(0xA1, 0), 0x58);   /* Shift destro */
    CHECK_EQ(kbd_code_from_vk(0x26, 1), 0x59);   /* freccia su */
    CHECK_EQ(kbd_code_from_vk(0x26, 0), 0x38);   /* KP8 senza NumLock */
    CHECK_EQ(kbd_code_from_vk(0x68, 0), 0x38);   /* KP8 */
    CHECK_EQ(kbd_code_from_vk(0x6F, 1), 0x23);   /* KP / */
    CHECK_EQ(kbd_code_from_vk(0x20, 0), 0x5F);   /* spazio */
    CHECK_EQ(kbd_code_from_vk(0x5B, 1), -1);     /* tasto Windows: nessuno */

    /* ogni tasto mappato produce un codice valido; due VK diversi possono
       dare lo stesso codice solo se sono alias noti */
    int seen[128] = {0}, bad = 0;
    for (int vk = 0; vk < 256; vk++)
        for (int ext = 0; ext < 2; ext++) {
            int c = kbd_code_from_vk(vk, ext);
            if (c == -1) continue;
            if (c < 0 || c > 0x67 || c == 0x4D) bad++;
            else seen[c] = 1;
        }
    CHECK_EQ(bad, 0);
    int covered = 0;
    for (int c = 0; c <= 0x67; c++) covered += seen[c];
    /* tutti tranne &25 (KP #, assente sul PC) e &4D (assente sulla UK) */
    CHECK_EQ(covered, 0x68 - 2);
}

int main(void)
{
    test_reset_sequence();
    test_key_pairs();
    test_protocol_errors();
    test_keys_held_at_reset();
    test_mouse();
    test_requests();
    test_vk_table();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
