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
  - `robot.dat`: la scena del Juggler, letta dall'eseguibile `ssg` (senza
    sorgente). `robot.rgb`: 320x200 RGB (192000 byte), l'immagine originale.
    Anche `dragon.dat`/`ele.dat` con le loro immagini.
- Licenza: Eric Graham, 10/2/2026: "anyone can do what they want with the code,
  so long as I get a mention!". Citarlo sempre (programma, README, post).

## Formato di robot.dat (da verificare con il riferimento)

```
(-10,-4,5.5)        posizione dell'osservatore
[-10,20]            altezza e azimut della vista in gradi (come rt2.c)
35                  focale (rt2.c: fl = 0.028*30 -> qui 0.028*35?)
<r,g,b> tipo (x,y,z):raggio;                 sfera (tipo 0 DULL, 1 BRIGHT, 2 MIRROR)
<r,g,b> tipo (x,y,z):r  n (x,y,z):r  m ...;  catena: n sfere interpolate
                                             (posizione e raggio) fino al punto
;;                  fine delle sfere
1                   numero di lampade
(x,y,z):raggio <r,g,b>
<scacchi 1> <scacchi 2> <illum> <cielo ?> <cielo ?>   ordine di struct world:
                    horizon[0], horizon[1], illum, skyhor, skyzen (da verificare)
```
Punti aperti: conteggio delle sfere nelle catene (n incluso o escluso il punto
finale), significato esatto di 35, ordine dei colori del cielo, lampfac.

## Errore nel sorgente archiviato

In `reflect()` di rt1.c: `y[k]=xv*v[k]/(xn*n[k]);` e' matematicamente sbagliato;
la riflessione corretta e' `y[k]=xv*v[k]-xn*n[k];` (si inverte la componente
normale). Probabile errore di trascrizione del listato. Nel programma: opzione
on/off ("come archiviato" / "corretto"); il confronto con robot.rgb dira' quale
usava il Juggler.

## Passi

1. **Riferimento sul PC** (`tools/juggler_ref.py` o C): rt1.c tradotto fedelmente
   in double, lettore di robot.dat, uscita PNG 320x200; confronto pixel per pixel
   con robot.rgb (che ha 16 livelli per componente: confrontare dopo la stessa
   quantizzazione di ham(), `16*brite+0.5` limitato a 0..15). Chiudere i punti
   aperti del formato e la questione di reflect().
2. **Programma BBC BASIC** (`!Juggler`, su HostFS):
   - lettore di robot.dat in BASIC (anche dragon.dat, ele.dat);
   - motore "Originale": rt1.c in BASIC con i float (lento, identico);
   - motore "Veloce": raytrace() intero in assembler ARM a virgola fissa
     (intersezioni, ombre, specchi, glint), verificato contro il riferimento;
   - opzione reflect() archiviata/corretta;
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
