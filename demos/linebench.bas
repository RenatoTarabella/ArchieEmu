REM >LineBench
REM Lines: BASIC LINE (OS_Plot) against Bresenham in ARM code
REM Each test repeats its set of lines for at least 2 seconds and
REM gives lines per second, so it works on any machine
PRINT "Line benchmark"
PRINT "  13 = 320 x 256, 256 colours"
PRINT "  15 = 640 x 256, 256 colours"
PRINT "  28 = 640 x 480, 256 colours (VGA monitor)"
INPUT "Mode ? "MO%
IF MO%<>15 AND MO%<>28 THEN MO%=13
MODE MO%
OFF
SYS "OS_ReadModeVariable",-1,9 TO ,,b%
IF b%<>3 THEN MODE 12:PRINT "256 colour modes only":END
SYS "OS_ReadModeVariable",-1,4 TO ,,XE%
SYS "OS_ReadModeVariable",-1,5 TO ,,YE%
SYS "OS_ReadModeVariable",-1,6 TO ,,LB%
SYS "OS_ReadModeVariable",-1,11 TO ,,W%
SYS "OS_ReadModeVariable",-1,12 TO ,,H%
W%+=1:H%+=1
DIM VB% 8,G% 16
!VB%=149:VB%!4=-1
SYS "OS_ReadVduVariables",VB%,VB%
!G%=!VB%:G%!4=LB%
PROCassemble

REM two sets: long lines anywhere on the screen, short ones (up to 8)
N%=200
DIM X1%(N%-1),Y1%(N%-1),X2%(N%-1),Y2%(N%-1)
DIM P1%(N%-1),Q1%(N%-1),P2%(N%-1),Q2%(N%-1)
DIM LL% N%*16,LS% N%*16
d%=RND(-12345)
FOR s%=0 TO 1
  FOR i%=0 TO N%-1
    x1%=RND(W%)-1:y1%=RND(H%)-1
    IF s%=0 THEN
      x2%=RND(W%)-1:y2%=RND(H%)-1
    ELSE
      x2%=x1%+RND(17)-9:y2%=y1%+RND(17)-9
      IF x2%<0 THEN x2%=0
      IF x2%>=W% THEN x2%=W%-1
      IF y2%<0 THEN y2%=0
      IF y2%>=H% THEN y2%=H%-1
    ENDIF
    IF s%=0 THEN p%=LL% ELSE p%=LS%
    p%!(i%*16)=x1%:p%!(i%*16+4)=y1%:p%!(i%*16+8)=x2%:p%!(i%*16+12)=y2%
  NEXT
NEXT

DIM R(1,3),PX(1)
FOR s%=0 TO 1
  IF s%=0 THEN L%=LL% ELSE L%=LS%
  REM average length in pixels (the longer of dx, dy, plus 1)
  t=0
  FOR i%=0 TO N%-1
    X1%(i%)=L%!(i%*16):Y1%(i%)=L%!(i%*16+4):X2%(i%)=L%!(i%*16+8):Y2%(i%)=L%!(i%*16+12)
    P1%(i%)=X1%(i%)<<XE%:Q1%(i%)=(H%-1-Y1%(i%))<<YE%
    P2%(i%)=X2%(i%)<<XE%:Q2%(i%)=(H%-1-Y2%(i%))<<YE%
    a%=ABS(X2%(i%)-X1%(i%)):c%=ABS(Y2%(i%)-Y1%(i%)):IF c%>a% THEN a%=c%
    t+=a%+1
  NEXT
  PX(s%)=t/N%
  REM 1 - BASIC LINE, through OS_Plot
  CLS:GCOL 3
  n%=0:T%=TIME
  REPEAT
    FOR i%=0 TO N%-1:LINE P1%(i%),Q1%(i%),P2%(i%),Q2%(i%):NEXT
    n%+=N%
  UNTIL TIME-T%>=200
  R(s%,0)=n%*100/(TIME-T%)
  REM 2 - CALL to the ARM code for each line
  CLS:E%=&0C:F%=G%
  n%=0:T%=TIME
  REPEAT
    FOR i%=0 TO N%-1:A%=X1%(i%):B%=Y1%(i%):C%=X2%(i%):D%=Y2%(i%):CALL line:NEXT
    n%+=N%
  UNTIL TIME-T%>=200
  R(s%,1)=n%*100/(TIME-T%)
  REM 3 - one CALL, the ARM code reads the whole list
  CLS:A%=L%:B%=N%:C%=&30:D%=G%
  n%=0:T%=TIME
  REPEAT
    CALL lines
    n%+=N%
  UNTIL TIME-T%>=200
  R(s%,2)=n%*100/(TIME-T%)
  REM 4 - Bresenham in BASIC, bytes into screen memory
  CLS
  n%=0:T%=TIME
  REPEAT
    FOR i%=0 TO 19:PROCline(X1%(i%),Y1%(i%),X2%(i%),Y2%(i%),&C0):NEXT
    n%+=20
  UNTIL TIME-T%>=200
  R(s%,3)=n%*100/(TIME-T%)
