| mt_dnr_start.s - entry of the 'dnrp' code resource and the Pascal call into the app's routines.
|
| The application (its dnr.c wrapper) loads the resource, locks it and calls offset 0 as a C function:
| the selector as a long on the stack, the arguments after it, result in D0 (Retro68's
| _entry_trampoline at offset 0 jumps here with the registers and the stack untouched).
| The first call must be OPENRESOLVER on a fresh load: the code is relocated in place once (the flag
| below lives in the resource's own memory; a fresh load of the resource starts at 0), then every
| call jumps to the C dispatcher mt_dnr_entry with the caller's stack frame as it was.
        .text
        .align  2
        .globl  _start
        .globl  mt_dnr_call2
        .extern mt_dnr_entry
        .extern Retro68Relocate

_start:
        lea     .Lrelocated(%pc),%a0
        tst.w   (%a0)
        bne.s   .Lgo
        move.w  #1,(%a0)
        lea     .Lb1(%pc),%a0
.Lb1:   move.l  #.Lb1,%d1                | link address (patched by the relocation, read before it ran)
        sub.l   %d1,%a0
        move.l  %a0,%d1
        and.l   #0x00FFFFFF,%d1          | displacement, 24-bit clean (as Retro68's StripAddress24)
        move.l  %d1,%a0
        add.l   #Retro68Relocate,%a0     | its run-time address before relocation
        jsr     (%a0)                    | relocates the block in place; D0-D1/A0-A1 are scratch for the caller
.Lgo:   jmp     mt_dnr_entry             | absolute: patched by the relocation

        .align  2
.Lrelocated:
        .word   0

| void mt_dnr_call2(uint32_t proc, uint32_t a, uint32_t b)
| Calls the application's Pascal routine  pascal void proc(a, b)  (result proc: hostInfo *, char *userData;
| enum proc: cacheEntryRecord *, char *userData): arguments pushed left to right, callee pops.
| MPW-compiled routines may change D0-D2/A0-A1; everything else is saved here.
mt_dnr_call2:
        movem.l %d2-%d7/%a2-%a6,-(%sp)   | 44 bytes
        move.l  48(%sp),%a1              | proc
        move.l  52(%sp),%d0              | a
        move.l  56(%sp),%d1              | b
        move.l  %d0,-(%sp)
        move.l  %d1,-(%sp)
        jsr     (%a1)
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts
