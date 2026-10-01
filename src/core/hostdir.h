/*
 * hostdir.h - File RISC OS in una cartella dell'host (comune alle due HostFS).
 *
 * Convenzione dei nomi (la stessa di RPCEmu/HostFS):
 *   - in RISC OS il separatore di cartelle e' '.', e '/' fa da estensione:
 *     "giochi.mandel/bas" <-> "giochi/mandel.bas" sull'host;
 *   - il tipo di file RISC OS sta in un suffisso ",xxx": un programma BASIC
 *     salvato con SAVE "mandel" diventa "mandel,ffb". Senza suffisso il
 *     file e' di tipo Text (&FFF). I file senza tipo (con indirizzi di
 *     caricamento ed esecuzione, come molti giochi) hanno ",llllllll-eeeeeeee";
 *   - lo spazio, che RISC OS non ammette nei nomi, diventa lo spazio duro (&A0).
 */
#ifndef HOSTDIR_H
#define HOSTDIR_H

#include <stddef.h>
#include <stdint.h>

#define HOSTDIR_TYPE_TEXT 0xFFFu
#define HOSTDIR_TYPE_DIR  0x1000u
#define HOSTDIR_MAX_NAME  40      /* nomi piu' lunghi accorciati per il Filer di RISC OS 3.11 */

enum { HOSTOBJ_NONE = 0, HOSTOBJ_FILE = 1, HOSTOBJ_DIR = 2 };

typedef struct HostObject {
    int      kind;                 /* HOSTOBJ_NONE, _FILE, _DIR */
    char     path[600];            /* percorso reale sull'host */
    uint32_t type;                 /* tipo RISC OS (0 se senza tipo) */
    uint32_t length;
    uint64_t date_cs;              /* centesimi dal 1900 */
    int      untyped;              /* indirizzi di caricamento/esecuzione nel suffisso */
    uint32_t load, exec;           /* ...e i loro valori */
} HostObject;

/* percorso relativo RISC OS ("giochi.mandel/bas", gia' senza "$.") -> percorso
   dell'host sotto 'root', senza suffisso del tipo. 0 se il nome non e' valido */
int  hostdir_map(const char *root, const char *rel, char *out, size_t size);

/* "nome,ffb" -> tipo; -1 se non c'e' un suffisso valido */
int  hostdir_suffix_type(const char *leaf);

/* nome dell'host -> nome RISC OS: senza suffisso, con '.' -> '/' */
void hostdir_ro_name(const char *host_leaf, char *out, size_t size);

/* scorre le voci di una cartella; cb ritorna 1 per fermarsi */
typedef int (*HostDirCallback)(const char *name, void *ctx);
void hostdir_list(const char *dir, HostDirCallback cb, void *ctx);

/* informazioni su o->path */
int  hostdir_stat(HostObject *o);

/* oggetto con percorso base 'base' (da hostdir_map): nome esatto oppure "nome,xxx" */
int  hostdir_find(const char *base, HostObject *o);

/* indirizzi di caricamento ed esecuzione con tipo e data */
void hostdir_load_exec(const HostObject *o, uint32_t *load, uint32_t *exec);

/* percorso per un file nuovo di tipo 'type' (togliendo le versioni con altri suffissi) */
void hostdir_new_file(const char *base, uint32_t type, char *out, size_t size);

/* come sopra, dagli indirizzi di caricamento ed esecuzione: tipo o suffisso con gli indirizzi */
void hostdir_new_file_le(const char *base, uint32_t load, uint32_t exec, char *out, size_t size);

/* nome dell'host per un oggetto che deve avere load/exec dati (stesso nome base) */
void hostdir_retype_path(const char *path, uint32_t load, uint32_t exec, char *out, size_t size);

/* centesimi dal 1900 -> data dell'host, e cambio di data di un file */
int  hostdir_set_date(const char *path, uint64_t date_cs);

int  hostdir_mkdir(const char *path);
int  hostdir_rmdir(const char *path);
int  hostdir_is_dir(const char *path);

#endif
