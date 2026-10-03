@ hostfs_module.s - Modulo HostFS per RISC OS 3.11 (sta nella ROM della scheda 0).
@
@ Dichiara a FileSwitch il filing system "HostFS" (numero &99, come RPCEmu):
@ ogni ingresso FSEntry_* esegue una SWI riservata all'emulatore, che fa il
@ lavoro in C sulla cartella dell'host (src/archie/hostfs.c) e torna con i
@ registri pronti e V acceso in caso di errore (R0 -> blocco d'errore,
@ scritto nell'area di lavoro). Contiene anche un piccolo task del desktop
@ che mette l'icona HostFS sulla barra: un clic apre la cartella principale.
@
@ Assemblato con Keystone da tools/gen_hostfs_module.py, che produce
@ hostfs_module.h. Solo istruzioni ARMv2; si assembla all'indirizzo 0,
@ quindi le etichette sono gli offset dall'inizio del modulo.

        .equ    TRAP,           0x56AC0         @ + numero dell'ingresso
        .equ    XOS_Module,     0x2001E
        .equ    XOS_FSControl,  0x20029
        .equ    XOS_CLI,        0x20005
        .equ    XOS_File,       0x20008
        .equ    XOS_SetVarVal,  0x20024
        .equ    OS_Exit,        0x11
        .equ    XWimp_Initialise, 0x600C0
        .equ    XWimp_CreateIcon, 0x600C2
        .equ    XWimp_Poll,     0x600C7
        .equ    XWimp_CloseDown, 0x600DD
        .equ    TASK_WORD,      0x4B534154      @ "TASK"

        @ area di lavoro (blocco in RMA, puntato dalla parola privata)
        .equ    WS_TASK,        0               @ maniglia del task (0 = no, -1 = in avvio)
        .equ    WS_ICONDATA,    4               @ dati indiretti dell'icona
        .equ    WS_ERROR,       64              @ blocco d'errore scritto dall'emulatore
        .equ    WS_POLL,        320             @ blocco per Wimp_Poll
        .equ    WS_NAME,        576             @ nome foglia restituito da File 0/255
        .equ    WS_SIZE,        576 + 256

module_base:
        .word   task_start
        .word   init
        .word   final
        .word   service
        .word   title
        .word   help
        .word   commands
        .word   0, 0, 0, 0

title:  .asciz  "HostFS"
help:   .asciz  "HostFS\t\t1.01 (03 Oct 2026) ArchieEmu"
fsname: .asciz  "HostFS"
banner: .asciz  "ArchieEmu HostFS"
taskname: .asciz "HostFS Filer"
        .align  2

@ ---------------------------------------------------------------- comandi
commands:
        .asciz  "Desktop_HostFS"
        .align  2
        .word   cmd_desktop, 0x00FF0000, 0, help_desktop
        .asciz  "HostFS"
        .align  2
        .word   cmd_select, 0, 0, help_select
        .asciz  "HostFS_Insert"
        .align  2
        .word   cmd_insert, 0x00010001, 0, help_insert
        .word   0
help_desktop: .asciz "Desktop_HostFS starts the HostFS icon on the icon bar. Do not use *Desktop_HostFS, use *Desktop instead.\r"
help_select:  .asciz "*HostFS selects HostFS, the folder of the host computer, as the current filing system.\r"
help_insert:  .asciz "*HostFS_Insert puts a floppy image (.adf or .hfe) stored on HostFS in drive 0 and opens it. Double-clicking a Floppy file (type &FCE) does the same.\rSyntax: *HostFS_Insert <file>"
        .align  2

cmd_desktop:                                    @ avvia il task (modulo come applicazione)
        stmfd   sp!, {lr}
        mov     r2, r0
        adr     r1, title
        mov     r0, #2
        svc     #XOS_Module
        ldmfd   sp!, {pc}

cmd_select:
        stmfd   sp!, {lr}
        mov     r0, #14
        adr     r1, fsname
        svc     #XOS_FSControl
        ldmfd   sp!, {pc}

cmd_insert:                                     @ R0 = nome del file
        stmfd   sp!, {r12, lr}
        ldr     r12, [r12]
        svc     #TRAP + 7                       @ l'emulatore inserisce il disco
        ldmfdvs sp!, {r12, pc}
        adr     r0, open_floppy
        svc     #XOS_CLI                        @ apre la finestra (fuori dal desktop non serve)
        cmn     r0, #0                          @ V spento
        ldmfd   sp!, {r12, pc}
open_floppy: .asciz "Filer_OpenDir ADFS::0.$"
        .align  2

@ tipo &FCE "Floppy": il doppio clic inserisce il dischetto
setvars:
        stmfd   sp!, {r0-r4, lr}
        adr     r0, var_type
        adr     r1, val_type
        mov     r2, #6
        mov     r3, #0
        mov     r4, #4                          @ stringa letterale
        svc     #XOS_SetVarVal
        adr     r0, var_run
        adr     r1, val_run
        mov     r2, #17
        mov     r3, #0
        mov     r4, #4
        svc     #XOS_SetVarVal
        ldmfd   sp!, {r0-r4, pc}
var_type: .asciz "File$Type_FCE"
val_type: .asciz "Floppy"
var_run:  .asciz "Alias$@RunType_FCE"
val_run:  .asciz "HostFS_Insert %*0"
        .align  2

@ ------------------------------------------------------------ inizio e fine
init:
        stmfd   sp!, {r7-r11, lr}
        mov     r0, #6                          @ area di lavoro
        mov     r3, #WS_SIZE
        svc     #XOS_Module
        ldmfdvs sp!, {r7-r11, pc}
        str     r2, [r12]
        mov     r12, r2
        mov     r0, #0
        str     r0, [r12, #WS_TASK]
        bl      declare
        bl      setvars
        cmn     r0, #0                          @ le variabili non sono essenziali
        ldmfd   sp!, {r7-r11, pc}

final:
        stmfd   sp!, {r7-r11, lr}
        ldr     r12, [r12]
        mov     r0, #16                         @ toglie il filing system
        adr     r1, fsname
        svc     #XOS_FSControl
        ldr     r0, [r12, #WS_TASK]
        cmp     r0, #0
        cmpne   r0, #-1
        ldrne   r1, task_word
        svcne   #XWimp_CloseDown
        mov     r0, #0
        str     r0, [r12, #WS_TASK]
        cmn     r0, #0                          @ V spento: si muore comunque
        ldmfd   sp!, {r7-r11, pc}

@ OS_FSControl 12: R1 = modulo, R2 = offset del blocco, R3 = valore per R12
declare:
        stmfd   sp!, {lr}
        adr     r1, here
        ldr     r2, [r1]
        sub     r1, r1, r2                      @ indirizzo del modulo
        mov     r0, #12
        ldr     r2, info_offset
        mov     r3, r12
        svc     #XOS_FSControl
        ldmfd   sp!, {pc}
here:   .word   here
info_offset: .word fs_info
task_word: .word TASK_WORD

@ --------------------------------------------------------------- servizi
service:
        teq     r1, #0x40                       @ Service_FSRedeclare
        teqne   r1, #0x49                       @ Service_StartWimp
        teqne   r1, #0x4A                       @ Service_StartedWimp
        teqne   r1, #0x27                       @ Service_Reset
        movne   pc, lr
        stmfd   sp!, {r0-r3, r12, lr}
        ldr     r12, [r12]
        teq     r1, #0x40
        bne     svc_wimp
        bl      declare
        ldmfd   sp!, {r0-r3, r12, pc}
svc_wimp:
        teq     r1, #0x49
        bne     svc_other
        ldr     r0, [r12, #WS_TASK]
        teq     r0, #0
        ldmfdne sp!, {r0-r3, r12, pc}
        mvn     r0, #0
        str     r0, [r12, #WS_TASK]             @ in avvio
        ldmfd   sp!, {r0-r3, r12, lr}
        adr     r0, desktop_cmd
        mov     r1, #0                          @ servizio reclamato
        mov     pc, lr
svc_other:
        ldr     r0, [r12, #WS_TASK]
        teq     r1, #0x27
        moveq   r0, #0                          @ reset: il task non c'e' piu'
        cmn     r0, #1
        moveq   r0, #0                          @ StartedWimp: non e' partito
        str     r0, [r12, #WS_TASK]
        ldmfd   sp!, {r0-r3, r12, pc}
desktop_cmd: .asciz "Desktop_HostFS"
        .align  2

@ ------------------------------------------------------- task del desktop
task_start:
        ldr     r12, [r12]
        ldr     r0, [r12, #WS_TASK]
        cmp     r0, #0
        cmpne   r0, #-1
        svcne   #OS_Exit                         @ gia' attivo
        mov     r0, #200
        ldr     r1, task_word
        adr     r2, taskname
        svc     #XWimp_Initialise
        svcvs   #OS_Exit
        str     r1, [r12, #WS_TASK]

        @ icona sulla barra, a sinistra con i dischi: sprite sopra, testo sotto
        add     r1, r12, #WS_POLL
        mvn     r0, #1
        str     r0, [r1, #0]                    @ -2 = barra delle icone, lato sinistro
        mov     r0, #0
        str     r0, [r1, #4]
        mvn     r0, #15
        str     r0, [r1, #8]                    @ y0 = -16
        mov     r0, #96
        str     r0, [r1, #12]
        mov     r0, #84
        str     r0, [r1, #16]
        ldr     r0, icon_flags
        str     r0, [r1, #20]
        adr     r0, icontext
        str     r0, [r1, #24]
        adr     r0, sprite
        str     r0, [r1, #28]
        mov     r0, #7
        str     r0, [r1, #32]
        svc     #XWimp_CreateIcon

poll:
        mov     r0, #1                          @ niente eventi nulli
        add     r1, r12, #WS_POLL
        svc     #XWimp_Poll
        bvs     poll
        teq     r0, #6                          @ Mouse_Click
        beq     click
        teq     r0, #17                         @ User_Message
        teqne   r0, #18                         @ User_Message_Recorded
        bne     poll
        ldr     r0, [r1, #16]
        teq     r0, #0                          @ Message_Quit
        bne     poll
quit:
        ldr     r0, [r12, #WS_TASK]
        ldr     r1, task_word
        svc     #XWimp_CloseDown
        mov     r0, #0
        str     r0, [r12, #WS_TASK]
        svc     #OS_Exit

click:
        ldr     r0, [r1, #8]                    @ tasti
        tst     r0, #5                          @ Select o Adjust
        beq     poll
        adr     r0, opendir
        svc     #XOS_CLI
        b       poll

icon_flags: .word 0x1700310B                    @ testo e sprite, indiretta, clic
sprite: .asciz  "Sharddisc"
icontext: .asciz "HostFS"
opendir: .asciz "Filer_OpenDir HostFS:$"
        .align  2

@ ------------------------------------------------- filing system per FileSwitch
fs_info:
        .word   fsname                          @ nome
        .word   banner                          @ testo d'avvio
        .word   fs_open
        .word   fs_getbytes
        .word   fs_putbytes
        .word   fs_args
        .word   fs_close
        .word   fs_file
        .word   0x00000099                      @ parola d'informazione: numero &99
        .word   fs_func
        .word   0                               @ niente GBPB: i file sono bufferizzati

fs_open:     svc #TRAP + 0
             mov pc, lr
fs_getbytes: svc #TRAP + 1
             mov pc, lr
fs_putbytes: svc #TRAP + 2
             mov pc, lr
fs_args:     svc #TRAP + 3
             mov pc, lr
fs_close:    svc #TRAP + 4
             mov pc, lr
fs_file:     svc #TRAP + 5
             mov pc, lr
fs_func:     teq r0, #10                        @ Func 10: avvio del filing system
             beq fs_boot
             svc #TRAP + 6
             mov pc, lr

@ con *Configure FileSystem HostFS e *Configure Boot, FileSwitch chiama
@ Func 10 all'accensione: come ADFS, si esegue !Boot se c'e' (sulla cartella
@ dell'host ne mettiamo uno che carica l'MDF dell'emulatore sulla 3.5-3.7)
fs_boot:
        stmfd   sp!, {r0-r5, lr}
        mov     r0, #17                         @ c'e' HostFS:$.!Boot?
        adr     r1, boot_name
        svc     #XOS_File
        bvs     boot_none
        teq     r0, #0
        beq     boot_none
        adr     r0, boot_cmd
        svc     #XOS_CLI
        addvs   sp, sp, #4                      @ errore di !Boot: torna con R0 -> blocco
        ldmfdvs sp!, {r1-r5, pc}
boot_none:
        ldmfd   sp!, {r0-r5, lr}
        cmn     r0, #0                          @ V spento
        mov     pc, lr
boot_name: .asciz "HostFS:$.!Boot"
boot_cmd:  .asciz "Run HostFS:$.!Boot"
        .align  2
