/* rsp_micro.S — pin unaligned lqv stitch semantics on THIS ares build.
 * DMEM 0x0000..: pattern bytes b[i]=(i%251)+1 (CPU-filled 128B)
 * Results -> DMEM 0x100..0x17F (read back by CPU). */
#include <rsp.inc>
    .set noreorder
    .set at
    .data
_dummy: .word 0
    .text
    .globl _start
_start:
    li   s0, 0x0000
    vsub $v00, $v00, $v00
    /* T1 aligned full (sanity) -> 0x100 */
    lqv  $v01, 0, s0
    sqv  $v01, 256, $0
    /* T2 unaligned e0 at +2 -> 0x110 */
    addi t1, s0, 2
    lqv  $v02, 0, t1
    sqv  $v02, 272, $0
    /* T3 aligned e14 at +16 -> 0x120 */
    addi t2, s0, 16
    lqv  $v03, 14, 0, t2
    sqv  $v03, 288, $0
    /* T4 stitch T2 then T3 -> 0x130 (shift-left-2-byte window +2..+17) */
    lqv  $v04, 0, t1
    lqv  $v04, 14, 0, t2
    sqv  $v04, 304, $0
    /* T5 reverse stitch order -> 0x140 (clobber check) */
    lqv  $v05, 14, 0, t2
    lqv  $v05, 0, t1
    sqv  $v05, 320, $0
    /* T6 unaligned e0 at +30 -> 0x150 (2-byte fill case) */
    addi t3, s0, 30
    lqv  $v06, 0, t3
    sqv  $v06, 336, $0
    /* T7 aligned e2 at +32 -> 0x160 (fills bytes 2..15?) */
    addi t4, s0, 32
    lqv  $v07, 2, 0, t4
    sqv  $v07, 352, $0
    /* T8 stitch T6 then T7 -> 0x170 (RIGHT window -2..+13) */
    lqv  $v10, 0, t3
    lqv  $v10, 2, 0, t4
    sqv  $v10, 368, $0
    /* ===== store tests, isolated AA-prefilled regions ===== */
    /* SA: unaligned e0 store at 0x402 (d=2) */
    li   t7, 0x402
    sqv  $v01, 0, t7
    /* SB: stitch at 0x442 then 0x450 */
    li   t1, 0x442
    sqv  $v01, 0, t1
    li   t2, 0x450
    sqv  $v01, 14, 0, t2
    /* SC: reverse stitch order */
    li   t3, 0x490
    sqv  $v01, 14, 0, t3
    li   t4, 0x482
    sqv  $v01, 0, t4
    break 0
    nop
