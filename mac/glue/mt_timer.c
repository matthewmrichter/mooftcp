/* mt_timer.c - one-shot deadline timer over a Time Manager task (see mt_timer.h). */
#include <Timer.h>
#include <stddef.h>
#include "mt_timer.h"

/* Short interrupt-mask section around the Time Manager queue manipulation (a few instructions,
 * never any EWrite or core call inside): keeps the task from firing between our checks and the
 * RmvTime/InsTime/PrimeTime that follow. */
typedef uint16_t sr_t;
static inline sr_t irq_off(void)
{
    sr_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}
static inline void irq_on(sr_t sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

_Static_assert(offsetof(MtTimer, qType) == offsetof(TMTask, qType), "qType");
_Static_assert(offsetof(MtTimer, tmAddr) == offsetof(TMTask, tmAddr), "tmAddr");
_Static_assert(offsetof(MtTimer, tmCount) == offsetof(TMTask, tmCount), "tmCount");
_Static_assert(offsetof(MtTimer, tmWakeUp) == offsetof(TMTask, tmWakeUp), "tmWakeUp");
_Static_assert(offsetof(MtTimer, tmReserved) == offsetof(TMTask, tmReserved), "tmReserved");

extern void mt_timer_tm(void);

/* Called by mt_timer_tm at interrupt time with the task record. */
void mt_timer_fire(MtTimer *t);
void mt_timer_fire(MtTimer *t)
{
    t->armed = 0;
    t->in_cb = 1;
    t->fired++;
    if (t->cb) t->cb(t->ctx);
    t->in_cb = 0;
}

int mt_timer_init(MtTimer *t, mt_timer_cb cb, void *ctx)
{
    uint8_t *p = (uint8_t *)t;
    size_t i;
    for (i = 0; i < sizeof *t; i++) p[i] = 0;
    t->tmAddr = (uint32_t)(uintptr_t)mt_timer_tm;
    t->cb = cb;
    t->ctx = ctx;
    InsTime((QElemPtr)t);
    t->inserted = 1;
    return 0;
}

void mt_timer_cancel(MtTimer *t)
{
    sr_t g;
    g = irq_off();
    if (t->inserted && t->armed) {
        RmvTime((QElemPtr)t);
        t->inserted = 0;
    }
    t->armed = 0;
    irq_on(g);
}

void mt_timer_arm(MtTimer *t, uint32_t ms)
{
    sr_t g;
    if (ms == MT_TIMER_NONE) { mt_timer_cancel(t); return; }
    if (ms < 1) ms = 1;
    if (ms > 0x3FFFFFFFu) ms = 0x3FFFFFFFu;
    g = irq_off();
    if (!t->in_cb) {
        if (t->inserted && t->armed) RmvTime((QElemPtr)t);
        if (!t->inserted || t->armed) {
            t->tmWakeUp = 0;
            t->tmReserved = 0;
            InsTime((QElemPtr)t);
            t->inserted = 1;
        }
    }
    t->armed = 1;
    PrimeTime((QElemPtr)t, (long)ms);
    irq_on(g);
}

void mt_timer_close(MtTimer *t)
{
    sr_t g;
    g = irq_off();
    if (t->inserted) RmvTime((QElemPtr)t);
    t->inserted = 0;
    t->armed = 0;
    irq_on(g);
}
