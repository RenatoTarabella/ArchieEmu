REM >SDF
REM Ray marching of signed distance fields in BBC BASIC V + ARM code
REM Formulas by Inigo Quilez (iquilezles.org): rounded box, sphere,
REM smooth union / subtraction / intersection, soft shadows,
REM ambient occlusion, tetrahedral normal.
REM BASIC builds the scene (a list of primitives) and the camera; the
REM marching and shading run in ARM code, fixed point 16.16. The floor
REM (the plane y=0) is intersected exactly, the solids are marched only
REM inside a sphere that holds them all.
PRINT "Signed distance fields - a cube and a sphere"
PRINT '"Operation:"
PRINT "  1 = smooth union"
PRINT "  2 = smooth subtraction (the sphere carves the cube)"
PRINT "  3 = smooth intersection"
INPUT "Operation ? "OP%
IF OP%<1 OR OP%>3 THEN OP%=2
INPUT "Smoothness k (Return = 0.15, 0 = sharp) ? "K$
IF K$="" THEN SK=0.15 ELSE SK=VAL(K$)
IF SK<0 THEN SK=0
INPUT "Detail (1=full, 2, 4, 8=fastest) ? "DT%
IF DT%<>2 AND DT%<>4 AND DT%<>8 THEN DT%=1
PRINT '"Screen modes:"
PRINT "  13 = 320 x 256, 256 colours"
PRINT "  28 = 640 x 480, 256 colours (VGA monitor)"
PRINT "  32 = 640 x 480, 16M colours (Risc PC, or the emulator)"
INPUT "Mode ? "MO%
IF MO%<>13 AND MO%<>28 AND MO%<>32 THEN MO%=13

DIM G% 512,PR% 48*16,SB% 32,VB% 8
PROCassemble

REM ---------------- the scene ----------------
REM cube of half side 0.75 resting on the floor, sphere of radius 0.7
REM on its top front corner; the first primitive has operation 3
NP%=0
PROCprim(1,3,0,0.75,0,0.75,0.75,0.75,0.06,0)
PROCprim(0,OP%-1,0.6,1.4,0.6,0.7,0,0,0,SK)
AR=0.75:AG=0.3:AB=0.15
REM the camera and the light (direction towards it)
OX=2.9:OY=2.3:OZ=3.5:TX=0:TY=0.6:TZ=0:FL=2.2
LX=-0.5:LY=0.8:LZ=0.6

PROCmode
PROCsetup
T%=TIME
FOR j%=0 TO H%-1 STEP DT%
  v=1-2*(j%+DT%/2)/H%:u=(DT%/W%-1)*AS
  G%!g_ux=FNf(FX*FL+RX*u+UX*v):G%!g_uy=FNf(FY*FL+RY*u+UY*v):G%!g_uz=FNf(FZ*FL+RZ*u+UZ*v)
  G%!g_j=j%
  A%=G%:CALL RP%
NEXT
T%=TIME-T%
K%=GET
MODE 12
PRINT "Render time: ";T%/100;" seconds"
END

DEF FNf(v)=INT(v*65536+0.5)

REM type 0 sphere (a = radius), 1 box (a,b,c = half sides, r = rounding)
REM op 0 union, 1 subtraction (this one carves), 2 intersection, 3 set
DEF PROCprim(ty%,op%,x,y,z,a,b,c,r,k)
LOCAL p%
p%=PR%+NP%*48:NP%+=1
p%!0=ty%:p%!4=op%:p%!8=FNf(x):p%!12=FNf(y):p%!16=FNf(z)
p%!20=FNf(a):p%!24=FNf(b):p%!28=FNf(c):p%!32=FNf(r):p%!36=FNf(k)
IF k>0 THEN p%!40=FNf(0.5/k) ELSE p%!40=0
ENDPROC

DEF PROCmode
LOCAL f%
IF MO%=32 THEN
  SB%!0=1:SB%!4=640:SB%!8=480:SB%!12=5:SB%!16=-1:SB%!20=-1
  SYS "XOS_ScreenMode",0,SB% TO ;f%
  IF f% AND 1 THEN MODE 49
ELSE
  MODE MO%
ENDIF
OFF
SYS "OS_ReadModeVariable",-1,9 TO ,,b%
SYS "OS_ReadModeVariable",-1,4 TO ,,XE%
SYS "OS_ReadModeVariable",-1,5 TO ,,YE%
SYS "OS_ReadModeVariable",-1,6 TO ,,LB%
SYS "OS_ReadModeVariable",-1,11 TO ,,W%
SYS "OS_ReadModeVariable",-1,12 TO ,,H%
W%+=1:H%+=1
IF b%<>3 AND b%<>5 THEN MODE 12:PRINT "The screen must have 256 or 16M colours":END
!VB%=149:VB%!4=-1
SYS "OS_ReadVduVariables",VB%,VB%
REM aspect ratio of the screen in OS units
AS=(W%<<XE%)/(H%<<YE%)
ENDPROC

