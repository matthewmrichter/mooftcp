/* mt_pb_udp.c - MacTCP UDP calls (csCodes 20..29) on the core's mt_udp_* API.
 *
 * Same rules as mt_pb_tcp.c (see there and mt_pb.h):
 *  - A PB leaves the read queue BEFORE its completion routine runs; completion routines and ASRs
 *    may call the driver again, so after every call out the stream is re-validated (same():
 *    slot live, same generation). No pointer into a queue is held across a call out.
 *  - Pending reads complete before the stream is released (UDPRelease: -23012, matrix 906).
 *  - UDPRead is zero-copy as in MacTCP (matrix 903): rcvBuff is the datagram's address inside the
 *    app's own receive buffer and it stays queued in the core until UDPBfrReturn. The core can
 *    only peek at its oldest datagram, so one datagram is lent at a time (s->loan); a further
 *    UDPRead waits until the loan is returned (carry-over note in PLAN2-CARRYOVER.md).
 *  - UDPRelease frees the core endpoint at once, in every context. Unlike a TCB, a UDP endpoint
 *    is released by a memset and no core code touches it after a callback returns, so there is
 *    nothing to defer; the core stops writing the app's buffer right away. The glue slot stays
 *    "releasing" until the pending reads have been failed, so it cannot be re-created under a
 *    running completion routine. */
#include <string.h>
#include "mt_pb.h"

#define LIVE      1
#define RELEASING 2
#define UDP_MAX_DGRAM 65507u
#define MS(sec) ((uint32_t)(uint16_t)(sec) * 1000u)

static int same(const MtUdpStream *s, uint8_t gen) { return s->used == LIVE && s->gen == gen; }

static MtUdpStream *stream_of(MtDrv *d, StreamPtr h) {
    MacPtr32 base = mt_os_addr(&d->udp[0]);
    uint32_t off = h - base, i;
    if (off >= (uint32_t)d->nudp * (uint32_t)sizeof(MtUdpStream)) return NULL;
    i = off / (uint32_t)sizeof(MtUdpStream);
    if (d->udp[i].used != LIVE || d->udp[i].handle != h) return NULL;   /* stale or misaligned */
    return &d->udp[i];
}

static void complete(MtUdpStream *s, MtAnyPb *pb, int16_t err) { mt_pb_call_complete(s->d, pb, err); }

static void asr(MtUdpStream *s, uint16_t ev, MacPtr32 icmp) {
    MtStreamAsr a;
    a.proc = s->asr;
    a.stream = s->handle;
    a.user = s->user;
    a.a5 = s->a5;
    a.kind = MT_ASR_UDP;
    mt_pb_call_asr(s->d, &a, ev, 0, icmp);
}

static MtAnyPb *rq_take(MtUdpStream *s, uint8_t k) {
    MtAnyPb *pb = s->rq[k].pb;
    for (; k + 1 < s->rq_n; k++) s->rq[k] = s->rq[k + 1];
    s->rq_n--;
    return pb;
}

/* ---- read ---- */

/* Give the oldest queued datagram to a read PB and lend it. 0 if none is queued. */
static int take_datagram(MtUdpStream *s, MtAnyPb *pb) {
    mt_udp_dgram dg;
    if (s->loan || !mt_udp_peek(s->u, &dg)) return 0;
    pb->udp.csParam.receive.remoteHost = dg.src;
    pb->udp.csParam.receive.remotePort = dg.sport;
    pb->udp.csParam.receive.rcvBuff = mt_os_addr(dg.data);       /* inside the app's buffer */
    pb->udp.csParam.receive.rcvBuffLen = dg.len;
    s->loan = mt_os_addr(dg.data);
    return 1;
}

/* Serve pending reads in FIFO order while datagrams are queued and none is lent. */
static void serve_reads(MtUdpStream *s) {
    uint8_t g = s->gen;
    while (s->rq_n && s->u && !s->loan) {
        MtAnyPb *pb = s->rq[0].pb;
        if (!take_datagram(s, pb)) break;
        rq_take(s, 0);
        complete(s, pb, 0);
        if (!same(s, g)) return;
    }
}

static int16_t do_read(MtDrv *d, MtUdpStream *s, MtAnyPb *pb) {
    uint16_t tmo = pb->udp.csParam.receive.timeOut;
    if (!s->rq_n && take_datagram(s, pb)) return 0;              /* matrix 903b: at once */
    if (s->rq_n >= MT_PB_UDPRQ) return insufficientResources;
    s->rq[s->rq_n].pb = pb;                                      /* matrix 906: several, FIFO */
    s->rq[s->rq_n].timed = (uint8_t)(tmo != 0);                  /* 0 = no timeout (906) */
    s->rq[s->rq_n].dl = d->now + MS(tmo);
    s->rq_n++;
    pb->hdr.ioResult = inProgress;
    return inProgress;
}

