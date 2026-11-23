addi 3, 4, -2
addis 3, 0, 0x7fff
addic 3, 4, -1
addic. 3, 4, 5
subfic 3, 4, 100
mulli 5, 6, -7
ori 3, 4, 0x1234
oris 3, 4, 0xffff
xori 3, 4, 1
xoris 3, 4, 1
andi. 3, 4, 0xff
andis. 3, 4, 0x8000
cmpwi 7, 3, -5
cmplwi 2, 3, 0xffff
twi 4, 3, 0
tw 31, 0, 0
add 3, 4, 5
addo. 3, 4, 5
addc 3, 4, 5
addco 3, 4, 5
adde 3, 4, 5
addeo. 3, 4, 5
addze 3, 4
addmeo 3, 4
subf 3, 4, 5
subfo. 3, 4, 5
subfc 3, 4, 5
subfe 3, 4, 5
subfze 3, 4
subfme. 3, 4
neg 3, 4
nego 3, 4
mullw 3, 4, 5
mullwo. 3, 4, 5
mulhw 3, 4, 5
mulhwu. 3, 4, 5
divw 3, 4, 5
divwuo 3, 4, 5
and 3, 4, 5
andc. 3, 4, 5
or 3, 4, 5
orc 3, 4, 5
xor 3, 4, 5
nand 3, 4, 5
nor 3, 4, 5
eqv 3, 4, 5
slw 3, 4, 5
srw. 3, 4, 5
sraw 3, 4, 5
srawi 3, 4, 31
cntlzw 3, 4
extsb 3, 4
extsh. 3, 4
cmpw 1, 3, 4
cmplw 6, 3, 4
rlwinm 3, 4, 5, 6, 7
rlwimi. 3, 4, 31, 0, 31
rlwnm 3, 4, 5, 28, 3
lwz 3, 129(4)
lwzu 3, -4(1)
lbz 3, 0(0)
lbzu 3, 1(4)
stw 3, 8(1)
stwu 1, -16(1)
stb 3, 0(4)
stbu 3, 1(4)
lhz 3, 2(4)
lhzu 3, 2(4)
lha 3, 2(4)
lhau 3, 2(4)
sth 3, 2(4)
sthu 3, 2(4)
lmw 29, 8(1)
stmw 29, 8(1)
lfs 1, 0(3)
lwzx 3, 4, 5
lwzux 3, 4, 5
lbzx 3, 4, 5
lhax 3, 4, 5
stwx 3, 4, 5
stbux 3, 4, 5
lwbrx 3, 4, 5
lhbrx 3, 4, 5
stwbrx 3, 4, 5
sthbrx 3, 4, 5
lwarx 3, 4, 5
stwcx. 3, 4, 5
lswi 5, 4, 7
stswi 5, 4, 0
ba 0x100
bclr 20, 0
bclrl 12, 1
bcctr 20, 0
bcctrl 4, 6
crand 1, 2, 3
cror 4, 5, 6
crxor 7, 8, 9
crnand 1, 2, 3
crnor 1, 2, 3
creqv 1, 2, 3
crandc 1, 2, 3
crorc 1, 2, 3
mcrf 2, 5
mfcr 3
mtcrf 0x81, 3
mfspr 3, 8
mtspr 9, 3
mfspr 3, 1008
mtspr 528, 4
mfspr 5, 287
mtspr 22, 6
mfmsr 3
mtmsr 3
mtsr 5, 3
mfsr 3, 15
mtsrin 3, 4
mfsrin 3, 4
sc
rfi
isync
sync
eieio
tlbie 4
dcbz 3, 4
dcbf 3, 4
dcbst 3, 4
dcbt 3, 4
dcbi 3, 4
icbi 3, 4
fadd 1, 2, 3
fadds. 1, 2, 3
fsub 4, 5, 6
fsubs 4, 5, 6
fmul 1, 2, 3
fmuls 1, 2, 3
fdiv 1, 2, 3
fdivs 1, 2, 3
fmadd 1, 2, 3, 4
fmadds 1, 2, 3, 4
fmsub 1, 2, 3, 4
fmsubs. 1, 2, 3, 4
fnmadd 1, 2, 3, 4
fnmadds 1, 2, 3, 4
fnmsub 1, 2, 3, 4
fnmsubs 1, 2, 3, 4
fsel 1, 2, 3, 4
fres 1, 2
frsqrte 1, 2
fsqrt 1, 2
fcmpu 3, 1, 2
fcmpo 7, 1, 2
frsp 1, 2
fctiw 1, 2
fctiwz. 1, 2
fneg 1, 2
fmr 1, 2
fnabs 1, 2
fabs 1, 2
mffs 5
mtfsf 255, 3
mtfsfi 7, 3
mtfsb0 24
mtfsb1 3
mcrfs 2, 5
lfs 1, 0(3)
lfsu 1, 4(3)
lfd 1, -8(3)
lfdu 1, 8(3)
stfs 1, 0(3)
stfsu 1, 4(3)
stfd 1, 8(3)
stfdu 1, 8(3)
lfsx 1, 3, 4
lfsux 1, 3, 4
lfdx 1, 3, 4
lfdux 1, 3, 4
stfsx 1, 3, 4
stfsux 1, 3, 4
stfdx 1, 3, 4
stfdux 1, 3, 4
stfiwx 1, 3, 4
