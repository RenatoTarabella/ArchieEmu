/*
 * mac_keys.h - Tastiera del Mac: codici dei tasti e caratteri.
 *
 * I front end Cocoa riusano la traduzione scritta per Windows: ogni tasto
 * del Mac diventa il codice virtuale di Windows nella stessa posizione
 * (con il flag 'extended' dove serve), e i caratteri si ottengono dalla
 * disposizione attiva con UCKeyTranslate, tasti morti compresi.
 */
#ifndef MAC_KEYS_H
#define MAC_KEYS_H

#include <stdint.h>

/* codice virtuale di Windows per il tasto del Mac, -1 se non c'e' */
int mac_key_to_vk(unsigned short keycode, int *extended);

/* caratteri prodotti dal tasto con i modificatori di Cocoa (NSEventModifierFlags).
   *dead conserva lo stato dei tasti morti fra una chiamata e l'altra.
   Restituisce il numero di caratteri UTF-16 scritti in out (0 per un tasto morto). */
int mac_key_chars(unsigned short keycode, unsigned long modifier_flags, uint32_t *dead,
                  uint16_t *out, int max);

/* Command/Ctrl/Option ecc.: maschere dei singoli tasti in NSEvent.modifierFlags */
int mac_modifier_down(unsigned short keycode, unsigned long modifier_flags);

#endif
