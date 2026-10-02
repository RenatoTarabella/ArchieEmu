REM >Juggler
REM The Juggler - Eric Graham's 1987 ray tracer in BBC BASIC V
REM Ray tracer (rt1.c) and scenes by Eric Graham, 1987, who
REM released them in 2026: "anyone can do what they want with
REM the code, so long as I get a mention!"
REM Scene files: robot.dat (the Juggler), ele.dat, dragon.dat
REM A%-H% are kept free for the assembler engine (USR/CALL)
MAXSP%=400:MAXLMP%=8
DIM SX(MAXSP%+MAXLMP%),SY(MAXSP%+MAXLMP%),SZ(MAXSP%+MAXLMP%),SR(MAXSP%+MAXLMP%)
DIM KR(MAXSP%+MAXLMP%),KG(MAXSP%+MAXLMP%),KB(MAXSP%+MAXLMP%),ST%(MAXSP%+MAXLMP%)
DIM FG%(MAXSP%+MAXLMP%),XN%(MAXSP%+MAXLMP%),XX%(MAXSP%+MAXLMP%),YN%(MAXSP%+MAXLMP%),YX%(MAXSP%+MAXLMP%)
DIM AC%(MAXSP%+1),AL%(MAXLMP%+1),HR(1),HG(1),HB(1)
DIM IM% 320*200*3
REM for the assembler: sphere table, lists of entries, parameters
DIM TB% 64*MAXSP%,LT% 64*MAXLMP%,ALL% 4*MAXSP%+4,LL% 4*MAXLMP%+4,RL% 4*MAXSP%+4,CB% 4*MAXSP%+4,GB% 256
PRINT "The Juggler - ray tracer by Eric Graham, 1987"
PRINT
INPUT "Scene (robot, ele, dragon) ? "SC$
IF SC$="" THEN SC$="robot"
INPUT "Detail (1=full, 2, 4, 8=fastest) ? "SK%
IF SK%<>2 AND SK%<>4 AND SK%<>8 THEN SK%=1
INPUT "Engine: 1=original (BASIC only), 2=fast (ARM assembler) ? "EN%
FA%=(EN%<>1)
INPUT "reflect() as in the 1987 listing (Y/N) ? "S$
RA%=(LEFT$(S$,1)="Y" OR LEFT$(S$,1)="y")
PRINT '"Screen modes:"
PRINT "  13 = 320 x 256, 256 colours"
PRINT "  15 = 640 x 256, 256 colours"
PRINT "  28 = 640 x 480, 256 colours (VGA monitor)"
PRINT "  49 = 640 x 480, 16M colours (emulator only, no Archimedes)"
INPUT "Mode ? "MO%
IF MO%<>13 AND MO%<>15 AND MO%<>28 AND MO%<>49 THEN MO%=13
INPUT "Save the image as (Return = don't save) ? "SV$
PROCload(FNscene(SC$))
PROCexpose
PROCproject
IF FA% THEN PROCassemble:PROCtable
TE%=0
OW%=1+(NX%-1) DIV SK%:OH%=1+(NY%-1) DIV SK%
MODE MO%
OFF
SYS "OS_ReadModeVariable",-1,4 TO ,,XE%
SYS "OS_ReadModeVariable",-1,5 TO ,,YE%
SYS "OS_ReadModeVariable",-1,11 TO ,,WI%
SYS "OS_ReadModeVariable",-1,12 TO ,,HE%
WI%+=1:HE%+=1
REM pixel size on screen: 2x2 in 640 x 480, 2x1 in 640 x 256
ZX%=WI% DIV 320:ZY%=HE% DIV 240:IF ZY%<1 THEN ZY%=1
X0%=(WI%-320*ZX%) DIV 2:Y0%=(HE%-200*ZY%) DIV 2
LAST%=-1
T%=TIME
PROCrender
T%=TIME-T%
IF SV$<>"" THEN SYS "OS_File",10,SV$,&FFD,,IM%,IM%+OW%*OH%*3
K%=GET
MODE 12
PRINT "Render time: ";T%/100;" seconds"
IF FA% THEN PRINT "Ray tracing in assembler: ";TE%/100;" seconds"
END

DEF FNscene(s$)
IF INSTR(s$,".")=0 AND INSTR(s$,"/")=0 THEN s$="Scenes."+s$+"/dat"
=s$

REM ---------------- scene file reader ----------------
DEF PROCload(f$)
LOCAL k%,n%,a%,t,kr,kg,kb,ty%,lx,ly,lz,lr,nx,ny,nz,nr,alt,az
FH%=OPENIN f$
IF FH%=0 THEN ERROR 214,"Scene "+f$+" not found"
PROCch
PROCvec(40,41):OX=V1:OY=V2:OZ=V3
PROCexp(91):alt=FNnum:PROCexp(44):az=FNnum:PROCexp(93)
FL=0.028*FNnum
REM observer exactly as in rt2.c (including its degtorad)
NX%=320:NY%=200:PX=1/NX%:PY=0.75/NY%
alt=alt*0.0174533:az=az*0.0174533
VX=COS(az)*COS(alt):VY=SIN(az)*COS(alt):VZ=SIN(alt)
UX=SIN(az):UY=-COS(az):UZ=0
WX=-COS(az)*SIN(alt):WY=-SIN(az)*SIN(alt):WZ=COS(alt)
REM objects: <r,g,b> type (x,y,z):r [n (x,y,z):r]... ;   ends with ;
NS%=0
WHILE FNpeek<>59
  PROCvec(60,62):kr=V1:kg=V2:kb=V3
  ty%=FNnum
  PROCsph:lx=V1:ly=V2:lz=V3:lr=V4
  WHILE FNpeek<>59
    n%=FNnum
    PROCsph:nx=V1:ny=V2:nz=V3:nr=V4
    REM as ssg: n+1 spheres from the last point up to the next one
    FOR a%=0 TO n%
      t=a%/(n%+1)
      PROCadd(lx+(nx-lx)*t,ly+(ny-ly)*t,lz+(nz-lz)*t,lr+(nr-lr)*t,kr,kg,kb,ty%)
    NEXT
    lx=nx:ly=ny:lz=nz:lr=nr
  ENDWHILE
  PROCadd(lx,ly,lz,lr,kr,kg,kb,ty%)
  PROCexp(59)
ENDWHILE
PROCexp(59)
NL%=FNnum
IF NL%>MAXLMP% THEN ERROR 214,"Too many lamps"
FOR k%=NS%+1 TO NS%+NL%
  PROCsph:SX(k%)=V1:SY(k%)=V2:SZ(k%)=V3:SR(k%)=V4
  PROCvec(60,62):KR(k%)=V1:KG(k%)=V2:KB(k%)=V3
