| mt_drv_stubs.s - .IPP driver entry dispatcher and the calls into application code (Task 10).
|
| The DRVR resource (mac/moof/moof.r) is: the 18-byte header, the name "\p.ipp" padded to 24, six
| two-instruction stubs (open, prime, control, status, close, boot) that load a selector into D0
| and branch to offset 48, where this flat Retro68 code block starts. Offset 0 of the block is
| Retro68's _entry_trampoline, which jumps (PC-relative) to `_start` below with all registers
| untouched. So `_start` sees D0 = selector, and for 0..4 the Device Manager's registers:
| A0 = the parameter block, A1 = the DCE.
|
|   0 open, 4 close, 2 control: call the C entry, return its result in D0 with RTS.
|   1 prime, 3 status: queued calls; finish through jIODone (A1 = DCE, D0 = result) unless the
|     call is immediate (ioTrap bit 9), which returns with RTS.
|   5 boot (from the INIT, C convention, before relocation): call Retro68Relocate through a
|     run-time displacement (absolute addresses are not valid before it ran), then jump to
|     mt_drv_boot. The 68000 has no instruction cache, so the patched code runs at once.
| Every register but D0 is preserved for the Device Manager (it keeps using A0/A1 after a driver
| routine returns: the synchronous wait loop reads ioResult through A0).
        .text
        .align  2
        .globl  _start
        .globl  mt_drv_call_completion
        .globl  mt_drv_call_asr_tcp
        .globl  mt_drv_call_asr_udp
        .extern mt_drv_open
        .extern mt_drv_prime
        .extern mt_drv_control
        .extern mt_drv_status
        .extern mt_drv_close
        .extern mt_drv_boot
        .extern Retro68Relocate

        .equ    JIODONE, 0x08FC

_start:
        cmp.w   #5,%d0
        beq.s   .Lboot
        movem.l %d1-%d7/%a0-%a6,-(%sp)
        move.l  %a1,-(%sp)               | dce
        move.l  %a0,-(%sp)               | pb
        cmp.w   #2,%d0
        beq.s   .Lctl
        cmp.w   #1,%d0
        beq.s   .Lprime
        cmp.w   #3,%d0
        beq.s   .Lstat
        tst.w   %d0
        beq.s   .Lopen
        jsr     mt_drv_close
        bra.s   .Lret
.Lopen: jsr     mt_drv_open
        bra.s   .Lret
.Lctl:  jsr     mt_drv_control
.Lret:  addq.l  #8,%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        rts
.Lprime:
        jsr     mt_drv_prime
        bra.s   .Lfin
.Lstat: jsr     mt_drv_status
.Lfin:  addq.l  #8,%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        ext.l   %d0
        btst    #1,6(%a0)                | ioTrap bit 9 (high byte, bit 1): immediate
        bne.s   .Limm
        move.l  JIODONE,-(%sp)           | jIODone: A1 = DCE, D0 = result
.Limm:  rts

.Lboot:
        lea     .Lb1(%pc),%a0
.Lb1:   move.l  #.Lb1,%d1                | link address (patched by the relocation)
        sub.l   %d1,%a0
        move.l  %a0,%d1
        and.l   #0x00FFFFFF,%d1          | displacement, 24-bit clean (as Retro68's StripAddress24)
        move.l  %d1,%a0
        add.l   #Retro68Relocate,%a0     | its run-time address before relocation
        jsr     (%a0)                    | relocate the block in place (once; later calls return)
        jmp     mt_drv_boot              | absolute: patched by the relocation just done. A C call:
                                         | the INIT's arguments are still on the stack

| void mt_drv_call_completion(uint32_t proc, void *pb, uint32_t a5)
| An ioCompletion routine as MacTCP calls it (matrix 150): A0 = the PB and the same pointer at
| 4(SP), plain RTS (we pop the argument), with the application's A5. MPW-compiled routines may
| change D0-D2/A0-A1; everything else is saved here as well.
mt_drv_call_completion:
        movem.l %d2-%d7/%a2-%a6,-(%sp)   | 11 registers, 44 bytes
        move.l  48(%sp),%a1              | proc
        move.l  52(%sp),%a0              | pb
        move.l  56(%sp),%a5              | a5
        move.l  %a0,-(%sp)
        jsr     (%a1)
        addq.l  #4,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| void mt_drv_call_asr_tcp(uint32_t proc, uint32_t a5, uint32_t stream, uint32_t ev,
|                          uint32_t user, uint32_t reason, uint32_t icmp)
| Pascal: pascal void (StreamPtr, unsigned short eventCode, Ptr userDataPtr,
| unsigned short terminReason, ICMPReport *); arguments pushed left to right, callee pops 16.
mt_drv_call_asr_tcp:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movem.l 48(%sp),%d1-%d7          | d1 proc, d2 a5, d3 stream, d4 ev, d5 user, d6 reason, d7 icmp
        move.l  %d2,%a5
        move.l  %d1,%a1
        move.l  %d3,-(%sp)
        move.w  %d4,-(%sp)
        move.l  %d5,-(%sp)
        move.w  %d6,-(%sp)
        move.l  %d7,-(%sp)
        jsr     (%a1)
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| void mt_drv_call_asr_udp(uint32_t proc, uint32_t a5, uint32_t stream, uint32_t ev,
|                          uint32_t user, uint32_t icmp)
| Pascal: pascal void (StreamPtr, unsigned short eventCode, Ptr userDataPtr, ICMPReport *);
| callee pops 14.
mt_drv_call_asr_udp:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movem.l 48(%sp),%d1-%d6          | d1 proc, d2 a5, d3 stream, d4 ev, d5 user, d6 icmp
        move.l  %d2,%a5
        move.l  %d1,%a1
        move.l  %d3,-(%sp)
        move.w  %d4,-(%sp)
        move.l  %d5,-(%sp)
        move.l  %d6,-(%sp)
        jsr     (%a1)
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts
