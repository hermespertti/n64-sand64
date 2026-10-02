/* sand64 fused RSP ucode — per-row pipeline (contract = CPU live steps):
 *   slab k=nsim-1 .. 0:
 *     V(k): cur(k),bel(k)->final row k+1 written back
 *     (slab row k+1 now finalized; row k = post-V)
 *     D(k): reads post-V row k + finalized row k+1; cur->S1; bel_final->S2
 *     merge S1 -> row k
 *     H(k): reads post-D row k; finals -> S1; merge S1 -> row k
 *     merge S2 -> row k+1   (only when k < nsim-1; bottom: lands in halo, skipped)
 * Snapshot-vs-live equivalence proven for all three phases (unique claimant
 * per dest/source + AIR-is-invariant argument; see main.c comment block).
 * lqv/sqv probed semantics (test/rspmicro):
 *   win B+2 = lqv e0@(B+2); lqv e14,16,B   win B-2 = lqv e0,(B-2); lqv e2,0,B
 * GW=160 u16=320B/row, 8 cells/vec, NVEC=20, NSIM=8.
 * DMEM: hdr@0 BE u16 nsim, u8 dirflag@2 (1 => d=-1/h=+1); consts@0x10..;
 *   slab@0x0080 (<=0x0C00); masks@0x0D00..0x0D5F (CPU-written per frame, ABOVE slab end 0x0CE0); S1@0x0D80; S2@0x0EC0 (=S1+320, row-below writes alias into S2).
 */
#include <rsp.inc>
    .set noreorder
    .set at
    .equ SLAB,  0x0080
    .equ NVEC,  20
    .equ MFF,   0x0BE0        /* not used */
    .equ TBL,   0x0D00        /* table base (16-aligned) */
    .equ D0M,   0x0BF0
    .equ D19M,  0x0C00
    .equ D0I,   0x0C10
    .equ D19I,  0x0C20
    .equ H0M,   0x0C30
    .equ H19M,  0x0C40
    .equ H0I,   0x0C50
    .equ H19I,  0x0C60
    .equ S1,    0x0D80
    .equ S2,    0x0EC0   /* == S1+320; S1+t5+320 aliases here */
/* dsel: s5 = DMEM addr of D source-validity mask for vector t4
 * all-FF @TBL ; edge mask @TBL+16 (CPU bakes per dir).
 * v==0 -> edge ; v==19 -> edge+? (two edge slots: v0 @+32, v19 @+48... simplify:
 * CPU writes TBL (all FF default for interior load), TBL+32 = v0 mask,
 * TBL+48 = v19 mask. */
    .equ DTBL0, TBL+16
    .equ DTBL19, TBL+32
    .macro dsel
    li   s5, TBL
    beqz t4, 1f
    li   t8, 19
    bne  t4, t8, 2f
    nop
    li   s5, DTBL19
    j    2f
    nop
1:  li   s5, DTBL0
2:
    .endm
    .equ HTBL0, TBL+48
    .equ HTBL19, TBL+64
    .macro hsel
    li   s5, TBL
    beqz t4, 1f
    li   t8, 19
    bne  t4, t8, 2f
    nop
    li   s5, HTBL19
    j    2f
    nop
1:  li   s5, HTBL0
2:
    .endm
    .data
_dummy: .word 0
    .text
    .globl _start
_start:
    vsub $v00, $v00, $v00
    lhu  t0, 0($0)
    slti t8, t0, 1
    bnez t8, ncl
    nop
    slti t8, t0, 9                 /* NSIM=8 */
    beqz t8, ncl
    nop
    j    ncl2
    nop
ncl: li t0, 0
ncl2: sh   t0, 0($0)                 /* halfword: must NOT clobber dirflag@2 / mask@4 */
    lbu  s7, 2($0)                 /* dirflag: 1 => d=-1 (asc), h=+1 */
    lbu  s4, 4($0)                 /* phase mask: 1=V 2=D 4=H */
    lbu  s2, 5($0)                 /* row0 skip: k=0 is grid row 0 (sky): no source moves */
    sb   s7, 6($0)                 /* echo dirflag for verify */
    sb   s2, 7($0)
    lqv  $v16, 16, $0
    lqv  $v17, 32, $0
    lqv  $v18, 48, $0
    lqv  $v19, 64, $0
    lqv  $v20, 80, $0
    addi s6, t0, -1                /* k */

