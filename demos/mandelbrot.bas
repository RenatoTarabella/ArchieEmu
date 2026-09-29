10 REM Mandelbrot - BBC BASIC V, ciclo interno in assembler ARM
20 REM Virgola fissa con 12 bit di frazione: l'ARM2 moltiplica solo a 32 bit
30 S%=2 : REM lato dei punti in pixel (1 = massimo dettaglio, 4 volte piu' lento)
40 M%=64 : REM iterazioni massime
50 MODE "X640 Y480 C16M"
60 OFF
70 W%=640 DIV S% : H%=480 DIV S%
80 PROCassembla
90 DIM R%(M%),G%(M%),B%(M%),CX%(W%)
100 REM rampa nero-rosso-giallo-bianco: la luminosita' cresce sempre
110 FOR N%=0 TO M%-1
120   T=SQR(N%/M%)*3
130   R%(N%)=FNc(T) : G%(N%)=FNc(T-1) : B%(N%)=FNc(T-2)
140 NEXT
150 R%(M%)=0 : G%(M%)=0 : B%(M%)=0
160 FOR X%=0 TO W%-1 : CX%(X%)=INT((X%*3/W%-2.2)*4096) : NEXT
170 C%=M% : T%=TIME
180 FOR Y%=0 TO H%-1
190   B%=INT((1.125-Y%*2.25/H%)*4096)
200   FOR X%=0 TO W%-1
210     A%=CX%(X%) : N%=USR(iter%)
220     GCOL R%(N%),G%(N%),B%(N%)
230     RECTANGLE FILL X%*S%*2,960-(Y%+1)*S%*2,S%*2-1,S%*2-1
240   NEXT
250 NEXT
260 ON
270 PRINT TAB(0,0);"Tempo: ";(TIME-T%)/100;" s"
280 END
290 :
300 DEF FNc(V)
310 IF V<0 THEN V=0
320 IF V>1 THEN V=1
330 =INT(V*255)
340 :
350 REM USR passa A%,B%,C% in R0,R1,R2: cx, cy, iterazioni massime
360 REM x in R3, y in R4, contatore in R5; ritorna le iterazioni in R0
370 DEF PROCassembla
380 DIM iter% 128
390 FOR pass%=0 TO 2 STEP 2
400   P%=iter%
410   [OPT pass%
420   MOV R3,#0
430   MOV R4,#0
440   MOV R5,#0
450   .loop
460   MUL R6,R3,R3          ; x*x
470   MOV R6,R6,ASR #12
480   MUL R7,R4,R4          ; y*y
490   MOV R7,R7,ASR #12
500   ADD R6,R6,R7
510   CMP R6,#&4000         ; x*x+y*y > 4 ?
520   BGT done
530   SUB R6,R6,R7
540   SUB R6,R6,R7          ; x*x-y*y
550   MUL R7,R3,R4          ; x*y
560   ADD R4,R1,R7,ASR #11  ; y = 2*x*y+cy
570   ADD R3,R6,R0          ; x = x*x-y*y+cx
580   ADD R5,R5,#1
590   CMP R5,R2
600   BLT loop
610   .done
620   MOV R0,R5
630   MOV PC,R14
640   ]
650 NEXT
660 ENDPROC