DEF PROCsetup
LOCAL i%,l,p%,cx,cy,cz,r,m,e,w
FX=TX-OX:FY=TY-OY:FZ=TZ-OZ:l=SQR(FX*FX+FY*FY+FZ*FZ):FX=FX/l:FY=FY/l:FZ=FZ/l
RX=-FZ:RY=0:RZ=FX:l=SQR(RX*RX+RZ*RZ):RX=RX/l:RZ=RZ/l
UX=RY*FZ-RZ*FY:UY=RZ*FX-RX*FZ:UZ=RX*FY-RY*FX
l=SQR(LX*LX+LY*LY+LZ*LZ)
G%!g_lx=FNf(LX/l):G%!g_ly=FNf(LY/l):G%!g_lz=FNf(LZ/l)
G%!g_ox=FNf(OX):G%!g_oy=FNf(OY):G%!g_oz=FNf(OZ)
G%!g_np=NP%:G%!g_prim=PR%
G%!g_ar=FNf(AR):G%!g_ag=FNf(AG):G%!g_ab=FNf(AB)
REM bounding sphere: centre of the primitives, radius that holds them
cx=0:cy=0:cz=0
FOR i%=0 TO NP%-1:p%=PR%+i%*48:cx+=p%!8/65536:cy+=p%!12/65536:cz+=p%!16/65536:NEXT
cx=cx/NP%:cy=cy/NP%:cz=cz/NP%:m=0
FOR i%=0 TO NP%-1
  p%=PR%+i%*48
  IF !p%=0 THEN r=p%!20/65536 ELSE r=SQR((p%!20)^2+(p%!24)^2+(p%!28)^2)/65536
  r+=SQR((p%!8/65536-cx)^2+(p%!12/65536-cy)^2+(p%!16/65536-cz)^2)+p%!36/65536
  IF r>m THEN m=r
NEXT
m+=0.05
G%!g_bcx=FNf(cx):G%!g_bcy=FNf(cy):G%!g_bcz=FNf(cz)
G%!g_br2=FNf(m*m):G%!g_bs2=FNf((m+0.3)^2):G%!c_aon=FNf((m+0.8)^2)
REM the screen
G%!g_w=W%:G%!g_dt=DT%:G%!g_scr=!VB%:G%!g_lb=LB%
IF b%=5 THEN G%!g_bpp=32 ELSE G%!g_bpp=8
du=2*DT%/W%*AS
G%!g_sx=FNf(RX*du):G%!g_sy=FNf(RY*du):G%!g_sz=FNf(RZ*du)
REM ordered dithering 4x4 (Bayer) for the 256 colour modes
RESTORE
FOR i%=0 TO 15:READ e:G%!(g_bay+4*i%)=FNf((e+0.5)/16):NEXT
DATA 0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5
REM constants
G%!c_e=FNf(0.002):G%!c_02=FNf(0.02):G%!c_p15=FNf(1.5)
G%!c_sky=FNf(0.55):G%!(c_sky+4)=FNf(0.35):G%!(c_sky+8)=FNf(0.7):G%!(c_sky+12)=FNf(0.25)
REM light per channel: sun (warm), sky (from above, blue), ambient
FOR i%=0 TO 8:READ e:G%!(c_lit+4*i%)=FNf(e):NEXT
DATA 1.3,0.25,0.08, 1.15,0.3,0.07, 0.95,0.45,0.06
G%!c_fog=FNf(0.0015)
G%!c_fogc=FNf(0.8):G%!(c_fogc+4)=FNf(0.85):G%!(c_fogc+8)=FNf(1)
REM ambient occlusion: 5 samples along the normal, weights 0.85^i
w=1
FOR i%=0 TO 4:G%!(c_ao+4*i%)=FNf(0.14+0.12*i%):G%!(c_ao+20+4*i%)=FNf(w):w=w*0.85:NEXT
REM checkered floor
G%!c_fl1=FNf(0.42):G%!(c_fl1+4)=FNf(0.4):G%!(c_fl1+8)=FNf(0.38)
G%!c_fl2=FNf(0.2):G%!(c_fl2+4)=FNf(0.2):G%!(c_fl2+8)=FNf(0.22)
ENDPROC

REM ---------------- the engine: ARM code ----------------
REM d=(a*b)>>16 for 16.16 numbers, rounded; t,u are scratch registers
REM (d must differ from a and b: ARM2 MUL)
DEF FNmul(d%,a%,b%,t%,u%)
[OPT pass%
MOV t%,a%,ASR #16
MUL d%,b%,t%
BIC t%,a%,t%,LSL #16
MOV u%,b%,ASR #16
MLA d%,t%,u%,d%
BIC u%,b%,u%,LSL #16
MUL u%,t%,u%
ADD u%,u%,#&8000
ADD d%,d%,u%,LSR #16
]
=pass%

