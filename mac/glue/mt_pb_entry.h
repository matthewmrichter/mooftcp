/* mt_pb_entry.h - the .IPP Control entry above the PB dispatcher: routing (mt_drv_logic.h), the
 * PBs queued by preemption, refusals, KillIO and the goodbye kiss. Plain C on mt_os.h, so it is
 * host-tested (tests/test_drv.c through tests/pbfake.c); the 68k driver (mt_drv.c) only takes
 * the PB off the Device Manager queue and calls mt_entry_control.
 *
 * Contract of mt_entry_control (the driver returns its result in D0):
 *   - csCode -1 (goodbye kiss): every stream, echo and queued PB of the application is forgotten
 *     silently (mt_pb_goodbye). If an application routine was running (gate owned, call-out or
 *     inside the core), the application is leaving through ExitToShell from inside that routine:
 *     its frames will never return, so the ownership they held is cleared first.
 *   - csCode 1 (KillIO): 0, nothing is cancelled (nothing of ours sits in the Device Manager
 *     queue; MacTCP's behaviour for pending PBs is unmeasured, PLAN2-CARRYOVER.md).
 *   - Routed MT_DRV_QUEUE: ioResult = inProgress, the PB joins `pend`; the gate owner runs it in
 *     mt_entry_drain before it lets go of the core.
 *   - Refused (MT_DRV_REFUSE, or the pending ring full): an ASYNC PB while the core is owned is
 *     put on `refq` (ioResult = inProgress) and its completion runs from mt_entry_drain, never
 *     inside the caller's PBControl: an application that retries from its completion routine
 *     would otherwise recurse on the interrupt stack. A SYNC PB (or `refq` full) is completed at
 *     once, but at most MT_ENTRY_REFUSE_NEST deep; deeper, only ioResult is set (counted).
 *   - Otherwise mt_pb_control and the mt_pb.h result contract (completion once, after the glue
 *     returned; none for -28 and inProgress). The completion is a call-out (MtDrv.depth); nested
 *     MT_PB_INLINE_MAX deep it is deferred (`doneq`, ioResult already final) to the outermost
 *     entry frame or the gate owner's drain, in order. */
#ifndef MT_PB_ENTRY_H
#define MT_PB_ENTRY_H

#include <stdint.h>
#include <string.h>
#include "mt_pb.h"
#include "mt_gate.h"
#include "mt_drv_logic.h"

/* MtEntry itself is in mt_drv_logic.h (no MacTCP types: mt_drv.h and MoofDiag use it). */

void    mt_entry_init(MtEntry *e, struct MtDrv *d, MtGate *g);
/* One Control call. The PB is already off the Device Manager queue. trap = its ioTrap; irq:
 * the call is made at interrupt time (SR mask != 0). */
int16_t mt_entry_control(MtEntry *e, MtAnyPb *pb, uint16_t trap, int irq);
/* MultiFinder per-application cleanup (Plan 3 Task 12): mt_pb_sweep with the entry's queues. note = the
 * current call is a task-time application call (its partition is recorded). The timer calls it with 0. */
enum { MT_SWEEP_CALL = 0, MT_SWEEP_TIMER = 1, MT_SWEEP_RX = 2 };   /* who swept (MtDrv.n_gone_*) */
int     mt_entry_sweep(MtEntry *e, int note, int why);
/* The input path (why MT_SWEEP_RX) and the timer (MT_SWEEP_TIMER) call this before the core: 0 = an
 * owner is gone and could not be swept now: drop the frame / skip the tick (MtDrv.n_dead_drops). */
int     mt_entry_core_ok(MtEntry *e, int why);
/* mt_entry_drain only when mt_entry_core_ok allows (the timer and the gate's after-callback). */
void    mt_entry_drain_ok(MtEntry *e, int why);
/* The gate owner (tick / after callbacks): run the queued PBs, then complete the refused ones. */
void    mt_entry_drain(MtEntry *e);

/* The A5 recorded at issue time lives in the PB's ioCmdAddr (bytes 8..11 of the Device Manager's
 * fill12), which the Device Manager does not use for a control call once the PB is off its
 * queue; the ICMPEcho notify block copies it with the header. 0 = not recorded. */
static inline void mt_entry_stash_a5(MtAnyPb *pb, MacPtr32 a5) { memcpy(&pb->hdr.fill12[8], &a5, 4); }
static inline MacPtr32 mt_entry_stashed_a5(const MtAnyPb *pb) {
    MacPtr32 a5;
    memcpy(&a5, &pb->hdr.fill12[8], 4);
    return a5;
}
static inline uint16_t mt_entry_trap(const MtAnyPb *pb) {
    return (uint16_t)((uint16_t)(uint8_t)pb->hdr.fill12[6] << 8 | (uint8_t)pb->hdr.fill12[7]);
}

#endif /* MT_PB_ENTRY_H */
