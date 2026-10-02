/* rsp_sand64.S — V-phase (fall down + sink through water) on the RSP.
 *
 * One lane = one uint16 cell. AIR=0 WALL=1 SAND=2 WATER=3 SEED=4:
 * movable = 2<=cur<5, dense={2,4}, belowAIR = below<1.
 *
 * lqv/sqv immediates are BYTE offsets (mult of 16, +-1008 encodable).
 * Row stride = GW*2 = 320B, so `below` uses immediate 320 off the same
 * base. NVEC=20 vectors/row. RSP JUMPS HAVE DELAY SLOTS — every `j`
 * is followed by `nop` (a missing nop silently decremented t0 per
 * vector loop and broke everything).
 *
 * COMPARES: vsubc sets VCOL where vs<vt (unsigned); "vsub vd,$v00,$v00"
 * folds borrow chain to 0xFFFF lanes. $v00 kept zero. EQ(a,b) =
 * NOR(VCOL(b<a), VCOL(a<b)).
 *
 * DMEM: hdr u32 nsim @0x0000; 8xu16 constants ONE@0x10 TWO@0x20
 * THREE@0x30 FOUR@0x40 FIVE@0x50; slab @0x0080 (nsim+1 rows x 320B);
 * trace @0x1800 (RSP_TRACE).
 */

#include <rsp.inc>

    .set noreorder
    .set at

    .equ SLAB,   0x0080
    .equ NVEC,   20

    .data
    .align 8
_dummy: .word 0

    .text
    .globl _start
_start:
    vsub $v00, $v00, $v00          /* $v00 = 0 */
    lhu  t0, 0($0)                 /* nsim (BE u16 at DMEM 0) */
    slti t8, t0, 1                 /* garbage/zero? clamp 0 */
    bnez t8, ncl
    nop
    slti t8, t0, 17          /* sane? keep */
    beqz t8, ncl
    nop
    j    ncl2
    nop
ncl: li t0, 0
ncl2: sw   t0, 0($0)               /* write clamped nsim back */
    lqv  $v16, 16, $0              /* ONE   @0x10 */
    lqv  $v17, 32, $0              /* TWO   @0x20 */
    lqv  $v18, 48, $0              /* THREE @0x30 */
    lqv  $v19, 64, $0              /* FOUR  @0x40 */
    lqv  $v20, 80, $0              /* FIVE  @0x50 */
    addi t0, t0, -1                /* k = nsim-1 */

rowloop:
    bltz t0, fin
    nop
    sll  t1, t0, 8                 /* k*256 */
    sll  t2, t0, 6                 /* k*64 */
    add  t1, t1, t2                /* k*320 */
    li   t3, SLAB
    add  t1, t1, t3                /* row k base */
    li   t4, NVEC

vecloop:
    addi t4, t4, -1
    bltz t4, nextrow
    nop
    sll  t5, t4, 4
    add  s0, t1, t5                /* cur base (8 cells) */

    lqv  $v01, 0, s0               /* cur   */
    lqv  $v02, 320, s0             /* below = same cols, row+1 */

    vsubc $v26, $v01, $v17
    vsub  $v03, $v00, $v00         /* lt2 = cur<2 */
    vsubc $v26, $v01, $v20
    vsub  $v04, $v00, $v00         /* lt5 = cur<5 */
    vnor  $v05, $v03, $v03
    vand  $v06, $v05, $v04         /* movable */

    vsubc $v26, $v02, $v16
    vsub  $v07, $v00, $v00         /* belowAIR */
    vand  $v08, $v06, $v07         /* moveair */

    vsubc $v26, $v17, $v01
    vsub  $v21, $v00, $v00
    vsubc $v26, $v01, $v17
    vsub  $v22, $v00, $v00
    vnor  $v21, $v21, $v22         /* cur==SAND */
    vsubc $v26, $v19, $v01
    vsub  $v22, $v00, $v00
    vsubc $v26, $v01, $v19
    vsub  $v27, $v00, $v00
    vnor  $v22, $v22, $v27         /* cur==SEED */
    vor   $v21, $v21, $v22         /* dense */

    vsubc $v26, $v18, $v02
    vsub  $v23, $v00, $v00
    vsubc $v26, $v02, $v18
    vsub  $v28, $v00, $v00
    vnor  $v23, $v23, $v28         /* below==WATER */
    vand  $v24, $v21, $v23         /* sink */

    vor   $v10, $v08, $v24         /* moved */

    vnor  $v11, $v10, $v10
    vand  $v12, $v01, $v11
    vand  $v13, $v24, $v18
    vor   $v12, $v12, $v13         /* cur_new */
    sqv   $v12, 0, s0

    vand  $v14, $v02, $v11
    vand  $v15, $v10, $v01
    vor   $v14, $v14, $v15         /* below_new */
    sqv   $v14, 320, s0

#ifdef RSP_TRACE
    addiu t8, t0, -1               /* fire at k==1, first vector */
    bnez  t8, notrc
    nop
    bnez  t4, notrc
    nop
    li    t8, 0x1800
    sqv   $v01, 0, t8
    sqv   $v02, 16, t8
    sqv   $v10, 32, t8
    sqv   $v12, 48, t8
    sqv   $v14, 64, t8
    sqv   $v16, 80, t8
notrc:
#endif
    j    vecloop
    nop
nextrow:
    addi t0, t0, -1
    j    rowloop
    nop
fin:
    break 0
    nop
