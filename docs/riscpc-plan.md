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
| `3. Uncommon/ROM380.ARM6ARM7` | 4 MB | 3.80 (Phoebe) per ARM6/7 |
| `3. Uncommon/ROM380.SA` | 4 MB | 3.80 per StrongARM |
| `2. NCOS/ROM361` | | 3.61 (NC) |

RISC OS 3.7 (StrongARM) non c'e': per la fase StrongARM serve un dump.

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

- Il core in `src/cpu` diventa configurabile: ARMv2a (Archimedes) o ARMv3
  (ARM610/710), poi ARMv4. Bus a 32 bit con il segnale dei modi a 26 bit.
- Nuova cartella `src/riscpc` (IOMD, VIDC20, 82C711, IDE, MMU di CP15 se non
  sta nel core), con `riscpc_boot` come `archie_boot` per la diagnosi.
- Frontend: una scelta della macchina (Archimedes / Risc PC) o un eseguibile
  separato; da decidere quando si arriva al desktop.

## Passi

1. **Core ARMv3**: modi a 32 bit, CPSR/SPSR, MRS/MSR, eccezioni nei due
   modi, CP15 e MMU. Test unitari e confronto con Unicorn nei modi a 32 bit
   (ALU, load/store, eccezioni); i test dell'ARM2 devono restare verdi.
2. **Fino allo schermo di avvio**: mappa della memoria, IOMD (interrupt,
   timer, memoria), VIDC20 essenziale. `riscpc_boot` con il rapporto del POST
   e la PNG dello schermo, come per l'Archimedes; traccia degli accessi I/O
   sconosciuti per guidare il lavoro sulla ROM350.
3. **Tastiera, mouse, CMOS**: PS/2 tramite l'IOMD, mouse a quadratura,
   PCF8583 sulle linee I2C. Obiettivo: il desktop di RISC OS 3.5.
4. **Floppy**: 82C711/82077 con il suo DMA o FIQ; le immagini ADF esistenti.
5. **IDE**: immagini di hard disc (`.hdf`), ADFS su IDE, formattazione con
   HForm.
6. **VIDC20 completo e suono**: modi alti a 16/32 bpp, cursore hardware,
   suono; poi l'integrazione nei frontend Win32 e Mac.
7. **ARM710**, poi StrongARM e RISC OS 3.7 o 3.8.

Ogni passo si chiude con i test e con la ROM che arriva un po' piu' avanti
nell'avvio; i tempi (cicli, cache, banda della DRAM e della VRAM) si curano
come per l'Archimedes dopo che la macchina funziona.
