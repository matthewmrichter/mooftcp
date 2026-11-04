/* mt_pb.c - .IPP dispatcher: driver state, entry points, deferred work, code tables.
 * TCP calls live in mt_pb_tcp.c. See mt_pb.h for the contract. */
#include <string.h>
#include "mt_pb.h"

#if defined(__GNUC__)
#define OWN_BARRIER() __asm__ volatile("" ::: "memory")
#else
#define OWN_BARRIER() ((void)0)
#endif

#define DRV_ROUND(n) (((n) + 7u) & ~7u)   /* the core arena starts 8-aligned after the MtDrv */

uint32_t mt_pb_size(const mt_limits *lim) {
    return DRV_ROUND((uint32_t)sizeof(MtDrv)) + mt_stack_size(lim);
}

MtDrv *mt_pb_init(void *mem, uint32_t len, const mt_limits *lim, const mt_link *link,
                  uint32_t seed) {
    MtDrv *d = (MtDrv *)mem;
    uint32_t off = DRV_ROUND((uint32_t)sizeof(MtDrv));
    if (!mem || !lim || lim->max_tcp > MT_PB_MAX_TCP || lim->max_udp > MT_PB_MAX_UDP || len < mt_pb_size(lim)) return NULL;
    memset(d, 0, sizeof *d);
    d->s = mt_stack_init((uint8_t *)mem + off, len - off, lim, link, seed);
    if (!d->s) return NULL;
    d->own_ser = 1;                             /* objects created before any owner: age 1, never 0 */
    d->ntcp = lim->max_tcp;
    d->nudp = lim->max_udp;
    mt_pb_ip_init(d);
    return d;
}

void mt_pb_set_config(MtDrv *d, const mt_ifconfig *c) {
    int sv = mt_os_irq_save();
    mt_set_config(d->s, c);
    d->configured = (uint8_t)(c->addr != 0);
    mt_os_irq_restore(sv);
}

void mt_pb_dhcp_event(void *user, int ev, uint8_t state, mt_ip4 addr, uint32_t lease_s) {
    MtDrv *d = (MtDrv *)user;
    MtStatusView *v = d->view;
    if (ev == MT_DHCP_EV_BOUND) d->configured = 1;     /* stays 1 for the boot (ruling) */
    if (!v) return;
    v->state = state;
    v->addr = addr;
    v->lease_s = lease_s;
    if (state == MT_DHCP_BOUND && (ev == MT_DHCP_EV_BOUND || ev == MT_DHCP_EV_STATE))
        v->t_bound = mt_os_ticks();                 /* a lease or a renewal began */
}

int mt_pb_dhcp_start(MtDrv *d, const mt_ifconfig *c, mt_ip4 hint, MtStatusView *view) {
    int sv = mt_os_irq_save(), r;
    d->view = view;
    mt_set_config(d->s, c);                     /* mt_dhcp_start below drops any address in it */
    d->configured = 0;
    mt_dhcp_set_cb(d->s, mt_pb_dhcp_event, d);
    r = mt_dhcp_start(d->s, hint, mt_os_ms());
    mt_os_irq_restore(sv);
    return r;
}

/* ---- calling out ----
 * Every call into application code goes through these two, so depth says whether any glue
 * frame is below us: a TCPRelease made from inside one defers the core release (decision 3). */

static int same(const MtAppMem *a, const MtAppMem *b);
static int overlaps(const MtAppMem *a, const MtAppMem *b);

/* Before a call-out: the process whose stack is about to hold the routine's frames (mt_pb_unstick).
 * Outermost (depth 0 -> 1) at task time: the current application's partition; at interrupt time
 * nothing (no process quits there, and the interrupted one may be mid-quit). Nested at task time on
 * another process's stack than co_ctx: the chain yielded (WaitNextEvent inside a routine) and another
 * process runs nested above it; such a mixed chain is never abandoned (co_mixed). */
static void co_note(MtDrv *d) {
    MtAppMem m;
    uint8_t k;
    int have;
    if (!d->depth) {
        d->co_n = 0;
        d->co_over = 0;
    }
    have = !mt_os_in_interrupt() && mt_os_app_mem(&m);
    if (!have || (d->depth && !d->co_n)) return; /* interrupt time: no process's stack is new here */
    for (k = 0; k < d->co_n; k++)
        if (same(&d->co_ctx[k], &m)) return;    /* a stack already listed */
    if (d->co_n < MT_PB_CO_CTX) {
        d->co_ctx[d->co_n] = m;
        d->co_depth[d->co_n] = d->depth;
        d->co_n++;
    } else if (!d->co_over) {
        d->co_over = (uint8_t)(d->depth + 1u);
    }
}

