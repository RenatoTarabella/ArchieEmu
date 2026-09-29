/*
 * arm2_disasm.h - Disassemblatore ARMv2a, sintassi dell'assembler BBC BASIC
 */
#ifndef ARM2_DISASM_H
#define ARM2_DISASM_H

#include <stdint.h>
#include <stddef.h>

/* 'addr' serve a risolvere i salti. Ritorna la lunghezza del testo. */
int arm2_disasm(uint32_t instr, uint32_t addr, char *out, size_t size);

#endif
