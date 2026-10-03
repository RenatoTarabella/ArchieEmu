# Risc PC e RISC OS 3.5: piano

Obiettivo: una terza macchina accanto alla BBC BASIC e all'Archimedes, il
**Risc PC** (1994), che fa girare le ROM originali di RISC OS 3.5, 3.6 e 3.7 con
i processori ARM610 e ARM710 (ARMv3); dopo, lo StrongARM (ARMv4).

Non e' un aggiornamento dell'Archimedes: cambiano quasi tutti i chip. Si
riusano il core ARM (esteso), CMOS, podule bus (quindi HostFS), i frontend
Win32/Mac, gli strumenti di test.

## ROM disponibili (in `roms/`, mai da committare)

| File | Dimensione | Note |
|---|---|---|
| `1. Major/ROM350` | 2 MB | RISC OS 3.50, primo Risc PC (ARM610) |
| `1. Major/ROM360` | 4 MB | RISC OS 3.60 |
| `1. Major/ROM370` | 4 MB | RISC OS 3.70 (StrongARM) |
| `1. Major/ROM371` | 4 MB | RISC OS 3.71 |
| `3. Uncommon/ROM380.ARM6ARM7` | 4 MB | 3.80 (Phoebe) per ARM6/7 |
| `3. Uncommon/ROM380.SA` | 4 MB | 3.80 per StrongARM |
| `2. NCOS/ROM361` | | 3.61 (NC) |

Per la fase StrongARM ci sono gia' la 3.70 e la 3.71.

## L'hardware (da verificare sui datasheet durante il lavoro)

- **CPU** su scheda: ARM610 a 30 MHz (cache 4 KB) o ARM710 a 40 MHz (8 KB),
  piu' tardi StrongARM SA-110. ARMv3: spazio d'indirizzi a 32 bit, modi a
  32 bit (USR32, FIQ32, IRQ32, SVC32, ABT32, UND32) con CPSR e SPSR,
  istruzioni MRS/MSR; restano i modi a 26 bit, e RISC OS 3.5-3.7 ci gira quasi
  tutto (bit P/D del registro di controllo di CP15 da capire). Niente piu'
  address exception. Coprocessore 15: ID, controllo (MMU, cache, write buffer,
  P, D, endianness), base della tabella delle pagine, domini, stato e
  indirizzo del fault, flush di cache e TLB. MMU: sezioni da 1 MB, pagine da
  64 KB e 4 KB, permessi AP con i 16 domini.
- **IOMD** (sostituisce IOC e MEMC): controller della DRAM (2 slot SIMM) e
  della VRAM, interrupt (registri come l'IOC piu' banchi nuovi), timer 0/1,
  DMA per video, cursore e suono, interfaccia PS/2 della tastiera, contatori
  del mouse a quadratura, linee I2C per la CMOS, ID del chip.
- **VIDC20**: palette a 24 bit da 256 voci, modi a 1-32 bpp, timing piu'
  flessibili, cursore hardware a 32 pixel, suono a 8 bit logaritmico (e il
  percorso a 16 bit verso il DAC esterno).
