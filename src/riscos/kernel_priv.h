/*
 * kernel_priv.h - Servizi interni del kernel condivisi con i suoi
 * sottomoduli (HostFS). Non fa parte dell'interfaccia pubblica.
 */
#ifndef RISCOS_KERNEL_PRIV_H
#define RISCOS_KERNEL_PRIV_H

#include "kernel.h"

#define KERNEL_X_BIT 0x20000u

uint8_t  kernel_rd8(RiscosKernel *k, uint32_t a);
uint32_t kernel_rd32(RiscosKernel *k, uint32_t a);
void     kernel_wr8(RiscosKernel *k, uint32_t a, uint8_t v);
void     kernel_wr32(RiscosKernel *k, uint32_t a, uint32_t v);
int      kernel_read_str(RiscosKernel *k, uint32_t a, char *out, int max);
void     kernel_error(RiscosKernel *k, uint32_t swi, uint32_t num, const char *msg);
void     kernel_print(RiscosKernel *k, const char *s);   /* testo sul VDU, \n = a capo */

/* HostFS: file del BASIC in una cartella dell'host */
void hostfs_file(RiscosKernel *k, uint32_t swi);            /* OS_File */
int  hostfs_open(RiscosKernel *k, uint32_t swi, uint32_t reason, const char *name, int *handle);
void hostfs_close(RiscosKernel *k, int handle);
int  hostfs_bget(RiscosKernel *k, int handle);              /* -1 a fine file */
int  hostfs_bput(RiscosKernel *k, int handle, uint8_t b);
void hostfs_gbpb(RiscosKernel *k, uint32_t swi);            /* OS_GBPB */
void hostfs_args(RiscosKernel *k, uint32_t swi);            /* OS_Args */
int  hostfs_eof(RiscosKernel *k, int handle);
int  hostfs_command(RiscosKernel *k, uint32_t swi, const char *cmd, const char *args);

#endif