rloop:
    bltz s6, rdone
    nop
    bnez s6, 9f
    nop
    beqz s2, 9f                    /* k==0 in a mid-grid chunk: process */
    nop
    j    rnext                     /* k==0 == grid row 0 (sky): never a source */
    nop
9:
    sll  t1, s6, 8
    sll  t2, s6, 6
    add  t1, t1, t2
    li   t3, SLAB
    add  t1, t1, t3                /* row k base */

/* ---- V(k) ascending ---- */
    andi t8, s4, 1
    beqz t8, vdone
    nop
    li   t4, NVEC
vv:
    addi t4, t4, -1
    bltz t4, vdone
    nop
    sll  t5, t4, 4
    add  s0, t1, t5
    lqv  $v01, 0, s0
    lqv  $v02, 320, s0
    vsubc $v26, $v01, $v17
    vsub  $v03, $v00, $v00         /* lt2 */
    vsubc $v26, $v01, $v20
    vsub  $v04, $v00, $v00         /* lt5 */
    vnor  $v05, $v03, $v03
    vand  $v06, $v05, $v04         /* mov */
    vsubc $v26, $v02, $v16
    vsub  $v07, $v00, $v00         /* belAIR */
    vand  $v08, $v06, $v07
    vsubc $v26, $v17, $v01
    vsub  $v21, $v00, $v00
    vsubc $v26, $v01, $v17
    vsub  $v22, $v00, $v00
    vor   $v21, $v21, $v22
    vnor  $v21, $v21, $v21         /* cur==2 */
    vsubc $v26, $v19, $v01
    vsub  $v22, $v00, $v00
    vsubc $v26, $v01, $v19
    vsub  $v27, $v00, $v00
    vor   $v22, $v22, $v27
    vnor  $v22, $v22, $v22         /* cur==4 */
    vor   $v21, $v21, $v22         /* dense */
    vsubc $v26, $v18, $v02
    vsub  $v23, $v00, $v00
    vsubc $v26, $v02, $v18
    vsub  $v28, $v00, $v00
    vor   $v23, $v23, $v28
    vnor  $v23, $v23, $v23         /* belWATER */
    vand  $v24, $v21, $v23         /* sink */
    vor   $v10, $v08, $v24         /* moved */
    vnor  $v11, $v10, $v10
    vand  $v12, $v01, $v11
    vand  $v13, $v24, $v18
    vor   $v12, $v12, $v13
    sqv   $v12, 0, s0              /* row k updated in place */
    vand  $v14, $v02, $v11
    vand  $v15, $v10, $v01
    vor   $v14, $v14, $v15
    sqv   $v14, 320, s0            /* row k+1 finalized */
    j    vv
    nop

/* ---- D(k): snapshot semantics; scans mirror dir ---- */
vdone:
    andi t8, s4, 2
    beqz t8, ddone
    nop
    beqz s7, dpos
    li   t4, NVEC                /* d=-1 first: start 20, dv decrements to 19 */
    j    dv
    nop
dpos:
    li   t4, -1                  /* d=+1: start -1, dvb increments to 0 */
dv:
    beqz s7, dvb
    nop
    addi t4, t4, -1
    bltz t4, dmerge
    nop
    j    dload
    nop
dvb:
    addi t4, t4, 1
    slti t8, t4, NVEC
    beqz t8, dmerge
    nop
dload:
    sll  t5, t4, 4
    add  s0, t1, t5
    sub  t7, s0, t1
    addi t7, t7, S1                /* S1 slot for cell j */
    lqv  $v01, 0, s0               /* cur (post-V) */
    lqv  $v02, 320, s0             /* bel (final row k+1) */
    bnez s7, dneg
    nop
    /* d=+1: belD win (B+320)+2 ; smat win B-2 */
    addi t6, s0, 322
    lqv  $v03, 0, t6
    lqv  $v03, 14, 336, s0
    addi t6, s0, -2
    lqv  $v04, 0, t6
    lqv  $v04, 2, 0, s0
    dsel
    j    dc
    nop
