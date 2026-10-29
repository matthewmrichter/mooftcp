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

int  mt_timer_init(MtTimer *t, mt_timer_cb cb, void *ctx);   /* 0 or a Toolbox error */
void mt_timer_arm(MtTimer *t, uint32_t ms);
void mt_timer_cancel(MtTimer *t);
void mt_timer_close(MtTimer *t);

#ifdef __cplusplus
}
#endif

#endif /* MT_TIMER_H */
