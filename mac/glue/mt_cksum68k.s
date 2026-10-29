| mt_cksum68k.s - 68000 versions of mt_cksum_add and mt_cksum_copy (core/mt_util.h).
|
| Contract (pinned by tests/test_util.c, checked on the 68k by mac/asmtest): the result is the
| canonical 16-bit ones-complement sum of the carried-in sum (any uint32) plus the buffer read as
| big-endian 16-bit words counted from p (an odd last byte is a high byte): 0 only if the sum
| and every byte are 0, else the value in 1..0xFFFF congruent to the exact sum mod 0xFFFF. It
| is bit-identical to the C versions in core/mt_util.c (built when MT_ASM_CKSUM is not defined).
| Precondition: len <= 65535 (the dbf block counters are 16 bit).
|
| Calling convention (Retro68 m68k-apple-macos-gcc, checked with -S): arguments on the stack,
| result in d0, d0-d1/a0-a1 scratch (Retro68 also treats d2 as scratch; we save it anyway).
| Every argument is 32 bit (uint32_t or a pointer), so the stack offsets are the same with or
| without -mshort. Saved: d2-d7/a2 (7 registers, 28 bytes), so after the movem the arguments
| are at 32(sp), 36(sp), 40(sp), 44(sp).
|
| Algorithm. Sums run over big-endian long words with an addx chain (move.l and dbf leave X
| alone), and the final X is absorbed with one addx of zero. 2^16 == 1 mod 0xFFFF, so a long
| word b0b1b2b3 counts as b0b1 + b2b3 and the 32-bit end-around sum is congruent to the 16-bit
| word sum. The chain starts with X = 0, so it can never reach acc = 0xFFFFFFFF with X = 1 and
| the absorbing addx never carries. The accumulator is folded to 16 bits once per call.
| An odd start address is handled with one leading byte b0 (worth b0 << 8) and the rest summed
| from the now even address; that rest has its byte roles swapped, so its folded sum is rotated
| by 8 (RFC 1071 byte-order independence). No word or long access ever touches an odd address:
| every move.w/move.l below runs only after the pointers were made even.
|
| mt_cksum_copy cases, most frequent first in the stack:
|  1. src and dst both even (send path: txbuf payload at 54 from an even caller buffer; receive
|     path: frame payload into an even ring position): one pass, move.l read + move.l write +
|     addx per long word, 8 long words per loop iteration. If dst is 2 mod 4 (txbuf + 54) one
|     leading word long-aligns dst first (free on the 68000, saves a bus cycle per long on
|     68020/030).
|  2. src and dst both odd: one leading byte, then case 1 with the rotated rest.
|  3. src and dst of different parity (a WDS run that starts at an odd payload offset, an odd
|     ring position): no word move can be aligned on both sides, and on the 68000 move.b
|     (a0)+,(a1)+ (12 cycles/byte, 16 per loop) beats word reads with shifted byte writes, so
|     the bytes are copied first and then the even one of the two buffers is summed with the
|     long-word sum loop (about 5 cycles/byte).

        .text
        .align  2

| FOLD16 reg, tmp: reg = canonical 16-bit end-around fold of reg (0 stays 0)
        .macro  FOLD16 reg, tmp
        move.l  \reg,\tmp
        swap    \tmp
        add.w   \tmp,\reg
        moveq   #0,\tmp
        addx.w  \tmp,\reg
        and.l   #0xFFFF,\reg
        .endm

| ---------------------------------------------------------------------------------------
| uint32_t mt_cksum_add(uint32_t sum, const uint8_t *p, uint32_t len)
        .globl  mt_cksum_add
mt_cksum_add:
        movem.l %d2-%d7/%a2,-(%sp)
        move.l  36(%sp),%a0             | p
        move.l  40(%sp),%d1             | len
        moveq   #0,%d4                  | head term (odd start byte << 8)
        moveq   #0,%d5                  | swap flag
        tst.l   %d1
        beq.s   1f
        move.w  %a0,%d0
        btst    #0,%d0
        beq.s   1f
        move.b  (%a0)+,%d4              | odd start: one leading byte
        lsl.w   #8,%d4
        subq.l  #1,%d1
        moveq   #1,%d5
1:      moveq   #0,%d0
        bsr.s   sum_even
        bra     finish

| ---------------------------------------------------------------------------------------
| sum_even: a0 = even pointer, d1 = len, d0 = initial accumulator (< 0x10000).
| Returns d0 = fold16(fold16(acc) + tail word + tail byte << 8). Clobbers d2, d3, d6, d7, a0.
sum_even:
        move.l  %d1,%d2
        lsr.l   #5,%d2                  | 32-byte blocks
        move.l  %d1,%d3
        lsr.w   #2,%d3
        and.w   #7,%d3                  | remaining long words
        sub.l   %d7,%d7                 | d7 = 0, X = 0 (start of the addx chain)
        bra.s   2f