dneg:
    /* d=-1: belD win (B+320)-2 ; smat win B+2 */
    addi t6, s0, 318
    lqv  $v03, 0, t6
    lqv  $v03, 2, 320, s0
    addi t6, s0, 2
    lqv  $v04, 0, t6
    lqv  $v04, 14, 16, s0
    dsel
dc:
    lqv  $v29, 0, s5               /* source-validity mask */
    /* mov = 2<=cur<5 */
    vsubc $v26, $v01, $v17
    vsub  $v05, $v00, $v00
    vsubc $v26, $v01, $v20
    vsub  $v06, $v00, $v00
    vnor  $v05, $v05, $v05
    vand  $v05, $v05, $v06
    vand  $v05, $v05, $v29         /* mov & src-valid */
    /* dAIR = belD<1 ; dWAT = belD==3 */
    vsubc $v26, $v03, $v16
    vsub  $v06, $v00, $v00
    vsubc $v26, $v03, $v18
    vsub  $v07, $v00, $v00
    vsubc $v26, $v18, $v03
    vsub  $v08, $v00, $v00
    vor   $v07, $v07, $v08
    vnor  $v07, $v07, $v07         /* dWAT */
    /* dense(cur) */
    vsubc $v26, $v01, $v17
    vsub  $v09, $v00, $v00
    vsubc $v26, $v17, $v01
    vsub  $v21, $v00, $v00
    vor   $v09, $v09, $v21
    vnor  $v09, $v09, $v09
    vsubc $v26, $v01, $v19
    vsub  $v21, $v00, $v00
    vsubc $v26, $v19, $v01
    vsub  $v22, $v00, $v00
    vor   $v21, $v21, $v22
    vnor  $v21, $v21, $v21
    vor   $v09, $v09, $v21         /* dense */
    vand  $v10, $v09, $v07         /* sink */
    vor   $v11, $v06, $v10
    vand  $v11, $v05, $v11         /* moved */
    vnor  $v12, $v11, $v11
    vand  $v01, $v01, $v12
    vand  $v10, $v10, $v18
    vor   $v01, $v01, $v10         /* cur_new */
    sqv   $v01, 0, t7
    /* incoming into bel(j) from smat(j-d): always recompute+merge */
    vsubc $v26, $v04, $v17
    vsub  $v12, $v00, $v00
    vsubc $v26, $v04, $v20
    vsub  $v13, $v00, $v00
    vnor  $v12, $v12, $v12
    vand  $v12, $v12, $v13         /* movS */
    vsubc $v26, $v04, $v18
    vsub  $v13, $v00, $v00
    vsubc $v26, $v18, $v04
    vsub  $v14, $v00, $v00
    vor   $v13, $v13, $v14
    vnor  $v13, $v13, $v13         /* smat==3 */
    vsubc $v26, $v04, $v17
    vsub  $v14, $v00, $v00
    vsubc $v26, $v17, $v04
    vsub  $v21, $v00, $v00
    vor   $v14, $v14, $v21
    vnor  $v14, $v14, $v14         /* smat==2 */
    vsubc $v26, $v04, $v19
    vsub  $v21, $v00, $v00
    vsubc $v26, $v19, $v04
    vsub  $v22, $v00, $v00
    vor   $v21, $v21, $v22
    vnor  $v21, $v21, $v21         /* smat==4 */
    vor   $v14, $v14, $v21         /* densS */
    vsubc $v26, $v02, $v16
    vsub  $v21, $v00, $v00         /* belAIR */
    vsubc $v26, $v02, $v18
    vsub  $v22, $v00, $v00
    vsubc $v26, $v18, $v02
    vsub  $v23, $v00, $v00
    vor   $v22, $v22, $v23
    vnor  $v22, $v22, $v22         /* belWAT */
    vand  $v23, $v14, $v22
    vor   $v21, $v21, $v23
    vand  $v12, $v12, $v21         /* inc */
    vnor  $v13, $v12, $v12
    vand  $v02, $v02, $v13
    vand  $v04, $v04, $v12
    vor   $v02, $v02, $v04         /* bel_final */
    addi  t6, t7, 320
    sqv   $v02, 0, t6              /* S2 slot */
    j    dv
    nop

