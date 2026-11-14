/* mt_trace_mac.c - the echo-path trace's Mac side (PERF BUILDS ONLY: MT_TUNE + MT_TRACE, task 18; mt_trace.h): the
 * microsecond clock and the EWrite completion routine. Every other build compiles this file to nothing.
 *
 * Clock (mt_os_trace_us). What a 68000 Mac Classic under System 6.0.8 offers:
 *   - Microseconds (A193): not implemented on 6.0.8 (docs/dev/MACTCP-INTERFACE.md "Time Manager": calling it gave
 *     "unimplemented trap"), although Gestalt('tmgr') says 3.
 *   - The VIA timers: reading a counter's low byte clears that timer's interrupt flag, so a read can swallow the
 *     interrupt of whoever owns the timer (the Time Manager, the Sound Driver, the disk speed code); reading only the
 *     high byte gives neither resolution nor a known period. Not used.
 *   - Ticks: 16.6 ms steps. Too coarse.
 *   - The revised Time Manager (6.0.3 and later) as a stopwatch, the method MoofPort's hwsend diagnostic driver and
 *     TcpSend use on the same Classic: a task primed for -1,000,000 us; RmvTime leaves the time not yet elapsed in
 *     tmCount (negative us), so elapsed = 1 s + tmCount; InsTime + PrimeTime start it again. If no read comes for a
 *     second the task fires and adds the second itself. Only public Time Manager calls: no VIA register is touched,
 *     so MoofPort, DaynaPORT, sound and the VBL are not disturbed. Legal at interrupt time (no Memory Manager).
 *     Resolution: the Time Manager's (about 20 us nominal); a read costs about 0.2-0.25 ms on a Classic (measured
 *     there by MoofPort's driver, 205-246 us): lap_us is measured at boot and reported. The time between RmvTime and
 *     the next PrimeTime is not counted, so the clock runs slightly slow (a part of each read's own cost). The sum is
 *     a uint32_t of microseconds: monotonic, wrapping after 2^32 us (71.6 minutes); readers take differences modulo
 *     2^32. Each read masks interrupts (level 7) for its duration, so the task cannot fire between RmvTime and
 *     PrimeTime and no other read can interleave.
 * The EWrite completion routine: the Device Manager's IODone calls it with A0 = the PB (ioResult already set); the stub
 * passes A0 to mt_trace_ew_done, which only records (D0-D2/A0-A1 are free in a completion routine). */
#ifdef MT_TRACE
#include <Timer.h>
#include "mt_trace.h"

/* mt_os.h cannot be included next to the Toolbox headers (duplicate enumerators): declare the two services */
uint32_t mt_os_trace_us(void);
uint32_t mt_os_trace_ew_compl(void);

#define CLK_US 1000000L

extern void mt_trace_ewc_stub(void);
__asm__(
    "        .text\n"
    "        .align  2\n"
    "        .globl  mt_trace_ewc_stub\n"
    "mt_trace_ewc_stub:\n"
    "        move.l  %a0,-(%sp)\n"
    "        jsr     mt_trace_ew_done\n"
    "        addq.l  #4,%sp\n"
    "        rts\n");

uint32_t mt_os_trace_ew_compl(void) { return (uint32_t)(uintptr_t)mt_trace_ewc_stub; }

/* The stopwatch task (mt_timer_tm -> mt_timer_fire -> here): a second went by without a read. */
static void clk_fire(void *ctx)
{
    MtTrace *t = (MtTrace *)ctx;
    t->acc += (uint32_t)CLK_US;
    PrimeTime((QElemPtr)&t->clk, -CLK_US);
}

uint32_t mt_os_trace_us(void)
{
    MtTrace *t = mt_trace_get();
    uint16_t sr;
    uint32_t v;
    if (!t || !(t->flags & MT_TRACE_F_TMCLK)) return 0;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    RmvTime((QElemPtr)&t->clk);
    t->acc += (uint32_t)(CLK_US + t->clk.tmCount);   /* tmCount: the us not yet elapsed, negative */
    v = t->acc;
    t->clk.tmCount = 0;
    t->clk.tmWakeUp = 0;
    t->clk.tmReserved = 0;
    InsTime((QElemPtr)&t->clk);
    PrimeTime((QElemPtr)&t->clk, -CLK_US);
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
    return v;
}

void mt_trace_mac_start(MtTrace *t)
{
    uint32_t a, b, best = 0xFFFFFFFFu;
    int i;
    mt_timer_init(&t->clk, clk_fire, t);         /* InsTime; the entry is mt_timer_tm (A1 = the task) */
    PrimeTime((QElemPtr)&t->clk, -CLK_US);
    t->flags |= MT_TRACE_F_TMCLK;
    for (i = 0; i < 8; i++) {                    /* the cost of one read: the best of 8 back-to-back pairs */
        a = mt_os_trace_us();
        b = mt_os_trace_us();
        if (b - a < best) best = b - a;
    }
    t->lap_us = best;
}
#endif
