# La FPA10 (coprocessore in virgola mobile)

Il chip di ARM e GEC Plessey (1993, 99 sterline) che Acorn vendeva per gli
Archimedes con ARM3: A540, A5000, R260. Si monta nella finestra di avvio
(casella "FPA10 floating point unit", solo per l'Archimedes) o con `--fpa`
(`archie`, `archie_boot`). Sul Risc PC non esisteva: ARM610, ARM710 e
StrongARM non hanno l'interfaccia per il coprocessore. La FPA11 e' dentro
l'ARM7500FE dell'A7000+ (prossimo passo: la macchina A7000+).

## Fonti

- Data sheet GEC Plessey della FPA10 (scansione su chrisacorns.computinghistory.org.uk):
  istruzioni eseguite in hardware, FPSR, FPCR, tempi.
- Sorgenti ARM del FPEmulator e del codice di supporto (FPASC), licenza BSD,
  pubblicati da RISC OS Open (gitlab.riscosopen.org/RiscOS/Sources/HWSupport/FPASC):
  formato interno, conversioni di LDF/LFM, flag dei confronti, come il
  FPEmulator riconosce il chip e come gestisce i rimbalzi.
- Comunicato Acorn: "floating point performance increasing by as much as a factor of 50".

## Come funziona (src/cpu/fpa.c)

- Registri F0-F7 nel formato interno a 96 bit (segno, uncommon, esponente a
  15 bit con eccesso 16383; unita' esplicita e 63 bit di frazione), FPSR con
  l'identificativo 0x81 (sola lettura), FPCR.
- In hardware: LDF/STF S D E, LFM/SFM, ADF SUF RSF MUF DVF RDF, MVF MNF ABS,
  FLT FIX, CMF CNF CMFE CNFE, WFS RFS WFC RFC. Arrotondamento IEEE esatto
  (mantisse a 64 bit, conti a 128 bit) nelle precisioni S D E e nei quattro modi.
- Al codice di supporto ("rimbalzo"): formato decimale (P), SQT RND RMF POW
  RPW POL, le trascendenti, e qui anche FML FDV FRD URD NRM; e ogni istruzione
  con operandi speciali (infiniti, NaN, denormali) o con un'eccezione
  (operazione non valida, divisione per zero, overflow, underflow, inesatto
  con la trappola abilitata).
- I rimbalzi sono tutti sincroni (bit SB del FPCR): il gestore del FPEmulator
  emula da capo l'istruzione, leggendo e scrivendo i registri del chip. Il
  chip vero rimbalza le eccezioni aritmetiche in modo asincrono (AB, con
  l'istruzione nel FPCR) salvo nel modo SO; per i programmi non cambia niente.
- DA (chip disattivato): tutto rimbalza tranne RFC e WFC. Al reset DA, AB e
  SB sono a 1; RFC li azzera; WFC scrive solo quelli. RFC e WFC in modo
  utente rimbalzano. Cosi' il FPEmulator 4 riconosce il chip (un RFC che non
  rimbalza, poi RFS = 0x81) e lo spegne finche' un programma non lo usa.
- Tempi del data sheet (cicli in cui la CPU aspetta): ADF 2, MUF 8, DVF 30/58/70
  (S/D/E), FLT 6, FIX 8, confronti 5, LDF 2/3/4, LFM 4/7/10/13.

## Il software

- RISC OS 3.11 ha il FPEmulator 2.87, che non conosce la FPA: come sul vero
  A5000 serve il FPEmulator 4 caricato da disco (`FPE400`, versione 4.03
  del 4 gennaio 1994, nell'archivio HENSA `MATHS/FPE400.ARC`).
  Il 2.87 accende comunque il chip (fa un RFC), e allora le istruzioni che
  rimbalzano (SQT, SIN...) danno risultati sbagliati: e' il comportamento
  del vero hardware senza il modulo nuovo.
- RISC OS 3.5 (FPEmulator 4.04) e 3.7 (4.08) hanno gia' il codice di supporto.

## Prove

- `test_fpa`: 20000 operazioni casuali per ADF SUF MUF DVF in doppia e
  singola precisione confrontate bit per bit con l'IEEE del PC, FLT/FIX nei
  quattro modi, confronti, estesa, LDF dei valori speciali, rimbalzi, DA.
- RISC OS 3.11 con `archie_boot --fpa --hostfs dir`, `RMLoad hostfs:$.FPE400`
  e un programma BASIC con le istruzioni FPA scritte con EQUD (l'assemblatore
  del BASIC V non le conosce). 20480 giri di MUFD ADFD DVFD SUFD:

  | configurazione              | FPSR      | tempo   |
  |-----------------------------|-----------|---------|
  | FPA10 + FPE400              | &81070000 | 25 cs   |
  | FPE400 senza chip           | &01070000 | 357 cs  |
  | FPEmulator 2.87 della ROM   | &00070000 | 1293 cs |

  SQTD e SIND (rimbalzati) danno gli stessi risultati del software, e la
  divisione per zero da' "Floating point exception: division by zero".

## Sul core ARMv3 (verso l'A7000+)

La FPA si aggancia anche ad `arm6.c` (FPA11, per l'ARM7500FE). Prima della
macchina A7000+, `riscpc_boot --fpa` la monta sul Risc PC solo per provarla
(storicamente non esisteva): RISC OS 3.71 (FPEmulator 4.08) la riconosce da
solo, FPSR &81070000; il programma di prova fa 4 cs invece di 55 sull'ARM710,
e SQT, SIN e la divisione per zero (rimbalzati, gestore a 32 bit) danno gli
stessi risultati del software.
