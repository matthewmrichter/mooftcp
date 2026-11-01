/* mt_pb_entry.c - the .IPP Control entry: see mt_pb_entry.h. */
#include "mt_pb_entry.h"
#include "mt_os.h"

void mt_entry_init(MtEntry *e, struct MtDrv *d, MtGate *g) {
    memset(e, 0, sizeof *e);
    e->d = d;
    e->gate = g;
}

/* The rings are filled by preempting interrupt-time callers and emptied by the owner: a short
 * interrupt-mask section around each push / pop (producers may nest). */
static int push(MtDrvPend *q, MtAnyPb *pb) {
    int m = mt_os_mask(), ok = mt_drv_pend_push(q, pb);
    mt_os_unmask(m);
    return ok;
}

static MtAnyPb *pop(MtDrvPend *q) {
    int m = mt_os_mask();
    MtAnyPb *pb = (MtAnyPb *)mt_drv_pend_pop(q);
    mt_os_unmask(m);
    return pb;
}

/* Keep only the PBs that do not live in the application's memory (PB or completion routine in m). The
 * queued PBs carry no age (born 0: mt_pb_in_gone decides by the range alone), so a live new tenant's PB
 * queued by preemption while the dead previous occupant was still recorded is dropped with the dead
 * owner's and stays inProgress (RC item 12 (e); rare: queued means issued at interrupt time over a held gate). */
static uint32_t drop_app(const MtDrv *d, MtDrvPend *q, const MtAppMem *m) {
    void *keep[MT_DRV_PENDQ];
    uint32_t n = 0, k, dropped = 0;
    int sm = mt_os_mask();
    void *pb;
    while ((pb = mt_drv_pend_pop(q)) != NULL) {
        MacPtr32 c = ((const MtAnyPb *)pb)->hdr.ioCompletion;
        if (mt_pb_in_gone(d, m, 0, mt_os_addr(pb), 0) || (c && mt_pb_in_gone(d, m, 0, c, 0))) dropped++;
        else keep[n++] = pb;
    }
    for (k = 0; k < n; k++) (void)mt_drv_pend_push(q, keep[k]);
    q->queued -= n;                              /* re-pushes are not new PBs */
    mt_os_unmask(sm);
    return dropped;
}

static void drop_queues(void *ctx, const MtAppMem *m) {
    MtEntry *e = (MtEntry *)ctx;
    e->n_pend_dropped += drop_app(e->d, &e->pend, m) + drop_app(e->d, &e->refq, m) + drop_app(e->d, &e->doneq, m);
}

/* MultiFinder per-application cleanup (mt_pb_sweep): from every Control entry (note = a task-time
 * call: record the caller's partition) and from the timer. Skipped while the gate is held by a frame
 * below us or a glue frame is active (mt_pb_sweep checks the latter). */
/* Queued PBs run only when the core may run (mt_entry_core_ok): a quit application's queued PBs are
 * dropped by the sweep, which cannot run while a glue frame is active; until then nothing is drained
 * (the timer and the gate's after-callback use this). */
void mt_entry_drain_ok(MtEntry *e, int why) {
    if ((e->d->n_own || e->d->depth) && !mt_entry_core_ok(e, why)) return;
    mt_entry_drain(e);
}

/* Glue frames left by a routine whose process quit inside it (Task 12 round 4): abandon them if that
 * is proven (mt_pb_unstick) and release the gate levels the dead chain held. gate_own = the detector's
 * own gate holding (1 inside the gate owner's callbacks, 0 at a Control entry). Returns 1 if abandoned. */
static int unstick(MtEntry *e, const MtAppMem *cur, unsigned gate_own) {
    MtDrv *d = e->d;
    MtGate *g = e->gate;
    unsigned held = d->co_gate, below = g->depth >= gate_own ? g->depth - gate_own : 0;
    if (!mt_pb_unstick(d, cur, below)) return 0;
    if (held) {                                  /* the dead chain entered the gate this many times */
        g->depth = (uint8_t)(g->depth - held);   /* below == held, so this never drops the detector's own */
        if (!g->depth) g->in_irq = 0;
    }
    return 1;
}

