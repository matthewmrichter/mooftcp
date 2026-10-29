| mt_timer_tm.s - Time Manager task entry for mt_timer.c.
| The Time Manager calls the task procedure with A1 = the TMTask (our MtTimer, whose first 20
| bytes are the TMTask), at interrupt level, and expects D0-D2/A0-A1 to be free; the C routine
| only clobbers what the compiler treats as scratch, so passing A1 on the stack is all it takes.
        .text
        .align  2
        .globl  mt_timer_tm
        .extern mt_timer_fire
mt_timer_tm:
        move.l  %a1,-(%sp)
        jsr     mt_timer_fire
        addq.l  #4,%sp
        rts
