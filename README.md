# ARM — emulatore modulare ARMv2 (stile Acorn Archimedes)

Emulatore in C99, costruito a moduli che si parlano solo attraverso interfacce:

```
 ┌────────────┐   ArmBus    ┌───────────┐   pagine 4 KB   ┌──────────────┐
 │  CPU ARMv2 │────────────▶│    Bus    │────────────────▶│ RAM / ROM    │
 │  src/cpu   │             │ src/core  │────────────────▶│ dispositivi  │
 └─────┬──────┘             └───────────┘                 └──────────────┘
       │ gancio SWI
 ┌─────▼──────────────┐
 │ HLE RISC OS (SWI)  │  OS_WriteC, OS_Write0, OS_NewLine, OS_ReadC, OS_Exit...
 │ src/hle            │
 └────────────────────┘
```

- **`src/cpu/arm2.c`**: ARMv2a (ARM2 + SWP). R15 contiene insieme PC e PSR a 26 bit,
  con i banchi FIQ/IRQ/SVC, tutte le eccezioni (anche address exception e abort),
  TEQP e LDM `^`, e i cicli S/N/I stimati.
- **`src/cpu/arm2_disasm.c`**: disassemblatore con la sintassi del BBC BASIC.
- **`src/core/bus.c`**: spazio di 64 MB, memoria diretta o callback dei dispositivi.
- **`src/hle/riscos_swi.c`**: le SWI di RISC OS eseguite dall'host.

## Compilare e provare

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\test_arm2.exe                       # test specifici dei 26 bit
.venv\Scripts\python tests\diff_unicorn.py        # confronto con Unicorn
.venv\Scripts\python tools\asm.py demos\primes.s build\demos\primes.bin
build\Release\armemu.exe -s build\demos\primes.bin
```

`armemu` carica il binario a &8000 e lo esegue in modo utente. Le opzioni sono
`-t` (traccia disassemblata), `-s` (statistiche), `-a` (indirizzo di caricamento),
`-m` (MB di RAM) e `-c` (limite di cicli).

L'ambiente `.venv` contiene Unicorn, Keystone e Capstone. Si ricrea con
`python -m venv .venv` seguito da `.venv\Scripts\pip install unicorn keystone-engine capstone`.

## Verifica

- `test_arm2`: 111 controlli sui comportamenti propri dell'ARMv2: R15 come Rn
  (solo PC) e come Rm (PC+PSR), +12 con lo shift da registro e in STR/STM,
  banchi, eccezioni e ritorni, IRQ, protezione del PSR in modo utente, LDR non
  allineata, write-back della base in STM, abort e disassemblatore.
- `diff_unicorn.py`: elaborazione dati e moltiplicazione confrontate bit per bit
  con Unicorn (QEMU) su 1,2 milioni di istruzioni casuali, con zero differenze.

## BBC BASIC V

`third_party/riscos/BASIC` è il BBC BASIC V 1.87 originale di RISC OS Open
(Apache 2.0), compilato per RISC OS 3.10: gira sulla nostra CPU ARMv2 come
modulo "ROM". Il kernel RISC OS non è emulato istruzione per istruzione:
le SWI che il BASIC chiama sono eseguite in C (`src/riscos/kernel.c`), e
l'uscita passa da un driver VDU (`src/riscos/vdu.c`) che disegna nel
framebuffer a &2000000, nel formato nativo del modo (fino a 32 bpp truecolor).

```
build\Release\armwin.exe                  # finestra: clock ARM2 a 8 MHz, F12 = turbo
build\Release\armwin.exe --mhz 25         # un ARM3 a 25 MHz
build\Release\armbasic.exe < listato.txt  # console, per i test
build\Release\test_basic.exe              # test della macchina completa
```

Nella finestra: Escape interrompe il programma, Ctrl+V incolla un listato,
F12 alterna il clock limitato e il turbo. Per il truecolor:
`MODE "X640 Y480 C16M"` oppure `MODE 49`, poi `COLOUR r,g,b` e `GCOL r,g,b`.

### File

Il disco è la cartella `disc` del progetto (opzione `--disc` per cambiarla).
Come in RPCEmu/HostFS, il tipo RISC OS sta nel suffisso: `SAVE "mandel"`
crea `disc\mandel,ffb`. Il `.` di RISC OS separa le cartelle e `/` fa da
estensione: `mandelbrot/bas` è `mandelbrot.bas` su Windows.

- `SAVE`, `LOAD`, `CHAIN`: programmi tokenizzati;
- `TEXTLOAD`, `TEXTSAVE`: listati di testo, apribili con il Blocco note;
- `OPENIN`/`OPENOUT`/`OPENUP`, `PRINT#`, `INPUT#`, `BGET#`, `BPUT#`, `PTR#`, `EXT#`, `EOF#`;
- `*CAT` (o `*.`), `*EX`, `*DELETE`, `*RENAME`, `*CDIR`, `*TYPE`.

