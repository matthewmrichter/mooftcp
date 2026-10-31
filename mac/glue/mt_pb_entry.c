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

/* Keep only the PBs that do not live in the application's memory. */
static uint32_t drop_app(MtDrvPend *q) {
    void *keep[MT_DRV_PENDQ];
    uint32_t n = 0, k, dropped = 0;
    int m = mt_os_mask();
    void *pb;
    while ((pb = mt_drv_pend_pop(q)) != NULL) {
        MacPtr32 c = ((const MtAnyPb *)pb)->hdr.ioCompletion;
        if (mt_os_app_mem(mt_os_addr(pb)) || (c && mt_os_app_mem(c))) dropped++;
        else keep[n++] = pb;
    }
    for (k = 0; k < n; k++) (void)mt_drv_pend_push(q, keep[k]);
    q->queued -= n;                              /* re-pushes are not new PBs */
    mt_os_unmask(m);
    return dropped;
}

static void goodbye(MtEntry *e) {
    MtDrv *d = e->d;
    MtGate *g = e->gate;
    if (g->depth || d->depth || d->in_core) {
        /* The goodbye kiss comes from ExitToShell. An application routine (completion or ASR)
         * that calls it never returns into the frames that own the core, so their ownership is
         * cleared here; mt_pb_goodbye then releases the core objects. Core callbacks fire at
         * consistent points of the core, so the core itself is in a usable state. */
        e->n_goodbye_forced++;
        d->depth = 0;
        d->co_gate = 0;
        d->in_core = 0;
        g->in_irq = 0;
        g->depth = 0;
    }
    e->n_pend_dropped += drop_app(&e->pend) + drop_app(&e->refq) + drop_app(&e->doneq);
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
