| spike_ph.s - EnetSpike protocol handler for the .ENET driver (spike version).
|
| Entry convention (ref: Macintosh OS Ethernet Driver design proposal, 1987-05-18, on bitsavers;
| confirmed by the DaynaPORT DRVR 47 code): A3 = just past the 14-byte data link header,
| A4 = ReadPacket (ReadRest is A4+2), D1 = bytes left in the packet, A0/A1 driver-private until
| ReadRest. Every register is saved and restored here. The context (a Ring, see enetspike.c)
| comes from spike_ph_ctx, patched into the code at run time; no A5 globals at interrupt time.
|
| Ring: head@0 tail@2 over@4 hook@6 slots@8; Slot (1568 bytes): len@0 sr@2 type@4 rr@6
| ticks@8 a3@12 a4@16 d1@20 sp@24 us0@28 us1@32 buf@36 (1536 bytes, header + data).
        .text
        .align  2
        .globl  spike_ph
        .globl  spike_ph_set_ctx
        .extern spike_hook_c

spike_ph_ctx:
        .long   0

| void spike_ph_set_ctx(void *ring)
spike_ph_set_ctx:
        move.l  4(%sp),%d0
        lea     spike_ph_ctx(%pc),%a0
        move.l  %d0,(%a0)
        rts

spike_ph:
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        move.w  %sr,%d6
        move.l  %sp,%d5
        move.l  spike_ph_ctx(%pc),%d0
        beq     .Ldrop
        move.l  %d0,%a2
        move.w  (%a2),%d0               | head
        move.w  %d0,%d2
        addq.w  #1,%d2
        and.w   #7,%d2                  | next head
        cmp.w   2(%a2),%d2
        beq     .Lfull
        mulu.w  #1572,%d0
        lea     8(%a2,%d0.l),%a5        | slot
        move.w  %d6,2(%a5)
        move.l  0x16A,%d0
        move.l  %d0,8(%a5)
        move.l  %a3,12(%a5)
        move.l  %a4,16(%a5)
        move.l  %d1,20(%a5)
        move.l  %d5,24(%a5)
        clr.l   28(%a5)
        move.l  %a2,%d7
        lea     36(%a5),%a2
        lea     -14(%a3),%a6
        moveq   #6,%d0
.Lh:    move.w  (%a6)+,(%a2)+
        dbf     %d0,.Lh
        move.w  -2(%a2),4(%a5)          | ethertype
        move.l  %a2,%a3
        move.w  #1522,%d3
        jsr     2(%a4)                  | ReadRest
        move.w  %d3,6(%a5)              | rr = D3 result
        move.l  %a3,%d0
        lea     36(%a5),%a0
        sub.l   %a0,%d0
        move.w  %d0,(%a5)               | len incl. header
        move.l  %d7,%a2
        clr.l   32(%a5)
        move.l  %d7,%a2
        move.w  %d2,(%a2)               | publish: head = next
        tst.w   6(%a2)
        beq     .Ldone
        clr.w   6(%a2)
        move.l  %a2,-(%sp)
        jsr     spike_hook_c
        addq.l  #4,%sp
        bra     .Ldone
.Lfull: addq.w  #1,4(%a2)
.Ldrop: moveq   #0,%d3
        jsr     2(%a4)                  | still must ReadRest
.Ldone: movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts

| Time Manager task entry: A1 = the task record (a TmRec, enetspike.c). Passes it to C.
        .globl  spike_tm
        .extern spike_tm_c
spike_tm:
        move.l  %a1,-(%sp)
        jsr     spike_tm_c
        addq.l  #4,%sp
        rts

| Deferred Task entry: A1 = dtParm (the Ring). Passes it to C.
        .globl  spike_dt
        .extern spike_dt_c
spike_dt:
        move.l  %a1,-(%sp)
        jsr     spike_dt_c
        addq.l  #4,%sp
        rts
