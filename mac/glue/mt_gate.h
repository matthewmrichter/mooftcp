/* mt_gate.h - the core gate and the deferred-input ring (Plan 2 ruling P2, decided from the
 * Task 9 spike; the evidence is in docs/dev/MACTCP-INTERFACE.md "Gate ruling").
 *
 * RULING: P2 said "raise the interrupt mask around core calls made outside interrupt context".
 * The spike disproved both halves of that idea on the DaynaPORT driver in Snow:
 *   1. A synchronous EWrite never returns when the 68000 interrupt mask is >= 1 (or when called
 *      from the protocol handler, which runs at SR 0x2600), and the core calls link.tx, hence
 *      EWrite, in the middle of a core call. A masked gate would hang the machine.
 *   2. The driver delivers received frames by calling the protocol handler SYNCHRONOUSLY from
 *      inside EWrite (observed: the handler's stack is nested deeper than the interrupted
 *      handler's, even for an async EWrite). A mask cannot keep out a plain subroutine call.
 * So the gate is a software ownership flag plus a small ring of received frames (the
 * "deferred-frame ring" alternative P2 allowed), and link.tx uses asynchronous EWrite (mt_enet.c),
 * which never waits. No interrupt mask is touched, so no interrupt latency is added.
 *
 * MODEL. Exactly one context at a time owns the core: depth != 0. The contexts:
 *   - M, the application's thread (PBControl on the driver): MT_GATE_ENTER / MT_GATE_LEAVE,
 *     nestable (an ASR may call the driver again).
 *   - H, the protocol handler (interrupt time, or nested inside EWrite): mt_gate_poke after the
 *     handler stub has put the frame into the ring.
 *   - T, the Time Manager task: mt_gate_tick.
 * An H or T that finds the core owned leaves its work behind (the frame already sits in the ring;
 * T sets tick_pending) and returns at once. The owner, before it lets go of the core, runs
 * everything left behind (a pending tick first, then the frames in arrival order), then calls
 * `after` (the glue
 * re-queries the core's deadline and re-arms the timer: the embedder contract's "after EVERY
 * API call") and releases. The release re-checks the ring so a frame posted in the window between
 * the last look and the release is not lost.
 *
 * Atomicity on the 68000: H and T preempt M, never the reverse, and each runs to completion
 * before M resumes. depth is only ever written 0 -> 1 -> 0 by H/T (while M is suspended) and
 * +-1 by M with H/T observing a nonzero value; a byte store is one instruction. head is written
 * only by the producer (the stub), tail only by the consumer, both uint16 (one instruction).
 * Plain `volatile` plus the compiler barrier in mt_gate_barrier is therefore enough.
 *
 * Callbacks `rx`, `tick`, `after` run with depth != 0 and must not call MT_GATE_ENTER (the core
 * entry points they use are not gated: they ARE the owner). They run at interrupt time when H or
 * T owned the core (mt_gate_in_irq() != 0), else at task time.
 *
 * Layout for mt_enet_ph.s: head@0, tail@2, drops@4, slot[0]@8; slot = len@0, pad@2, buf@4.
 * mt_gate.c is plain C, host-tested (tests/test_gate.c). */
#ifndef MT_GATE_H
#define MT_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_GATE_FRAME 1520u      /* header + data + the 4 trailing driver bytes (obs: FCS) */
#define MT_GATE_SLOTS 8u         /* frames that can wait while the core is owned (obs: the driver can
                                    deliver a flurry of replies from inside one EWrite; 4 slots lost 2
                                    of a 60-frame echo burst, 8 do not) */

typedef struct MtRxSlot {
    uint16_t len;                /* bytes in buf (header included) */
    uint16_t pad;
    uint8_t  buf[MT_GATE_FRAME];
} MtRxSlot;

typedef struct MtGate {
    volatile uint16_t head;      /* next slot the stub fills */
    volatile uint16_t tail;      /* next slot to run */
    volatile uint16_t drops;     /* frames discarded because the ring was full or the gate closed */
    uint16_t          pad;
    MtRxSlot          slot[MT_GATE_SLOTS];
    volatile uint8_t  depth;     /* nonzero: some context owns the core */
    volatile uint8_t  tick_pending;
    volatile uint8_t  in_irq;    /* the owner entered from H or T */
    volatile uint8_t  open;      /* 0: poke/tick ignore everything */
    void  *ctx;
    void (*rx)(void *ctx, const uint8_t *frame, uint16_t len);
    void (*tick)(void *ctx);
    void (*after)(void *ctx);    /* may be NULL */
    /* statistics */
    uint32_t n_rx, n_deferred, n_tick, n_tick_deferred, n_after;
    uint32_t max_depth, max_ring;
} MtGate;

void mt_gate_init(MtGate *g, void *ctx, void (*rx)(void *, const uint8_t *, uint16_t),
                  void (*tick)(void *), void (*after)(void *));

/* ---- M: the application's thread ---- */
void mt_gate_enter(MtGate *g);
void mt_gate_leave(MtGate *g);      /* the outermost leave runs what H/T left behind */
#define MT_GATE_ENTER(g) mt_gate_enter(g)
#define MT_GATE_LEAVE(g) mt_gate_leave(g)

/* ---- H: the protocol handler (the stub has committed a frame: head advanced) ---- */
void mt_gate_poke(MtGate *g);

/* ---- T: the Time Manager task ---- */
void mt_gate_tick(MtGate *g);

/* Nonzero while the code now running was entered from H or T (interrupt time). */
static inline int mt_gate_in_irq(const MtGate *g) { return g->in_irq != 0; }

/* Slot the stub should fill, or NULL when the ring is full (drops counted). The stub commits
 * the frame by storing the new head itself; this helper exists for the host tests. */
MtRxSlot *mt_gate_slot(MtGate *g);
void      mt_gate_commit(MtGate *g, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* MT_GATE_H */