static int16_t do_bfr_return(MtUdpStream *s, MtAnyPb *pb) {
    if (!s->loan || pb->udp.csParam.receive.rcvBuff != s->loan)
        return invalidBufPtr;                                    /* matrix 903: again, or made up */
    s->loan = 0;
    mt_udp_done(s->u);
    serve_reads(s);                                              /* a read may have waited for it */
    return 0;
}

/* ---- core callback ---- */

static void on_icmp(MtUdpStream *s, uint32_t arg) {
    MtDrv *d = s->d;
    mt_ifconfig c;
    mt_get_config(d->s, &c);
    d->icmp.streamPtr = s->handle;
    d->icmp.localHost = c.addr;
    d->icmp.localPort = mt_udp_port(s->u);
    d->icmp.remoteHost = 0;              /* the core's UDP callback does not pass the quoted target */
    d->icmp.remotePort = 0;
    d->icmp.reportType = mt_pb_icmp_type(arg);
    d->icmp.optionalAddlInfo = 0;
    d->icmp.optionalAddlInfoPtr = 0;
    asr(s, UDPICMPReceived, mt_os_addr(&d->icmp));
}

static void udp_cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    MtUdpStream *s = (MtUdpStream *)user;
    if (s->used != LIVE || s->u != u) return;       /* released: nothing reaches the app */
    if (ev == MT_UDP_EV_DATA) {
        if (s->rq_n) serve_reads(s);                /* matrix 903: no UDPDataArrival with a read pending */
        else asr(s, UDPDataArrival, 0);             /* matrix 903b */
    } else if (ev == MT_UDP_EV_ICMP) {
        on_icmp(s, arg);
    }
}

/* ---- create / release ---- */

static int16_t do_create(MtDrv *d, MtAnyPb *pb) {
    uint32_t len = pb->udp.csParam.create.rcvBuffLen;
    MacPtr32 buf = pb->udp.csParam.create.rcvBuff;
    uint16_t port = pb->udp.csParam.create.localPort;
    MtUdpStream *s = NULL;
    uint8_t i = 0, k;
    if (len < MT_PB_MIN_UDPBUF) return invalidLength;            /* matrix 900, 120.20 */
    if (!buf) return invalidBufPtr;
    for (k = 0; k < d->nudp; k++) {                              /* rotate: a freed slot rests */
        i = (uint8_t)((d->next_uslot + k) % d->nudp);
        if (d->udp[i].used == 0) { s = &d->udp[i]; break; }
    }
    if (!s) return insufficientResources;
    {
        uint8_t gen = (uint8_t)(s->gen + 1u);
        memset(s, 0, sizeof *s);
        s->gen = gen;
    }
    s->u = mt_udp_create(d->s, port, (uint8_t *)mt_os_ptr(buf), len, udp_cb, s);
    if (!s->u) return port ? duplicateSocket : insufficientResources;   /* matrix 901b: -23017 */
    d->next_uslot = (uint8_t)((i + 1u) % d->nudp);
    s->d = d;
    s->used = LIVE;
    s->handle = mt_os_addr(s) + ((uint32_t)(s->gen & 7u) << 1);
    s->asr = pb->udp.csParam.create.notifyProc;
    s->user = pb->udp.csParam.create.userDataPtr;
    s->a5 = mt_os_a5();
    s->rbuf = buf;
    s->rlen = len;
    pb->udp.udpStream = s->handle;
    pb->udp.csParam.create.localPort = mt_udp_port(s->u);       /* matrix 901: ephemeral port written back */
    return 0;
}

static int16_t do_release(MtUdpStream *s, MtAnyPb *pb) {
    mt_udp_release(s->u);                    /* first: the core stops using the app's buffer */
    s->u = NULL;
    s->used = RELEASING;                     /* the handle is invalid from here on */
    s->loan = 0;
    while (s->rq_n) complete(s, rq_take(s, 0), connectionTerminated);   /* matrix 906 */
    pb->udp.csParam.create.rcvBuff = s->rbuf;                    /* the buffer goes back to the app */
    pb->udp.csParam.create.rcvBuffLen = s->rlen;
    s->used = 0;
    return 0;
}

/* ---- write ---- */

static int16_t map_send_err(int r) {
    switch (r) {
    case MT_OK:        return 0;
    case MT_ERR_INVAL: return invalidLength;
    case MT_ERR_NOMEM: return ipNoFragMemErr;
    default:           return ipRouteErr;
    }
}

static int16_t do_write(MtDrv *d, MtUdpStream *s, MtAnyPb *pb) {
    const uint8_t *w = (const uint8_t *)mt_os_ptr(pb->udp.csParam.send.wdsPtr);
    const uint8_t *p = NULL;
    uint32_t total = 0, len = 0;
    uint8_t k, n = 0;
    wdsEntry e;
    if (!w) return invalidWDS;                                   /* matrix 911.28 */
    for (k = 0; k < MT_PB_WDS_MAX; k++) {
        memcpy(&e, w + (uint32_t)k * sizeof(wdsEntry), sizeof e);
        if (e.length == 0) break;                                /* the terminator; a datagram of
                                                                    zero bytes is legal (matrix 904) */
        if (!e.ptr) return invalidWDS;
        total += e.length;
        n++;
    }
    if (k == MT_PB_WDS_MAX) return invalidWDS;                   /* unterminated */
    if (total > UDP_MAX_DGRAM) return invalidLength;
    if (!mt_pb_dest_ok(pb->udp.csParam.send.remoteHost)) return ipBadAddr;
    if (n == 1) {
        memcpy(&e, w, sizeof e);
        p = (const uint8_t *)mt_os_ptr(e.ptr);                   /* sent from the app's buffer */
        len = total;
    } else if (n > 1) {                                          /* matrix 904c: gathered in order */
        if (total > MT_PB_UDP_GATHER) return invalidLength;
        for (k = 0; k < n; k++) {
            memcpy(&e, w + (uint32_t)k * sizeof(wdsEntry), sizeof e);
            memcpy(d->ubuf + len, mt_os_ptr(e.ptr), e.length);
            len += e.length;
        }
        p = d->ubuf;
    }
    return map_send_err(mt_udp_send(s->u, pb->udp.csParam.send.remoteHost,
                                    pb->udp.csParam.send.remotePort, p, (uint16_t)len,
                                    pb->udp.csParam.send.checkSum != 0, d->now));
}

static int16_t do_mtu(MtDrv *d, MtAnyPb *pb) {
    mt_ifconfig c;
    mt_ip4 r = pb->udp.csParam.mtu.remoteHost;
    uint32_t m;
    mt_get_config(d->s, &c);
    m = (uint32_t)c.mtu - 28u;                                   /* IP + UDP headers */
    if (r == 0 || ((r ^ c.addr) & c.mask) != 0) {                /* off our subnet: 576 - 28 (matrix 910) */
        if (m > MT_PB_UDP_MTU_OFFNET) m = MT_PB_UDP_MTU_OFFNET;
    }
    pb->udp.csParam.mtu.mtuSize = (uint16_t)m;
    return 0;
}

int16_t mt_pb_udp_control(MtDrv *d, MtAnyPb *pb) {
    int16_t cs = pb->hdr.csCode;
    MtUdpStream *s;
    switch (cs) {
    case UDPCreate:    return do_create(d, pb);
    case UDPMaxMTUSize: return do_mtu(d, pb);                    /* no stream (matrix 120/121: 0) */
    case UDPStatus:    return 0;       /* layout OPEN (matrix 911.26): accepted, nothing written */
    case UDPMultiCreate:               /* exists in MacTCP 2.1; not supported here */
        return pb->udp.csParam.create.rcvBuffLen < MT_PB_MIN_UDPBUF ? invalidLength : MT_PB_UNIMP;
    default: break;
    }
    s = stream_of(d, pb->udp.udpStream);
    if (!s) return invalidStreamPtr;                             /* matrix 120/121, 907 */
    switch (cs) {
    case UDPRead:      return do_read(d, s, pb);
    case UDPBfrReturn: return do_bfr_return(s, pb);
    case UDPWrite:     return do_write(d, s, pb);
    case UDPRelease:   return do_release(s, pb);
    case UDPMultiSend:
        return pb->udp.csParam.send.wdsPtr ? MT_PB_UNIMP : invalidWDS;   /* matrix 911.28 */
    default:           return MT_PB_UNIMP;                       /* UDPMultiRead: not supported */
    }
}

/* ---- timers ---- */

static int due(uint32_t dl, uint32_t now) { return (int32_t)(dl - now) <= 0; }

void mt_pb_udp_timers(MtDrv *d) {
    uint8_t i, k;
    for (i = 0; i < d->nudp; i++) {
        MtUdpStream *s = &d->udp[i];
        uint8_t g = s->gen;
        if (s->used != LIVE) continue;
        for (k = 0; k < s->rq_n && same(s, g);) {
            if (s->rq[k].timed && due(s->rq[k].dl, d->now)) {
                complete(s, rq_take(s, k), commandTimeout);      /* matrix 905: -23016 */
                k = 0;                                           /* rescan: the app may have changed it */
            } else {
                k++;
            }
        }
    }
}

uint32_t mt_pb_udp_deadline(MtDrv *d) {
    uint32_t best = MT_NO_DEADLINE;
    uint8_t i, k;
    for (i = 0; i < d->nudp; i++) {
        const MtUdpStream *s = &d->udp[i];
        if (s->used != LIVE) continue;
        for (k = 0; k < s->rq_n; k++)
            if (s->rq[k].timed) {
                uint32_t r = due(s->rq[k].dl, d->now) ? 0 : s->rq[k].dl - d->now;
                if (r < best) best = r;
            }
    }
    return best;
}