REM one sample of the tetrahedral normal: n += k * map(p + k*e)
REM with k = (x%,y%,z%), each +1 or -1 (R3 = e, R4 = -e, R5 = -map)
DEF FNtet(x%,y%,z%)
LOCAL a%,b%,c%,d%,e%,f%
a%=3-(x%<0):b%=3-(y%<0):c%=3-(z%<0)
d%=5*-(x%<0):e%=5*-(y%<0):f%=5*-(z%<0)
[OPT pass%
ADD R3,R12,#g_hx
LDMIA R3,{R0-R2}
LDR R3,[R12,#c_e]
RSB R4,R3,#0
ADD R0,R0,a%
ADD R1,R1,b%
ADD R2,R2,c%
BL map
RSB R5,R0,#0
LDR R1,[R12,#g_nx]
ADD R1,R1,d%
STR R1,[R12,#g_nx]
LDR R1,[R12,#g_ny]
ADD R1,R1,e%
STR R1,[R12,#g_ny]
LDR R1,[R12,#g_nz]
ADD R1,R1,f%
STR R1,[R12,#g_nz]
]
=pass%

REM R6-R8 = the vector at R12+o% minus the bounding centre
DEF FNoc(o%)
[OPT pass%
ADD R3,R12,#o%
LDMIA R3,{R6-R8}
LDR R3,[R12,#g_bcx]
SUB R6,R6,R3
LDR R3,[R12,#g_bcy]
SUB R7,R7,R3
LDR R3,[R12,#g_bcz]
SUB R8,R8,R3
]
=pass%

REM R0 = R6*R9 + R7*R10 + R8*R11 (dot product)
DEF FNdot
[OPT pass%
OPT FNmul(0,6,9,4,5)
OPT FNmul(1,7,10,4,5)
ADD R0,R0,R1
OPT FNmul(1,8,11,4,5)
ADD R0,R0,R1
]
=pass%

REM R0 = R6*R6 + R7*R7 + R8*R8
DEF FNsq
[OPT pass%
OPT FNmul(0,6,6,4,5)
OPT FNmul(1,7,7,4,5)
ADD R0,R0,R1
OPT FNmul(1,8,8,4,5)
ADD R0,R0,R1
]
=pass%

DEF PROCassemble
LOCAL pass%,code%
REM global block (R12); offsets used by ADD Rn,R12,#o must be valid
REM ARM immediates
g_px=0:g_py=4:g_pz=8:g_np=12:g_prim=16:g_ox=20:g_oy=24:g_oz=28
g_dx=32:g_dy=36:g_dz=40:g_ux=44:g_uy=48:g_uz=52:g_sx=56:g_sy=60:g_sz=64
g_lx=68:g_ly=72:g_lz=76:g_hx=80:g_hy=84:g_hz=88:g_nx=92:g_ny=96:g_nz=100
g_i=104:g_j=108:g_w=112:g_dt=116:g_scr=120:g_lb=124:g_bpp=128
g_cr=132:g_cg=136:g_cb=140:g_t=144:g_cnt=148:g_m=152:g_tmax=156
g_sha=160:g_occ=164:g_dif=168:g_spe=172:g_res=176:g_st=180
g_ar=184:g_ag=188:g_ab=192:g_bcx=196:g_bcy=200:g_bcz=204:g_br2=208
g_bs2=212:g_tf=216:g_al=224:g_bay=236:g_v=304
c_e=320:c_02=324:c_p15=336:c_sky=340:c_lit=356:c_fog=392:c_fogc=396
c_ao=408:c_fl1=448:c_fl2=464:c_aon=480
REM primitive record
p_type=0:p_op=4:p_cx=8:p_cy=12:p_cz=16:p_a=20:p_b=24:p_c=28:p_r=32
p_k=36:p_ik=40:PS=48
DIM code% 8192
FOR pass%=0 TO 2 STEP 2
P%=code%
[OPT pass%

; ---- map - R0-R2 = point, R0 = distance to the solids
; the primitives are combined in order. Uses R0-R10.
.map
STMFD R13!,{R14}
ADD R3,R12,#g_px
STMIA R3,{R0-R2}
LDR R9,[R12,#g_prim]
LDR R10,[R12,#g_np]
MOV R8,#&7F000000
.map_l
SUBS R10,R10,#1
BMI map_e
ADD R3,R12,#g_px
LDMIA R3,{R0-R2}
LDR R3,[R9,#p_cx]
SUB R0,R0,R3
LDR R3,[R9,#p_cy]
SUB R1,R1,R3
LDR R3,[R9,#p_cz]
SUB R2,R2,R3
LDR R3,[R9,#p_type]
CMP R3,#0
BNE map_box
; sphere - length(p) - r
OPT FNmul(3,0,0,4,5)
OPT FNmul(6,1,1,4,5)
ADD R3,R3,R6
OPT FNmul(6,2,2,4,5)
ADD R0,R3,R6
BL fsqrt
LDR R3,[R9,#p_a]
SUB R0,R0,R3
B map_op
; rounded box - q = abs(p) - b + r
; length(max(q,0)) + min(max(qx,qy,qz),0) - r
.map_box
LDR R7,[R9,#p_r]
CMP R0,#0
RSBLT R0,R0,#0
LDR R3,[R9,#p_a]
SUB R3,R3,R7
SUB R0,R0,R3
CMP R1,#0
RSBLT R1,R1,#0
LDR R3,[R9,#p_b]
SUB R3,R3,R7
SUB R1,R1,R3
CMP R2,#0
RSBLT R2,R2,#0
LDR R3,[R9,#p_c]
SUB R3,R3,R7
SUB R2,R2,R3
MOV R6,R0
CMP R1,R6
MOVGT R6,R1
CMP R2,R6
MOVGT R6,R2
CMP R6,#0
MOVGT R6,#0
SUB R7,R6,R7
CMP R0,#0
MOVLT R0,#0
CMP R1,#0
MOVLT R1,#0
CMP R2,#0
MOVLT R2,#0
OPT FNmul(3,0,0,4,5)
OPT FNmul(6,1,1,4,5)
ADD R3,R3,R6
OPT FNmul(6,2,2,4,5)
ADD R0,R3,R6
BL fsqrt
ADD R0,R0,R7
; combine the new distance R0 with the one so far R8
; union    h = 0.5+(new-acc)/2k  d = new+(acc-new)*h - k*h*(1-h)
; inters.  h = 0.5-(new-acc)/2k  d = new+(acc-new)*h + k*h*(1-h)
; subtr.   h = 0.5-(new+acc)/2k  d = acc+(-new-acc)*h + k*h*(1-h)
.map_op
LDR R3,[R9,#p_op]
CMP R3,#3
MOVEQ R8,R0
BEQ map_nx
LDR R7,[R9,#p_ik]
CMP R7,#0
BEQ map_hard
CMP R3,#1
SUBNE R1,R0,R8
ADDEQ R1,R0,R8
OPT FNmul(2,1,7,4,5)
CMP R3,#0
RSBNE R2,R2,#0
ADD R2,R2,#&8000
CMP R2,#0
MOVLT R2,#0
CMP R2,#&10000
MOVGT R2,#&10000
CMP R3,#1
SUBNE R1,R8,R0
MOVNE R6,R0
RSBEQ R1,R0,#0
SUBEQ R1,R1,R8
MOVEQ R6,R8
OPT FNmul(0,1,2,4,5)
ADD R6,R6,R0
RSB R1,R2,#&10000
OPT FNmul(0,2,1,4,5)
LDR R7,[R9,#p_k]
OPT FNmul(1,0,7,4,5)
CMP R3,#0
SUBEQ R8,R6,R1
ADDNE R8,R6,R1
B map_nx
.map_hard
CMP R3,#0
BNE map_h1
CMP R0,R8
MOVLT R8,R0
B map_nx
.map_h1
CMP R3,#1
RSBEQ R0,R0,#0
CMP R0,R8
MOVGT R8,R0
.map_nx
ADD R9,R9,#PS
B map_l
.map_e
MOV R0,R8
LDMFD R13!,{PC}

; ---- hitpt - point O + D*t to R0-R2 and g_hx. Uses R0-R8.
.hitpt
LDR R3,[R12,#g_t]
ADD R6,R12,#g_dx
LDMIA R6,{R6-R8}
OPT FNmul(0,6,3,4,5)
OPT FNmul(1,7,3,4,5)
OPT FNmul(2,8,3,4,5)
ADD R3,R12,#g_ox
LDMIA R3,{R6-R8}
ADD R0,R0,R6
ADD R1,R1,R7
ADD R2,R2,R8
ADD R3,R12,#g_hx
STMIA R3,{R0-R2}
MOV PC,R14

; ---- nrm - normalises the 3 words at R11. Uses R0-R10.
.nrm
STMFD R13!,{R14}
LDMIA R11,{R6-R8}
OPT FNsq
CMP R0,#0
LDMEQFD R13!,{PC}
BL fsqrt
MOVS R1,R0
LDMEQFD R13!,{PC}
MOV R0,#&10000
BL fdiv
MOV R9,R0
OPT FNmul(0,6,9,4,5)
OPT FNmul(1,7,9,4,5)
OPT FNmul(2,8,9,4,5)
STMIA R11,{R0-R2}
LDMFD R13!,{PC}

; ---- rowpix (CALL with R0 = global block) - one row of the picture
.rowpix
STMFD R13!,{R0-R12,R14}
MOV R12,R0
MOV R0,#0
STR R0,[R12,#g_i]
.rp_l
LDR R0,[R12,#g_i]
LDR R1,[R12,#g_w]
CMP R0,R1
BGE rp_done
BL pixel
BL plot
LDR R0,[R12,#g_i]
LDR R1,[R12,#g_dt]
ADD R0,R0,R1
STR R0,[R12,#g_i]
ADD R3,R12,#g_ux
LDMIA R3,{R0-R2}
ADD R4,R12,#g_sx
LDMIA R4,{R5-R7}
ADD R0,R0,R5
ADD R1,R1,R6
ADD R2,R2,R7
STMIA R3,{R0-R2}
B rp_l
.rp_done
LDMFD R13!,{R0-R12,PC}

; ---- pixel - the colour of the ray along g_ux to g_cr,g_cg,g_cb
.pixel
STMFD R13!,{R14}
ADD R3,R12,#g_ux
LDMIA R3,{R0-R2}
ADD R11,R12,#g_dx
STMIA R11,{R0-R2}
BL nrm
; the sky - blue that fades to white near the horizon
LDR R1,[R12,#g_dy]
RSB R3,R1,#&10000
LDR R6,[R12,#c_sky+4]
OPT FNmul(0,3,6,4,5)
LDR R6,[R12,#c_sky]
ADD R0,R0,R6
CMP R0,#&10000
MOVGT R0,#&10000
STR R0,[R12,#g_cr]
LDR R6,[R12,#c_sky+12]
OPT FNmul(0,3,6,4,5)
LDR R6,[R12,#c_sky+8]
ADD R0,R0,R6
CMP R0,#&10000
MOVGT R0,#&10000
STR R0,[R12,#g_cg]
MOV R0,#&10000
STR R0,[R12,#g_cb]
; the floor - t = oy/(-dy) if the ray goes down
MVN R0,#0
STR R0,[R12,#g_tf]
CMN R1,#66
BGE px_nf
LDR R0,[R12,#g_oy]
RSB R1,R1,#0
BL fdiv
STR R0,[R12,#g_tf]
.px_nf
; the bounding sphere - b = oc.d, c = oc.oc - r*r, t = -b +- sqrt(b*b-c)
OPT FNoc(g_ox)
ADD R3,R12,#g_dx
LDMIA R3,{R9-R11}
OPT FNdot
MOV R9,R0
OPT FNsq
LDR R1,[R12,#g_br2]
SUB R10,R0,R1
OPT FNmul(0,9,9,4,5)
SUBS R0,R0,R10
BLE px_fl
BL fsqrt
RSB R1,R9,#0
SUB R2,R1,R0
ADD R3,R1,R0
CMP R3,#0
BLE px_fl
CMP R2,#0
MOVLT R2,#0
LDR R1,[R12,#g_tf]
CMP R1,#0
BLT px_nt
CMP R3,R1
MOVGT R3,R1
.px_nt
CMP R2,R3
BGE px_fl
STR R2,[R12,#g_t]
STR R3,[R12,#g_tmax]
MOV R0,#100
STR R0,[R12,#g_cnt]
; march - t += d until d < t/512
.px_m
BL hitpt
BL map
LDR R1,[R12,#g_t]
CMP R0,R1,ASR #9
BLT px_hit
ADD R1,R1,R0
STR R1,[R12,#g_t]
LDR R2,[R12,#g_tmax]
CMP R1,R2
BGT px_fl
LDR R2,[R12,#g_cnt]
SUBS R2,R2,#1
STR R2,[R12,#g_cnt]
BGT px_m
.px_hit
BL hitpt
MOV R0,#0
STR R0,[R12,#g_nx]
STR R0,[R12,#g_ny]
STR R0,[R12,#g_nz]
OPT FNtet(1,-1,-1)
OPT FNtet(-1,-1,1)
OPT FNtet(-1,1,-1)
OPT FNtet(1,1,1)
ADD R11,R12,#g_nx
LDMIA R11,{R0-R2}
MOV R0,R0,LSL #6
MOV R1,R1,LSL #6
MOV R2,R2,LSL #6
STMIA R11,{R0-R2}
BL nrm
ADD R3,R12,#g_ar
LDMIA R3,{R0-R2}
ADD R3,R12,#g_al
STMIA R3,{R0-R2}
MOV R0,#2
STR R0,[R12,#g_m]
BL shade
LDMFD R13!,{PC}
; the floor, checkered - (floor(1.5x)+floor(1.5z)) AND 1
.px_fl
LDR R0,[R12,#g_tf]
CMP R0,#0
LDMLTFD R13!,{PC}
STR R0,[R12,#g_t]
BL hitpt
LDR R3,[R12,#c_p15]
OPT FNmul(6,0,3,4,5)
OPT FNmul(7,2,3,4,5)
MOV R6,R6,ASR #16
ADD R6,R6,R7,ASR #16
TST R6,#1
ADDNE R3,R12,#c_fl1
ADDEQ R3,R12,#c_fl2
LDMIA R3,{R0-R2}
ADD R3,R12,#g_al
STMIA R3,{R0-R2}
MOV R0,#0
MOV R1,#&10000
MOV R2,#0
ADD R3,R12,#g_nx
STMIA R3,{R0-R2}
MOV R0,#1
STR R0,[R12,#g_m]
BL shade
LDMFD R13!,{PC}

; ---- shade - point g_hx, normal g_nx, albedo g_al to g_cr..g_cb
.shade
STMFD R13!,{R14}
ADD R3,R12,#g_nx
LDMIA R3,{R6-R8}
ADD R3,R12,#g_lx
LDMIA R3,{R9-R11}
OPT FNdot
MOV R1,#&10000
STR R1,[R12,#g_sha]
CMP R0,#0
MOVLE R0,#0
STR R0,[R12,#g_dif]
BLGT shadow
; ambient occlusion, only near the solids
MOV R0,#&10000
STR R0,[R12,#g_occ]
OPT FNoc(g_hx)
OPT FNsq
LDR R1,[R12,#c_aon]
CMP R0,R1
BLLE ao
; specular (Blinn), half vector between the light and the eye
MOV R0,#0
STR R0,[R12,#g_spe]
LDR R0,[R12,#g_m]
CMP R0,#1
BEQ sh_li
ADD R3,R12,#g_lx
LDMIA R3,{R0-R2}
ADD R3,R12,#g_dx
LDMIA R3,{R6-R8}
SUB R0,R0,R6
SUB R1,R1,R7
SUB R2,R2,R8
ADD R11,R12,#g_v
STMIA R11,{R0-R2}
BL nrm
LDMIA R11,{R9-R11}
ADD R3,R12,#g_nx
LDMIA R3,{R6-R8}
OPT FNdot
CMP R0,#0
MOVLT R0,#0
OPT FNmul(1,0,0,4,5)
OPT FNmul(0,1,1,4,5)
OPT FNmul(1,0,0,4,5)
OPT FNmul(2,1,1,4,5)
OPT FNmul(0,1,2,4,5)
LDR R1,[R12,#g_dif]
OPT FNmul(2,0,1,4,5)
LDR R1,[R12,#g_sha]
OPT FNmul(0,2,1,4,5)
STR R0,[R12,#g_spe]
; light - k = dif*sha, e = sky from above * occ
.sh_li
LDR R0,[R12,#g_dif]
LDR R1,[R12,#g_sha]
OPT FNmul(9,0,1,4,5)
LDR R0,[R12,#g_ny]
MOV R0,R0,ASR #1
ADD R0,R0,#&8000
LDR R11,[R12,#g_occ]
OPT FNmul(10,0,11,4,5)
; fog - f = x/(1+x) with x = 0.0015*t*t (t at most 40)
LDR R0,[R12,#g_t]
CMP R0,#&280000
MOVGT R0,#&280000
OPT FNmul(1,0,0,4,5)
LDR R2,[R12,#c_fog]
OPT FNmul(0,1,2,4,5)
ADD R1,R0,#&10000
BL fdiv
MOV R3,R0
MOV R6,#0
.sh_c
ADD R7,R12,R6
ADD R8,R7,R6,LSL #1
LDR R1,[R8,#c_lit]
OPT FNmul(0,9,1,4,5)
LDR R1,[R8,#c_lit+4]
OPT FNmul(2,10,1,4,5)
ADD R0,R0,R2
LDR R1,[R8,#c_lit+8]
OPT FNmul(2,11,1,4,5)
ADD R0,R0,R2
LDR R1,[R7,#g_al]
OPT FNmul(2,0,1,4,5)
LDR R1,[R12,#g_spe]
ADD R2,R2,R1
LDR R1,[R7,#c_fogc]
SUB R1,R1,R2
OPT FNmul(0,1,3,4,5)
ADD R0,R2,R0
; gamma 2.2, about the square root
CMP R0,#0
MOVLT R0,#0
CMP R0,#&10000
MOVGT R0,#&10000
STMFD R13!,{R3}
BL fsqrt
LDMFD R13!,{R3}
STR R0,[R7,#g_cr]
ADD R6,R6,#4
CMP R6,#12
BLT sh_c
LDMFD R13!,{PC}

; ---- shadow - soft shadow towards the light to g_sha
; res = min(res, 8*h/t), only if the ray meets the bounding sphere
.shadow
STMFD R13!,{R14}
OPT FNoc(g_hx)
ADD R3,R12,#g_lx
LDMIA R3,{R9-R11}
OPT FNdot
MOV R9,R0
OPT FNsq
LDR R1,[R12,#g_bs2]
SUBS R10,R0,R1
BLE sd_go
CMP R9,#0
LDMGEFD R13!,{PC}
OPT FNmul(0,9,9,4,5)
CMP R0,R10
LDMLTFD R13!,{PC}
.sd_go
MOV R0,#&10000
STR R0,[R12,#g_res]
LDR R0,[R12,#c_02]
STR R0,[R12,#g_st]
MOV R0,#48
STR R0,[R12,#g_cnt]
.sd_l
LDR R11,[R12,#g_st]
ADD R3,R12,#g_lx
LDMIA R3,{R6-R8}
OPT FNmul(0,6,11,4,5)
OPT FNmul(1,7,11,4,5)
OPT FNmul(2,8,11,4,5)
ADD R3,R12,#g_hx
LDMIA R3,{R6-R8}
ADD R0,R0,R6
ADD R1,R1,R7
ADD R2,R2,R8
BL map
MOV R10,R0
LDR R11,[R12,#g_st]
LDR R9,[R12,#g_res]
OPT FNmul(1,9,11,4,5)
MOV R2,R10,LSL #3
CMP R2,R1
BGE sd_nx
MOV R0,R2
MOV R1,R11
BL fdiv
STR R0,[R12,#g_res]
CMP R0,#640
BLT sd_e
.sd_nx
LDR R1,[R12,#c_02]
CMP R10,R1
MOVLT R10,R1
ADD R11,R11,R10
STR R11,[R12,#g_st]
CMP R11,#&60000
BGT sd_e
LDR R2,[R12,#g_cnt]
SUBS R2,R2,#1
STR R2,[R12,#g_cnt]
BGT sd_l
.sd_e
LDR R0,[R12,#g_res]
CMP R0,#0
MOVLT R0,#0
OPT FNmul(1,0,0,4,5)
MOV R2,#&30000
SUB R2,R2,R0,LSL #1
OPT FNmul(0,1,2,4,5)
STR R0,[R12,#g_sha]
LDMFD R13!,{PC}

; ---- ao - occ = 1 - 2.5 * sum (e - map(p + n*e)) * w to g_occ
.ao
STMFD R13!,{R14}
MOV R0,#0
STR R0,[R12,#g_occ]
STR R0,[R12,#g_cnt]
.ao_l
LDR R10,[R12,#g_cnt]
ADD R10,R12,R10,LSL #2
LDR R11,[R10,#c_ao]
ADD R3,R12,#g_nx
LDMIA R3,{R6-R8}
OPT FNmul(0,6,11,4,5)
OPT FNmul(1,7,11,4,5)
OPT FNmul(2,8,11,4,5)
ADD R3,R12,#g_hx
LDMIA R3,{R6-R8}
ADD R0,R0,R6
ADD R1,R1,R7
ADD R2,R2,R8
BL map
LDR R1,[R12,#g_py]
CMP R1,R0
MOVLT R0,R1
LDR R10,[R12,#g_cnt]
ADD R10,R12,R10,LSL #2
LDR R11,[R10,#c_ao]
SUB R0,R11,R0
LDR R11,[R10,#c_ao+20]
OPT FNmul(1,0,11,4,5)
LDR R2,[R12,#g_occ]
ADD R2,R2,R1
STR R2,[R12,#g_occ]
LDR R10,[R12,#g_cnt]
ADD R10,R10,#1
STR R10,[R12,#g_cnt]
CMP R10,#5
BLT ao_l
LDR R0,[R12,#g_occ]
ADD R0,R0,R0,LSL #2
MOV R1,#&10000
SUB R0,R1,R0,ASR #1
CMP R0,#0
MOVLT R0,#0
CMP R0,#&10000
MOVGT R0,#&10000
STR R0,[R12,#g_occ]
LDMFD R13!,{PC}

; ---- plot - a block of g_dt x g_dt pixels at g_i,g_j
.plot
LDR R0,[R12,#g_scr]
LDR R1,[R12,#g_lb]
LDR R2,[R12,#g_j]
MLA R0,R2,R1,R0
LDR R2,[R12,#g_i]
LDR R3,[R12,#g_bpp]
CMP R3,#32
ADDNE R0,R0,R2
BNE pl_8
ADD R0,R0,R2,LSL #2
LDR R4,[R12,#g_cr]
RSB R4,R4,R4,LSL #8
MOV R4,R4,LSR #16
LDR R5,[R12,#g_cg]
RSB R5,R5,R5,LSL #8
MOV R5,R5,LSR #16
LDR R6,[R12,#g_cb]
RSB R6,R6,R6,LSL #8
MOV R6,R6,LSR #16
ORR R4,R4,R5,LSL #8
ORR R4,R4,R6,LSL #16
LDR R7,[R12,#g_dt]
MOV R8,R7
.pl_y
MOV R9,#0
.pl_x
STR R4,[R0,R9,LSL #2]
ADD R9,R9,#1
CMP R9,R7
BLT pl_x
ADD R0,R0,R1
SUBS R8,R8,#1
BGT pl_y
MOV PC,R14
; 8 bpp (VIDC) - bits 0-1 tint, 2 R2, 3 B2, 4 R3, 5 G2, 6 G3, 7 B3
; each channel 0-15 = 4*top + tint, one tint for the three
.pl_8
STMFD R13!,{R14}
LDR R7,[R12,#g_dt]
LDR R10,[R12,#g_cr]
RSB R10,R10,R10,LSL #4
LDR R11,[R12,#g_cg]
RSB R11,R11,R11,LSL #4
LDR R14,[R12,#g_cb]
RSB R14,R14,R14,LSL #4
MOV R8,#0
.p8_y
MOV R9,#0
.p8_x
LDR R2,[R12,#g_i]
ADD R2,R2,R9
AND R2,R2,#3
LDR R3,[R12,#g_j]
ADD R3,R3,R8
AND R3,R3,#3
ADD R2,R2,R3,LSL #2
ADD R2,R12,R2,LSL #2
LDR R2,[R2,#g_bay]
; tint - the low two bits of the luminance (r+2g+b)/4, dithered
ADD R3,R10,R14
ADD R3,R3,R11,LSL #1
ADD R3,R2,R3,LSR #2
MOV R3,R3,LSR #16
AND R3,R3,#3
; top two bits of each channel - (v - tint)/4, dithered, 0 to 3
SUB R4,R10,R3,LSL #16
MOV R4,R4,ASR #2
ADD R4,R4,R2
MOVS R4,R4,ASR #16
MOVMI R4,#0
CMP R4,#3
MOVGT R4,#3
SUB R5,R11,R3,LSL #16
MOV R5,R5,ASR #2
ADD R5,R5,R2
MOVS R5,R5,ASR #16
MOVMI R5,#0
CMP R5,#3
MOVGT R5,#3
SUB R6,R14,R3,LSL #16
MOV R6,R6,ASR #2
ADD R6,R6,R2
MOVS R6,R6,ASR #16
MOVMI R6,#0
CMP R6,#3
MOVGT R6,#3
MOV R2,R3
AND R3,R4,#1
ORR R2,R2,R3,LSL #2
AND R3,R4,#2
ORR R2,R2,R3,LSL #3
AND R3,R6,#1
ORR R2,R2,R3,LSL #3
AND R3,R6,#2
ORR R2,R2,R3,LSL #6
AND R3,R5,#3
ORR R2,R2,R3,LSL #5
STRB R2,[R0,R9]
ADD R9,R9,#1
CMP R9,R7
BLT p8_x
ADD R0,R0,R1
ADD R8,R8,#1
CMP R8,R7
BLT p8_y
LDMFD R13!,{PC}

; ---- fdiv - R0 = R0/R1 (16.16, signed). Uses R0-R3.
.fdiv
STMFD R13!,{R4,R14}
EOR R4,R0,R1
CMP R0,#0
RSBLT R0,R0,#0
CMP R1,#0
RSBLT R1,R1,#0
MVNEQ R0,#&80000000
BEQ fd_s
MOV R2,#0
MOV R3,#0
MOV R14,#48
.fd_l
MOVS R0,R0,LSL #1
ADC R3,R3,R3
CMP R3,R1
SUBHS R3,R3,R1
ADC R2,R2,R2
SUBS R14,R14,#1
BNE fd_l
MOV R0,R2
.fd_s
CMP R4,#0
RSBLT R0,R0,#0
LDMFD R13!,{R4,PC}

; ---- fsqrt - R0 = sqrt(R0) (16.16). Uses R0-R3.
.fsqrt
STMFD R13!,{R4,R14}
CMP R0,#0
MOVLE R0,#0
LDMLEFD R13!,{R4,PC}
MOV R4,#8
.fq_n
TST R0,#&F0000000
BNE fq_d
MOV R0,R0,LSL #2
SUB R4,R4,#1
B fq_n
.fq_d
MOV R1,#0
MOV R2,#&40000000
.fq_1
CMP R2,R0
MOVHI R2,R2,LSR #2
BHI fq_1
.fq_2
ADD R3,R1,R2
MOV R1,R1,LSR #1
CMP R0,R3
SUBHS R0,R0,R3
ADDHS R1,R1,R2
MOVS R2,R2,LSR #2
BNE fq_2
CMP R4,#0
MOVGE R0,R1,LSL R4
RSBLT R4,R4,#0
MOVLT R0,R1,LSR R4
LDMFD R13!,{R4,PC}
]
NEXT
RP%=rowpix
ENDPROC
