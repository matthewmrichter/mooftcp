/* mt_enet_pump.c - the transmit pump (PERF BUILDS ONLY: MT_TUNE lever txpump, task 17; see mt_enet.h "transmit
 * pump"). Plain C, no Toolbox: the host tests (tests/test_enet_pump.c, test_fuzz_pb) build it against
 * tests/pbfake.c; the two Toolbox calls (mt_enet_io_write, mt_enet_pump_prime) are in mt_enet.c.
 * A release build compiles this file to nothing: its driver keeps mt_enet_write as link.tx. */
#ifdef MT_TUNE
#include <string.h>
#include "mt_enet.h"
#include "mt_stackmark.h"
#ifdef MT_TRACE
#include "mt_trace.h"
#endif

#define E_NOTOPEN    (-28)         /* notOpenErr: the link closed before the task issued the frame */

_Static_assert((MT_ENET_PUMP_Q & (MT_ENET_PUMP_Q - 1u)) == 0 && MT_ENET_PUMP_Q > MT_ENET_TXSLOTS,
               "the FIFO holds every slot and wraps by mask");

static void barrier(void) { __asm__ volatile("" : : : "memory"); }

/* The slot is started: the watchdog times it and the owner's reap counts its result (owner context only). */
static void count_started(MtEnet *e, MtEnetTx *s)
{
    mt_enet_tx_mark_started(e, s);
    e->tx_ok++;
}

int mt_enet_tx_write(MtEnet *e, MtEnetPump *p, const uint8_t *frame, uint16_t len)
{
    MtEnetTx *s;
    uint16_t n;
    uint8_t h, depth;

    h = p->head;
    if (!p->on && h == p->tail)                 /* lever off, nothing queued: the release path itself */
        return mt_enet_write(e, frame, len);
    if (!e->opened) { e->tx_drop_closed++; return 1; }
    if (!mt_enet_len_ok(len)) { e->tx_drop_len++; return 1; }
    s = mt_enet_tx_claim(e);
    if (!s) { e->tx_drop_busy++; return 1; }

    memcpy(s->frame, frame, len);
    n = mt_enet_pad(s->frame, len);
    if (n != len) e->tx_pad++;
    mt_enet_tx_prep(e, s, n);              /* the same PB mt_enet_write prepares */
    s->pb.ioResult = 1;                         /* ours until the task issues it: mt_enet_tx_claim skips it */
    count_started(e, s);
    p->q[h] = (uint8_t)(s - e->tx);
    barrier();
    p->head = (uint8_t)((h + 1u) & (MT_ENET_PUMP_Q - 1u));
    barrier();
    depth = (uint8_t)((p->head - p->tail) & (MT_ENET_PUMP_Q - 1u));
    if (depth > p->max_q) p->max_q = depth;
    if (p->running) { p->nested++; return 0; }  /* inside the task's EWrite: the task drains it */
    if (!p->armed) {
        p->armed = 1;
        mt_enet_pump_prime(p);
    }
    return 0;
}

void mt_enet_pump_hold(MtEnetPump *p, int on)
{
    if (on) { p->hold = 1; return; }
    p->hold = 0;
    barrier();
    if (p->head != p->tail && !p->armed) {      /* a run during the hold left them: prime again */
        p->armed = 1;
        mt_enet_pump_prime(p);
    }
}

void mt_enet_pump_run(MtEnet *e, MtEnetPump *p)
{
    int16_t r;
    if (p->hold) { p->armed = 0; return; }      /* synchronous .ENET0 calls in progress: mt_enet_pump_hold re-primes */
    mt_os_stack_enter();                        /* an episode of its own: the driver may deliver inside EWrite */
    p->armed = 0;
    p->runs++;
    for (;;) {
        p->running = 1;
        barrier();
        while (p->tail != p->head) {
            uint8_t t = p->tail;
            MtEnetTx *s = &e->tx[p->q[t]];
            p->tail = (uint8_t)((t + 1u) & (MT_ENET_PUMP_Q - 1u));
            barrier();
            if (!e->opened) { s->pb.ioResult = E_NOTOPEN; continue; }   /* the owner's reap counts it */
            p->issued++;
            mt_os_stack_mark();
#ifdef MT_TRACE
            mt_trace_ewrite(s, s->wds[0].len, 1);   /* perf builds with the trace: issued by the pump's task */
#endif
            r = mt_enet_io_write(e, s);
            if (r < 0) s->pb.ioResult = r;      /* refused at once: the owner's reap counts it (tx_err) */
        }
#ifdef MT_TEST_HOOKS
        if (p->test_window) p->test_window(p);  /* host tests: an interrupt queues a frame right here */
#endif
        p->running = 0;
        barrier();
        if (p->tail == p->head) break;          /* else an interrupt queued one after the last look */
    }
    mt_os_stack_leave();
}
#endif