/* ---- merge S1 -> row k AND S2 -> row k+1 ---- */
dmerge:
    li   t4, NVEC
m1:
    addi t4, t4, -1
    bltz t4, m2
    nop
    sll  t5, t4, 4
    addi t6, t5, S1
    lqv  $v13, 0, t6
    add  t6, t1, t5
    sqv  $v13, 0, t6
    j    m1
    nop
m2:
    li   t4, NVEC
m2b:
    addi t4, t4, -1
    bltz t4, ddone
    nop
    sll  t5, t4, 4
    addi t6, t5, S2
    lqv  $v14, 0, t6
    addi t7, t1, 320
    add  t7, t7, t5
    sqv  $v14, 0, t7
    j    m2b
    nop
ddone:

/* ---- H(k): h=-d; snapshot -> S1 ---- */
hstart:
    andi t8, s4, 4
    beqz t8, hdone
    nop
    lqv  $v31, 64, $0              /* sentinel: const 4 */
    li   t6, S1
    sqv  $v31, 0, t6
    li   t4, NVEC
hv:
    addi t4, t4, -1
    bltz t4, hmerge
    nop
    sll  t5, t4, 4
    add  s0, t1, t5
    sub  t7, s0, t1
    addi t7, t7, S1
    lqv  $v01, 0, s0               /* cur (post-D, merged) */
    bnez t4, nohp
    nop
    sqv  $v01, 96, $0              /* probe: row cur cells for v0 -> 0x60 */
nohp:
    bnez s7, hneg
    nop
    /* h=-1: nb win B-2, smat win B+2 */
    addi t6, s0, -2
    lqv  $v02, 0, t6
    lqv  $v02, 2, 0, s0
    addi t6, s0, 2
    lqv  $v03, 0, t6
    lqv  $v03, 14, 16, s0
    hsel
    j    hc
    nop
hneg:
    /* h=+1: nb win B+2, smat win B-2 */
    addi t6, s0, 2
    lqv  $v02, 0, t6
    lqv  $v02, 14, 16, s0
    addi t6, s0, -2
    lqv  $v03, 0, t6
    lqv  $v03, 2, 0, s0
    hsel
hc:
    lqv  $v29, 0, s5
    vsubc $v26, $v01, $v17
    vsub  $v04, $v00, $v00
    vsubc $v26, $v01, $v20
    vsub  $v05, $v00, $v00
    vnor  $v04, $v04, $v04
    vand  $v06, $v04, $v05
    vand  $v06, $v06, $v29         /* mov & src-valid */
    vsubc $v26, $v02, $v16
    vsub  $v07, $v00, $v00         /* nbAIR */
    vand  $v08, $v06, $v07         /* moved */
    vsubc $v26, $v03, $v17
    vsub  $v09, $v00, $v00
    vsubc $v26, $v03, $v20
    vsub  $v21, $v00, $v00
    vnor  $v09, $v09, $v09
    vand  $v09, $v09, $v21         /* movS */
    vsubc $v26, $v01, $v16
    vsub  $v22, $v00, $v00         /* curAIR */
    vand  $v10, $v09, $v22         /* inc */
    vnor  $v11, $v08, $v08
    vand  $v01, $v01, $v11
    vand  $v03, $v03, $v10
    vor   $v01, $v01, $v03
    bnez t4, nohp2
    nop
    sqv  $v01, 112, $0             /* probe: cnew v0 -> 0x70 */
nohp2:
    sqv   $v01, 0, t7
    j    hv
    nop

/* ---- merge S1 -> row k (S2 merged in D phase) ---- */
hmerge:
    li   t4, NVEC
m3:
    addi t4, t4, -1
    bltz t4, hdone
    nop
    sll  t5, t4, 4
    addi t6, t5, S1
    lqv  $v13, 0, t6
    add  t6, t1, t5
    sqv  $v13, 0, t6
    j    m3
    nop
hdone:
rnext:
    addi s6, s6, -1
    j    rloop
    nop
rdone:
    break 0
    nop