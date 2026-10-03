/*
 * sdl_ui.h - Interfaccia minima per i front end SDL2 (Linux).
 *
 * SDL non ha menu, finestre di dialogo ne' testo: qui c'e' quel poco che
 * serve, disegnato con il font di sistema di RISC OS (8x8, ingrandito) su
 * uno strato trasparente sopra l'immagine della macchina. Le finestre
 * (menu, messaggi, scelta dei file) sono modali: hanno il proprio ciclo
 * degli eventi e la macchina resta ferma finche' non si chiudono.
 *
 * La scelta dei file usa zenity o kdialog se ci sono (le finestre del
 * desktop), altrimenti un elenco dentro la finestra.
 */
#ifndef SDL_UI_H
#define SDL_UI_H

#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

enum { UI_CHECKED = 1, UI_DISABLED = 2, UI_SEPARATOR = 4 };

typedef struct UiItem {
    const char *label;
    int         flags;
} UiItem;

typedef struct UiCanvas {
    uint32_t *px;                  /* ARGB, alfa nel byte alto */
    int       w, h;
} UiCanvas;

/* colori */
#define UI_BG      0xFFDDDDDDu
#define UI_TEXT    0xFF000000u
#define UI_GREY    0xFF888888u
#define UI_HILITE  0xFF2050C0u
#define UI_WHITE   0xFFFFFFFFu
#define UI_TITLE   0xFFBBBBBBu
#define UI_DIM     0x70000000u

void ui_init(SDL_Window *w, SDL_Renderer *r);
void ui_quit(void);
/* chi disegna la macchina sotto le finestre modali (senza SDL_RenderPresent) */
void ui_set_backdrop(void (*draw)(void *ctx), void *ctx);

/* strato trasparente grande quanto la finestra: ui_begin lo svuota,
   ui_end lo copia sopra quello che il renderer ha gia' disegnato */
UiCanvas *ui_begin(void);
void      ui_end(void);

int  ui_scale(void);               /* ingrandimento del font */
int  ui_char_w(void);
int  ui_line_h(void);              /* altezza di una riga di menu */
int  ui_text_w(const char *s);
void ui_fill(UiCanvas *c, int x, int y, int w, int h, uint32_t argb);
void ui_frame(UiCanvas *c, int x, int y, int w, int h, uint32_t argb);
void ui_text(UiCanvas *c, int x, int y, const char *s, uint32_t argb);

/* menu a comparsa con l'angolo in alto a sinistra in (x, y): indice scelto o -1 */
int  ui_menu(const UiItem *items, int n, int x, int y);
/* messaggio con OK */
void ui_message(const char *title, const char *text);
/* OK/Annulla: 1 se OK */
int  ui_confirm(const char *title, const char *text, const char *ok_label);
/* scelta di un file da aprire (save = 0) o da creare (save = 1, con nome proposto):
   1 e il percorso in out, 0 se si rinuncia */
int  ui_file(const char *title, const char *dir, int save, const char *name, char *out, size_t size);

#endif
