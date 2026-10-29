| mt_enet_ph.s - protocol handler stub for the .ENET driver (DaynaPORT DRVR 47), Task 9.
|
| Entry convention (ref: "Macintosh OS Ethernet Driver" design proposal, 1987-05-18, bitsavers;
| obs: the DRVR 47 disassembly and the EnetSpike runs, docs/dev/MACTCP-INTERFACE.md):
|   A3 = just past the 14-byte data link header (the header is the 14 bytes before A3),
|   A4 = ReadPacket (ReadRest is A4+2), D1 = bytes of the packet still unread, A0/A1 driver
|   private until ReadRest, SR = 0x2600 (supervisor, interrupt mask 6) in Snow. Return with RTS.
| The packet is already in the driver's buffer; one ReadRest copies it (data after the header).
| The driver may call this stub from inside its own EWrite (nested), so the stub never uses a
| shared scratch buffer: it reads straight into the next free slot of the gate's ring and then
| pokes the gate (mt_gate_poke runs the core now if nobody owns it, else the owner drains the
| slot later). Every register is saved and restored. The MtGate pointer is patched into the code
| at run time (mt_enet_ph_ctx), so no A5 globals are touched at interrupt time.
|
| Layout of MtGate / MtRxSlot (mt_gate.h; checked by _Static_assert in mt_enet.c):
        .equ    GATE_HEAD, 0
        .equ    GATE_TAIL, 2
        .equ    GATE_DROPS, 4
        .equ    GATE_SLOT0, 8
        .equ    GATE_NSLOTS, 8
        .equ    SLOT_SIZE, 1524
        .equ    SLOT_BUF, 4
        .equ    READ_MAX, 1520-14      | ReadRest D3: room for data after the header

        .text
        .align  2
        .globl  mt_enet_ph
        .globl  mt_enet_ph_set_ctx
        .extern mt_gate_poke

mt_enet_ph_ctx:
        .long   0

| void mt_enet_ph_set_ctx(MtGate *g): patch the context pointer into the code (0 = closed)
mt_enet_ph_set_ctx:
        move.l  4(%sp),%d0
        lea     mt_enet_ph_ctx(%pc),%a0
        move.l  %d0,(%a0)
        rts

mt_enet_ph:
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        move.l  mt_enet_ph_ctx(%pc),%d0
        beq     .Ldrain                  | closed: still read the packet out
        move.l  %d0,%a2                  | a2 = MtGate
        move.w  GATE_HEAD(%a2),%d2       | d2 = head (ReadRest preserves D2 and A0-A2)
        move.w  %d2,%d4
        addq.w  #1,%d4
        cmp.w   #GATE_NSLOTS,%d4
        bne     .Lnw
        moveq   #0,%d4
.Lnw:   cmp.w   GATE_TAIL(%a2),%d4
        beq     .Lfull                   | ring full: drop
        mulu.w  #SLOT_SIZE,%d2
        lea     GATE_SLOT0(%a2,%d2.l),%a5   | a5 = slot
        lea     -14(%a3),%a6             | source: the 14 header bytes before A3
        lea     SLOT_BUF(%a5),%a3        | destination
        moveq   #6,%d0
.Lh:    move.w  (%a6)+,(%a3)+            | 7 words (2-aligned in the driver, checked in Snow)
        dbf     %d0,.Lh
        move.w  #READ_MAX,%d3
        jsr     2(%a4)                   | ReadRest: A3 = one past the last byte read
        move.l  %a3,%d0
        lea     SLOT_BUF(%a5),%a0
        sub.l   %a0,%d0
        move.w  %d0,(%a5)                | slot length (header included)
        move.w  GATE_HEAD(%a2),%d2       | commit: head = head + 1 (mod slots)
        addq.w  #1,%d2
        cmp.w   #GATE_NSLOTS,%d2
        bne     .Lc
        moveq   #0,%d2
.Lc:    move.w  %d2,GATE_HEAD(%a2)
        move.l  %a2,-(%sp)
        jsr     mt_gate_poke             | C: runs the core now, or leaves it to the owner
        addq.l  #4,%sp
        bra     .Ldone
.Lfull: addq.w  #1,GATE_DROPS(%a2)
.Ldrain:
        moveq   #0,%d3
        jsr     2(%a4)                   | ReadRest with nothing wanted: discards the packet
.Ldone: movem.l (%sp)+,%d0-%d7/%a0-%a6
        rts
