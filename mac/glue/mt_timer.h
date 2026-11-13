/* mt_timer.h - one-shot deadline timer over a Time Manager task (Task 9).
 *
 * Contains Toolbox includes only in mt_timer.c; this header is plain C so any glue file can use
 * it. The caller owns the MtTimer (non-relocatable memory: the Time Manager keeps the pointer;
 * system heap for the driver). The callback runs at interrupt time (Time Manager task context,
 * SR mask 1 in Snow, A1 = the task record, no A5 globals; it must not EWrite synchronously) and may call mt_timer_arm for the next deadline.
 *
 * Contract:
 *  - mt_timer_arm(t, ms): the callback runs about `ms` milliseconds from now (at least 1; Time
 *    Manager resolution is the VIA timer, about 1 ms). Re-arming replaces the pending shot. From
 *    non-interrupt code an armed task is first removed (RmvTime) and re-inserted so a shot that
 *    is already in flight cannot double fire; inside the callback only PrimeTime is used (the
 *    task is inactive then; this is the documented interrupt-safe reload).
 *  - mt_timer_cancel(t): no callback after it returns (RmvTime + InsTime again); callable from
 *    anywhere, including the callback.
 *  - ms == MT_TIMER_NONE (0xFFFFFFFF, the core's MT_NO_DEADLINE) cancels.
 *  - ms is clamped to 0x7FFFFFFF/2 (about 12 days; a longer delay re-arms through the core's own
 *    2^31 ms tick rule anyway).
 */
#ifndef MT_TIMER_H
#define MT_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_TIMER_NONE 0xFFFFFFFFu

typedef void (*mt_timer_cb)(void *ctx);

/* Layout: the first 20 bytes mirror TMTask (qLink, qType, tmAddr, tmCount, tmWakeUp,
 * tmReserved); mt_timer.c checks the offsets at compile time. */
#pragma pack(push, 2)
typedef struct MtTimer {
    uint32_t    qLink;
    int16_t     qType;
    uint32_t    tmAddr;
    int32_t     tmCount;
    int32_t     tmWakeUp;
    int32_t     tmReserved;
    mt_timer_cb cb;
    void       *ctx;
    volatile uint8_t armed;     /* primed and not yet fired */
    volatile uint8_t in_cb;
    volatile uint8_t inserted;
    uint8_t     pad;
    volatile uint32_t fired;    /* callbacks run */
} MtTimer;
#pragma pack(pop)

/* The coarse clock (mt_os_ms: 16/17 ms steps, lagging real time by 0..16.6 ms) and the exact-ms
 * PrimeTime disagree: a core deadline d ms away, armed from a clock that has not stepped yet,
 * fires 1 ms apart until the tick count advances (up to ~16 interrupt-time passes per expiry,
 * obs Task 9 review). So a deadline below one tick step is armed one step (17 ms) away, which
 * is also what the clock can resolve; MT_NO_DEADLINE becomes the longest shot (never cancel:
 * the core must be ticked at least every 2^31 ms). Pure arithmetic, host-tested. */
#define MT_TIMER_STEP_MS 17u
#define MT_TIMER_MAX_MS  0x3FFFFFFFu     /* about 12.4 days */
static inline uint32_t mt_timer_deadline_ms(uint32_t d)
{
    if (d == MT_TIMER_NONE || d > MT_TIMER_MAX_MS) return MT_TIMER_MAX_MS;
    return d < MT_TIMER_STEP_MS ? MT_TIMER_STEP_MS : d;
}

/* 1.1d1: the pending shot already fires in time. t is armed (primed, not yet fired) for the absolute ms time `due`
 * (the coarse clock, mt_os_ms), and a new shot would be `ms` after `now`: nonzero when due <= now + ms (wrap-safe).
 * Pure, host-tested. A shot kept this way fires no later than the new one would (both are exact-ms PrimeTimes from
 * a coarse "now": either may land up to one clock step before the coarse deadline, never later than one step
 * after it); when it fires early the tick finds nothing due (mt_drv_tick_idle) and the deadline is armed again.
 * 1.1d2 (liveness): a shot still marked armed but overdue by more than MT_TIMER_LOST_MS (two clock steps) is taken as
 * lost and armed again, so a missed shot can never stop the timers for good (before, any due in the past counted as
 * "fires in time"). A short shot fires within about one step of its coarse due time; a long one (seconds, minutes)
 * may drift past the slack because the exact-ms PrimeTime and the coarse Ticks clock disagree, and is then re-armed
 * before it fires: harmless (one extra arm, the same deadline), but it also counts in MtTimerShot.lost below. */
#define MT_TIMER_LOST_MS (2u * MT_TIMER_STEP_MS)
static inline int mt_timer_keep(const MtTimer *t, uint32_t due, uint32_t now, uint32_t ms)
{
    return t->armed && (int32_t)(now + ms - due) >= 0 && (int32_t)(due - now) >= -(int32_t)MT_TIMER_LOST_MS;
}

/* The caller's record of the pending shot: its due time, and the lost shots found (mt_timer_note_lost). */
typedef struct MtTimerShot {
    uint32_t due;               /* absolute time (the coarse clock) the pending shot is for; valid while t->armed */
    uint32_t lost;              /* shots found armed but overdue beyond MT_TIMER_LOST_MS (re-armed; see above) */
    uint32_t lost_at;           /* the clock time of the last such find */
} MtTimerShot;

/* Count a lost shot (armed, overdue beyond the slack) when it is found at `now`; nonzero if it was. Pure, host-tested. */
static inline int mt_timer_note_lost(const MtTimer *t, MtTimerShot *sh, uint32_t now)
{
    if (!t->armed || (int32_t)(sh->due - now) >= -(int32_t)MT_TIMER_LOST_MS) return 0;
    sh->lost++;
    sh->lost_at = now;
    return 1;
}

/* The Time Manager steps mt_timer_arm takes before its PrimeTime (pure, host-tested with a model of the task). */
#define MT_TIMER_RMV 1u
#define MT_TIMER_INS 2u
static inline unsigned mt_timer_arm_steps(const MtTimer *t)
{
    unsigned st = 0;
    if (!t->in_cb || t->armed) {
        if (t->inserted && t->armed) st |= MT_TIMER_RMV;
        if (!t->inserted || t->armed) st |= MT_TIMER_INS;
    }
    return st;
}

int  mt_timer_init(MtTimer *t, mt_timer_cb cb, void *ctx);   /* 0 or a Toolbox error */
void mt_timer_arm(MtTimer *t, uint32_t ms);
/* Arm for a core deadline `d` (mt_next_deadline's value, MT_NO_DEADLINE allowed), computed at `now` (mt_os_ms):
 * the helper every caller of the "re-query after every API call" rule uses (Task 10 reuses it as is). Since 1.1d1
 * the Time Manager task is touched only when the deadline is earlier than the pending shot (mt_timer_keep); sh->due
 * is the caller's record of that shot's absolute time (written here; one writer context: the gate owner). */
void mt_timer_arm_deadline(MtTimer *t, uint32_t d, uint32_t now, MtTimerShot *sh);   /* 1.1d2: counts lost shots */
void mt_timer_cancel(MtTimer *t);
void mt_timer_close(MtTimer *t);

#ifdef __cplusplus
}
#endif

#endif /* MT_TIMER_H */