- **Super I/O 82C711** (o SMC 37C665): floppy con controller di tipo PC
  (82077, non piu' WD1772), seriale, parallela, decodifica dell'IDE.
- **IDE** per il disco fisso: finalmente le immagini di hard disc.
- Mappa fisica (indicativa): ROM da 0, I/O da &03000000 (IOMD, VIDC20,
  periferiche), VRAM da &02000000, DRAM da &10000000.

Fonti: datasheet ARM610/ARM710, IOMD, VIDC20 (si trovano in rete), PRM di
RISC OS 3.5. RPCEmu e' GPL: si puo' consultare il comportamento, ma niente
codice copiato in questo progetto MIT.

## Struttura nel codice

- Il core ARMv3 e' un modulo a parte, `src/cpu/arm6.c` (`Arm6`), accanto
  all'ARM2: l'Archimedes resta sul suo core gia' calibrato. CP15 e MMU
  stanno nel core (TLB software a blocchi da 1 KB); sul bus (`ArmBus`, ora
  in `armbus.h`) arrivano solo indirizzi fisici a 32 bit. Poi ARMv4.
- Nuova cartella `src/riscpc` (IOMD, VIDC20, 82C711, IDE), con `riscpc_boot` come `archie_boot` per la diagnosi.
- Frontend: una scelta della macchina (Archimedes / Risc PC) o un eseguibile
  separato; da decidere quando si arriva al desktop.

## Passi

1. **Core ARMv3** (fatto): modi a 26 e 32 bit, CPSR/SPSR, MRS/MSR,
   eccezioni nelle due configurazioni (bit P), address exception solo coi
   dati a 26 bit (bit D), CP15 (ID, controllo, TTB, domini, FSR/FAR, flush)
   e MMU (sezioni, pagine da 64 e 4 KB con sottopagine, domini, AP con S e
   R, allineamento). `tests/test_arm6.c` (116 controlli) e
   `tests/diff_unicorn_arm6.py` (Unicorn come SA1100, 0 differenze su
   300 000 istruzioni nei sei modi a 32 bit). Scelte da verificare sulla
   ROM: il modello degli abort (ARM610 base ripristinata su LDR/STR, ARM710
   con L aggiornata; LDM/STM sempre aggiornata), TSTP & co. nei modi a 32
   bit (copiano l'SPSR), STR/STM di PC = +12. La cache non e' emulata (e'
   write-through: conta solo per i tempi).
2. **Fino allo schermo di avvio** (fatto, si arriva al desktop):
   `src/riscpc` (iomd.c, vidc20.c, riscpc.c) e `tools/riscpc_boot`. La
   ROM350 passa il POST e porta il desktop a 640x480 a 16 colori in ~8 s
   emulati, con 1 o 2 MB di VRAM o senza; anche con l'ARM710 e con le ROM
   3.60, 3.70 e 3.71 (la 3.71 chiede un disco: "Disc drive not known").
   Cose imparate sulla ROM:
   - il POST spegne il bit P in SVC32 e poi usa TEQP: senza P la CPU
     ricade nel modo a 26 bit corrispondente;
   - il POST misura il DMA del suono: 1024 byte con SFR = 8 devono durare
     10,03-10,29 ms sul timer 1 (un byte ogni SFR + 2 us). Stato del
     canale: bit 0 il buffer su cui lavora il DMA, la CPU riempie l'altro;
   - la VRAM si misura col bus a 64 bit (VREFCR &41): con 1 MB la parola a
     +4 ricade su +0, allora si riprova a 32 bit (&21);
   - tasti del mouse a &03310000 (bit 4-6, attivi bassi); le schede assenti
     (&033C0000 e lo spazio EASI) leggono &FF;
   - il risultato del POST: bordo verde o rosso e codice lampeggiato sul
     LED del floppy (porta &3F2); lo stato e' in R12 del banco FIQ.
   Strumenti di `riscpc_boot`: `--trace-io`, `--watch-io lo hi`,
   `--trace-modes`, `--watch-low`, `--break pc`, `--ring N`, `--hist`.
   Ancora da fare qui: tempi del frame dal VIDC20 (ora 50 Hz fissi),
   cursore hardware, il Super I/O (RISC OS lo configura a &03010FC0).
3. **Tastiera, mouse, CMOS** (fatto, da riga di comando): tastiera PS/2
   (`ps2kbd.c`, set 2, risponde a reset, LED, ripetizione) sul canale
   dell'IOMD (KBDDAT/KBDCR, IRQB bit 6/7; il driver controlla la parita'
   nel bit 2 di KBDCR e senza chiede di rispedire); mouse a quadratura
   (MOUSEX/MOUSEY, RISC OS ne legge la differenza; passo x1,5) e tasti a
   &03310000; cursore hardware del VIDC20 (HCSR = x + HDSR - 20); la CMOS
   PCF8583 riusata dall'Archimedes, `*Configure` resta dopo il riavvio.
   Lo schermo si disegna come lo legge il DMA: da VIDINIT, e a fine area
   si torna a VIDSTART; la fine e' VIDEND + l'ultimo trasferimento (&800
   o &400 dalla VRAM secondo VIDCR, 16 byte dalla DRAM), cosi' il testo
   che scorre esce giusto. `riscpc_boot --keys` scrive e muove il mouse
   (`{MOUSE dx,dy}`, `{SELECT}`, `{MENU}`, `{ADJUST}`). Senza disco di boot
   la cartella Apps resta vuota ("Resources:$.Apps not found", la
   riempiono gli `*AddApp` del boot).
4. **Floppy**: 82C711/82077 con il suo DMA o FIQ; le immagini ADF esistenti.
5. **IDE**: immagini di hard disc (`.hdf`), ADFS su IDE, formattazione con
   HForm.
6. **VIDC20 completo e suono**: modi alti a 16/32 bpp, cursore hardware,
   suono; poi l'integrazione nei frontend Win32 e Mac, con una finestra
   iniziale (splash) che chiede prima del boot quale macchina aprire:
   Archimedes (ROM fino alla 3.11, ARM2) o Risc PC (ROM 3.5/3.6/3.7,
   ARM610 o ARM710, RAM e VRAM).
7. **ARM710**, poi StrongARM e RISC OS 3.7 o 3.8.

Ogni passo si chiude con i test e con la ROM che arriva un po' piu' avanti
nell'avvio; i tempi (cicli, cache, banda della DRAM e della VRAM) si curano
come per l'Archimedes dopo che la macchina funziona.