NEXT

MODE 12
PRINT "Lines per second (";W%;" x ";H%;", 256 colours)"
PRINT
PRINT "                          long lines     short lines"
PRINT "average length (pixels)  ";FNr(PX(0),1,15);FNr(PX(1),1,16)
RESTORE
FOR k%=0 TO 3
  READ a$
  PRINT a$;FNr(R(0,k%),0,15);FNr(R(1,k%),0,16)
NEXT
DATA "BASIC LINE (OS_Plot)    ","ARM code, CALL per line ","ARM code, whole list    ","Bresenham in BASIC      "
PRINT
PRINT "ARM code (CALL per line) / LINE: ";FNr(R(0,1)/R(0,0),2,0);" long, ";FNr(R(1,1)/R(1,0),2,0);" short"
PRINT "ARM code (whole list) / LINE:    ";FNr(R(0,2)/R(0,0),2,0);" long, ";FNr(R(1,2)/R(1,0),2,0);" short"
PRINT "LINE / Bresenham in BASIC:       ";FNr(R(0,0)/R(0,3),2,0);" long, ";FNr(R(1,0)/R(1,3),2,0);" short"
END

REM v with d% decimals, right aligned in w% characters
DEF FNr(v,d%,w%)
LOCAL s$
s$=STR$(INT(v*10^d%+0.5))
IF d%>0 THEN s$=STRING$(d%+1-LEN(s$),"0")+s$:s$=LEFT$(s$,LEN(s$)-d%)+"."+RIGHT$(s$,d%)
=STRING$(w%-LEN(s$)," ")+s$

REM Bresenham in BASIC, the same algorithm as the ARM code
DEF PROCline(x0%,y0%,x1%,y1%,c%)
LOCAL a%,dx%,dy%,sx%,sy%,e%,n%
a%=!G%+y0%*LB%+x0%
dx%=x1%-x0%:sx%=1:IF dx%<0 THEN dx%=-dx%:sx%=-1
dy%=y1%-y0%:sy%=LB%:IF dy%<0 THEN dy%=-dy%:sy%=-LB%
IF dx%>=dy% THEN
  e%=dx%>>1
  FOR n%=0 TO dx%
    ?a%=c%:a%+=sx%:e%-=dy%:IF e%<0 THEN a%+=sy%:e%+=dx%
  NEXT
ELSE
  e%=dy%>>1
  FOR n%=0 TO dy%
    ?a%=c%:a%+=sy%:e%-=dx%:IF e%<0 THEN a%+=sx%:e%+=dy%
  NEXT
ENDIF
ENDPROC

DEF PROCassemble
LOCAL pass%,code%
DIM code% 512
FOR pass%=0 TO 2 STEP 2
P%=code%
[OPT pass%
; line (CALL) - A%,B% = x0,y0  C%,D% = x1,y1  E% = colour  F% = block
.line
STMFD R13!,{R0-R12,R14}
MOV R12,R5
BL ln
LDMFD R13!,{R0-R12,PC}

; lines (CALL) - A% = list of x0,y0,x1,y1  B% = count  C% = colour
; D% = block
.lines
STMFD R13!,{R0-R12,R14}
MOV R12,R3
STR R0,[R12,#8]
STR R1,[R12,#12]
MOV R4,R2
.ls_l
LDR R5,[R12,#8]
LDMIA R5!,{R0-R3}
STR R5,[R12,#8]
BL ln
LDR R5,[R12,#12]
SUBS R5,R5,#1
STR R5,[R12,#12]
BGT ls_l
LDMFD R13!,{R0-R12,PC}

; ln - Bresenham, 8 bpp. R0-R3 = x0,y0,x1,y1  R4 = colour
; R12 = block (screen, bytes per line). Uses R0-R11, keeps R4.
.ln
LDR R5,[R12,#0]
LDR R6,[R12,#4]
MLA R7,R1,R6,R5
ADD R7,R7,R0
SUBS R8,R2,R0
MOV R9,#1
RSBLT R8,R8,#0
MVNLT R9,#0
SUBS R10,R3,R1
MOV R11,R6
RSBLT R10,R10,#0
RSBLT R11,R6,#0
CMP R8,R10
BLT ln_y
MOV R0,R8,ASR #1
ADD R1,R8,#1
.ln_xl
STRB R4,[R7],R9
SUBS R0,R0,R10
ADDLT R7,R7,R11
ADDLT R0,R0,R8
SUBS R1,R1,#1
BNE ln_xl
MOV PC,R14
.ln_y
MOV R0,R10,ASR #1
ADD R1,R10,#1
.ln_yl
STRB R4,[R7],R11
SUBS R0,R0,R8
ADDLT R7,R7,R9
ADDLT R0,R0,R10
SUBS R1,R1,#1
BNE ln_yl
MOV PC,R14
]
NEXT
ENDPROC
