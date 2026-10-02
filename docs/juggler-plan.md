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

## Passo 2 fatto: programma BBC BASIC

`demos/juggler.bas` (tokenizzato in `disc/Juggler` con `tools/make_juggler.sh`,
scene in `disc/Scenes`). Gira sulla macchina BASIC e sull'Archimedes con
RISC OS 3.11 (BASIC 1.05), da HostFS: `*DIR HostFS:$` e `CHAIN "Juggler"`.

- Motore 1 "originale": rt1.c in BASIC, con le liste per riga di ssg.
  Identico al riferimento (2 byte di 1 a passo 8). Robot a passo 4: 796 s.
- Motore 2 "veloce": raytrace() intero in assembler a virgola fissa 16.16
  (FNmul spezza la moltiplicazione, fdiv e fsqrt intere), filtro a 1/64
  che lascia solo le sfere candidate, test esatto su quelle. Robot pieno:
  143-171 s di ray tracing su ARM2 a 8 MHz (secondo la banda del video).
  Dal riferimento: 822 pixel di 1-2 livelli, ~400 sulla fascia
  dell'orizzonte (scacchi lontani), ~90 ai bordi delle riflessioni.
  Due scelte: il raggio riflesso non testa la sua sfera (in virgola fissa
  SMALL non basta a scartarla), FNmul arrotonda.
- Schermo in assembler: 32 bpp diretto; 8 bpp (MODE 13, 15, 28) con i 16
  registri del VIDC1 come bit bassi (R 3, G 2, B 3) e i bit alti nel pixel:
  palette generica durante il calcolo, poi k-means sull'istogramma a 12 bit
  e Floyd-Steinberg. Robot pieno in MODE 13 sulla macchina BASIC: 214 s in
  tutto. Sull'Archimedes, passo 4: 16,9 s (10,8 di ray tracing).
- `BASIC: IF t THEN` con t reale piccolo vale falso (troncato a 0): usare `t>0`.

## Passo 3 fatto: animazione (ricostruzione procedurale)

Le 24 scene originali sono perse (Eric le generava con un suo programma);
i fotogrammi originali esistono (`movie.data`, `media/juggler.avi`) ma
hanno una telecamera diversa da robot.dat (quasi orizzontale, in diagonale
sugli scacchi, focale ~25-28) e il robot cammina: il tentativo di
ricavare la telecamera dagli scacchi e' arrivato al 71-74% dei pixel e si
e' fermato li'. Scelta dell'utente: animazione procedurale.

- `tools/juggler_anim.py` -> `disc/Scenes/Anim/j00..j23.dat`: geometria e telecamera di
  robot.dat; cascata a 3 palloni (lancio ogni 4 fotogrammi, 8 in volo, 4
  in mano, apice z=6,8 come robot.dat); mani che seguono i palloni, gomiti
  e ginocchia in IK. Il robot sta fermo come nell'originale (il pavimento
  dei 24 fotogrammi originali e' identico): piedi di robot.dat, leggero
  molleggio. (Una prima versione lo faceva camminare: sbagliato.)
- Nel programma: scena `anim` (solo 256 colori), palette scelta sul primo
  fotogramma, fotogrammi in memoria come pixel e salvati in un file
  (header JUGA, modo, passo, larghezza, altezza, 24, palette, pixel);
  scena `play` per rivederli. Player: LDM/STM a 8 registri per MODE 13 a
  passo 1, altrimenti copia con ingrandimento. Tasti 1-9 = velocita'.
- Passo 4 sull'Archimedes: 24 fotogrammi in ~7 minuti, riproduzione ok.
  Piena risoluzione (MODE 13): 4824 s, 80 minuti su ARM2 a 8 MHz per tutti
  e 24 (l'Amiga impiegava circa un'ora per fotogramma); file di 1,5 MB.

## Animazione: stato e prossimo passo

L'animazione attuale ha tempi e percorso dei palloni misurati sui fotogrammi
originali (shower, giro di 72 fotogrammi, tre palloni sfasati di 24), ma il
corpo e' inventato e diverso dall'originale. Nell'originale (osservazione
dell'utente) gli avambracci salgono al rimbalzo e tutto il corpo si piega
verso il basso con le ginocchia, rialzandosi quando le due palle toccano le
mani. Piano: l'utente rifara' l'animazione in Cinema 4D 2025 (oggetti
nominati per le articolazioni: head, body_top/bottom, shoulder/elbow/hand,
hip/knee/foot L/R, ball_1..3; video originale come sfondo) e uno script
Python per C4D esportera' j00..j23.dat nel formato di Eric (catene come
robot.dat; Y in alto -> Z in alto; telecamera -> osservatore, alt/az,
focale: il "35" di robot.dat e' la focale in mm su pellicola 35 mm).

## Passi

1. ~~**Riferimento sul PC**~~: fatto, vedi sopra.
2. ~~**Programma BBC BASIC**~~: fatto, vedi sopra (manca l'applicazione
   `!Juggler` per il desktop). Il piano era:
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
3. ~~**Animazione**~~ (fatta, procedurale, vedi sopra): i fotogrammi del Juggler (palloni in volo, braccia), poi
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
