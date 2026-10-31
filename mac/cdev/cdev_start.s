| cdev_start.s - entry of the MoofTCP control panel's 'cdev' code resource (Plan 3 Task 8).
|
| The Control Panel calls offset 0 as a Pascal function:
|   pascal long main(short message, short item, short numItems, short CPanelID,
|                    EventRecord *theEvent, long cdevStorage, DialogPtr CPDialog)
| Pascal pushes the result space first, then the arguments left to right, and the callee pops the
| arguments. At _start (Retro68's _entry_trampoline at offset 0 jumps here with the stack as the
| caller left it):
|    0(sp) return address     4 CPDialog     8 cdevStorage   12 theEvent
|   16 CPanelID   18 numItems   20 item   22 message        24 result (long)
| The arguments from 4(sp) up are exactly the C struct CdevArgs (cdev_main.c), so the trampoline
| passes their address to the C function, stores its D0 in the result slot and pops 20 bytes.
| The C code preserves D2-D7/A2-A6 (more than Pascal requires).
|
| No relocation: the cdev code is built with -mpcrel and never calls Retro68Relocate, so the
| block works wherever the Control Panel loads it and keeps working if the handle moves between
| calls (mac/cdev/CMakeLists.txt checks the objects carry no absolute relocations).
        .text
        .align  2
        .globl  _start
        .extern cdev_main

_start:
        pea     4(%sp)                   | &args
        bsr.w   cdev_main                | long cdev_main(const CdevArgs *)
        addq.l  #4,%sp
        move.l  (%sp)+,%a0               | return address
        lea     20(%sp),%sp              | pop the 20 bytes of arguments
        move.l  %d0,(%sp)                | function result
        jmp     (%a0)

| unsigned long __umodsi3(unsigned long a, unsigned long b): libgcc's version calls __udivsi3 and
| __mulsi3 with absolute JSRs (relocations); this one calls them PC-relative. The linker takes it
| from this object, so libgcc's member is never pulled in (check_pcrel.sh would catch it).
        .globl  __umodsi3
        .extern __udivsi3
        .extern __mulsi3
__umodsi3:
        move.l  8(%sp),-(%sp)            | b
        move.l  8(%sp),-(%sp)            | a
        bsr.w   __udivsi3                | q = a / b
        addq.l  #8,%sp
        move.l  8(%sp),-(%sp)            | b
        move.l  %d0,-(%sp)               | q
        bsr.w   __mulsi3                 | q * b
        addq.l  #8,%sp
        move.l  4(%sp),%d1
        sub.l   %d0,%d1                  | a - q * b
        move.l  %d1,%d0
        rts
