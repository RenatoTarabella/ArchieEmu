@ primes.s - crivello di Eratostene su ARMv2, stampa con le SWI di RISC OS
@
@   OS_WriteC = 0   OS_Write0 = 2   OS_NewLine = 3   OS_Exit = &11
@
@ L'ARM2 non ha la divisione: print_dec divide per 10 per sottrazioni
@ successive su potenze di dieci, come nei programmi dell'epoca.

        .equ LIMIT, 10000

start:
        stmfd   sp!, {lr}
        adr     r0, title
        svc     #2                      @ OS_Write0

        @ azzera il crivello: un byte per numero
        adr     r4, sieve
        mov     r0, #0
        mov     r1, #0
        ldr     r2, limit
clear:  strb    r1, [r4, r0]
        add     r0, r0, #1
        cmp     r0, r2
        blt     clear

        @ crivello: per ogni p primo con p*p < LIMIT, segna i multipli
        mov     r5, #2                  @ p
outer:  mul     r6, r5, r5              @ p*p
        cmp     r6, r2
        bge     count
        ldrb    r0, [r4, r5]
        cmp     r0, #0
        bne     next_p
mark:   mov     r0, #1
        strb    r0, [r4, r6]
        add     r6, r6, r5
        cmp     r6, r2
        blt     mark
next_p: add     r5, r5, #1
        b       outer

        @ conta i primi e stampa quelli sotto 100
count:  mov     r5, #2
        mov     r7, #0                  @ contatore
cloop:  ldrb    r0, [r4, r5]
        cmp     r0, #0
        bne     cnext
        add     r7, r7, #1
        cmp     r5, #100
        bge     cnext
        mov     r0, r5
        bl      print_dec
        mov     r0, #32
        svc     #0                      @ OS_WriteC ' '
cnext:  add     r5, r5, #1
        cmp     r5, r2
        blt     cloop

        svc     #3                      @ OS_NewLine
        adr     r0, msg1
        svc     #2
        mov     r0, r7
        bl      print_dec
        adr     r0, msg2
        svc     #2
        ldr     r0, limit
        bl      print_dec
        svc     #3
        ldmfd   sp!, {pc}               @ torna allo stub OS_Exit

@ print_dec: stampa R0 senza segno in decimale. Preserva tutto tranne R0.
print_dec:
        stmfd   sp!, {r1-r4, lr}
        adr     r3, powers
        mov     r4, #0                  @ 1 = gia' stampata una cifra
pd_next:
        ldr     r1, [r3], #4
        cmp     r1, #1
        beq     pd_last
        mov     r2, #0
pd_sub: cmp     r0, r1
        subhs   r0, r0, r1
        addhs   r2, r2, #1
        bhs     pd_sub
        orrs    r4, r4, r2              @ salta gli zeri iniziali
        beq     pd_next
        stmfd   sp!, {r0}
        add     r0, r2, #48
        svc     #0
        ldmfd   sp!, {r0}
        b       pd_next
pd_last:
        add     r0, r0, #48
        svc     #0
        ldmfd   sp!, {r1-r4, pc}

powers: .word 1000000000, 100000000, 10000000, 1000000, 100000
        .word 10000, 1000, 100, 10, 1
limit:  .word LIMIT
title:  .asciz "Crivello di Eratostene su ARMv2\n\nPrimi sotto 100:\n"
msg1:   .asciz "Ci sono "
msg2:   .asciz " numeri primi sotto "
        .align 2
sieve:
