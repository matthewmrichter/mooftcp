/* mt_pb.c - .IPP dispatcher: driver state, entry points, deferred work, code tables.
 * TCP calls live in mt_pb_tcp.c. See mt_pb.h for the contract. */
#include <string.h>
#include "mt_pb.h"

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

/* ---- calling out ----
 * Every call into application code goes through these two, so depth says whether any glue
 * frame is below us: a TCPRelease made from inside one defers the core release (decision 3). */

void mt_pb_call_complete(MtDrv *d, MtAnyPb *pb, int16_t err) {
    d->depth++;
    mt_os_complete(pb, err);
    d->depth--;
}

void mt_pb_call_asr(MtDrv *d, const MtStreamAsr *a, uint16_t ev, uint16_t reason, MacPtr32 icmp) {
    if (!a->proc) return;                       /* no notifyProc given: accepted (matrix 202b) */
    d->depth++;
    mt_os_asr(a, ev, reason, icmp);
    d->depth--;
}

/* Perform the core releases that were deferred while a glue frame was active. */
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
    mt_os_irq_restore(sv);
    return g < c ? g : c;
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

/* Goodbye kiss: drop everything the departing application owns, silently (no routine of its
 * may run any more). Generations and connection counters move on, so a stale handle or a late
 * core callback for an old connection is recognised as such. */
void mt_pb_goodbye(MtDrv *d) {
    int sv = mt_os_irq_save();
    uint8_t i;
    for (i = 0; i < d->ntcp; i++) {
        MtStream *s = &d->tcp[i];
        uint8_t gen = s->gen;
        uint16_t conn = (uint16_t)(s->conn + 1u);
        if (!s->used || !mt_os_app_mem(s->rbuf)) continue;   /* system-heap clients stay */
        if (s->t) mt_tcp_release(s->t);         /* no callbacks; a live connection gets an RST */
        memset(s, 0, sizeof *s);
        s->gen = gen;
        s->conn = conn;
        d->rel_mask &= (uint16_t)~(1u << i);
    }
    for (i = 0; i < d->nudp; i++) {
        MtUdpStream *u = &d->udp[i];
        uint8_t gen = u->gen;
        if (!u->used || !mt_os_app_mem(u->rbuf)) continue;
        if (u->u) mt_udp_release(u->u);
        memset(u, 0, sizeof *u);
        u->gen = gen;
    }
    for (i = 0; i < MT_PB_ECHOQ; i++) {           /* a late reply is ignored */
        MtEcho *e = &d->echo[i];
        if (!e->blk.ioCompletion || mt_os_app_mem(e->blk.ioCompletion)) e->state = 0;
    }
    mt_os_irq_restore(sv);
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