1:      move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        addx.l  %d6,%d0
2:      dbf     %d2,1b
        bra.s   4f
3:      move.l  (%a0)+,%d6
        addx.l  %d6,%d0
4:      dbf     %d3,3b
        addx.l  %d7,%d0                 | absorb the last carry (never carries again)
        FOLD16  %d0,%d2
        btst    #1,%d1
        beq.s   5f
        moveq   #0,%d6
        move.w  (%a0)+,%d6
        add.l   %d6,%d0
5:      btst    #0,%d1
        beq.s   6f
        moveq   #0,%d6
        move.b  (%a0)+,%d6
        lsl.w   #8,%d6
        add.l   %d6,%d0
6:      FOLD16  %d0,%d2
        rts

| ---------------------------------------------------------------------------------------
| finish: d0 = folded part, d4 = head term, d5 = swap flag; sum at 32(sp). Returns.
finish:
        tst.b   %d5
        beq.s   1f
        rol.w   #8,%d0                  | the part started at an odd offset of the data
1:      add.l   %d4,%d0
        move.l  32(%sp),%d1             | carried-in sum: add both halves
        moveq   #0,%d2
        move.w  %d1,%d2
        add.l   %d2,%d0
        swap    %d1
        move.w  %d1,%d2
        add.l   %d2,%d0                 | <= 0xFFFF + 0xFF00 + 2 * 0xFFFF
        FOLD16  %d0,%d2
        movem.l (%sp)+,%d2-%d7/%a2
        rts

| ---------------------------------------------------------------------------------------
| uint32_t mt_cksum_copy(uint32_t sum, uint8_t *dst, const uint8_t *src, uint32_t len)
        .globl  mt_cksum_copy
mt_cksum_copy:
        movem.l %d2-%d7/%a2,-(%sp)
        move.l  36(%sp),%a1             | dst
        move.l  40(%sp),%a0             | src
        move.l  44(%sp),%d1             | len
        moveq   #0,%d4
        moveq   #0,%d5
        move.w  %a0,%d0
        move.w  %a1,%d2
        eor.w   %d0,%d2
        btst    #0,%d2
        bne     mixed
        moveq   #0,%d0                  | initial accumulator
        tst.l   %d1
        beq     9f
        move.w  %a0,%d2
        btst    #0,%d2
        beq.s   1f
        move.b  (%a0)+,%d4              | both odd: one leading byte
        move.b  %d4,(%a1)+
        lsl.w   #8,%d4
        subq.l  #1,%d1
        moveq   #1,%d5
1:      moveq   #2,%d2
        cmp.l   %d2,%d1
        bcs.s   2f
        move.w  %a1,%d2
        btst    #1,%d2
        beq.s   2f
        move.w  (%a0)+,%d0              | dst is 2 mod 4: leading word seeds the accumulator
        move.w  %d0,(%a1)+
        subq.l  #2,%d1
2:      move.l  %d1,%d2
        lsr.l   #5,%d2                  | 32-byte blocks
        move.l  %d1,%d3
        lsr.w   #2,%d3
        and.w   #7,%d3                  | remaining long words
        sub.l   %d7,%d7                 | d7 = 0, X = 0
        bra.s   4f
3:      move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
        move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
4:      dbf     %d2,3b
        bra.s   6f
5:      move.l  (%a0)+,%d6
        move.l  %d6,(%a1)+
        addx.l  %d6,%d0
6:      dbf     %d3,5b
        addx.l  %d7,%d0
        FOLD16  %d0,%d2
        btst    #1,%d1
        beq.s   7f
        moveq   #0,%d6
        move.w  (%a0)+,%d6
        move.w  %d6,(%a1)+
        add.l   %d6,%d0
7:      btst    #0,%d1
        beq.s   8f
        moveq   #0,%d6
        move.b  (%a0)+,%d6
        move.b  %d6,(%a1)+
        lsl.w   #8,%d6
        add.l   %d6,%d0
8:      FOLD16  %d0,%d2
9:      bra     finish

| src and dst differ in parity: copy bytes, then sum whichever buffer is even.
mixed:
        move.l  %a0,%a2                 | sum src if it is even ...
        btst    #0,%d0                  | (d0.w = low word of src)
        beq.s   1f
        move.l  %a1,%a2                 | ... else dst, which then is even
1:      move.l  %d1,%d2
        lsr.l   #4,%d2                  | 16-byte blocks
        move.l  %d1,%d3
        and.w   #15,%d3
        bra.s   3f
2:      move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
        move.b  (%a0)+,(%a1)+
3:      dbf     %d2,2b
        bra.s   5f
4:      move.b  (%a0)+,(%a1)+
5:      dbf     %d3,4b
        move.l  %a2,%a0
        moveq   #0,%d0
        bsr     sum_even
        bra     finish
