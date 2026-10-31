/* mt_os_mac.c - the clock, gate and interrupt-context services of mt_os.h for the real Mac
 * (the rest of mt_os.h, completion and ASR calls, is Task 10's). No Toolbox calls.
 *
 * Clock: System 6.0.8 on the Mac Classic has neither the Microseconds trap (A193) nor a cheap
 * 1 ms counter (obs, Task 9: a self-priming 1 ms Time Manager task costs 28% of the CPU), so the
 * ms clock is the tick count at 60.15 Hz converted exactly (mt_ms_from_ticks): 16/17 ms steps.
 * Deadlines are armed with PrimeTime in true milliseconds (mt_timer.c); only "now" is coarse.
 *
 * Gate: mt_os_irq_save / mt_os_irq_restore are MT_GATE_ENTER / MT_GATE_LEAVE on the one gate the
 * driver registers with mt_os_mac_set_gate (no interrupt mask is touched: see mt_gate.h).
 * mt_os_in_interrupt() reads the interrupt mask from the SR (Task 10 ruling). */
#include "mt_os.h"
#include "mt_gate.h"
#include "mt_enet_logic.h"
#include "mt_os_mac.h"

/* The low-memory globals live at fixed small addresses; GCC would flag them as null derefs. */
#pragma GCC diagnostic ignored "-Warray-bounds"

static MtGate *g_gate;

void mt_os_mac_set_gate(MtGate *g) { g_gate = g; }

uint32_t mt_os_ms(void)
{
    return mt_ms_from_ticks(*(volatile const uint32_t *)0x16A);      /* low-memory Ticks */
}

uint32_t mt_os_ticks(void)
{
    return *(volatile const uint32_t *)0x16A;
}

int mt_os_irq_save(void)
{
    if (g_gate) mt_gate_enter(g_gate);
    return 0;
}

void mt_os_irq_restore(int saved)
{
    (void)saved;
    if (g_gate) mt_gate_leave(g_gate);
}

/* Interrupt time = a nonzero processor interrupt mask (SR bits 8-10). `move sr` is unprivileged
 * on the 68000, and Mac OS code runs in supervisor mode on the later CPUs anyway. Completion
 * routines, ASRs, the protocol handler (mask 6) and the Time Manager task (mask 1) all run with
 * a nonzero mask; the application thread runs at 0. */
int mt_os_in_interrupt(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    return (sr & 0x0700u) != 0;
}

int mt_os_mask(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}

void mt_os_unmask(int saved)
{
    uint16_t sr = (uint16_t)saved;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

/* ApplZone (0x2AA) .. BufPtr (0x10C). 24-bit addressing (the Mac Classic): the high byte of a
 * pointer may carry Memory Manager flags, so compare the low 24 bits. */
int mt_os_app_mem(MacPtr32 p)
{
    uint32_t lo = *(volatile uint32_t *)0x2AA, hi = *(volatile uint32_t *)0x10C;
    if (hi <= 0x00FFFFFFu) {
        p &= 0x00FFFFFFu;
        lo &= 0x00FFFFFFu;
    }
    return p >= lo && p < hi;
}
