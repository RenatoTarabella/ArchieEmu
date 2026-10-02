# Il Juggler di Eric Graham sull'Archimedes: piano

Obiettivo: rendere sull'Archimedes (e sulla macchina BBC BASIC dell'emulatore)
la scena originale del Juggler di Eric Graham (Amiga, 1986-87) con il suo
algoritmo, per vedere la resa reale, prima un fotogramma e poi l'animazione.

## Materiale

- Repository clonato in `C:\Users\renat\source\repos\Juggler-1987`
  (github.com/AlphaPixel/Eric-Graham-1987-Juggler-Raytracer-1.0), cartella
  `Raytracer_1987_Graham_Source_Code`:
  - `rt1.c`: il motore (sfere, lampade, pavimento a scacchi, cielo sfumato;
    superfici DULL/BRIGHT/MIRROR; ombre; specchi ricorsivi; glint).
  - `rt2.c`: setup di esempio (telecamera da posizione, altezza e azimut in
    gradi, focale `0.028*30`; 320x200 con `skip=2`; calcolo di `lampfac`).
  - `rt3.c`: uscita HAM dell'Amiga (non serve, solo come riferimento).
  - `robot.dat`: la scena del Juggler, letta dall'eseguibile `ssg`.
    `robot.rgb`: 320x200 RGB (192000 byte), il dump del binario originale.
    Anche `dragon.dat`/`ele.dat` con i loro dump.
  - `ssg-src-authentic/` (nella radice del repo): sorgente di `ssg`
    ricostruito dal binario.
- Licenza: Eric Graham, 10/2/2026: "anyone can do what they want with the code,
  so long as I get a mention!". Citarlo sempre (programma, README, post).

## Passo 1 fatto: riferimento sul PC

`tools/juggler_ref.c` (eseguibili `juggler_ref` in double come rt1.c e
`juggler_ref_f` in float come ssg): rt1.c tradotto fedelmente, lettore dei
`.dat`, lampfac di rt2.c, uscita PNG e dump `.rgb`, confronto con `--cmp`.

```
build\Release\juggler_ref ..\Juggler-1987\Raytracer_1987_Graham_Source_Code
obot.dat
    --png robot.png --cmp ...
obot.rgb --diff diff.png [--skip 4] [--reflect archived]
```

Il repository Juggler contiene anche `ssg-src-authentic/`, una ricostruzione
del sorgente di `ssg` dal binario (Ghidra), e i dump del binario originale
eseguito sotto vamos: `robot.rgb`, `ele.rgb` (320x200) e `*-s2*.rgb` (80x50,
`S=2` di ssg = un pixel ogni 4, cioe' `--skip 4`). Da li' e dal confronto:

- **Formato dei .dat** (chiuso, vedi `datformat-README.md` nel repo Juggler):
  `(osservatore) [altezza,azimut] focale`, poi gli oggetti
  `<r,g,b> tipo (x,y,z):r [n (x,y,z):r]... ;` e un `;` che chiude la lista;
  `numero_lampade`, `(x,y,z):r <r,g,b>` per lampada; infine
  `<scacchi 0> <scacchi 1> <illum> <cielo zenit> <cielo orizzonte>`.
  Focale = `0.028*valore` (35 per robot). Catena con conteggio n: n+1 sfere
  da `t=0` (punto precedente incluso) a `t=n/(n+1)`, il punto finale si
  aggiunge a fine oggetto. robot 79 sfere, ele 120, dragon 288.
- **Il dump `.rgb` non e' a 16 livelli**: ogni byte e' `(int)(128*brite+4)`
  limitato a 0..255; i 4 bit dell'Amiga sono `byte/8` (= `16*brite+0.5` di
  ham() in rt2.c, con un piccolo dither prima). A schermo: `byte*2`.
- **reflect()**: il Juggler usava quella corretta `y = x - 2(x.n)n`.
  Risultato su robot.rgb: corretta 252 byte diversi su 192000 (214 pixel),
  archiviata 8302 byte (3501 pixel: le sfere a specchio vengono nere con
  macchie). Opzione on/off nel programma: si', ma "corretta" come default.
- Le differenze residue (float o double danno lo stesso ordine) sono la riga
  dell'orizzonte (scacchi lontanissimi, `(int)x` sensibile alla precisione) e
  qualche pixel ai bordi di ombre e glint: l'originale usava il formato FFP
  Motorola (mantissa 24 bit), non riproducibile esattamente in IEEE. Campioni
  80x50: robot 8 byte, ele 2, dragon 5 su 12000.
- Il programma originale non e' a forza bruta: per ogni sfera calcola il
  rettangolo a schermo e per ogni riga tiene la lista delle sfere attive
  (`project()`/`actsp()` in ssg-src-authentic); stesso risultato, molto piu'
  veloce. Da copiare per l'ARM. A forza bruta robot fa 9,2 milioni di test
  raggio-sfera per fotogramma 320x200 (ele 11,0, dragon 28,8).
- Scena con 1 lampada; profondita' massima degli specchi in robot: 3.

## Passi

1. ~~**Riferimento sul PC**~~: fatto, vedi sopra.
2. **Programma BBC BASIC** (`!Juggler`, su HostFS):
   - lettore di robot.dat in BASIC (anche dragon.dat, ele.dat);
   - motore "Originale": rt1.c in BASIC con i float (lento, identico);
   - motore "Veloce": raytrace() intero in assembler ARM a virgola fissa
     (intersezioni, ombre, specchi, glint), verificato contro il riferimento;
   - opzione reflect() archiviata/corretta (corretta di default: e' quella
     del Juggler);
   - liste delle sfere attive per riga, come ssg;
   - modi grafici selezionabili: 320x200 come l'Amiga; sulla macchina BASIC
     truecolor 640x480; sull'Archimedes MODE 13, 15, 28 a 256 colori con
     palette ottimizzata (16 registri base del VIDC1) e dithering
     Floyd-Steinberg; tempo di rendering a 8 MHz misurato (--stats).
3. **Animazione**: i fotogrammi del Juggler (palloni in volo, braccia), poi
   riproduzione come la demo originale da 24 fotogrammi.
4. **Dopo**: palette diversa per ogni riga (FIQ che riscrive i 16 registri del
   VIDC a ogni riga, il "copper" dell'Archimedes). Serve prima nell'emulatore la
   registrazione della palette riga per riga in vidc.c (oggi il fotogramma usa la
   palette di fine frame), utile anche per le demo dell'epoca.

## Note sull'hardware

- ARM2 8 MHz senza FPU: i float del BASIC e le istruzioni FP passano dal
  FPEmulator (lento); la virgola fissa richiede attenzione ai bordi delle sfere
  (vedi CLAUDE.md, lezione sul ray tracer: raffinamento esatto ai bordi).
- VIDC1 a 256 colori: 4 bit bassi = uno dei 16 registri (4096 colori), 4 bit alti
  sostituiscono i bit alti di R, G, B.
- L'originale: Amiga HAM 320x200, 4096 colori, ~1 ora per fotogramma (da verificare).
