/* test_gate.c - the core gate and deferred-input ring (mac/glue/mt_gate.c). The 68k handler stub
 * is simulated by mt_gate_slot / mt_gate_commit / mt_gate_poke; "H arrives" is a function the
 * test calls at the interesting moments (inside rx, inside after, between enter and leave). */
#include "t.h"
#include "mt_gate.h"

typedef struct Ctx {
    MtGate g;
    int log[64], nlog;
    int rx_depth, rx_max_depth;       /* nesting of rx callbacks (must stay 1) */
    int irq_at_rx[64];
    int after_calls;
    int inject_in_rx;                 /* frame tag to inject when the first rx runs (0 = none) */
    int inject_in_after;              /* same, in the next after() */
    int ticks, irq_at_tick;
} Ctx;

static void arrive(Ctx *c, int tag)   /* what the handler stub does for one frame */
{
    MtRxSlot *s = mt_gate_slot(&c->g);
    if (!s) return;
    s->buf[0] = (uint8_t)tag;
    mt_gate_commit(&c->g, 14);
    mt_gate_poke(&c->g);
}

static void rx_cb(void *ctx, const uint8_t *f, uint16_t len)
{
    Ctx *c = ctx;
    c->rx_depth++;
    if (c->rx_depth > c->rx_max_depth) c->rx_max_depth = c->rx_depth;
    T_EQ(len, 14);
    c->irq_at_rx[c->nlog] = mt_gate_in_irq(&c->g);
    c->log[c->nlog++] = f[0];
    if (c->inject_in_rx) { int t = c->inject_in_rx; c->inject_in_rx = 0; arrive(c, t); }
    c->rx_depth--;
}
static void tick_cb(void *ctx)
{
    Ctx *c = ctx;
    c->ticks++;
    c->irq_at_tick = mt_gate_in_irq(&c->g);
    c->log[c->nlog++] = -1;
}
static void after_cb(void *ctx)
{
    Ctx *c = ctx;
    c->after_calls++;
    if (c->inject_in_after) { int t = c->inject_in_after; c->inject_in_after = 0; arrive(c, t); }
}

static void setup(Ctx *c)
{
    memset(c, 0, sizeof *c);
    mt_gate_init(&c->g, c, rx_cb, tick_cb, after_cb);
}

static void idle_frame_runs_at_once(void)
{
    static Ctx c;
    setup(&c);
    arrive(&c, 7);
    T_EQ(c.nlog, 1);
    T_EQ(c.log[0], 7);
    T_EQ(c.irq_at_rx[0], 1);          /* entered from H: interrupt time */
    T_EQ(c.g.depth, 0);
    T_EQ(c.g.head, c.g.tail);
    T_EQ(c.after_calls, 1);
    T_EQ(c.g.n_deferred, 0);
}

static void frames_during_m_wait_for_leave(void)
{
    static Ctx c;
    setup(&c);
    MT_GATE_ENTER(&c.g);
    arrive(&c, 1);
    arrive(&c, 2);
    arrive(&c, 3);
    T_EQ(c.nlog, 0);                  /* nothing ran inside the owner's call */
    T_EQ(c.g.n_deferred, 3);
    T_EQ(c.g.max_ring, 3);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.nlog, 3);
    T_EQ(c.log[0], 1); T_EQ(c.log[1], 2); T_EQ(c.log[2], 3);
    T_EQ(c.irq_at_rx[0], 0);          /* task time */
    T_EQ(c.after_calls, 1);
    T_EQ(c.g.depth, 0);
}

static void nested_enter_only_outermost_drains(void)
{
    static Ctx c;
    setup(&c);
    MT_GATE_ENTER(&c.g);
    MT_GATE_ENTER(&c.g);
    arrive(&c, 9);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.nlog, 0);
    T_EQ(c.after_calls, 0);
    T_EQ(c.g.depth, 1);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.nlog, 1);
    T_EQ(c.after_calls, 1);
    T_EQ(c.g.max_depth, 2);
}

static void empty_leave_still_requeries_deadline(void)
{
    static Ctx c;
    setup(&c);
    MT_GATE_ENTER(&c.g);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.after_calls, 1);           /* after every API call, even with nothing posted */
    T_EQ(c.nlog, 0);
}

static void ring_full_drops_and_counts(void)
{
    static Ctx c;
    int i;
    setup(&c);
    MT_GATE_ENTER(&c.g);
    for (i = 1; i <= (int)MT_GATE_SLOTS + 2; i++) arrive(&c, i);
    T_EQ(c.g.drops, (MT_GATE_SLOTS + 2) - (MT_GATE_SLOTS - 1));
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.nlog, MT_GATE_SLOTS - 1);
    for (i = 0; i < (int)MT_GATE_SLOTS - 1; i++) T_EQ(c.log[i], i + 1);   /* the oldest survive */
    /* and the ring works again afterwards */
    arrive(&c, 50);
    T_EQ(c.log[c.nlog - 1], 50);
}

static void frame_arriving_inside_rx_is_not_nested(void)
{
    static Ctx c;
    setup(&c);
    c.inject_in_rx = 2;               /* the nested delivery inside EWrite, during rx of frame 1 */
    arrive(&c, 1);
    T_EQ(c.nlog, 2);
    T_EQ(c.log[0], 1); T_EQ(c.log[1], 2);
    T_EQ(c.rx_max_depth, 1);          /* never re-entered */
    T_EQ(c.g.n_deferred, 1);
    T_EQ(c.g.depth, 0);
}

static void frame_arriving_in_the_release_window_is_not_lost(void)
{
    static Ctx c;
    setup(&c);
    c.inject_in_after = 5;            /* H arrives after the last drain, before the release */
    MT_GATE_ENTER(&c.g);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.nlog, 1);
    T_EQ(c.log[0], 5);
    T_EQ(c.after_calls, 2);           /* re-queried after the second round too */
    T_EQ(c.g.depth, 0);
}

static void ticks(void)
{
    static Ctx c;
    setup(&c);
    mt_gate_tick(&c.g);               /* idle: runs now, at interrupt time */
    T_EQ(c.ticks, 1);
    T_EQ(c.irq_at_tick, 1);
    MT_GATE_ENTER(&c.g);
    mt_gate_tick(&c.g);               /* while M owns the core: posted */
    mt_gate_tick(&c.g);               /* a second one merges */
    T_EQ(c.ticks, 1);
    T_EQ(c.g.n_tick_deferred, 2);
    arrive(&c, 4);
    MT_GATE_LEAVE(&c.g);
    T_EQ(c.ticks, 2);
    T_EQ(c.irq_at_tick, 0);
    T_EQ(c.log[c.nlog - 2], -1);      /* the tick runs before the frame */
    T_EQ(c.log[c.nlog - 1], 4);
}

static void closed_gate_swallows_input(void)
{
    static Ctx c;
    setup(&c);
    c.g.open = 0;
    arrive(&c, 1);
    mt_gate_tick(&c.g);
    T_EQ(c.nlog, 0);
    T_EQ(c.ticks, 0);
    T_EQ(c.g.head, c.g.tail);
}

T_MAIN(idle_frame_runs_at_once, frames_during_m_wait_for_leave, nested_enter_only_outermost_drains,
       empty_leave_still_requeries_deadline, ring_full_drops_and_counts,
       frame_arriving_inside_rx_is_not_nested, frame_arriving_in_the_release_window_is_not_lost,
       ticks, closed_gate_swallows_input)