int mt_entry_sweep(MtEntry *e, int note, int why) {
    MtDrv *d = e->d;
    int n;
    if (!note && !d->n_own) return 0;
    n = mt_pb_sweep(d, note, drop_queues, e);
    if (why == MT_SWEEP_RX) d->n_gone_rx += (uint32_t)n;
    else if (why == MT_SWEEP_TIMER) d->n_gone_timer += (uint32_t)n;
    else d->n_gone_call += (uint32_t)n;
    return n;
}

/* Before the core runs from the input path or the timer: 1 = go on. If an owner's partition is gone
 * and the sweep cannot forget it now (a glue frame is active: an application routine runs with the
 * gate free and an interrupt took it), the frame is dropped / the tick skipped (counted): the core
 * would otherwise write into (or send from) the freed partition. TCP retransmits; the next entry,
 * frame or tick (MT_PB_SWEEP_RETRY_MS) sweeps. */
int mt_entry_core_ok(MtEntry *e, int why) {
    MtDrv *d = e->d;
    if (d->depth && d->co_n)
        (void)unstick(e, NULL, 1);               /* the processes running that routine quit inside it (round 4):
                                                 * its chain cannot hold the gate (we are its owner now), so
                                                 * only the glue accounting is cleared */
    if (!d->n_own || mt_pb_owners_alive(d)) return 1;
    mt_entry_sweep(e, 0, why);
    if (mt_pb_owners_alive(d)) return 1;
    d->n_dead_drops++;
    return 0;
}

static void goodbye(MtEntry *e) {
    MtDrv *d = e->d;
    MtGate *g = e->gate;
    MtAppMem m;
    if (g->depth || d->depth || d->in_core) {
        /* The goodbye kiss comes from ExitToShell. An application routine (completion or ASR)
         * that calls it never returns into the frames that own the core, so their ownership is
         * cleared here; mt_pb_goodbye then releases the core objects. Core callbacks fire at
         * consistent points of the core, so the core itself is in a usable state. */
        e->n_goodbye_forced++;
        d->depth = 0;
        d->co_gate = 0;
        d->in_core = 0;
        d->co_n = 0;
        d->co_over = 0;
        d->co_gen++;                             /* should a frame resume anyway, it must not depth-- */
        g->in_irq = 0;
        g->depth = 0;
    }
    (void)mt_os_app_mem(&m);                     /* its MultiFinder partition, else ApplZone..BufPtr */
    drop_queues(e, &m);
    mt_pb_goodbye(d);
    e->n_goodbye++;
}

static int16_t refuse(MtEntry *e, MtAnyPb *pb, int async) {
    e->n_refused++;
    if (async && e->gate->depth && push(&e->refq, pb)) {
        pb->hdr.ioResult = inProgress;
        e->gate->tick_pending = 1;               /* the owner drains before it lets go */
        return 0;
    }
    if (e->refuse_nest >= MT_ENTRY_REFUSE_NEST) {
        pb->hdr.ioResult = MT_DRV_REFUSED;       /* a retry storm from completion routines */
        e->n_compl_skipped++;
        return MT_DRV_REFUSED;
    }
    e->refuse_nest++;
    mt_os_complete(pb, MT_DRV_REFUSED);
    e->refuse_nest--;
    return MT_DRV_REFUSED;
}

static int16_t entry_control(MtEntry *e, MtAnyPb *pb, uint16_t trap, int irq) {
    MtDrv *d = e->d;
    int16_t cs = pb->hdr.csCode, res;
    int async = (trap & MT_DRV_TRAP_ASYNC) != 0, route;

    mt_entry_stash_a5(pb, mt_os_a5());
    e->n_calls++;

    if (cs == -1) {                              /* goodBye kiss: the app heap is going away */
        goodbye(e);
        pb->hdr.ioResult = 0;
        return 0;
    }
    /* MultiFinder sends no goodbye kiss when an application quits: notice gone partitions here (and
     * from the timer). Only a task-time call outside any frame of ours names its caller reliably. */
    /* Frames abandoned by a routine whose process quit inside it (no goodbye under MultiFinder) may
     * still hold the gate: abandon them and release those levels, then sweep (round 4). A Control entry
     * holds nothing of its own yet (gate_own 0). */
    if (d->depth) {
        MtAppMem cur;
        int part = !irq && mt_os_app_mem(&cur);  /* task time: the caller names itself */
        (void)unstick(e, part ? &cur : NULL, 0);
    }
    if (!irq && !e->gate->depth) mt_entry_sweep(e, 1, MT_SWEEP_CALL);
    else if (!irq) mt_entry_sweep(e, MT_PB_SWEEP_NOTE_ONLY, MT_SWEEP_CALL);   /* gate held: record only */
    else if (e->d->n_own && !e->gate->depth) mt_entry_sweep(e, 0, MT_SWEEP_CALL);
    if (cs == 1) {                               /* killCode (KillIO): nothing of ours is queued */
        pb->hdr.ioResult = 0;                    /* in the Device Manager; MacTCP answers 0 (120.1) */
        return 0;
    }

    route = mt_drv_route(e->gate->depth != 0, mt_drv_callout(e->gate->depth, d->depth, d->co_gate),
                         async, irq, cs);
    if (route == MT_DRV_QUEUE) {
        if (push(&e->pend, pb)) {
            pb->hdr.ioResult = inProgress;
            e->gate->tick_pending = 1;           /* the owner loops once more before it lets go */
            return 0;
        }
        route = MT_DRV_REFUSE;
    }
    if (route == MT_DRV_REFUSE) return refuse(e, pb, async);
    res = mt_pb_control(d, pb);
    if (res == inProgress) return 0;             /* contract 2: the PB is the glue's now */
    if (res == MT_PB_NOTOPEN) return res;        /* contract 4: no completion */
    /* contract 3: once, after the glue returned. It is a call-out like every other (depth,
     * co_gate), so an app that re-issues from its completion nests at most MT_PB_INLINE_MAX
     * deep (a read that empties the buffer completes here, and new data can arrive meanwhile:
     * the driver delivers frames from inside EWrite). Deeper, ioResult is final at once (a
     * sync caller goes on) and the routine runs from the outermost frame or the owner's drain. */
    pb->hdr.ioResult = res;
    if (d->depth >= MT_PB_INLINE_MAX && push(&e->doneq, pb)) {
        e->n_compl_deferred++;
        e->gate->tick_pending = 1;
        return res;
    }
    mt_pb_call_complete(d, pb, res);
    if (d->depth == 0) {                         /* the outermost frame: run what was deferred */
        MtAnyPb *x;
        while ((x = pop(&e->doneq)) != NULL) mt_pb_call_complete(d, x, x->hdr.ioResult);
        mt_pb_flush(d);
    }
    return res;
}

int16_t mt_entry_control(MtEntry *e, MtAnyPb *pb, uint16_t trap, int irq) {
    int16_t r;
    mt_os_stack_enter();                         /* stack-depth episode (no-op on the host) */
    r = entry_control(e, pb, trap, irq);
    mt_os_stack_leave();
    return r;
}

/* The gate is held, so a completion routine that calls the driver nests (call-out, depth). */
void mt_entry_drain(MtEntry *e) {
    MtDrv *d = e->d;
    MtAnyPb *pb;
    for (;;) {
        if ((pb = pop(&e->doneq)) != NULL) {        /* older results first */
            mt_pb_call_complete(d, pb, pb->hdr.ioResult);
            continue;
        }
        if ((pb = pop(&e->pend)) != NULL) {
            int16_t res = mt_pb_control(d, pb);
            e->n_pend_run++;
            if (res != inProgress && res != MT_PB_NOTOPEN) mt_pb_call_complete(d, pb, res);
            continue;
        }
        if ((pb = pop(&e->refq)) != NULL) {
            mt_pb_call_complete(d, pb, MT_DRV_REFUSED);
            continue;
        }
        return;
    }
}
