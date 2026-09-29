/*
 * module.h - Moduli relocatable di RISC OS: caricamento e decompressione
 * (formato "squeeze" di modsqz, lo stesso che il kernel espande al *RMLoad).
 */
#ifndef RISCOS_MODULE_H
#define RISCOS_MODULE_H

#include <stdint.h>
#include <stddef.h>

/* Offset dell'intestazione di un modulo */
enum {
    MODULE_START = 0x00, MODULE_INIT = 0x04, MODULE_FINAL = 0x08,
    MODULE_SERVICE = 0x0C, MODULE_TITLE = 0x10, MODULE_HELP = 0x14,
    MODULE_COMMANDS = 0x18, MODULE_MESSAGES = 0x2C, MODULE_FLAGS = 0x30
};

typedef struct RiscosModule {
    uint8_t *data;          /* immagine espansa (malloc) */
    size_t   size;
    int      was_squeezed;
} RiscosModule;

/* Carica un file modulo e lo espande se compresso. 0 = errore (msg in err). */
int  module_load(const char *path, RiscosModule *mod, char *err, size_t errsize);
/* Espande un'immagine compressa. Ritorna NULL se il formato non torna. */
uint8_t *module_unsqueeze(const uint8_t *in, size_t insize, size_t *outsize);
void module_free(RiscosModule *mod);

uint32_t    module_word(const RiscosModule *mod, uint32_t offset);
const char *module_string(const RiscosModule *mod, uint32_t offset);

#endif
