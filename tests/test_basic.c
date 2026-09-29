/*
 * test_basic.c - Test della macchina completa con il BBC BASIC: digita
 * righe, esegue, preme Escape e controlla il testo prodotto.
 *
 *   test_basic [percorso del modulo BASIC]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine/machine.h"

static char out[65536];
static size_t out_len;

static void echo(void *ctx, int ch)
{
    (void)ctx;
    if (out_len + 2 >= sizeof out) return;
    if (ch >= 32 && ch < 127) out[out_len++] = (char)ch;
    else if (ch == 10) out[out_len++] = '\n';
    out[out_len] = 0;
}

static Machine m;
static int failures, checks;

static void type(const char *s)
{
    for (; *s; s++) kernel_key(&m.kernel, *s == '\n' ? 13 : (uint8_t)*s);
}

/* esegue finche' il BASIC non aspetta input con il buffer vuoto */
static void settle(uint64_t max_cycles)
{
    uint64_t done = 0;
    while (done < max_cycles && !m.cpu.halted) {
        done += machine_run(&m, 1000000);
        if (m.kernel.waiting && !kernel_keys_pending(&m.kernel) && !m.kernel.inkey_active) break;
    }
}

static void expect(const char *what, const char *text)
{
    checks++;
    if (!strstr(out, text)) {
        failures++;
        fprintf(stderr, "FALLITO [%s]: manca \"%s\"\n--- uscita ---\n%s\n--------------\n", what, text, out);
    }
}

static void reset_out(void) { out_len = 0; out[0] = 0; }

int main(int argc, char **argv)
{
    MachineConfig cfg = { argc > 1 ? argv[1] : "third_party/riscos/BASIC", 4, -1, 0,
                          argc > 2 ? argv[2] : "build/test_disc", 1 };
    char err[256];
    if (!machine_create(&m, &cfg, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    m.vdu.echo = echo;

    settle(50000000);
    expect("avvio", "ARM BBC BASIC V");
    expect("avvio", "bytes free");
    checks++;
    if (!m.cpu.halted && !m.kernel.waiting) { failures++; fprintf(stderr, "FALLITO: non aspetta al prompt\n"); }

    /* Escape interrompe un ciclo infinito */
    reset_out();
    type("10 I%=0\n20 REPEAT I%+=1:UNTIL FALSE\nRUN\n");
    settle(20000000);
    kernel_key(&m.kernel, 27);
    settle(20000000);
    expect("escape", "Escape at line 20");
    type("PRINT I%>1000\n");
    settle(20000000);
    expect("escape: il programma girava", "-1");

    /* GET aspetta un tasto */
    reset_out();
    type("NEW\n10 A%=GET\n20 PRINT \"tasto \";A%\nRUN\n");
    settle(20000000);
    checks++;
    if (!m.kernel.waiting) { failures++; fprintf(stderr, "FALLITO: GET non aspetta\n"); }
    type("A");
    settle(20000000);
    expect("get", "tasto 65");

    /* INKEY con timeout scade */
    reset_out();
    type("NEW\n10 T%=TIME:K%=INKEY(20):PRINT \"inkey \";K%;\" dopo \";TIME-T%>=19\nRUN\n");
    settle(400000000);
    expect("inkey", "inkey -1 dopo -1");

    /* ON ERROR e ERR/ERL/REPORT$ */
    reset_out();
    type("NEW\n10 ON ERROR PRINT \"errore \";ERR;\" riga \";ERL;\": \";REPORT$:END\n20 X=1/0\nRUN\n");
    settle(20000000);
    expect("on error", "errore 18 riga 20: Division by zero");

    /* procedure, funzioni ricorsive, stringhe */
    reset_out();
    type("NEW\n10 PRINT FNf(10)\n20 PROCs(\"ARM\")\n30 END\n40 DEF FNf(n) IF n<2 THEN =n ELSE =FNf(n-1)+FNf(n-2)\n"
         "50 DEF PROCs(a$):PRINT STRING$(3,a$);:PRINT \" \";MID$(\"Archimedes\",5,3):ENDPROC\nRUN\n");
    settle(200000000);
    expect("fn", "55");
    expect("proc", "ARMARMARM ime");

    /* modo truecolor da stringa e colori RGB */
    reset_out();
    type("MODE \"X640 Y480 C16M\":COLOUR 255,128,0:PRINT \"16M\"\n");
    settle(20000000);
    checks++;
    if (m.vdu.log2bpp != 5 || m.vdu.width != 640) {
        failures++;
        fprintf(stderr, "FALLITO: modo 16M non attivo (log2bpp=%d, %dx%d)\n%s\n", m.vdu.log2bpp, m.vdu.width, m.vdu.height, out);
    }
    expect("16M", "16M");

    /* file: SAVE/LOAD/CHAIN e canali aperti sulla cartella dell'host */
    reset_out();
    type("NEW\n10 PRINT \"dal disco \";6*7\nSAVE \"t_prog\"\nNEW\nCHAIN \"t_prog\"\n");
    type("F%=OPENOUT \"t_dati\":PRINT#F%,\"abc\",99:CLOSE#F%\n");
    type("F%=OPENIN \"t_dati\":INPUT#F%,A$,N%:PRINT \"letto \";A$;N%;EOF#F%:CLOSE#F%\n");
    type("*DELETE t_prog\n*DELETE t_dati\nLOAD \"t_prog\"\n");
    settle(50000000);
    expect("chain", "dal disco 42");
    expect("file", "letto abc99-1");
    expect("delete", "File 't_prog' not found");

    /* il testo si rilegge dai pixel (copia), con qualunque colore */
    const char *modes[] = { "MODE 28", "MODE 12", "MODE \"X640 Y480 C16M\"" };
    for (int k = 0; k < 3; k++) {
        char cmd[160];
        snprintf(cmd, sizeof cmd, "%s:COLOUR 3:COLOUR 132:PRINT \"Ciao\";:COLOUR 0:COLOUR 135:PRINT \"Mondo!\"\n", modes[k]);
        type(cmd);
        settle(20000000);
        char got[16] = { 0 };
        for (int c = 0; c < 10; c++) {
            int ch = vdu_char_at(&m.vdu, c, 0);
            got[c] = (char)(ch ? ch : '?');
        }
        checks++;
        if (strcmp(got, "CiaoMondo!")) {
            failures++;
            fprintf(stderr, "FALLITO [rilettura, %s]: \"%s\"\n", modes[k], got);
        }
    }

    printf("%d controlli, %d falliti\n", checks, failures);
    machine_destroy(&m);
    return failures ? 1 : 0;
}
