/* mt_gate.c - the core gate and the deferred-input ring (see mt_gate.h). Plain C, no Toolbox. */
#include <stddef.h>
#include "mt_gate.h"

static void barrier(void) { __asm__ volatile("" : : : "memory"); }

void mt_gate_init(MtGate *g, void *ctx, void (*rx)(void *, const uint8_t *, uint16_t),
                  void (*tick)(void *), void (*after)(void *))
{
    uint8_t *p = (uint8_t *)g;
    size_t i;
    for (i = 0; i < sizeof *g; i++) p[i] = 0;
    g->ctx = ctx;
    g->rx = rx;
    g->tick = tick;
    g->after = after;
    g->open = 1;
}

static void drain(MtGate *g)
{
    for (;;) {
        if (g->tick_pending) {
            g->tick_pending = 0;
            g->n_tick++;
            g->tick(g->ctx);
            continue;
        }
        if (g->tail != g->head) {
            MtRxSlot *s = &g->slot[g->tail];
            g->n_rx++;
            g->rx(g->ctx, s->buf, s->len);
            barrier();
            g->tail = (uint16_t)((g->tail + 1u) % MT_GATE_SLOTS);
            continue;
        }
        break;
    }
}

/* The caller owns the core (depth == 1). Run everything posted, then let go; the re-check
 * after the release picks up work posted in between and takes ownership again. */
static void own_and_run(MtGate *g, uint8_t irq)
{
    for (;;) {
        g->in_irq = irq;
        drain(g);
        if (g->after) { g->n_after++; g->after(g->ctx); }
        g->in_irq = 0;
        barrier();
        g->depth = 0;
        barrier();
        if (g->head == g->tail && !g->tick_pending) return;
        g->depth = 1;
    }
}

void mt_gate_enter(MtGate *g)
{
    g->depth++;
    if (g->depth > g->max_depth) g->max_depth = g->depth;
}

void mt_gate_leave(MtGate *g)
{
    if (g->depth > 1) { g->depth--; return; }
    own_and_run(g, 0);
}

void mt_gate_poke(MtGate *g)
{
    uint32_t n = (uint32_t)((g->head + MT_GATE_SLOTS - g->tail) % MT_GATE_SLOTS);
    if (n > g->max_ring) g->max_ring = n;
    if (!g->open) { g->tail = g->head; g->drops++; return; }
    if (g->depth) { g->n_deferred++; return; }
    g->depth = 1;
    own_and_run(g, 1);
}

void mt_gate_tick(MtGate *g)
{
    if (!g->open) return;
    g->tick_pending = 1;
    if (g->depth) { g->n_tick_deferred++; return; }
    g->depth = 1;
    own_and_run(g, 1);
}

MtRxSlot *mt_gate_slot(MtGate *g)
{
    uint16_t next = (uint16_t)((g->head + 1u) % MT_GATE_SLOTS);
    if (next == g->tail) { g->drops++; return NULL; }
    return &g->slot[g->head];
}

void mt_gate_commit(MtGate *g, uint16_t len)
{
    g->slot[g->head].len = len;
    barrier();
    g->head = (uint16_t)((g->head + 1u) % MT_GATE_SLOTS);
}