/* After a call-out returned: its level is gone, and so are the processes listed for deeper levels. */
static void co_done(MtDrv *d, uint8_t sv) {
    d->depth--;
    d->co_gate = sv;
    while (d->co_n > 1 && d->depth <= d->co_depth[d->co_n - 1]) d->co_n--;
    if (d->co_over && d->depth < d->co_over) d->co_over = 0;
}

/* Dead = the partition's header changed, or a task-time caller's partition overlaps it under another
 * identity (two live partitions cannot overlap: the block was freed and reused). A different
 * application merely calling proves nothing: the glue runs A's routines inline while B is current
 * (B's drain), and A's routine re-issuing a call arrives as "B calling at depth 1". */
static int dead(const MtAppMem *c, const MtAppMem *cur) {
    return !mt_os_app_alive(c) || (cur && overlaps(c, cur) && !same(c, cur));
}

int mt_pb_unstick(MtDrv *d, const MtAppMem *cur, unsigned gate_below) {
    uint8_t k;
    if (!d->depth || !d->co_n) return 0;
    for (k = 0; k < d->co_n; k++)
        if (!dead(&d->co_ctx[k], cur)) return 0;   /* alive (slow, hung or yielding): never reset */
    /* The innermost abandoned routine died in its body, where the gate stands at co_gate (every level
     * from there down belongs to the dead chain: task-time frames of the listed processes). A gate
     * deeper than that is a live frame above the routine (or the detector's own, which it leaves out
     * of gate_below): the routine is not provably dead then. co_over: an unlisted process's frames. */
    if (gate_below != d->co_gate || d->co_over) {
        d->n_unstick_wait++;
        return 0;
    }
    d->depth = 0;
    d->co_gate = 0;
    d->in_core = 0;
    d->co_n = 0;
    d->co_gen++;
    d->n_unstuck++;
    return 1;
}

void mt_pb_call_complete(MtDrv *d, MtAnyPb *pb, int16_t err) {
    uint8_t sv = d->co_gate, gen;
    if (d->n_own && (mt_pb_gone(d, mt_os_addr(pb)) || mt_pb_gone(d, pb->hdr.ioCompletion)))
        return;                                 /* its application quit (MultiFinder): the sweep forgets it */
    if (pb->hdr.ioCompletion) co_note(d);       /* no routine, nothing can quit inside it: skip the context
                                                 * (every sync call completes through here; round 5) */
    gen = d->co_gen;
    d->co_gate = (uint8_t)mt_os_gate_depth();   /* calls from the routine find the gate here */
    d->depth++;
    mt_os_complete(pb, err);
    if (d->co_gen != gen) return;               /* abandoned meanwhile (a wrongful unstick): the accounting
                                                 * was reset, a depth-- would wrap it */
    co_done(d, sv);
}

void mt_pb_call_asr(MtDrv *d, const MtStreamAsr *a, uint16_t ev, uint16_t reason, MacPtr32 icmp) {
    uint8_t sv, gen;
    if (!a->proc) return;                       /* no notifyProc given: accepted (matrix 202b) */
    if (d->n_own && mt_pb_gone(d, a->proc)) return;   /* its application quit (MultiFinder) */
    co_note(d);
    gen = d->co_gen;
    sv = d->co_gate;
    d->co_gate = (uint8_t)mt_os_gate_depth();
    d->depth++;
    mt_os_asr(a, ev, reason, icmp);
    if (d->co_gen != gen) return;
    co_done(d, sv);
}

/* Perform the core releases that were deferred while a glue frame was active. Also called by
 * the driver entry after it ran a completion routine itself (mt_pb_flush). */
static void flush_deferred(MtDrv *d) {
    uint8_t i;
    if (d->depth || d->in_core || !d->rel_mask) return;
    for (i = 0; i < d->ntcp; i++)
        if (d->rel_mask & (1u << i)) {
            d->rel_mask &= (uint16_t)~(1u << i);
            mt_pb_tcp_free(d, &d->tcp[i]);
        }
}

/* ---- entry points ---- */

/* csCodes MacTCP 2.1 answers with 0 for a zeroed PB (matrix 120/121, "0" row). */
static int answers_zero(int16_t cs) {
    return cs == 1 || cs == 9 || cs == 10 || cs == 11 || cs == 14 || cs == 16 || cs == 18 ||
           cs == 44;
}

int16_t mt_pb_control(MtDrv *d, MtAnyPb *pb) {
    int16_t cs = pb->hdr.csCode, r;
    int sv;
    if (!d->configured) {                       /* matrix "Unconfigured run": -28 at once, */
        pb->hdr.ioResult = MT_PB_NOTOPEN;       /* no completion routine runs */
        return MT_PB_NOTOPEN;
    }
    sv = mt_os_irq_save();
    d->now = mt_os_ms();
    if ((cs >= TCPCreate && cs <= TCPGlobalInfo) || cs == 50)
        r = mt_pb_tcp_control(d, pb);           /* 50 is stream-checked like them (matrix 120.50) */
    else if (cs >= UDPCreate && cs <= UDPCtlMax)
        r = mt_pb_udp_control(d, pb);
    else if (cs == ipctlGetAddr || cs == ipctlEchoICMP || cs == ipctlLAPStats)
        r = mt_pb_ip_control(d, pb);
    else if (cs == MT_CS_DNR)
        r = mt_pb_dns_control(d, pb);           /* private: dnrp's lookups (not a MacTCP code) */
    else if (answers_zero(cs))
        r = 0;
    else
        r = MT_PB_UNIMP;                        /* matrix 120: the codes MacTCP does not implement */
    if (r != inProgress) pb->hdr.ioResult = r;
    flush_deferred(d);
    mt_os_irq_restore(sv);
    return r;
}

void mt_pb_input(MtDrv *d, const uint8_t *frame, uint16_t len) {
    int sv = mt_os_irq_save();
    if (d->in_core) {                           /* mt_input must never be re-entered */
        d->reenter_drops++;
        mt_os_irq_restore(sv);
        return;
    }
    d->now = mt_os_ms();
    d->in_core = 1;
    mt_input(d->s, frame, len, d->now);
    d->in_core = 0;
    mt_pb_tcp_after_core(d);
    flush_deferred(d);
    mt_os_irq_restore(sv);
}

uint32_t mt_pb_next_deadline_ms(MtDrv *d) {
    int sv = mt_os_irq_save();
    uint32_t c, g;
    d->now = mt_os_ms();
    c = mt_next_deadline(d->s, d->now);
    g = mt_pb_tcp_deadline(d);
    if (g > c) g = c;
    c = mt_pb_udp_deadline(d);
    if (g > c) g = c;
    c = mt_pb_ip_deadline(d);
    if (g > c) g = c;
    if (d->n_own) {                             /* MultiFinder owners: the periodic sweep (Task 12) */
        c = (int32_t)(d->own_dl - d->now) > 0 ? d->own_dl - d->now : 0;
        if (g > c) g = c;
    }
    mt_os_irq_restore(sv);
    return g;
}

void mt_pb_tick(MtDrv *d) {
    int sv = mt_os_irq_save();
    if (d->in_core) {
        d->reenter_drops++;
        mt_os_irq_restore(sv);
        return;
    }
    d->now = mt_os_ms();
    if (mt_next_deadline(d->s, d->now) == 0) {
        d->in_core = 1;
        mt_tick(d->s, d->now);
        d->in_core = 0;
    }
    mt_pb_tcp_timers(d);
    mt_pb_udp_timers(d);
    mt_pb_ip_timers(d);
    mt_pb_tcp_after_core(d);
    flush_deferred(d);
    mt_os_irq_restore(sv);
}

void mt_pb_flush(MtDrv *d) {
    int sv;
    if (d->depth || d->in_core || !d->rel_mask) return;
    sv = mt_os_irq_save();
    flush_deferred(d);
    mt_os_irq_restore(sv);
}

/* ---- per-application cleanup (goodbye kiss, MultiFinder sweep: Plan 3 Task 12) ---- */

int mt_pb_in_app(const MtAppMem *m, MacPtr32 p) {
    MacPtr32 lo = m->lo;
    if (m->hi <= 0x00FFFFFFu) {                 /* 24-bit: the high byte may carry Memory Manager flags */
        p &= 0x00FFFFFFu;
        lo &= 0x00FFFFFFu;
    }
    return p >= lo && p < m->hi;
}

static int same(const MtAppMem *a, const MtAppMem *b);

/* p lies in the departing range m and not in a LIVE recorded partition other than m: a dead owner that
 * could not be swept at once (a glue frame was active) may have had its block reused by a new application
 * that already called; the new tenant's objects are its own (round 4). */
int mt_pb_in_gone(const MtDrv *d, const MtAppMem *m, uint16_t m_born, MacPtr32 p, uint16_t born) {
    uint8_t i, left = d->n_own;
    if (!mt_pb_in_app(m, p)) return 0;
    if (!born) return 1;
    for (i = 0; left && i < MT_PB_OWNERS; i++) {
        const MtAppMem *o = &d->own[i];
        if (!o->lo) continue;
        left--;
        if (same(o, m) || !mt_pb_in_app(o, p)) continue;
        if ((int16_t)(d->own_born[i] - m_born) <= 0) continue;   /* older than m: a previous occupant's stale
                                                             * record (an alias can look alive), not a tenant */
        if ((int16_t)(born - d->own_born[i]) >= 0 && mt_os_app_alive(o))
            return 0;                           /* the tenant's: created after it was recorded */
    }
    return 1;
}

/* A stream belongs to the departing application if its receive buffer, its ASR or any PB it
 * holds (the PB itself or its completion routine) lies in the application's memory: its
 * routines must never run again either way. */
typedef struct { const MtDrv *d; const MtAppMem *m; uint16_t m_born; } Gone;
static int gone_p(const Gone *g, MacPtr32 p, uint16_t born) { return mt_pb_in_gone(g->d, g->m, g->m_born, p, born); }
static int app_pb(const Gone *g, const MtAnyPb *pb, uint16_t born) {
    return pb && (gone_p(g, mt_os_addr(pb), born) || (pb->hdr.ioCompletion && gone_p(g, pb->hdr.ioCompletion, born)));
}
static int app_tcp(const Gone *g, const MtStream *s) {
    uint8_t k;
    if (gone_p(g, s->rbuf, s->born) || (s->asr && gone_p(g, s->asr, s->born))) return 1;
    if (app_pb(g, s->open_pb, s->born) || app_pb(g, s->close_pb, s->born)) return 1;
    for (k = 0; k < s->rq_n; k++) if (app_pb(g, s->rq[k].pb, s->born)) return 1;
    for (k = 0; k < s->sq_n; k++) if (app_pb(g, s->sq[k].pb, s->born)) return 1;
    return 0;
}
static int app_udp(const Gone *g, const MtUdpStream *u) {
    uint8_t k;
    if (gone_p(g, u->rbuf, u->born) || (u->asr && gone_p(g, u->asr, u->born))) return 1;
    for (k = 0; k < u->rq_n; k++) if (app_pb(g, u->rq[k].pb, u->born)) return 1;
    return 0;
}

/* Forget everything the application in m owns, silently (no routine of its may run any more).
 * Generations and connection counters move on, so a stale handle or a late core callback for an
 * old connection is recognised as such. A UDP stream's loans (Task 4) go with it. m_born: the age
 * of m's owner record (the sweep), or own_ser for the goodbye kiss (nothing is newer: nothing spared). */
static void forget_aged(MtDrv *d, const MtAppMem *m, uint16_t m_born) {
    int sv = mt_os_irq_save();
    uint8_t i;
    Gone g;
    g.d = d;
    g.m = m;
    g.m_born = m_born;
    for (i = 0; i < d->ntcp; i++) {
        MtStream *s = &d->tcp[i];
        uint8_t gen = s->gen;
        uint16_t conn = (uint16_t)(s->conn + 1u);
        if (!s->used || !app_tcp(&g, s)) continue;  /* system-heap clients and other partitions stay */
        if (s->t) mt_tcp_release(s->t);         /* no callbacks; a live connection gets an RST */
        memset(s, 0, sizeof *s);
        s->gen = gen;
        s->conn = conn;
        d->rel_mask &= (uint16_t)~(1u << i);
    }
    for (i = 0; i < d->nudp; i++) {
        MtUdpStream *u = &d->udp[i];
        uint8_t gen = u->gen;
        if (!u->used || !app_udp(&g, u)) continue;
        if (u->u) mt_udp_release(u->u);
        memset(u, 0, sizeof *u);
        u->gen = gen;
    }
    mt_pb_dns_goodbye(d, m, m_born);
    for (i = 0; i < MT_PB_ECHOQ; i++) {           /* a late reply is ignored */
        MtEcho *e = &d->echo[i];
        if (e->state && e->blk.ioCompletion && gone_p(&g, e->blk.ioCompletion, e->born)) e->state = 0;
    }
    mt_os_irq_restore(sv);
}

void mt_pb_forget(MtDrv *d, const MtAppMem *m) { forget_aged(d, m, d->own_ser); }

/* See mt_pb.h "Owner table slots": lo first, so a concurrent reader sees an empty slot. */
static void own_drop(MtDrv *d, uint8_t i) {
    *(volatile MacPtr32 *)&d->own[i].lo = 0;
    OWN_BARRIER();
    d->own[i].hi = d->own[i].h0 = d->own[i].name = 0;
    if (d->n_own) d->n_own--;
}
static void own_set(MtDrv *d, uint8_t i, const MtAppMem *m) {
    if (!++d->own_ser) d->own_ser = 1;          /* never 0: that is "age unknown" (mt_pb_in_gone) */
    d->own_born[i] = d->own_ser;
    d->own[i].hi = m->hi;
    d->own[i].h0 = m->h0;
    d->own[i].name = m->name;
    OWN_BARRIER();
    *(volatile MacPtr32 *)&d->own[i].lo = m->lo;    /* last: the slot becomes visible whole */
    d->n_own++;
}

/* Goodbye kiss: the current application (its partition under MultiFinder, else ApplZone..BufPtr). */
void mt_pb_goodbye(MtDrv *d) {
    MtAppMem m;
    uint8_t i;
    int part = mt_os_app_mem(&m);
    mt_pb_forget(d, &m);
    if (part)
        for (i = 0; i < MT_PB_OWNERS; i++)
            if (d->own[i].lo == m.lo) own_drop(d, i);
}

/* Any recorded dead owner containing p counts, even if a live owner (a new tenant of the block, round
 * 4) contains it too: a stale PB or routine of the dead one would point into the tenant's memory. The
 * tenant's own routines are withheld with them until the dead owner is swept (counted here as well). */
int mt_pb_gone(MtDrv *d, MacPtr32 p) {
    uint8_t i;
    if (!d->n_own || !p) return 0;
    for (i = 0; i < MT_PB_OWNERS; i++) {
        const MtAppMem *o = &d->own[i];
        if (o->lo && mt_pb_in_app(o, p) && !mt_os_app_alive(o)) {
            d->n_dead_calls++;
            return 1;
        }
    }
    return 0;
}

static int overlaps(const MtAppMem *a, const MtAppMem *b) {
    return a->lo < b->hi && b->lo < a->hi;
}

static int same(const MtAppMem *a, const MtAppMem *b) {
    return a->lo == b->lo && a->hi == b->hi && a->h0 == b->h0 && a->name == b->name;
}

/* Read-only pre-check (no gate: the table only changes inside it, and a stale read just sends us
 * through the full path or leaves the work to the next call or tick): every owner alive, none
 * overlapping the caller, and the caller (if any) already recorded. */
static int quiet(const MtDrv *d, const MtAppMem *cur) {
    const MtAppMem *o = d->own;
    uint8_t left = d->n_own, i;
    int known = 0;
    for (i = 0; left && i < MT_PB_OWNERS; i++, o++) {   /* bounded: n_own is only a hint */
        if (!*(volatile const MacPtr32 *)&o->lo) continue;
        left--;
        if (cur && same(o, cur)) { known = 1; continue; }
        if (!mt_os_app_alive(o) || (cur && overlaps(o, cur))) return 0;
    }
    return !cur || known;
}

/* The input path's check (mt_drv.c rx_cb, before every frame while owners exist): every recorded
 * partition still there? Inline header loads only; the full sweep runs when one is gone. */
int mt_pb_owners_alive(const MtDrv *d) {
    const MtAppMem *o = d->own;
    uint8_t left = d->n_own, i;
    for (i = 0; left && i < MT_PB_OWNERS; i++, o++) {
        if (!*(volatile const MacPtr32 *)&o->lo) continue;
        left--;
        if (!mt_os_app_alive(o)) return 0;
    }
    return 1;
}

