/*
 * sdl_keys.h - Tastiera per i front end SDL2 (Linux).
 *
 * Come per il Mac (mac_keys.c), i front end SDL riusano la traduzione scritta
 * per Windows (archie_keys.c): ogni tasto diventa il codice virtuale di
 * Windows nella stessa posizione (disposizione UK), con il flag 'extended'
 * dove Windows lo mette. I caratteri arrivano da SDL_TEXTINPUT.
 */
#ifndef SDL_KEYS_H
#define SDL_KEYS_H

#include <stdint.h>

/* codice virtuale di Windows per lo scancode SDL, -1 se non c'e' */
int sdl_scancode_to_vk(int scancode, int *extended);

/* decodifica il primo carattere UTF-8 di *s e avanza; 0 a fine stringa */
uint32_t sdl_utf8_next(const char **s);

#endif
