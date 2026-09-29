@ bench.s - ciclo misto (ALU, shift, memoria, salti) per misurare l'emulatore
        .equ N, 20000000
start:  ldr     r0, count
        adr     r4, buf
        mov     r1, #0
loop:   add     r1, r1, r0, lsl #1
        eor     r2, r1, r1, ror #7
        str     r2, [r4]
        ldr     r3, [r4]
        subs    r0, r0, #1
        bne     loop
        mov     pc, lr
count:  .word N
buf:    .word 0