int mt_pb_sweep(MtDrv *d, int note, void (*drop)(void *ctx, const MtAppMem *m), void *ctx) {
    MtAppMem cur, gone;
    int sv, n = 0, part = 0, known = 0, can;
    uint8_t i, fr = MT_PB_OWNERS;
    uint16_t gone_born;
    if (!note && !d->n_own) return 0;            /* no MultiFinder caller recorded: nothing to watch */
    if (note) part = mt_os_app_mem(&cur);        /* task time: the caller's own globals */
    if (!part && !d->n_own) return 0;
    if (!note) {                                 /* timer / input path (gate owner): every time */
        if (quiet(d, NULL)) {
            d->own_dl = mt_os_ms() + MT_PB_SWEEP_MS;
            return 0;
        }
    } else if (quiet(d, part ? &cur : NULL)) {
        return 0;                                /* the common case: no gate round trip */
    } else if (note == MT_PB_SWEEP_NOTE_ONLY && !part) {
        return 0;
    }
    sv = mt_os_irq_save();
    can = note != MT_PB_SWEEP_NOTE_ONLY && !(d->depth || d->in_core);   /* a glue frame active (or the gate held
                                                  * by one, NOTE_ONLY): forgetting waits for the next call or
                                                  * tick; the caller is still recorded (round 4: so its objects
                                                  * are swept when it quits, and as a new tenant of a dead
                                                  * owner's block its objects are spared, mt_pb_in_gone) */
    d->now = mt_os_ms();
    for (i = 0; i < MT_PB_OWNERS; i++) {
        MtAppMem *o = &d->own[i];
        if (!o->lo) { if (fr == MT_PB_OWNERS) fr = i; continue; }
        if (part && same(o, &cur)) {
            known = 1;                           /* the caller itself: alive */
            continue;
        }
        if (!can || (mt_os_app_alive(o) && !(part && overlaps(o, &cur)))) continue;
        gone = *o;                               /* gone, or its memory now belongs to the caller */
        gone_born = d->own_born[i];
        own_drop(d, i);
        if (fr == MT_PB_OWNERS) fr = i;
        d->n_own_gone++;
        n++;
        if (drop) drop(ctx, &gone);
        forget_aged(d, &gone, gone_born);
    }
    if (part && !known) {
        if (fr < MT_PB_OWNERS) {
            own_set(d, fr, &cur);
        } else {
            d->n_own_full++;
        }
    }
    if (!can) d->own_dl = d->now + MT_PB_SWEEP_RETRY_MS;   /* the timer must not re-fire at deadline 0 */
    else if (d->n_own) d->own_dl = d->now + MT_PB_SWEEP_MS;
    mt_os_irq_restore(sv);
    return n;
}

/* ---- code tables (core -> MacTCP) ---- */

uint16_t mt_pb_event_code(int core_ev) {
    switch (core_ev) {
    case MT_TCP_EV_DATA:        return TCPDataArrival;
    case MT_TCP_EV_URGENT:      return TCPUrgent;
    case MT_TCP_EV_CLOSING:     return TCPClosing;      /* delivered once all data is read */
    case MT_TCP_EV_ULP_TIMEOUT: return TCPULPTimeout;
    case MT_TCP_EV_TERMINATE:   return TCPTerminate;
    case MT_TCP_EV_ICMP:        return TCPICMPReceived;
    default:                    return 0;               /* ESTABLISHED completes the open PB */
    }
}

uint16_t mt_pb_term_reason(int core_reason) {
    switch (core_reason) {
    case MT_TERM_REMOTE_ABORT:      return TCPRemoteAbort;
    case MT_TERM_NETWORK_FAILURE:   return TCPNetworkFailure;
    case MT_TERM_SEC_PREC_MISMATCH: return TCPSecPrecMismatch;
    case MT_TERM_ULP_TIMEOUT:       return TCPULPTimeoutTerminate;
    case MT_TERM_ULP_ABORT:         return TCPULPAbort;
    case MT_TERM_ULP_CLOSE:         return TCPULPClose;
    case MT_TERM_SERVICE_ERROR:     return TCPServiceError;
    default:                        return 0;  /* OPEN_FAILED: the open PB fails, no ASR (matrix 301) */
    }
}

int16_t mt_pb_icmp_type(uint32_t core_arg) {
    uint8_t type = (uint8_t)(core_arg >> 8), code = (uint8_t)core_arg;
    if (type == 11) return timeExceeded;
    if (type == 12) return parmProblem;
    if (type != 3) return netUnreach;
    switch (code) {
    case 0: case 6: case 9: case 11: return netUnreach;
    case 2:  return protocolUnreach;
    case 3:  return portUnreach;
    case 4:  return fragReqd;
    case 5:  return sourceRouteFailed;
    default: return hostUnreach;              /* 1, 7, 8, 10, 12, 13, ... */
    }
}