NEXT
PROCvec(60,62):HR(0)=V1:HG(0)=V2:HB(0)=V3
PROCvec(60,62):HR(1)=V1:HG(1)=V2:HB(1)=V3
PROCvec(60,62):IR=V1:IG=V2:IB=V3
PROCvec(60,62):ZR=V1:ZG=V2:ZB=V3
PROCvec(60,62):YR=V1:YG=V2:YB=V3
CLOSE#FH%
ENDPROC

DEF PROCadd(x,y,z,r,kr,kg,kb,ty%)
IF NS%=MAXSP% THEN ERROR 214,"Too many spheres"
NS%+=1
SX(NS%)=x:SY(NS%)=y:SZ(NS%)=z:SR(NS%)=r
KR(NS%)=kr:KG(NS%)=kg:KB(NS%)=kb:ST%(NS%)=ty%
ENDPROC

DEF PROCch
IF EOF#FH% THEN C%=-1 ELSE C%=BGET#FH%
ENDPROC

DEF FNpeek
WHILE C%=32 OR C%=9 OR C%=10 OR C%=13:PROCch:ENDWHILE
IF C%=-1 THEN ERROR 214,"Unexpected end of scene file"
=C%

DEF PROCexp(c%)
IF FNpeek<>c% THEN ERROR 214,"Scene file: expected "+CHR$c%+" before "+CHR$C%
PROCch
ENDPROC

DEF FNnum
LOCAL n$
IF FNpeek=-1 THEN =0
WHILE INSTR("0123456789+-.eE",CHR$C%)
  n$+=CHR$C%:PROCch
ENDWHILE
IF n$="" THEN ERROR 214,"Scene file: expected a number before "+CHR$C%
=VAL(n$)

DEF PROCvec(o%,c%)
PROCexp(o%):V1=FNnum:PROCexp(44):V2=FNnum:PROCexp(44):V3=FNnum:PROCexp(c%)
ENDPROC

DEF PROCsph
PROCvec(40,41):PROCexp(58):V4=FNnum
ENDPROC

REM lamp brightness for the right exposure (lampfac in rt2.c)
DEF PROCexpose
LOCAL i%,j%,t,r,f,c,lc,il
f=1E10
FOR i%=1 TO NS%
  FOR j%=NS%+1 TO NS%+NL%
    r=SQR((SX(i%)-SX(j%))^2+(SY(i%)-SY(j%))^2+(SZ(i%)-SZ(j%))^2)-SR(i%)
    FOR k%=0 TO 2
      c=FNk(i%,k%):lc=FNk(j%,k%):il=FNill(k%)
      t=c*lc/(r*r)
      IF t<>0 THEN t=(1-c*il)/t:IF t<f THEN f=t
    NEXT
  NEXT
NEXT
FOR j%=NS%+1 TO NS%+NL%
  KR(j%)=KR(j%)*f:KG(j%)=KG(j%)*f:KB(j%)=KB(j%)*f
NEXT
ENDPROC
DEF FNk(i%,k%):IF k%=0 THEN =KR(i%) ELSE IF k%=1 THEN =KG(i%) ELSE =KB(i%)
DEF FNill(k%):IF k%=0 THEN =IR ELSE IF k%=1 THEN =IG ELSE =IB

REM ---------------- screen boxes, as ssg ----------------
REM ssg projects every sphere and lamp to a box on the screen and
REM tests a primary ray only against the boxes it crosses
DEF PROCproject
LOCAL k%,vx,vy,vz,d,l,nx,ny,nz,qx,qy,qz,a%,b%
FOR k%=1 TO NS%+NL%
  vx=SX(k%)-OX:vy=SY(k%)-OY:vz=SZ(k%)-OZ
  d=VX*vx+VY*vy+VZ*vz
  IF SR(k%)+d<0 THEN FG%(k%)=-1 ELSE IF d-SR(k%)<=0 THEN FG%(k%)=0 ELSE FG%(k%)=1
  IF FG%(k%)=1 THEN
    l=SQR(vx*vx+vy*vy+vz*vz):nx=vx/l:ny=vy/l:nz=vz/l
    REM horizontal edges: radius * (n x vhat)
    qx=(ny*WZ-nz*WY)*SR(k%):qy=(nz*WX-nx*WZ)*SR(k%):qz=(nx*WY-ny*WX)*SR(k%)
    a%=FNpx(vx+qx,vy+qy,vz+qz):b%=FNpx(vx-qx,vy-qy,vz-qz)
    IF a%<b% THEN XN%(k%)=a%-2:XX%(k%)=b%+2 ELSE XN%(k%)=b%-2:XX%(k%)=a%+2
    REM vertical edges: radius * (uhat x n)
    qx=(UY*nz-UZ*ny)*SR(k%):qy=(UZ*nx-UX*nz)*SR(k%):qz=(UX*ny-UY*nx)*SR(k%)
    a%=FNpy(vx+qx,vy+qy,vz+qz):b%=FNpy(vx-qx,vy-qy,vz-qz)
    IF a%<b% THEN YN%(k%)=a%-2:YX%(k%)=b%+2 ELSE YN%(k%)=b%-2:YX%(k%)=a%+2
  ENDIF
NEXT
ENDPROC
DEF FNpx(x,y,z)=FNclip(0.5*NX%+FL*(UX*x+UY*y+UZ*z)/(VX*x+VY*y+VZ*z)/PX)
DEF FNpy(x,y,z)=FNclip(0.5*NY%-FL*(WX*x+WY*y+WZ*z)/(VX*x+VY*y+VZ*z)/PY)
DEF FNclip(v):IF v<-10000 THEN =-10000 ELSE IF v>10000 THEN =10000 ELSE =INT(v)

REM ---------------- rendering ----------------
DEF PROCrender
LOCAL i%,j%,ii%,jj%,k%,m%,n%,x,y,tx,ty,tz,v%,o%
jj%=0
FOR j%=0 TO NY%-1 STEP SK%
  REM spheres and lamps whose box crosses this row
  m%=0:n%=0
  FOR k%=1 TO NS%+NL%
    IF FG%(k%)=0 OR (FG%(k%)=1 AND YN%(k%)<=j% AND j%<=YX%(k%)) THEN
      IF k%<=NS% THEN AC%(m%)=k%:RL%!(m%*4)=TB%+64*(k%-1):m%+=1 ELSE AL%(n%)=k%:n%+=1
    ENDIF
  NEXT
  AC%(m%)=-1:AL%(n%)=-1:RL%!(m%*4)=0
  IF FA% THEN
    y=(0.5*NY%-j%)*PY
    GB%!g_row=FNf(VX*FL+y*WX):GB%!(g_row+4)=FNf(VY*FL+y*WY):GB%!(g_row+8)=FNf(VZ*FL+y*WZ)
    GB%!g_out=IM%+jj%*OW%*3:H%=GB%
    o%=TIME:v%=USR(RP%):TE%+=TIME-o%
  ELSE
  ii%=0
  FOR i%=0 TO NX%-1 STEP SK%
    REM pixline(): the ray through pixel i%,j%
    y=(0.5*NY%-j%)*PY:x=(i%-0.5*NX%)*PX
    tx=VX*FL+y*WX+x*UX+OX:ty=VY*FL+y*WY+x*UY+OY:tz=VZ*FL+y*WZ+x*UZ+OZ
    PROCtrace(OX,OY,OZ,tx-OX,ty-OY,tz-OZ,i%)
    o%=IM%+(jj%*OW%+ii%)*3
    v%=128*BR+4:IF v%<0 THEN v%=0 ELSE IF v%>255 THEN v%=255
    ?o%=v%
    v%=128*BG+4:IF v%<0 THEN v%=0 ELSE IF v%>255 THEN v%=255
    o%?1=v%
    v%=128*BB+4:IF v%<0 THEN v%=0 ELSE IF v%>255 THEN v%=255
    o%?2=v%
    ii%+=1
  NEXT
  ENDIF
  o%=IM%+jj%*OW%*3
  FOR i%=0 TO NX%-1 STEP SK%
    PROCplot(i%,j%,?o%,o%?1,o%?2):o%+=3
  NEXT
  jj%+=1
NEXT
ENDPROC

REM one sample: a block of SK% x SK% Amiga pixels; the dump byte is
REM 128*brite+4, so brite 1 (byte 132) is full white on the screen
DEF PROCplot(i%,j%,r%,g%,b%)
LOCAL c%
r%=r%*2:IF r%>255 THEN r%=255
g%=g%*2:IF g%>255 THEN g%=255
b%=b%*2:IF b%>255 THEN b%=255
c%=b%<<24 OR g%<<16 OR r%<<8
IF c%<>LAST% THEN SYS "ColourTrans_SetGCOL",c%,,,0,0:LAST%=c%
RECTANGLE FILL (X0%+i%*ZX%)<<XE%,(HE%-Y0%-(j%+SK%)*ZY%)<<YE%,((SK%*ZX%)<<XE%)-1,((SK%*ZY%)<<YE%)-1
ENDPROC

REM ---------------- the ray tracer: rt1.c ----------------
REM The ray is Q0,Q1,Q2 + t*(Q3,Q4,Q5); QA = |dir|^2.
REM intsplin(): returns t, or 0 when the sphere is missed or behind
DEF FNhit(k%)
LOCAL b,c,d,p,t
p=Q0-SX(k%):b=Q3*p:c=p*p-SR(k%)*SR(k%)
p=Q1-SY(k%):b+=Q4*p:c+=p*p
p=Q2-SZ(k%):b+=Q5*p:c+=p*p
b=b+b
d=b*b-4*QA*c
IF d<=0 THEN =0
d=SQR(d):t=-(b+d)/(QA+QA)
IF t<0.001 THEN t=(d-b)/(QA+QA)
IF t>0.001 THEN =t
=0

REM raytrace(): result in BR,BG,BB. cx% is the screen column of a
REM primary ray (only spheres whose box contains it are tested), -1
REM for a mirror bounce (all of them)
DEF PROCtrace(ox,oy,oz,dx,dy,dz,cx%)
LOCAL tmin,sn%,ln%,k%,m%,t,px,py,pz,nx,ny,nz,a
Q0=ox:Q1=oy:Q2=oz:Q3=dx:Q4=dy:Q5=dz:QA=dx*dx+dy*dy+dz*dz
tmin=1E10:sn%=0
IF cx%>=0 THEN
  m%=0:k%=AC%(0)
  WHILE k%>0
    IF cx%>=XN%(k%) AND cx%<=XX%(k%) THEN t=FNhit(k%):IF t>0 AND t<tmin THEN tmin=t:sn%=k%
    m%+=1:k%=AC%(m%)
  ENDWHILE
  ln%=0:m%=0:k%=AL%(0)
  WHILE k%>0
    IF cx%>=XN%(k%) AND cx%<=XX%(k%) THEN t=FNhit(k%):IF t>0 AND t<tmin THEN tmin=t:ln%=k%
    m%+=1:k%=AL%(m%)
  ENDWHILE
ELSE
  k%=1
  WHILE k%<=NS%
    t=FNhit(k%):IF t>0 AND t<tmin THEN tmin=t:sn%=k%
    k%+=1
  ENDWHILE
  ln%=0
  FOR k%=NS%+1 TO NS%+NL%
    t=FNhit(k%):IF t>0 AND t<tmin THEN tmin=t:ln%=k%
  NEXT
ENDIF
IF ln% THEN
  REM we see a lamp
  a=SR(ln%)*SR(ln%):BR=KR(ln%)/a:BG=KG(ln%)/a:BB=KB(ln%)/a
  ENDPROC
ENDIF
IF dz<>0 THEN
  t=-oz/dz
  IF t>0.001 AND t<tmin THEN
    REM the ground: cheap vinyl
    px=ox+dx*t:py=oy+dy*t:pz=oz+dz*t
    k%=FNgingham(px,py)
    PROCpixbrite(px,py,pz,0,0,1,HR(k%),HG(k%),HB(k%),0)
    ENDPROC
  ENDIF
ENDIF
IF sn% THEN
  px=ox+dx*tmin:py=oy+dy*tmin:pz=oz+dz*tmin
  a=1/SR(sn%):nx=(px-SX(sn%))*a:ny=(py-SY(sn%))*a:nz=(pz-SZ(sn%))*a
  CASE ST%(sn%) OF
    WHEN 1:IF NOT FNglint(px,py,pz,nx,ny,nz,sn%,dx,dy,dz) THEN PROCpixbrite(px,py,pz,nx,ny,nz,KR(sn%),KG(sn%),KB(sn%),sn%)
    WHEN 2:PROCmirror(px,py,pz,nx,ny,nz,KR(sn%),KG(sn%),KB(sn%),dx,dy,dz)
    OTHERWISE:PROCpixbrite(px,py,pz,nx,ny,nz,KR(sn%),KG(sn%),KB(sn%),sn%)
  ENDCASE
  ENDPROC
ENDIF
REM nothing else, must be sky
a=dz*dz:a=a/(dx*dx+dy*dy+a)
BR=(1-a)*YR+a*ZR:BG=(1-a)*YG+a*ZG:BB=(1-a)*YB+a*ZB
ENDPROC

DEF FNgingham(x,y)
LOCAL kx%,ky%
IF x<0 THEN x=-x:kx%=1
IF y<0 THEN y=-y:ky%=1
IF x>1E9 THEN x=1E9
IF y>1E9 THEN y=1E9
=((INT(x)+kx%) DIV 3+(INT(y)+ky%) DIV 3) MOD 2

REM is the segment from the patch to lamp l% blocked by a sphere?
DEF FNshadow(px,py,pz,lx,ly,lz,self%)
LOCAL k%
Q0=px:Q1=py:Q2=pz:Q3=lx:Q4=ly:Q5=lz:QA=lx*lx+ly*ly+lz*lz
FOR k%=1 TO NS%
  IF k%<>self% THEN IF FNhit(k%)>0 THEN =TRUE
NEXT
=FALSE

REM pixbrite(): how bright is the patch?
DEF PROCpixbrite(px,py,pz,nx,ny,nz,cr,cg,cb,self%)
LOCAL l%,lx,ly,lz,c,r,d
REM the sky is a hemisphere lamp
d=(nz+1.5)*0.4
BR=d*IR*cr:BG=d*IG*cg:BB=d*IB*cb
FOR l%=NS%+1 TO NS%+NL%
  lx=SX(l%)-px:ly=SY(l%)-py:lz=SZ(l%)-pz
  c=lx*nx+ly*ny+lz*nz
  IF c>0 THEN
    IF NOT FNshadow(px,py,pz,lx,ly,lz,self%) THEN
      REM Lambert times 1/r^2
      r=SQR(lx*lx+ly*ly+lz*lz):c=c/(r*r*r)
      BR+=c*cr*KR(l%):BG+=c*cg*KG(l%):BB+=c*cb*KB(l%)
    ENDIF
  ENDIF
NEXT
ENDPROC

REM glint(): are we looking at a highlight?
DEF FNglint(px,py,pz,nx,ny,nz,self%,dx,dy,dz)
LOCAL l%,lx,ly,lz,t,f%,r2
f%=TRUE
FOR l%=NS%+1 TO NS%+NL%
  lx=SX(l%)-px:ly=SY(l%)-py:lz=SZ(l%)-pz
  IF lx*nx+ly*ny+lz*nz>0 THEN
    IF NOT FNshadow(px,py,pz,lx,ly,lz,self%) THEN
      IF f% THEN PROCreflect(dx,dy,dz,nx,ny,nz):r2=RX*RX+RY*RY+RZ*RZ:f%=FALSE
      t=lx*RX+ly*RY+lz*RZ
      t=t*t/((lx*lx+ly*ly+lz*lz)*r2)
      IF t>0.95 THEN BR=1:BG=1:BB=1:=TRUE
    ENDIF
  ENDIF
NEXT
=FALSE

REM mirror(): bounce the ray off the mirror
DEF PROCmirror(px,py,pz,nx,ny,nz,cr,cg,cb,dx,dy,dz)
IF nx*dx+ny*dy+nz*dz>=0 THEN BR=0:BG=0:BB=0:ENDPROC
PROCreflect(dx,dy,dz,nx,ny,nz)
REM recursion saves the day
PROCtrace(px,py,pz,RX,RY,RZ,-1)
BR=BR*cr:BG=BG*cg:BB=BB*cb
ENDPROC

REM reflect(): incoming ray x, unit normal n, result in RX,RY,RZ.
REM The 1987 listing has y=xv*v/(xn*n), which is wrong; the program
REM that rendered the Juggler (ssg) used y = x - 2(x.n)n
DEF PROCreflect(x1,x2,x3,n1,n2,n3)
LOCAL u1,u2,u3,v1,v2,v3,xn,xv
u1=x2*n3-x3*n2:u2=x3*n1-x1*n3:u3=x1*n2-x2*n1
IF u1=0 AND u2=0 AND u3=0 THEN RX=-x1:RY=-x2:RZ=-x3:ENDPROC
xn=x1*n1+x2*n2+x3*n3
IF NOT RA% THEN RX=x1-2*xn*n1:RY=x2-2*xn*n2:RZ=x3-2*xn*n3:ENDPROC
v1=u2*n3-u3*n2:v2=u3*n1-u1*n3:v3=u1*n2-u2*n1
xv=(x1*v1+x2*v2+x3*v3)/(v1*v1+v2*v2+v3*v3)
RX=FNdiv(xv*v1,xn*n1):RY=FNdiv(xv*v2,xn*n2):RZ=FNdiv(xv*v3,xn*n3)
ENDPROC
REM C gives an infinity where BASIC stops with "Division by zero"
DEF FNdiv(a,b)
IF b<>0 THEN =a/b
IF a=0 THEN =0
=SGN(a)*1E30

REM ---------------- the fast engine: ARM assembler ----------------
REM raytrace() of rt1.c in fixed point 16.16 (65536 = 1.0). The ARM2
REM multiplies only 32x32->32 bits: FNmul splits the operands in
REM halves. Rays use unit directions, so t is a distance and rt1's
REM SMALL (0.001 times the length of its direction) is kept per ray.
REM A quick filter in 1/64 units (cand) leaves only the spheres a
REM ray may hit; isect() then tests those exactly.
DEF FNf(v)=INT(v*65536+0.5)

DEF PROCtable
LOCAL k%,l%,e%,c
c=0
FOR k%=1 TO NS%+NL%
  IF ABS(SX(k%))+SR(k%)>c THEN c=ABS(SX(k%))+SR(k%)
  IF ABS(SY(k%))+SR(k%)>c THEN c=ABS(SY(k%))+SR(k%)
  IF ABS(SZ(k%))+SR(k%)>c THEN c=ABS(SZ(k%))+SR(k%)
NEXT
FOR k%=1 TO NS%
  e%=TB%+64*(k%-1)
  !e%=FNf(SX(k%)):e%!4=FNf(SY(k%)):e%!8=FNf(SZ(k%)):e%!12=FNf(SR(k%))
  e%!16=XN%(k%):e%!20=XX%(k%):e%!24=k%
  e%!28=FNf(SR(k%)*SR(k%)):e%!32=FNf(1/SR(k%))
  e%!36=FNf(KR(k%)):e%!40=FNf(KG(k%)):e%!44=FNf(KB(k%)):e%!48=ST%(k%)
  ALL%!(4*k%-4)=e%
NEXT
ALL%!(4*NS%)=0
FOR l%=1 TO NL%
  k%=NS%+l%:e%=LT%+64*(l%-1)
  !e%=FNf(SX(k%)):e%!4=FNf(SY(k%)):e%!8=FNf(SZ(k%)):e%!12=FNf(SR(k%))
  e%!24=k%:e%!28=FNf(SR(k%)*SR(k%))
  REM lamp colour (with lampfac) /256, and the brightness of the lamp itself
  e%!32=FNf(KR(k%)/256):e%!36=FNf(KG(k%)/256):e%!40=FNf(KB(k%)/256)
  e%!44=FNf(FNmin(KR(k%)/SR(k%)^2)):e%!48=FNf(FNmin(KG(k%)/SR(k%)^2)):e%!52=FNf(FNmin(KB(k%)/SR(k%)^2))
  LL%!(4*l%-4)=e%
NEXT
LL%!(4*NL%)=0
!GB%=FNf(OX):GB%!4=FNf(OY):GB%!8=FNf(OZ)
GB%!g_hor=FNf(YR):GB%!(g_hor+4)=FNf(YG):GB%!(g_hor+8)=FNf(YB)
GB%!g_zen=FNf(ZR):GB%!(g_zen+4)=FNf(ZG):GB%!(g_zen+8)=FNf(ZB)
GB%!g_ill=FNf(IR):GB%!(g_ill+4)=FNf(IG):GB%!(g_ill+8)=FNf(IB)
GB%!g_g0=FNf(HR(0)):GB%!(g_g0+4)=FNf(HG(0)):GB%!(g_g0+8)=FNf(HB(0))
GB%!(g_g0+12)=FNf(HR(1)):GB%!(g_g0+16)=FNf(HG(1)):GB%!(g_g0+20)=FNf(HB(1))
GB%!g_cm=INT(c*64)+64:GB%!g_all=ALL%:GB%!g_rl=RL%
GB%!g_lmp=LT%:GB%!g_nl=NL%:GB%!g_ll=LL%:GB%!g_cb=CB%
GB%!g_nx=NX%:GB%!g_sk=SK%
GB%!g_us=FNf(UX*PX*256):GB%!(g_us+4)=FNf(UY*PX*256):GB%!(g_us+8)=FNf(UZ*PX*256)
ENDPROC
DEF FNmin(v):IF v>1000 THEN =1000 ELSE =v

REM d=(a*b)>>16 for 16.16 numbers, rounded; t,u are scratch registers
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

DEF PROCassemble
LOCAL pass%,code%,c%
REM global block (R12)
g_o=0:g_hor=12:g_zen=24:g_ill=36:g_g0=48:g_cm=72:g_all=76:g_rl=80
g_lmp=84:g_nl=88:g_ll=92:g_cb=96:g_nx=100:g_sk=104:g_row=108:g_us=120
g_out=132:g_i=136:g_pb=140
REM ray record (R11): origin, unit direction, SMALL, screen column,
REM brightness, nearest t and sphere, the patch, lamps and shadow ray
r_o=0:r_u=12:r_sm=24:r_col=28:r_b=32:r_tm=44:r_hit=48:r_p=52:r_n=64
r_c=76:r_self=88:r_lp=92:r_r=104:r_r2=108:r_ul=112:r_ref=124:r_fl=136
r_dep=140:r_lmp=144:r_k=148:r_sh=152:r_cnt=184:r_lc=188:r_skip=192:RS=196
DIM code% 8192
FOR pass%=0 TO 2 STEP 2
P%=code%
[OPT pass%

; ---- rowpix, called with USR, H% = global block
; traces one row of the picture into the bytes at g_out
.rowpix
STMFD R13!,{R0-R12,R14}
MOV R12,R7
SUB R13,R13,#RS
MOV R11,R13
MOV R0,#0
STR R0,[R12,#g_i]
.rp_loop
LDR R0,[R12,#g_i]
LDR R1,[R12,#g_nx]
CMP R0,R1
BGE rp_done
; pixline() D = row + (i-160)*uhat*px
SUB R1,R0,#160
LDR R2,[R12,#g_us]
MUL R3,R1,R2
LDR R2,[R12,#g_row]
ADD R4,R2,R3,ASR #8
LDR R2,[R12,#g_us+4]
MUL R3,R1,R2
LDR R2,[R12,#g_row+4]
ADD R5,R2,R3,ASR #8
LDR R2,[R12,#g_us+8]
MUL R3,R1,R2
LDR R2,[R12,#g_row+8]
ADD R6,R2,R3,ASR #8
STR R4,[R11,#r_u]
STR R5,[R11,#r_u+4]
STR R6,[R11,#r_u+8]
OPT FNmul(7,4,4,8,9)
OPT FNmul(0,5,5,8,9)
ADD R7,R7,R0
OPT FNmul(0,6,6,8,9)
ADD R0,R7,R0
BL fsqrt
MOV R7,R0
MOV R1,#66
OPT FNmul(2,7,1,8,9)
STR R2,[R11,#r_sm]
MOV R0,#65536
MOV R1,R7
BL fdiv
MOV R7,R0
LDR R4,[R11,#r_u]
OPT FNmul(0,4,7,8,9)
STR R0,[R11,#r_u]
LDR R4,[R11,#r_u+4]
OPT FNmul(0,4,7,8,9)
STR R0,[R11,#r_u+4]
LDR R4,[R11,#r_u+8]
OPT FNmul(0,4,7,8,9)
STR R0,[R11,#r_u+8]
LDMIA R12,{R0-R2}
STMIA R11,{R0-R2}
LDR R0,[R12,#g_i]
STR R0,[R11,#r_col]
MOV R0,#0
STR R0,[R11,#r_dep]
STR R0,[R11,#r_skip]
BL trace
; brightness to bytes, (int)(128*brite+4) as in ssg
LDR R3,[R12,#g_out]
LDR R0,[R11,#r_b]
BL rp_byte
LDR R0,[R11,#r_b+4]
BL rp_byte
LDR R0,[R11,#r_b+8]
BL rp_byte
STR R3,[R12,#g_out]
LDR R0,[R12,#g_i]
LDR R1,[R12,#g_sk]
ADD R0,R0,R1
STR R0,[R12,#g_i]
B rp_loop
.rp_done
ADD R13,R13,#RS
LDMFD R13!,{R0-R12,PC}
.rp_byte
MOV R0,R0,ASR #9
ADD R0,R0,#4
CMP R0,#0
MOVLT R0,#0
CMP R0,#255
MOVGT R0,#255
STRB R0,[R3],#1
MOV PC,R14

; ---- trace(R11) - raytrace() of rt1.c, the result in r_b
.trace
STMFD R13!,{R14}
ADD R0,R11,#r_o
LDMIA R0,{R0-R5}
LDR R7,[R11,#r_col]
CMP R7,#0
LDRGE R6,[R12,#g_rl]
LDRLT R6,[R12,#g_all]
LDR R8,[R11,#r_skip]
BL findc
STR R0,[R11,#r_cnt]
MVN R1,#&80000000
STR R1,[R11,#r_tm]
MOV R1,#0
STR R1,[R11,#r_hit]
STR R1,[R11,#r_k]
.tr_c
LDR R0,[R11,#r_k]
LDR R1,[R11,#r_cnt]
CMP R0,R1
BGE tr_cd
LDR R1,[R12,#g_cb]
LDR R10,[R1,R0,LSL #2]
ADD R0,R0,#1
STR R0,[R11,#r_k]
BL isect
CMP R0,#0
BLE tr_c
LDR R1,[R11,#r_tm]
CMP R0,R1
STRLT R0,[R11,#r_tm]
STRLT R10,[R11,#r_hit]
B tr_c
.tr_cd
; are we looking at a lamp?
LDR R0,[R11,#r_hit]
CMP R0,#0
BNE tr_nl
ADD R0,R11,#r_o
LDMIA R0,{R0-R5}
LDR R6,[R12,#g_ll]
MVN R7,#0
MOV R8,#0
BL findc
CMP R0,#0
BEQ tr_nl
LDR R1,[R12,#g_cb]
LDR R10,[R1]
ADD R10,R10,#44
LDMIA R10,{R0-R2}
ADD R3,R11,#r_b
STMIA R3,{R0-R2}
LDMFD R13!,{PC}
.tr_nl
; do we see the ground?
LDR R1,[R11,#r_u+8]
LDR R0,[R11,#r_o+8]
RSBS R0,R0,#0
BEQ tr_ng
TEQ R0,R1
BMI tr_ng
CMP R1,#0
BEQ tr_ng
BL fdiv
CMP R0,#&70000000
MOVGT R0,#&70000000
LDR R1,[R11,#r_sm]
CMP R0,R1
BLE tr_ng
LDR R1,[R11,#r_tm]
CMP R0,R1
BGE tr_ng
MOV R6,R0
LDR R3,[R11,#r_u]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o]
ADD R0,R0,R1
STR R0,[R11,#r_p]
LDR R3,[R11,#r_u+4]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o+4]
ADD R0,R0,R1
STR R0,[R11,#r_p+4]
LDR R3,[R11,#r_u+8]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o+8]
ADD R0,R0,R1
STR R0,[R11,#r_p+8]
MOV R0,#0
STR R0,[R11,#r_n]
STR R0,[R11,#r_n+4]
STR R0,[R11,#r_self]
MOV R0,#65536
STR R0,[R11,#r_n+8]
; gingham() - tiles 3 units wide
MOV R2,#&5500
ADD R2,R2,#&56
LDR R0,[R11,#r_p]
CMP R0,#0
RSBLT R0,R0,#0
MOV R0,R0,LSR #16
ADDLT R0,R0,#1
MUL R1,R0,R2
MOV R1,R1,LSR #16
LDR R0,[R11,#r_p+4]
CMP R0,#0
RSBLT R0,R0,#0
MOV R0,R0,LSR #16
ADDLT R0,R0,#1
MUL R3,R0,R2
ADD R1,R1,R3,LSR #16
AND R1,R1,#1
ADD R2,R12,#g_g0
ADD R2,R2,R1,LSL #3
ADD R2,R2,R1,LSL #2
LDMIA R2,{R3-R5}
ADD R0,R11,#r_c
STMIA R0,{R3-R5}
BL pixbrite
LDMFD R13!,{PC}
.tr_ng
LDR R10,[R11,#r_hit]
CMP R10,#0
BEQ tr_sky
; we see a sphere - the patch and its normal
LDR R6,[R11,#r_tm]
LDR R7,[R10,#32]
LDR R3,[R11,#r_u]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o]
ADD R0,R0,R1
STR R0,[R11,#r_p]
LDR R1,[R10]
SUB R0,R0,R1
OPT FNmul(1,0,7,8,9)
STR R1,[R11,#r_n]
LDR R3,[R11,#r_u+4]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o+4]
ADD R0,R0,R1
STR R0,[R11,#r_p+4]
LDR R1,[R10,#4]
SUB R0,R0,R1
OPT FNmul(1,0,7,8,9)
STR R1,[R11,#r_n+4]
LDR R3,[R11,#r_u+8]
OPT FNmul(0,6,3,8,9)
LDR R1,[R11,#r_o+8]
ADD R0,R0,R1
STR R0,[R11,#r_p+8]
LDR R1,[R10,#8]
SUB R0,R0,R1
OPT FNmul(1,0,7,8,9)
STR R1,[R11,#r_n+8]
ADD R0,R10,#36
LDMIA R0,{R0-R2}
ADD R3,R11,#r_c
STMIA R3,{R0-R2}
STR R10,[R11,#r_self]
LDR R0,[R10,#48]
CMP R0,#2
BEQ tr_mirror
CMP R0,#1
BNE tr_dull
BL glint
CMP R0,#0
LDMNEFD R13!,{PC}
.tr_dull
BL pixbrite
LDMFD R13!,{PC}
.tr_sky
; nothing else, must be sky
LDR R6,[R11,#r_u+8]
OPT FNmul(7,6,6,8,9)
RSB R6,R7,#65536
MOV R5,#0
.tr_s
ADD R4,R12,R5
LDR R0,[R4,#g_hor]
OPT FNmul(1,0,6,8,9)
LDR R0,[R4,#g_zen]
OPT FNmul(2,0,7,8,9)
ADD R1,R1,R2
ADD R4,R11,R5
STR R1,[R4,#r_b]
ADD R5,R5,#4
CMP R5,#12
BLT tr_s
LDMFD R13!,{PC}
.tr_mirror
; mirror() - bounce the ray, recursion saves the day
ADD R0,R11,#r_u
LDMIA R0,{R0-R2}
ADD R3,R11,#r_n
LDMIA R3,{R3-R5}
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADDS R6,R6,R7
BGE tr_black
LDR R7,[R11,#r_dep]
CMP R7,#16
BGE tr_black
ADD R6,R6,R6
OPT FNmul(7,6,3,8,9)
SUB R0,R0,R7
OPT FNmul(7,6,4,8,9)
SUB R1,R1,R7
OPT FNmul(7,6,5,8,9)
SUB R2,R2,R7
STMFD R13!,{R11}
SUB R13,R13,#RS
ADD R3,R13,#r_u
STMIA R3,{R0-R2}
ADD R3,R11,#r_p
LDMIA R3,{R0-R2}
STMIA R13,{R0-R2}
LDR R0,[R11,#r_sm]
STR R0,[R13,#r_sm]
MVN R0,#0
STR R0,[R13,#r_col]
LDR R0,[R11,#r_dep]
ADD R0,R0,#1
STR R0,[R13,#r_dep]
; a ray leaving a sphere cannot meet it again - rt1 tests it anyway
; and rejects it with SMALL, too tight for fixed point
LDR R0,[R11,#r_self]
STR R0,[R13,#r_skip]
MOV R11,R13
BL trace
ADD R0,R11,#r_b
LDMIA R0,{R4-R6}
ADD R13,R13,#RS
LDMFD R13!,{R11}
LDR R3,[R11,#r_c]
OPT FNmul(0,4,3,8,9)
STR R0,[R11,#r_b]
LDR R3,[R11,#r_c+4]
OPT FNmul(0,5,3,8,9)
STR R0,[R11,#r_b+4]
LDR R3,[R11,#r_c+8]
OPT FNmul(0,6,3,8,9)
STR R0,[R11,#r_b+8]
LDMFD R13!,{PC}
.tr_black
MOV R0,#0
STR R0,[R11,#r_b]
STR R0,[R11,#r_b+4]
STR R0,[R11,#r_b+8]
LDMFD R13!,{PC}

; ---- pixbrite(R11) - how bright is the patch?
.pixbrite
STMFD R13!,{R14}
LDR R0,[R11,#r_n+8]
ADD R0,R0,#&18000
MOV R1,#&6600
ADD R1,R1,#103
OPT FNmul(2,0,1,8,9)
MOV R5,#0
.pb_d
ADD R4,R12,R5
LDR R0,[R4,#g_ill]
OPT FNmul(3,2,0,8,9)
ADD R4,R11,R5
LDR R0,[R4,#r_c]
OPT FNmul(6,3,0,8,9)
STR R6,[R4,#r_b]
ADD R5,R5,#4
CMP R5,#12
BLT pb_d
LDR R0,[R12,#g_lmp]
STR R0,[R11,#r_lmp]
LDR R0,[R12,#g_nl]
STR R0,[R11,#r_lc]
.pb_l
LDR R0,[R11,#r_lc]
SUBS R0,R0,#1
LDMLTFD R13!,{PC}
STR R0,[R11,#r_lc]
LDR R10,[R11,#r_lmp]
BL lampvis
CMP R0,#0
BEQ pb_next
; Lambert times 1/r^2 cosi/r^3 = (ul.n)/r^2
ADD R0,R11,#r_ul
LDMIA R0,{R0-R2}
ADD R3,R11,#r_n
LDMIA R3,{R3-R5}
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADD R6,R6,R7
MOV R5,#0
.pb_c
LDR R10,[R11,#r_lmp]
ADD R10,R10,R5
LDR R0,[R10,#32]
LDR R1,[R11,#r_r2]
BL fdiv
OPT FNmul(7,6,0,8,9)
ADD R4,R11,R5
LDR R0,[R4,#r_c]
OPT FNmul(1,7,0,8,9)
LDR R2,[R4,#r_b]
ADD R2,R2,R1
STR R2,[R4,#r_b]
ADD R5,R5,#4
CMP R5,#12
BLT pb_c
.pb_next
LDR R0,[R11,#r_lmp]
ADD R0,R0,#64
STR R0,[R11,#r_lmp]
B pb_l

; ---- glint(R11) - are we looking at a highlight? R0=1 if so
.glint
STMFD R13!,{R14}
MOV R0,#1
STR R0,[R11,#r_fl]
LDR R0,[R12,#g_lmp]
STR R0,[R11,#r_lmp]
LDR R0,[R12,#g_nl]
STR R0,[R11,#r_lc]
.gl_l
LDR R0,[R11,#r_lc]
SUBS R0,R0,#1
MOVLT R0,#0
LDMLTFD R13!,{PC}
STR R0,[R11,#r_lc]
LDR R10,[R11,#r_lmp]
BL lampvis
CMP R0,#0
BEQ gl_next
LDR R0,[R11,#r_fl]
CMP R0,#0
BEQ gl_t
MOV R0,#0
STR R0,[R11,#r_fl]
; reflect() the incoming ray
ADD R0,R11,#r_u
LDMIA R0,{R0-R2}
ADD R3,R11,#r_n
LDMIA R3,{R3-R5}
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADD R6,R6,R7
ADD R6,R6,R6
OPT FNmul(7,6,3,8,9)
SUB R0,R0,R7
OPT FNmul(7,6,4,8,9)
SUB R1,R1,R7
OPT FNmul(7,6,5,8,9)
SUB R2,R2,R7
ADD R3,R11,#r_ref
STMIA R3,{R0-R2}
.gl_t
ADD R0,R11,#r_ul
LDMIA R0,{R0-R2}
ADD R3,R11,#r_ref
LDMIA R3,{R3-R5}
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADD R6,R6,R7
OPT FNmul(7,6,6,8,9)
MOV R0,#&F300
ADD R0,R0,#51
CMP R7,R0
BLE gl_next
MOV R0,#65536
STR R0,[R11,#r_b]
STR R0,[R11,#r_b+4]
STR R0,[R11,#r_b+8]
MOV R0,#1
LDMFD R13!,{PC}
.gl_next
LDR R0,[R11,#r_lmp]
ADD R0,R0,#64
STR R0,[R11,#r_lmp]
B gl_l

; ---- lampvis(R11, R10 = lamp) - R0=1 if the lamp lights the patch;
; leaves lp, r, r^2/256 and the unit vector ul to the lamp
.lampvis
STMFD R13!,{R14}
LDMIA R10,{R0-R2}
LDR R3,[R11,#r_p]
SUB R0,R0,R3
LDR R3,[R11,#r_p+4]
SUB R1,R1,R3
LDR R3,[R11,#r_p+8]
SUB R2,R2,R3
ADD R3,R11,#r_lp
STMIA R3,{R0-R2}
ADD R3,R11,#r_n
LDMIA R3,{R3-R5}
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADDS R6,R6,R7
MOVLE R0,#0
LDMLEFD R13!,{PC}
MOV R3,R0,ASR #4
OPT FNmul(6,3,3,8,9)
MOV R3,R1,ASR #4
OPT FNmul(7,3,3,8,9)
ADD R6,R6,R7
MOV R3,R2,ASR #4
OPT FNmul(7,3,3,8,9)
ADD R0,R6,R7
STR R0,[R11,#r_r2]
BL fsqrt
MOV R0,R0,LSL #4
STR R0,[R11,#r_r]
MOV R1,#66
OPT FNmul(2,0,1,8,9)
STR R2,[R11,#r_sh+24]
MOV R5,#0
.lv_u
ADD R6,R11,R5
LDR R0,[R6,#r_lp]
LDR R1,[R11,#r_r]
BL fdiv
ADD R6,R11,R5
STR R0,[R6,#r_ul]
STR R0,[R6,#r_sh+12]
LDR R0,[R6,#r_p]
STR R0,[R6,#r_sh]
ADD R5,R5,#4
CMP R5,#12
BLT lv_u
; is a sphere in the way?
ADD R0,R11,#r_sh
LDMIA R0,{R0-R5}
LDR R6,[R12,#g_all]
MVN R7,#0
LDR R8,[R11,#r_self]
BL findc
STR R0,[R11,#r_cnt]
MOV R0,#0
STR R0,[R11,#r_k]
.lv_l
LDR R0,[R11,#r_k]
LDR R1,[R11,#r_cnt]
CMP R0,R1
MOVGE R0,#1
LDMGEFD R13!,{PC}
LDR R1,[R12,#g_cb]
LDR R10,[R1,R0,LSL #2]
ADD R0,R0,#1
STR R0,[R11,#r_k]
STMFD R13!,{R11}
ADD R11,R11,#r_sh
BL isect
LDMFD R13!,{R11}
CMP R0,#0
BLE lv_l
MOV R0,#0
LDMFD R13!,{PC}

; ---- isect(R11 = ray, R10 = sphere) - intsplin(), R0 = t or 0
.isect
STMFD R13!,{R14}
LDMIA R10,{R0-R2}
LDR R3,[R11]
SUB R0,R0,R3
LDR R3,[R11,#4]
SUB R1,R1,R3
LDR R3,[R11,#8]
SUB R2,R2,R3
ADD R3,R11,#12
LDMIA R3,{R3-R5}
ORR R6,R0,R1
ORR R6,R6,R2
EOR R7,R0,R0,ASR #31
EOR R8,R1,R1,ASR #31
ORR R7,R7,R8
EOR R8,R2,R2,ASR #31
ORRS R7,R7,R8
CMP R7,#&400000
BHS is_far
; b = e.u
OPT FNmul(6,0,3,8,9)
OPT FNmul(7,1,4,8,9)
ADD R6,R6,R7
OPT FNmul(7,2,5,8,9)
ADD R6,R6,R7
STMFD R13!,{R6}
; distance from the ray - |e x u|^2
OPT FNmul(6,1,5,8,9)
OPT FNmul(7,2,4,8,9)
SUB R6,R6,R7
OPT FNmul(7,6,6,8,9)
OPT FNmul(6,2,3,8,9)
OPT FNmul(2,0,5,8,9)
SUB R6,R6,R2
OPT FNmul(2,6,6,8,9)
ADD R7,R7,R2
OPT FNmul(6,0,4,8,9)
OPT FNmul(2,1,3,8,9)
SUB R6,R6,R2
OPT FNmul(2,6,6,8,9)
ADD R7,R7,R2
LDR R0,[R10,#28]
SUBS R0,R0,R7
LDMFD R13!,{R6}
MOVLE R0,#0
LDMLEFD R13!,{PC}
STMFD R13!,{R6}
BL fsqrt
LDMFD R13!,{R6}
LDR R7,[R11,#24]
SUB R1,R6,R0
CMP R1,R7
ADDLE R1,R6,R0
CMP R1,R7
MOVGT R0,R1
MOVLE R0,#0
LDMFD R13!,{PC}
.is_far
; a sphere far from the origin (shadows of distant ground) - t = e.u
MOV R0,R0,ASR #10
MOV R1,R1,ASR #10
MOV R2,R2,ASR #10
MOV R3,R3,ASR #7
MOV R4,R4,ASR #7
MOV R5,R5,ASR #7
MUL R6,R0,R3
MLA R6,R1,R4,R6
MLA R6,R2,R5,R6
MOV R0,R6,LSL #1
LDR R7,[R11,#24]
CMP R0,R7
MOVLE R0,#0
LDMFD R13!,{PC}

; ---- findc - R0-R2 origin, R3-R5 unit direction (16.16), R6 list,
; R7 screen column (-1 none), R8 entry to skip. R0 = number of
; candidates, left at g_cb
.findc
STMFD R13!,{R7-R11,R14}
ADD R9,R12,#g_pb
STR R7,[R9,#4]
STR R8,[R9,#8]
LDR R10,[R12,#g_cb]
STR R10,[R9,#12]
EOR R10,R0,R0,ASR #31
EOR R11,R1,R1,ASR #31
CMP R11,R10
MOVGT R10,R11
EOR R11,R2,R2,ASR #31
CMP R11,R10
MOVGT R10,R11
MOV R10,R10,LSR #10
LDR R11,[R12,#g_cm]
ADD R10,R10,R11
MOV R11,#10
.fc_s
CMP R10,#16000
MOVHS R10,R10,LSR #1
ADDHS R11,R11,#1
BHS fc_s
STR R11,[R9]
MOV R0,R0,ASR R11
MOV R1,R1,ASR R11
MOV R2,R2,ASR R11
MOV R3,R3,ASR #2
MOV R4,R4,ASR #2
MOV R5,R5,ASR #2
MOV R7,R9
BL cand
LDR R10,[R12,#g_cb]
SUB R0,R0,R10
MOV R0,R0,LSR #2
LDMFD R13!,{R7-R11,PC}

; ---- cand - in R0-R2 the origin, R3-R5 the unit direction *16384,
; R6 the list of entries, R7 the parameters (shift, column, entry to
; skip, output pointer). The spheres whose distance from the ray is
; below radius + 10/64 and that are not wholly behind it go to the
; output. Returns the end of the output
.cand
STMFD R13!,{R8-R12,R14}
.cf_loop
LDR R10,[R6],#4
CMP R10,#0
BEQ cf_done
LDR R12,[R7,#8]
CMP R10,R12
BEQ cf_loop
LDR R12,[R7,#4]
CMP R12,#0
BLT cf_nocol
LDR R14,[R10,#16]
CMP R12,R14
BLT cf_loop
LDR R14,[R10,#20]
CMP R12,R14
BGT cf_loop
.cf_nocol
LDR R12,[R7]
LDR R8,[R10]
RSB R8,R0,R8,ASR R12
LDR R9,[R10,#4]
RSB R9,R1,R9,ASR R12
LDR R11,[R10,#8]
RSB R11,R2,R11,ASR R12
LDR R14,[R10,#12]
MOV R14,R14,ASR R12
ADD R14,R14,#10
MUL R12,R8,R3
MLA R12,R9,R4,R12
MLA R12,R11,R5,R12
CMN R14,R12,ASR #14
BLE cf_loop
MUL R12,R9,R5
RSB R11,R11,#0
MLA R12,R11,R4,R12
MOV R12,R12,ASR #14
MUL R10,R12,R12
MUL R12,R11,R3
MLA R12,R8,R5,R12
MOV R12,R12,ASR #14
MLA R10,R12,R12,R10
MUL R12,R8,R4
RSB R9,R9,#0
MLA R12,R9,R3,R12
MOV R12,R12,ASR #14
MLA R10,R12,R12,R10
MUL R12,R14,R14
CMP R10,R12
BGE cf_loop
LDR R10,[R6,#-4]
LDR R12,[R7,#12]
STR R10,[R12],#4
STR R12,[R7,#12]
B cf_loop
.cf_done
LDR R0,[R7,#12]
LDMFD R13!,{R8-R12,PC}

; ---- fdiv R0=(R0<<16)/R1, signed, saturated. Uses R0-R4
.fdiv
EOR R4,R0,R1
CMP R0,#0
RSBLT R0,R0,#0
CMP R1,#0
RSBLT R1,R1,#0
MOV R2,R0,LSR #16
MOV R0,R0,LSL #16
CMP R2,R1
BHS fd_ovf
MOV R3,#32
.fd_l
ADDS R0,R0,R0
ADCS R2,R2,R2
CMPCC R2,R1
SUBCS R2,R2,R1
ORRCS R0,R0,#1
SUBS R3,R3,#1
BNE fd_l
CMP R4,#0
RSBLT R0,R0,#0
MOV PC,R14
.fd_ovf
MVN R0,#&80000000
CMP R4,#0
RSBLT R0,R0,#0
MOV PC,R14

; ---- fsqrt R0=sqrt(R0) in 16.16. Uses R0-R3
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