## Macchina Archimedes (ROM originali)

`archie.exe` è un Archimedes A3000/A310 emulato a basso livello: la CPU esegue
la ROM vera di RISC OS e parla con i chip, come sulla macchina reale.

| Modulo | File | Cosa fa |
|---|---|---|
| MEMC1a | `src/archie/memc.c` | traduzione a pagine (CAM), protezioni PPL, ROM a 0 al reset, DMA |
| IOC | `src/archie/ioc.c` | IRQ/FIQ, timer a 2 MHz, seriale KART, pin I2C |
| VIDC1a | `src/archie/vidc.c` | palette, modi da 1 a 8 bpp, cursore hardware |
| Tastiera | `src/archie/kbd.c` | protocollo del micro della tastiera, mouse |
| CMOS | `src/archie/cmos.c` | PCF8583 su I2C, orologio, somma di controllo di RISC OS |
| Floppy | `src/archie/fdc.c` | WD1772 con immagini ADFS `.adf` |
| Macchina | `src/archie/archie.c` | mappa dell'I/O dell'A310, tempo a 24 MHz, eventi |

Le ROM vanno in `roms\` (non sono incluse: RISC OS 3.1 non è open source).
`archie.exe` cerca `roms\1. Major\ROM311`; la CMOS si salva accanto alla ROM
(`ROM311.cmos`). Il POST di RISC OS 3 passa tutti i test. Per seguire il boot:

```
build\Release\archie_boot.exe --rom "roms\1. Major\ROM311" --ms 20000 --png schermo.png
```

Stampa il rapporto del POST, le eccezioni, lo stato dei chip e il codice intorno al PC.

### Tempi, suono e tastiera

- **Tempi fedeli dell'ARM2**: ogni istruzione conta i cicli S, N e I del
  datasheet. Sul MEMC a 8 MHz un ciclo N vale 2 tick, i fetch dalla ROM costano
  quanto il tempo d'accesso programmato da RISC OS (325 ns), e il DMA video si
  prende la sua quota di banda (16% nei modi da 80 KB). La macchina BASIC usa
  gli stessi costi, più il lavoro del driver VDU tarato confrontando gli stessi
  programmi con RISC OS 3.11 emulato: vedi le costanti COST_* in `src/riscos/vdu.c`.
- **Suono** (`archie.exe`): il DMA del MEMC porta al VIDC i byte in formato
  logaritmico, uno ogni SFR+2 µs, sugli 8 canali stereo; si ricampiona a 48 kHz.
  Ctrl+F10 spegne e riaccende l'audio.
- **Tastiera** (`src/frontend/archie_keys.c`): lettere, cifre e tasti di
  controllo per posizione; i simboli seguono il carattere della disposizione di
  Windows (RISC OS 3.11 non ha una disposizione spagnola), le lettere accentate
  si compongono con Alt + tastierino. Gli eventi tradotti escono uno ogni 40 ms,
  perché RISC OS campiona la tastiera a ogni centesimo. `test_keys_es` digita con
  la disposizione spagnola vera di Windows e rilegge lo schermo con il font della ROM.
- **Pipeline dell'ARM2**: le due istruzioni successive sono già lette quando si
  esegue quella corrente; il codice che le riscrive non ne vede l'effetto (la
  protezione anticopia di Elite ci conta).

Scorciatoie di `archie.exe`: **Ctrl+F9** sceglie il dischetto (con Shift l'unità 1),
**Ctrl+F8** lo espelle, trascinare un `.adf` sulla finestra lo inserisce; clic nella
finestra cattura il mouse, **Ctrl+F11** lo libera; **Ctrl+F12** turbo,
**Ctrl+Shift+F12** reset, **Ctrl+F10** audio.

Giochi provati: Zarch (Play It Again Sam 2), Pacmania, Elite (va lanciato con un
doppio clic dal desktop: dalla riga di comando F12 si ferma con "Wimp is
currently active").

Le unità floppy sono due (`--floppy` e `--floppy2`; la CMOS ne configura sempre
almeno due). Genesis Professional 3.04 si avvia con il disco 1 in :0 e il disco 2
(che contiene `!GenLib`) in :1: si aprono le finestre di :0 e :1 (così il Filer
"vede" `!System`, `!Scrap` e `!GenLib`) e poi doppio clic su `!Genesis`.

## Prossimi passi

1. Tastiera completa (INKEY negativi, tasti funzione) e suono.
2. ROM intercambiabili (`--rom`): BASIC oggi, poi Forth e altri linguaggi.
3. Più avanti: CPU ARMv3/v4 a 32 bit e hardware del RiscPC per RISC OS 5.
