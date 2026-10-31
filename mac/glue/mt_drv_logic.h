/* mt_drv_logic.h - the pure decisions of the .IPP driver entry (mt_drv.c), header-only so the
 * host tests (tests/test_drv.c) run them. No Toolbox, no globals.
 *
 * WHO MAY ENTER THE CORE (binding Task 10 ruling, from the Task 9 review I2). The core gate
 * (mt_gate.h) has one owner at a time. A PBControl arriving while the gate is owned is one of:
 *   - a call-out: the owner is running an application routine (completion or ASR, MtDrv.depth
 *     != 0) and that routine calls the driver again. It nests: the glue is re-entrant there.
 *   - a preemption: some interrupt-time code (a VBL task, another driver's completion routine)
 *     interrupted the owner in the middle of a core call (MtDrv.depth == 0). It must not touch
 *     the core now. An asynchronous call is queued (ioResult = inProgress) and run by the owner
 *     when it lets go of the core; a synchronous call is refused (it could only spin forever:
 *     the owner it waits for cannot resume until it returns).
 * And a SYNCHRONOUS call that can stay pending is refused when it is made at interrupt time
 * (SR mask != 0) or from inside a call-out: the Device Manager would spin on ioResult with the
 * input path masked or the gate held, a certain hang. Calls that never go pending (status,
 * create, release, abort, ...) always run.
 *
 * Refusal code: MT_DRV_REFUSED (insufficientResources, -23009). Not measured on MacTCP (whose
 * sync calls at interrupt level are a documented misuse); see the Task 10 report's ruling. */
#ifndef MT_DRV_LOGIC_H
#define MT_DRV_LOGIC_H

#include <stdint.h>
#include "mt_gate.h"

#define MT_DRV_REFUSED (-23009)  /* insufficientResources */

enum { MT_DRV_RUN = 0, MT_DRV_QUEUE = 1, MT_DRV_REFUSE = 2 };

/* csCodes whose PB can stay pending (inProgress): TCPPassiveOpen/ActiveOpen 31/32, TCPSend 34,
 * TCPNoCopyRcv 35, TCPRcv 37, TCPClose 38, UDPRead 21, UDPMultiRead 29. */
static inline int mt_drv_may_pend(int16_t cs) {
    return cs == 31 || cs == 32 || cs == 34 || cs == 35 || cs == 37 || cs == 38 || cs == 21 ||
           cs == 29;
}

/* A call is a call-out only if an application routine is running (MtDrv.depth != 0) AND the gate
 * is still at the depth recorded when that routine was called (MtDrv.co_gate). If the routine has
 * itself called back into the glue and is now inside a core call, the gate is deeper: a call made
 * then comes from an interrupt that preempted it (final review, fix 1) and must not nest. */
static inline int mt_drv_callout(unsigned gate_depth, unsigned drv_depth, unsigned co_gate) {
    return drv_depth != 0 && gate_depth == co_gate;
}

/* gate_owned: MtGate.depth != 0; callout: mt_drv_callout; async: ioTrap bit 10;
 * irq: SR interrupt mask != 0. */
static inline int mt_drv_route(int gate_owned, int callout, int async, int irq, int16_t cs) {
    if (gate_owned && !callout) return async ? MT_DRV_QUEUE : MT_DRV_REFUSE;
    if (!async && (irq || gate_owned) && mt_drv_may_pend(cs)) return MT_DRV_REFUSE;
    return MT_DRV_RUN;
}

/* The queue of PBs that arrived by preemption: a ring of pointers, FIFO. The producer runs at
 * interrupt time over a suspended consumer, and producers may nest, so mt_drv.c wraps push and
 * pop in a short interrupt-mask section; the ring itself is plain. */
#define MT_DRV_PENDQ 16u
typedef struct MtDrvPend {
    void    *pb[MT_DRV_PENDQ];
    uint16_t head, tail;         /* push at head, pop at tail; one slot stays free */
    uint32_t queued, full;       /* statistics: PBs queued, PBs refused because the ring was full */
} MtDrvPend;

static inline int mt_drv_pend_push(MtDrvPend *q, void *pb) {
    uint16_t next = (uint16_t)((q->head + 1u) % MT_DRV_PENDQ);
    if (next == q->tail) { q->full++; return 0; }
    q->pb[q->head] = pb;
    q->head = next;
    q->queued++;
    return 1;
}

static inline void *mt_drv_pend_pop(MtDrvPend *q) {
    void *pb;
    if (q->tail == q->head) return 0;
    pb = q->pb[q->tail];
    q->tail = (uint16_t)((q->tail + 1u) % MT_DRV_PENDQ);
    return pb;
}

/* The Control entry's state (functions: mt_pb_entry.h). */
#define MT_ENTRY_REFUSE_NEST 4
struct MtDrv;
typedef struct MtEntry {
    struct MtDrv *d;
    MtGate       *gate;
    MtDrvPend     pend;          /* PBs queued by preemption (run by the gate owner) */
    MtDrvPend     refq;          /* async PBs refused while the core was owned (completed by the owner) */
    MtDrvPend     doneq;         /* results whose completion was too deep to run inline (ioResult set) */
    volatile uint8_t refuse_nest;/* inline refusal completions now running */
    uint8_t       pad[3];
    uint32_t      n_calls, n_refused, n_pend_run, n_goodbye;
    uint32_t      n_goodbye_forced;  /* goodbyes that found an application routine running */
    uint32_t      n_adopted;         /* PBs found left in the Device Manager queue (mt_drv.c) */
    uint32_t      n_compl_skipped;   /* refusals past MT_ENTRY_REFUSE_NEST: ioResult only */
    uint32_t      n_pend_dropped;    /* queued PBs of an application forgotten at its goodbye */
    uint32_t      n_compl_deferred;  /* results completed later because completions nested too deep */
} MtEntry;

/* ioTrap bits the Device Manager stores in the PB (offset 6). */
#define MT_DRV_TRAP_IMMED 0x0200u
#define MT_DRV_TRAP_ASYNC 0x0400u

#endif /* MT_DRV_LOGIC_H */
