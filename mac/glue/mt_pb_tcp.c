/* mt_pb_tcp.c - MacTCP TCP calls (csCodes 30..43) on the core's mt_tcp_* API.
 *
 * Rules every function here follows (docs: mt_pb.h, MACTCP-REFERENCE-MATRIX.md):
 *  - A PB leaves its queue BEFORE its completion routine runs, and an ASR/completion may call
 *    the driver again (decision 8). After every call out (mt_pb_call_complete / _asr) the
 *    stream is re-validated with same(): slot still in use, same generation (handle), same
 *    connection. No pointer into a queue is held across a call out.
 *  - Pending PBs of a stream complete before its TCPTerminate ASR (matrix Events, 603/605).
 *  - mt_tcp_release is never called while a glue frame is active (depth > 0) or inside the
 *    core: the release is deferred (d->rel_mask), the stream's PBs are completed by the glue
 *    at once, and the slot stays reserved until the core TCB is gone (decision 3). The core
 *    never fires a done callback after mt_tcp_release (it clears the TCB's queue), and dones
 *    of a stale connection are recognised by MtCoreReq.conn.
 *  - Send: TCPSend PBs queue FIFO in sq[]. feed() hands their bytes to the core in requests of
 *    up to MT_SEGS_MAX WDS entries, merging consecutive PBs (decision 2), at most MT_SENDQ_MAX
 *    requests at a time. A request ends after an urgent PB (the core's urgent mark is the end
 *    of a request); its push flag is the OR of the PBs ending in it (a pushed PB is never held
 *    back by Nagle). A PB completes when the peer has ACKed its last byte: at the request's
 *    done, or earlier (send_progress, from the core's byte counts) when it ends inside a
 *    merged request. */
#include <string.h>
#include "mt_pb.h"

#define LIVE  1
#define DYING 2
#define MS(sec) ((uint32_t)(uint8_t)(sec) * 1000u)

/* ---- small helpers ---- */

static int same(const MtStream *s, uint8_t gen, uint16_t conn) {
    return s->used != 0 && s->gen == gen && s->conn == conn;
}

static MtStream *stream_of(MtDrv *d, StreamPtr h) {
    MacPtr32 base = mt_os_addr(&d->tcp[0]);
    uint32_t off = h - base, i;
    if (off >= (uint32_t)d->ntcp * (uint32_t)sizeof(MtStream)) return NULL;
    i = off / (uint32_t)sizeof(MtStream);
    if (d->tcp[i].used != LIVE || d->tcp[i].handle != h) return NULL;   /* stale or misaligned */
    return &d->tcp[i];
}

static void info(const MtStream *s, mt_tcp_info *in) { mt_tcp_get_info(s->t, in); }

static mt_tcp_state state_of(const MtStream *s) {
    mt_tcp_info in;
    info(s, &in);
    return in.state;
}

static uint32_t unread(const MtStream *s) {
    mt_tcp_info in;
    info(s, &in);
    return in.rcv_avail - s->lent;
}

static void complete(MtStream *s, MtAnyPb *pb, int16_t err) { mt_pb_call_complete(s->d, pb, err); }

static void asr(MtStream *s, uint16_t ev, uint16_t reason, MacPtr32 icmp) {
    MtStreamAsr a;
    a.proc = s->asr;
    a.stream = s->handle;
    a.user = s->user;
    a.a5 = s->a5;
    a.kind = MT_ASR_TCP;
    mt_pb_call_asr(s->d, &a, ev, reason, icmp);
}

static MtAnyPb *sq_pop(MtStream *s) {
    MtAnyPb *pb = s->sq[0].pb;
    uint8_t i;
    for (i = 1; i < s->sq_n; i++) s->sq[i - 1] = s->sq[i];
    s->sq_n--;
    if (s->sub_i) {
        s->sub_i--;
    } else {                                  /* the head was not wholly submitted (connection end) */
        s->sub_e = 0;
        s->sub_left = s->sq_n ? s->sq[0].len : 0;
    }
    return pb;
}

static MtAnyPb *rq_take(MtStream *s, uint8_t k) {
    MtAnyPb *pb = s->rq[k].pb;
    for (; k + 1 < s->rq_n; k++) s->rq[k] = s->rq[k + 1];
    s->rq_n--;
    return pb;
}

static void ulp_apply(MtStream *s, int8_t vflags, int8_t value, int8_t action) {
    if (((uint8_t)vflags & timeoutValue) && (uint8_t)value) s->ulp_s = (uint8_t)value;
    if ((uint8_t)vflags & timeoutAction) s->ulp_abort = (uint8_t)(action != 0);
    mt_tcp_set_ulp_timeout(s->t, s->ulp_s, s->ulp_abort);
}

/* Complete every PB of the stream's connection: the open with open_err (or s->open_err), the
 * sends and reads with connectionTerminated, the close with close_err; then the TCPTerminate
 * ASR with `reason` (0 = none). `releasing`: called by TCPRelease (the stream is DYING and the
 * ASR still runs); otherwise the ASR is skipped once the app has released the stream from one
 * of the completion routines. 0 if the stream changed under us. */
static int fail_all(MtStream *s, int16_t open_err, int16_t close_err, uint16_t reason,
                    int releasing) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    MtAnyPb *pb;
    s->close_deferred = 0;
    if (s->open_pb) {
        int16_t e = s->open_err ? s->open_err : open_err;
        pb = s->open_pb;
        s->open_pb = NULL;
        s->open_timed = 0;
        s->open_err = 0;
        complete(s, pb, e);
        if (!same(s, g, c)) return 0;
    }
    while (s->sq_n) {
        pb = sq_pop(s);
        complete(s, pb, connectionTerminated);
        if (!same(s, g, c)) return 0;
    }
    while (s->rq_n) {
        pb = rq_take(s, 0);
        complete(s, pb, connectionTerminated);
        if (!same(s, g, c)) return 0;
    }
    if (s->close_pb) {
        pb = s->close_pb;
        s->close_pb = NULL;
        complete(s, pb, close_err);
        if (!same(s, g, c)) return 0;
    }
    if (reason && (releasing || s->used == LIVE)) {
        asr(s, TCPTerminate, reason, 0);
        if (!same(s, g, c)) return 0;
    }
    return 1;
}

/* A new connection on the stream (open / listen): anything left from the previous one is
 * failed first (normally nothing: TCPTerminate completed it), then the per-connection state
 * starts over. 0 if the app released or re-opened the stream while we failed its PBs. */
static int conn_begin(MtStream *s) {
    uint8_t i, g = s->gen;
    if ((s->sq_n || s->rq_n || s->close_pb) && !fail_all(s, connectionTerminated, connectionTerminated, 0, 0))
        return 0;
    if (s->used != LIVE || s->gen != g) return 0;
    s->conn++;
    s->fin = s->closing_sent = s->close_deferred = 0;
    s->sub_i = s->sub_e = 0;
    s->sub_left = s->q_total = s->sub_total = s->acked = 0;
    s->req_h = s->req_n = 0;
    for (i = 0; i < MT_SENDQ_MAX; i++) s->req[i].busy = 0;
    s->loan_n = 0;
    s->lent = 0;
    s->open_err = 0;
    return 1;
}

/* ---- send ---- */

static void send_done(void *ud, int status);

/* Hand queued PB bytes to the core while it has a free request slot. Never calls out. */
static void feed(MtStream *s) {
    MtDrv *d = s->d;
    while (s->req_n < MT_SENDQ_MAX && s->sub_i < s->sq_n) {
        MtCoreReq *r = &s->req[(s->req_h + s->req_n) % MT_SENDQ_MAX];
        uint8_t n = 0, push = 0, urg = 0, si = s->sub_i, se = s->sub_e;
        uint32_t len = 0, sl = s->sub_left;
        while (n < MT_SEGS_MAX && si < s->sq_n) {
            const MtSendRec *q = &s->sq[si];
            const uint8_t *w = (const uint8_t *)mt_os_ptr(q->pb->tcp.csParam.send.wdsPtr);
            wdsEntry e;
            uint32_t take;
            if (!w || se >= MT_PB_WDS_MAX) return;      /* WDS changed under us: stall (app misuse) */
            memcpy(&e, w + (uint32_t)se * sizeof(wdsEntry), sizeof e);
            se++;
            if (e.length == 0 || !e.ptr) continue;
            take = e.length < sl ? e.length : sl;
            r->seg[n].p = (const uint8_t *)mt_os_ptr(e.ptr);
            r->seg[n].len = (uint16_t)take;
            n++;
            len += take;
            sl -= take;
            if (sl == 0) {                              /* this PB's last byte is in the request */
                uint8_t u = q->urg;
                push |= q->push;
                si++;
                se = 0;
                sl = si < s->sq_n ? s->sq[si].len : 0;
                if (u) { urg = 1; break; }
            }
        }
        if (n == 0) break;
        if (mt_tcp_send_segs(s->t, r->seg, n, push, urg, send_done, r, d->now) != MT_OK) break;
        r->s = s;
        r->len = len;
        r->conn = s->conn;
        r->busy = 1;
        s->sub_total += len;
        r->end = s->sub_total;
        s->sub_i = si;
        s->sub_e = se;
        s->sub_left = sl;
        s->req_n++;
    }
    if (s->close_deferred && s->sub_i == s->sq_n) {    /* the FIN goes after the last PB byte */
        s->close_deferred = 0;
        mt_tcp_close(s->t, d->now);
    }
}

/* Complete the head PBs whose last byte the peer has ACKed. Bytes ACKed inside a request that
 * is not done yet are derived from the core's counts (unacked + not yet sent = bytes of the
 * outstanding requests not yet ACKed). 0 if the stream changed under us. */
static int send_progress(MtStream *s) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    while (s->sq_n && s->sub_i) {
        uint32_t end = s->sq[0].end;
        if ((int32_t)(s->acked - end) < 0) {
            mt_tcp_info in;
            uint32_t inflight = s->sub_total - s->acked, notack;
            if (!s->req_n || end == s->req[s->req_h].end) break;   /* its request's done reports it */
            info(s, &in);
            if (in.state < MT_TCP_ESTABLISHED || in.state == MT_TCP_TIME_WAIT) break;
            notack = in.unacked + in.send_queued;
            if (notack > inflight || (int32_t)(s->acked + inflight - notack - end) < 0) break;
            s->d->early_done++;                   /* completed inside a merged request */
        }
        complete(s, sq_pop(s), 0);
        if (!same(s, g, c) || s->used != LIVE) return 0;
    }
    return 1;
}

static void send_done(void *ud, int status) {
    MtCoreReq *r = (MtCoreReq *)ud;
    MtStream *s = r->s;
    if (!r->busy) return;
    r->busy = 0;
    if (s->used != LIVE || r->conn != s->conn || !s->req_n) return;   /* released / old connection */
    s->req_h = (uint8_t)((s->req_h + 1u) % MT_SENDQ_MAX);
    s->req_n--;
    if (status != MT_OK) return;              /* termination: TCPTerminate fails the PBs in order */
    s->d->oct_out += r->len;                  /* TCPStats.tcpOctetsOut (bytes the peer ACKed) */
    s->acked = r->end;
    if (send_progress(s)) feed(s);
}

static int16_t do_send(MtDrv *d, MtStream *s, MtAnyPb *pb) {
    const uint8_t *w = (const uint8_t *)mt_os_ptr(pb->tcp.csParam.send.wdsPtr);
    uint32_t total = 0;
    uint8_t k;
    mt_tcp_state st;
    MtSendRec *q;
    (void)d;
    if (!w) return invalidWDS;                                   /* matrix 990 */
    for (k = 0; k < MT_PB_WDS_MAX; k++) {
        wdsEntry e;
        memcpy(&e, w + (uint32_t)k * sizeof(wdsEntry), sizeof e);
        if (e.length == 0) break;                                /* the terminator */
        if (!e.ptr) return invalidWDS;
        total += e.length;
    }
    if (k == MT_PB_WDS_MAX) return invalidWDS;                   /* unterminated */
    if (total == 0) return invalidLength;                        /* matrix 402, 402b */
    st = state_of(s);
    if (st == MT_TCP_CLOSED || st == MT_TCP_LISTEN || st == MT_TCP_SYN_SENT || st == MT_TCP_SYN_RCVD)
        return connectionDoesntExist;                            /* matrix 405 */
    if (s->close_pb || s->close_deferred || (st != MT_TCP_ESTABLISHED && st != MT_TCP_CLOSE_WAIT))
        return connectionClosing;                                /* matrix 408 */
    if (s->sq_n >= MT_PB_SENDQ) return insufficientResources;
    ulp_apply(s, pb->tcp.csParam.send.validityFlags, pb->tcp.csParam.send.ulpTimeoutValue,
              pb->tcp.csParam.send.ulpTimeoutAction);
    q = &s->sq[s->sq_n++];
    q->pb = pb;
    q->len = total;
    s->q_total += total;
    q->end = s->q_total;
    q->push = (uint8_t)(pb->tcp.csParam.send.pushFlag != 0);
    q->urg = (uint8_t)(pb->tcp.csParam.send.urgentFlag != 0);
    if (s->sub_i == s->sq_n - 1u) {                              /* the next PB to submit */
        s->sub_e = 0;
        s->sub_left = total;
    }
    pb->hdr.ioResult = inProgress;                               /* completes on the ACK (matrix 400) */
    feed(s);
    return inProgress;
}

/* ---- receive ---- */

static int loan_add(MtStream *s, MacPtr32 key, uint32_t len, uint8_t returned) {
    if (s->loan_n >= MT_PB_LOANS) return 0;
    s->loan[s->loan_n].key = key;
    s->loan[s->loan_n].len = len;
    s->loan[s->loan_n].returned = returned;
    s->loan_n++;
    s->lent += len;
    return 1;
}

/* Give the returned prefix of the loans back to the core (the ring is consumed in order). */
static void loans_consume(MtStream *s) {
    uint32_t n = 0;
    uint8_t i, k = 0;
    while (k < s->loan_n && s->loan[k].returned) n += s->loan[k++].len;
    if (!k) return;
    for (i = k; i < s->loan_n; i++) s->loan[i - k] = s->loan[i];
    s->loan_n = (uint8_t)(s->loan_n - k);
    s->lent -= n;
    mt_tcp_consume(s->t, n, s->d->now);
}

/* Can the head read be served now (data there, and a loan slot if one is needed)? */
static int can_serve(const MtStream *s, const MtAnyPb *pb) {
    if (pb->hdr.csCode == TCPRcv && s->loan_n == 0) return 1;
    return s->loan_n < MT_PB_LOANS;
}

/* TCPRcv: copy up to rcvBuffLen unread bytes (stopping at an urgent mark). */
static void rcv_copy(MtStream *s, MtAnyPb *pb) {
    uint8_t *dst = (uint8_t *)mt_os_ptr(pb->tcp.csParam.receive.rcvBuff);
    uint32_t max = pb->tcp.csParam.receive.rcvBuffLen, n = 0, mark = 0;
    int urg = s->lent == 0 && mt_tcp_urgent(s->t, &mark);
    if (urg && mark && mark < max) max = mark;
    if (s->lent == 0) {
        n = mt_tcp_recv(s->t, dst, max, s->d->now);
    } else {                                    /* behind outstanding loans: copy, consume later */
        while (n < max) {
            const uint8_t *p;
            uint32_t m = mt_tcp_peek(s->t, s->lent + n, &p);
            if (!m) break;
            if (m > max - n) m = max - n;
            memcpy(dst + n, p, m);
            n += m;
        }
        loan_add(s, 0, n, 1);
    }
    pb->tcp.csParam.receive.rcvBuffLen = (uint16_t)n;
    pb->tcp.csParam.receive.urgentFlag = (uint8_t)(urg != 0);
    pb->tcp.csParam.receive.markFlag = 0;                      /* matrix 500 */
}

/* TCPNoCopyRcv: describe unread ring bytes in the app's RDS (up to rdsLength - 1 entries plus
 * the terminator) and lend them until TCPRcvBfrReturn (matrix 504). */
static void rcv_nocopy(MtStream *s, MtAnyPb *pb) {
    uint8_t *rds = (uint8_t *)mt_os_ptr(pb->tcp.csParam.receive.rdsPtr);
    uint16_t cnt = pb->tcp.csParam.receive.rdsLength, k = 0;
    uint32_t total = 0;
    rdsEntry e;
    MacPtr32 key = 0;
    while ((uint32_t)k + 1u < cnt) {
        const uint8_t *p;
        uint32_t m = mt_tcp_peek(s->t, s->lent + total, &p);
        if (!m) break;
        if (m > 65535u) m = 65535u;
        e.length = (uint16_t)m;
        e.ptr = mt_os_addr(p);
        if (!k) key = e.ptr;
        memcpy(rds + (uint32_t)k * sizeof(rdsEntry), &e, sizeof e);
        total += m;
        k++;
    }
    e.length = 0;
    e.ptr = 0;
    memcpy(rds + (uint32_t)k * sizeof(rdsEntry), &e, sizeof e);
    loan_add(s, key, total, 0);
    pb->tcp.csParam.receive.urgentFlag = 0;
    pb->tcp.csParam.receive.markFlag = 0;
}

/* The peer closed and everything is read: pending reads fail with connectionClosing, then
 * TCPClosing fires once (matrix 601: the read that drains the data completes first). */
static int closing_check(MtStream *s) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    if (!s->fin || unread(s) != 0) return 1;
    while (s->rq_n) {
        complete(s, rq_take(s, 0), connectionClosing);
        if (!same(s, g, c) || s->used != LIVE) return 0;
    }
    if (!s->closing_sent) {
        s->closing_sent = 1;
        asr(s, TCPClosing, 0, 0);
        if (!same(s, g, c) || s->used != LIVE) return 0;
    }
    return 1;
}

/* TCPDataArrival(n). An app that reads from its ASR may leave data again (one small read per
 * ASR is common): the ASR is then due once more, but it runs after the current one returns, in
 * this loop, never nested inside it (stack depth stays bounded on the 68k). */
static void data_asr(MtStream *s, uint32_t n) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    if (s->in_data_asr) { s->data_due = 1; return; }
    s->in_data_asr = 1;
    for (;;) {
        s->data_due = 0;
        asr(s, TCPDataArrival, (uint16_t)(n > 65535u ? 65535u : n), 0);
        if (!same(s, g, c) || s->used != LIVE || !s->data_due || s->rq_n) break;
        n = unread(s);
        if (!n) break;
    }
    s->in_data_asr = 0;          /* the slot cannot be re-created meanwhile (release is deferred) */
    s->data_due = 0;
}

/* A read completed: if data is still unread and no other read is pending, TCPDataArrival with
 * the unread count follows the completion (matrix 500: C:k1:r0 then A:e4:t16). 0 if the stream
 * changed under us. */
static int after_read(MtStream *s) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    uint32_t u;
    if (s->rq_n) return 1;                               /* matrix 400/502: not with a read pending */
    u = unread(s);
    if (u) data_asr(s, u);
    return same(s, g, c) && s->used == LIVE;
}

/* Serve pending reads in FIFO order while data is there (*served counts them). 0 if the
 * stream changed. */
static int serve_reads(MtStream *s, int *served) {
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    int n = 0;
    while (s->rq_n && unread(s) && can_serve(s, s->rq[0].pb)) {
        MtAnyPb *pb = rq_take(s, 0);
        if (pb->hdr.csCode == TCPRcv) rcv_copy(s, pb);
        else rcv_nocopy(s, pb);
        complete(s, pb, 0);
        n++;
        if (!same(s, g, c) || s->used != LIVE) return 0;
    }
    if (served) *served = n;
    if (n && !after_read(s)) return 0;
    return closing_check(s);
}

static int16_t do_rcv(MtDrv *d, MtStream *s, MtAnyPb *pb) {
    int16_t cs = pb->hdr.csCode;
    int8_t tmo = pb->tcp.csParam.receive.commandTimeoutValue;
    mt_tcp_state st;
    if (cs == TCPRcv) {
        if (!pb->tcp.csParam.receive.rcvBuff) return invalidBufPtr;      /* matrix 991 */
        if (!pb->tcp.csParam.receive.rcvBuffLen) return invalidLength;   /* matrix 501 */
    } else {
        if (!pb->tcp.csParam.receive.rdsPtr) return invalidBufPtr;       /* matrix 992 */
        if (pb->tcp.csParam.receive.rdsLength < 2) return invalidLength; /* no room for data + end */
    }
    st = state_of(s);
    if (st == MT_TCP_CLOSED) return connectionDoesntExist;               /* matrix 503 */
    if (!s->rq_n && unread(s) && can_serve(s, pb) && d->depth < MT_PB_INLINE_MAX) {
        uint8_t g = s->gen;
        uint16_t c = s->conn;
        uint32_t u;
        if (cs == TCPRcv) rcv_copy(s, pb);
        else rcv_nocopy(s, pb);
        u = unread(s);
        if (!u && !(s->fin && !s->closing_sent)) return 0;
        /* An ASR must follow this read's completion: TCPDataArrival for the data it left
         * (matrix 500), or TCPClosing once it drained the data after the peer's FIN (601). So
         * the glue completes it now and returns inProgress (see mt_pb.h, result contract). */
        pb->hdr.ioResult = inProgress;
        complete(s, pb, 0);
        if (same(s, g, c) && s->used == LIVE) {
            if (u) after_read(s);
            else closing_check(s);
        }
        return inProgress;
    }
    if (!s->rq_n && s->fin && unread(s) == 0) return connectionClosing; /* matrix 601: rcv2 */
    if (s->rq_n >= MT_PB_RCVQ) return insufficientResources;
    s->rq[s->rq_n].pb = pb;                                             /* matrix 502: FIFO */
    s->rq[s->rq_n].timed = (uint8_t)(tmo != 0);                         /* 0 = forever (501b) */
    s->rq[s->rq_n].dl = d->now + MS(tmo);
    s->rq_n++;
    if (s->rq_n == 1 && unread(s) && can_serve(s, pb)) d->rcv_kick = 1;   /* too deep to serve now */
    pb->hdr.ioResult = inProgress;
    return inProgress;
}

static int16_t do_bfr_return(MtStream *s, MtAnyPb *pb) {
    const uint8_t *rds = (const uint8_t *)mt_os_ptr(pb->tcp.csParam.receive.rdsPtr);
    rdsEntry e;
    uint8_t i;
    if (!rds) return invalidBufPtr;
    memcpy(&e, rds, sizeof e);
    for (i = 0; i < s->loan_n; i++)
        if (!s->loan[i].returned && s->loan[i].key && s->loan[i].key == e.ptr) {
            s->loan[i].returned = 1;
            loans_consume(s);
            serve_reads(s, NULL);               /* a read may have waited for a loan slot */
            return 0;
        }
    return invalidRDS;                          /* matrix 504: again, or made up */
}

/* ---- core callbacks ---- */

static void on_established(MtStream *s) {
    MtAnyPb *pb = s->open_pb;
    mt_tcp_info in;
    s->d->n_opened++;                                    /* TCPStats.tcpConnOpened */
    if (!pb) return;
    s->open_pb = NULL;
    s->open_timed = 0;
    s->open_err = 0;
    info(s, &in);
    pb->tcp.csParam.open.remoteHost = in.rip;            /* matrix 300 */
    pb->tcp.csParam.open.remotePort = in.rport;
    pb->tcp.csParam.open.localHost = in.lip;
    pb->tcp.csParam.open.localPort = in.lport;
    complete(s, pb, 0);
}

static void on_data(MtStream *s, uint32_t n) {
    uint32_t u;
    int served = 0;
    s->d->oct_in += n;                                    /* TCPStats.tcpOctetsIn */
    if (!send_progress(s) || !serve_reads(s, &served)) return;
    if (served || s->rq_n) return;   /* after_read reported what the reads left; 400/502: none pending */
    u = unread(s);
    if (!u) return;
    if (n > u) n = u;                                     /* the new bytes (matrix 504, 608) */
    data_asr(s, n);
}

static void on_terminate(MtStream *s, uint32_t reason) {
    int16_t open_err = reason == MT_TERM_OPEN_FAILED ? openFailed : connectionTerminated;
    int16_t close_err = reason == MT_TERM_ULP_CLOSE ? 0 : connectionTerminated;
    uint8_t g = s->gen;
    uint16_t c = s->conn;
    if (reason == MT_TERM_ULP_CLOSE) s->d->n_closed++;  /* TCPStats.tcpConnClosed / Aborted */
    else if (reason != MT_TERM_OPEN_FAILED) s->d->n_aborted++;
    if (!send_progress(s) || !same(s, g, c)) return;
    fail_all(s, open_err, close_err, mt_pb_term_reason((int)reason), 0);
}

static void on_icmp(MtStream *s, uint32_t arg) {
    MtDrv *d = s->d;
    mt_tcp_info in;
    info(s, &in);
    d->icmp.streamPtr = s->handle;
    d->icmp.localHost = in.lip;
    d->icmp.localPort = in.lport;
    d->icmp.remoteHost = in.rip;
    d->icmp.remotePort = in.rport;
    d->icmp.reportType = mt_pb_icmp_type(arg);
    d->icmp.optionalAddlInfo = 0;
    d->icmp.optionalAddlInfoPtr = 0;
    asr(s, TCPICMPReceived, 0, mt_os_addr(&d->icmp));
}

static void tcp_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    MtStream *s = (MtStream *)user;
    if (s->used != LIVE || s->t != t) return;            /* released: nothing reaches the app */
    switch (ev) {
    case MT_TCP_EV_ESTABLISHED: on_established(s); break;
    case MT_TCP_EV_DATA:        on_data(s, arg); break;
    case MT_TCP_EV_URGENT:      asr(s, mt_pb_event_code(ev), 0, 0); break;
    case MT_TCP_EV_CLOSING:     s->fin = 1; serve_reads(s, NULL); break;
    case MT_TCP_EV_ULP_TIMEOUT: asr(s, mt_pb_event_code(ev), 0, 0); break;
    case MT_TCP_EV_ICMP:        on_icmp(s, arg); break;
    case MT_TCP_EV_TERMINATE:   on_terminate(s, arg); break;
    default: break;
    }
}

/* ---- calls ---- */

static int16_t do_create(MtDrv *d, MtAnyPb *pb) {
    uint32_t len = pb->tcp.csParam.create.rcvBuffLen;
    MacPtr32 buf = pb->tcp.csParam.create.rcvBuff;
    MtStream *s = NULL;
    uint8_t i = 0, k;
    if (len < MT_PB_MIN_RCVBUF) return invalidLength;           /* matrix 200 */
    if (!buf) return invalidBufPtr;                              /* matrix 201 */
    for (k = 0; k < d->ntcp; k++) {                              /* rotate: a freed slot rests */
        i = (uint8_t)((d->next_slot + k) % d->ntcp);
        if (d->tcp[i].used == 0) { s = &d->tcp[i]; break; }
    }
    if (!s) return insufficientResources;
    {
        uint8_t gen = (uint8_t)(s->gen + 1u);
        uint16_t conn = s->conn;
        memset(s, 0, sizeof *s);
        s->gen = gen;
        s->conn = conn;
    }
    s->t = mt_tcp_create(d->s, (uint8_t *)mt_os_ptr(buf), len, tcp_cb, s);
    if (!s->t) return insufficientResources;
    d->next_slot = (uint8_t)((i + 1u) % d->ntcp);
    s->d = d;
    s->used = LIVE;
    s->handle = mt_os_addr(s) + ((uint32_t)(s->gen & 7u) << 1);
    s->asr = pb->tcp.csParam.create.notifyProc;                  /* 0 is fine (matrix 202b) */
    s->user = pb->tcp.csParam.create.userDataPtr;
    s->a5 = mt_os_a5();
    s->rbuf = buf;
    s->rlen = len;
    s->ulp_s = 120;
    s->ulp_abort = 1;
    pb->tcp.tcpStream = s->handle;
    return 0;
}

static int16_t do_open(MtDrv *d, MtStream *s, MtAnyPb *pb, int passive) {
    mt_ip4 rip = pb->tcp.csParam.open.remoteHost;
    uint16_t rport = pb->tcp.csParam.open.remotePort, lport = pb->tcp.csParam.open.localPort;
    int8_t tmo = pb->tcp.csParam.open.commandTimeoutValue;
    int r;
    if (s->open_pb || state_of(s) != MT_TCP_CLOSED) return connectionExists;   /* matrix 305 */
    if (!passive && (rip == 0 || rport == 0)) return openFailed;   /* matrix 304, 304b */
    if (!conn_begin(s)) return connectionTerminated;
    ulp_apply(s, pb->tcp.csParam.open.validityFlags, pb->tcp.csParam.open.ulpTimeoutValue,
              pb->tcp.csParam.open.ulpTimeoutAction);
    s->tos = (uint8_t)pb->tcp.csParam.open.tosFlags;
    s->prec = (uint8_t)pb->tcp.csParam.open.precedence;
    if (passive) {
        r = mt_tcp_listen(s->t, rip, rport, lport);
        if (r != MT_OK) return duplicateSocket;                 /* every listen port 1024..2047 taken */
        /* matrix 700: localPort / localHost are reported at completion, also on a timeout */
        if (tmo) {                                             /* matrix 700/702: commandTimeoutValue */
            s->open_timed = 1;
            s->open_dl = d->now + MS(tmo);
        }
    } else {                                                   /* commandTimeoutValue ignored (302c) */
        r = mt_tcp_open(s->t, rip, rport, lport, d->now);
        if (r == MT_ERR_INUSE) return duplicateSocket;          /* matrix 306 */
        if (r == MT_ERR_STATE) return connectionExists;
        if (r != MT_OK) return openFailed;
    }
    s->open_pb = pb;
    pb->hdr.ioResult = inProgress;
    return inProgress;
}

/* Fill in an open PB's addresses as far as known (a timed-out passive open, matrix 700). */
static void open_addrs(MtStream *s, MtAnyPb *pb) {
    mt_tcp_info in;
    info(s, &in);
    pb->tcp.csParam.open.localHost = in.lip;
    pb->tcp.csParam.open.localPort = in.lport;
    pb->tcp.csParam.open.remoteHost = in.rip;
    pb->tcp.csParam.open.remotePort = in.rport;
}

static int16_t do_close(MtDrv *d, MtStream *s, MtAnyPb *pb) {
    mt_tcp_state st = state_of(s);
    if (st == MT_TCP_CLOSED) return connectionDoesntExist;         /* matrix 602 */
    if (s->close_pb || s->close_deferred) return connectionClosing;
    switch (st) {
    case MT_TCP_LISTEN:                                            /* stop listening */
        mt_tcp_close(s->t, d->now);
        fail_all(s, connectionTerminated, connectionTerminated, TCPULPClose, 0);
        return 0;
    case MT_TCP_SYN_SENT:
    case MT_TCP_SYN_RCVD:                     /* the core terminates: TCPTerminate (ULP close) */
        mt_tcp_close(s->t, d->now);
        return 0;
    case MT_TCP_ESTABLISHED:
    case MT_TCP_CLOSE_WAIT:
        break;
    default:
        return connectionClosing;
    }
    ulp_apply(s, pb->tcp.csParam.close.validityFlags, pb->tcp.csParam.close.ulpTimeoutValue,
              pb->tcp.csParam.close.ulpTimeoutAction);
    s->close_pb = pb;
    pb->hdr.ioResult = inProgress;            /* completes when our FIN is ACKed (matrix 600) */
    if (s->sub_i < s->sq_n) s->close_deferred = 1;   /* PB bytes still to hand to the core first */
    else mt_tcp_close(s->t, d->now);
    return inProgress;
}

static int16_t do_abort(MtDrv *d, MtStream *s) {
    mt_tcp_state st = state_of(s);
    if (st == MT_TCP_CLOSED) return connectionDoesntExist;         /* matrix 602 */
    if (st == MT_TCP_LISTEN) {                                     /* matrix 702 */
        mt_tcp_abort(s->t, d->now);                                /* (silent in the core) */
        fail_all(s, connectionTerminated, connectionTerminated, TCPULPAbort, 0);
    } else {
        mt_tcp_abort(s->t, d->now);       /* TCPTerminate (ULP abort) fails the PBs, matrix 603 */
    }
    return 0;
}

void mt_pb_tcp_free(MtDrv *d, MtStream *s) {
    uint8_t i;
    (void)d;
    if (s->t) mt_tcp_release(s->t);       /* no callbacks; the core forgets our requests */
    s->t = NULL;
    for (i = 0; i < MT_SENDQ_MAX; i++) s->req[i].busy = 0;
    s->used = 0;
}

static int16_t do_release(MtDrv *d, MtStream *s, MtAnyPb *pb) {
    uint16_t reason = state_of(s) != MT_TCP_CLOSED ? TCPULPAbort : 0;   /* idle: no ASR (603/605) */
    uint8_t idx = (uint8_t)(s - d->tcp);
    s->used = DYING;                      /* the handle is invalid from here on */
    if (d->depth == 0 && !d->in_core) {   /* core first: it stops reading the app's buffers */
        mt_tcp_release(s->t);
        s->t = NULL;
    } else {
        /* Decision 3: no mt_tcp_release inside a callback. The connection is aborted now (legal
         * from callbacks; its done/TERMINATE callbacks find the stream DYING and do nothing), so
         * for the rest of this core call the core neither sends from the app's buffers nor
         * fills its receive buffer; the TCB itself is freed when the outermost entry returns. */
        if (reason) mt_tcp_abort(s->t, d->now);
        d->rel_mask |= (uint16_t)(1u << idx);
    }
    fail_all(s, connectionTerminated, connectionTerminated, reason, 1);   /* matrix 605 */
    pb->tcp.csParam.create.rcvBuff = s->rbuf;  /* the buffer goes back to the app */
    pb->tcp.csParam.create.rcvBuffLen = s->rlen;
    if (!s->t) mt_pb_tcp_free(d, s);
    return 0;
}

static int16_t do_status(MtDrv *d, MtStream *s, MtAnyPb *pb) {
    mt_tcp_info in;
    TCPStatusPB *p = &pb->tcp.csParam.status;
    uint32_t unacked;
    info(s, &in);
    if (in.state == MT_TCP_CLOSED) return connectionDoesntExist;   /* matrix 602, 603 */
    unacked = in.unacked + in.send_queued + (s->q_total - s->sub_total);
    p->ulpTimeoutValue = (int8_t)s->ulp_s;
    p->ulpTimeoutAction = (int8_t)s->ulp_abort;
    p->remoteHost = in.rip;
    p->remotePort = in.rport;
    p->localHost = in.lip;
    p->localPort = in.lport;
    p->tosFlags = (int8_t)s->tos;
    p->precedence = (int8_t)s->prec;
    p->connectionState = (int8_t)(in.state * 2);               /* MacTCP numbering (ref) */
    /* Matrix 601/606: after the peer's FIN MacTCP keeps reporting Established (8) until the app has read
     * the data (then CloseWait 14, with TCPClosing). NCSA Telnet's FTP server ends a download as soon as it
     * sees a state other than 8, and so lost the last 6 KB of a 1 MiB upload (Task 13, TAP). */
    if (in.state == MT_TCP_CLOSE_WAIT && in.rcv_avail > s->lent) p->connectionState = ConnectionStateEstablished;
    p->sendWindow = (uint16_t)(in.snd_wnd > 65535u ? 65535u : in.snd_wnd);
    p->rcvWindow = (uint16_t)(in.rcv_wnd > 65535u ? 65535u : in.rcv_wnd);
    p->amtUnackedData = (uint16_t)(unacked > 65535u ? 65535u : unacked);
    in.rcv_avail -= s->lent;
    p->amtUnreadData = (uint16_t)(in.rcv_avail > 65535u ? 65535u : in.rcv_avail);
    p->securityLevelPtr = 0;
    p->sendUnacked = in.snd_una;
    p->sendNext = in.snd_nxt;
    p->congestionWindow = in.cwnd;
    p->rcvNext = in.rcv_nxt;
    p->srtt = in.srtt_ms;
    p->lastRTT = in.srtt_ms;
    p->sendMaxSegSize = in.mss;
    p->connStatPtr = mt_os_addr(&d->cstats);
    return 0;
}

static int16_t do_global_info(MtDrv *d, MtAnyPb *pb) {
    const mt_stats *st = mt_get_stats(d->s);
    mt_ifconfig c;
    uint8_t i;
    mt_get_config(d->s, &c);
    memset(&d->gparam, 0, sizeof d->gparam);
    d->gparam.tcpRtoMin = 200;                /* the core's RTO bounds (ms) */
    d->gparam.tcpRtoMax = 60000;
    d->gparam.tcpMaxSegSize = (uint32_t)c.mtu - 40u;           /* matrix 800: 1460 */
    d->gparam.tcpMaxConn = d->ntcp;
    d->gparam.tcpMaxWindow = 65535;                            /* matrix 800 */
    memset(&d->gstats, 0, sizeof d->gstats);
    d->gstats.tcpConnAttempts = st->tcp_active_opens;
    d->gstats.tcpConnAccepted = st->tcp_passive_opens;
    d->gstats.tcpConnOpened = d->n_opened;
    d->gstats.tcpConnClosed = d->n_closed;
    d->gstats.tcpConnAborted = d->n_aborted;
    d->gstats.tcpOctetsIn = d->oct_in;
    d->gstats.tcpOctetsOut = d->oct_out;
    d->gstats.tcpInputPkts = st->tcp_in;
    d->gstats.tcpOutputPkts = st->tcp_out;
    d->gstats.tcpRetransPkts = st->tcp_rexmt;
    /* The table lists the connections first (TCBs past LISTEN: mt_tcp_count_open of them), then
     * the other live streams, then zeros. */
    memset(d->cdb, 0, sizeof d->cdb);
    {
        uint8_t open = mt_tcp_count_open(d->s), n = 0, pass;
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < d->ntcp && (pass || n < open); i++) {   /* pass 0 stops at the last connection */
                const MtStream *s = &d->tcp[i];
                if (s->used != LIVE) continue;
                if ((state_of(s) > MT_TCP_LISTEN) == (pass == 0)) d->cdb[n++] = s->handle;
            }
    }
    /* matrix 800: the driver's own tables replace the app's pointers; maxTCPConnections is left */
    pb->tcp.csParam.globalInfo.tcpParamPtr = mt_os_addr(&d->gparam);
    pb->tcp.csParam.globalInfo.tcpStatsPtr = mt_os_addr(&d->gstats);
    pb->tcp.csParam.globalInfo.tcpCDBTable[0] = mt_os_addr(&d->cdb[0]);
    return 0;
}

int16_t mt_pb_tcp_control(MtDrv *d, MtAnyPb *pb) {
    int16_t cs = pb->hdr.csCode;
    MtStream *s;
    if (cs == TCPCreate) return do_create(d, pb);
    if (cs == TCPGlobalInfo) return do_global_info(d, pb);
    s = stream_of(d, pb->tcp.tcpStream);
    if (!s) return invalidStreamPtr;                             /* matrix 120/121 */
    switch (cs) {
    case TCPPassiveOpen:  return do_open(d, s, pb, 1);
    case TCPActiveOpen:   return do_open(d, s, pb, 0);
    case TCPSend:         return do_send(d, s, pb);
    case TCPNoCopyRcv:
    case TCPRcv:          return do_rcv(d, s, pb);
    case TCPRcvBfrReturn: return do_bfr_return(s, pb);
    case TCPClose:        return do_close(d, s, pb);
    case TCPAbort:        return do_abort(d, s);
    case TCPStatus:       return do_status(d, s, pb);
    case TCPExtendedStat: return state_of(s) == MT_TCP_CLOSED ? connectionDoesntExist : 0;
    case TCPRelease:      return do_release(d, s, pb);
    default:              return MT_PB_UNIMP;                    /* 33, 50: no such call */
    }
}

/* ---- after input / ticks; timers ---- */

void mt_pb_tcp_after_core(MtDrv *d) {
    uint8_t i;
    if (d->rcv_kick && d->depth < MT_PB_INLINE_MAX) {      /* reads queued by the nesting limit */
        d->rcv_kick = 0;
        for (i = 0; i < d->ntcp; i++)
            if (d->tcp[i].used == LIVE && d->tcp[i].rq_n) (void)serve_reads(&d->tcp[i], NULL);
    }
    for (i = 0; i < d->ntcp; i++) {
        MtStream *s = &d->tcp[i];
        if (s->used != LIVE) continue;
        if (s->close_pb && !s->close_deferred) {                 /* our FIN ACKed (matrix 600) */
            mt_tcp_state st = state_of(s);
            if (st == MT_TCP_FIN_WAIT_2 || st == MT_TCP_TIME_WAIT) {
                MtAnyPb *pb = s->close_pb;
                s->close_pb = NULL;
                complete(s, pb, 0);
                if (s->used != LIVE) continue;
            }
        }
        if (s->sq_n && s->sub_i && !send_progress(s)) continue;
        if (s->sub_i < s->sq_n && s->req_n < MT_SENDQ_MAX) feed(s);
    }
}

static int due(uint32_t dl, uint32_t now) { return (int32_t)(dl - now) <= 0; }

void mt_pb_tcp_timers(MtDrv *d) {
    uint8_t i, k;
    for (i = 0; i < d->ntcp; i++) {
        MtStream *s = &d->tcp[i];
        if (s->used != LIVE) continue;
        if (s->open_timed && due(s->open_dl, d->now)) {          /* passive open: matrix 700 */
            mt_tcp_state st = state_of(s);
            s->open_timed = 0;
            s->open_err = commandTimeout;
            if (s->open_pb) open_addrs(s, s->open_pb);
            mt_tcp_abort(s->t, d->now);       /* SYN_RCVD: TCPTerminate does the rest */
            if (st == MT_TCP_LISTEN && s->used == LIVE)   /* LISTEN: the core is silent */
                fail_all(s, commandTimeout, connectionTerminated, TCPULPAbort, 0);   /* matrix 701 */
            if (s->used != LIVE) continue;
        }
        for (k = 0; k < s->rq_n && s->used == LIVE;) {
            if (s->rq[k].timed && due(s->rq[k].dl, d->now)) {
                complete(s, rq_take(s, k), commandTimeout);    /* matrix 500: -23016 */
                k = 0;                                         /* rescan: the app may have changed it */
            } else {
                k++;
            }
        }
    }
}

uint32_t mt_pb_tcp_deadline(MtDrv *d) {
    uint32_t best = MT_NO_DEADLINE;
    uint8_t i, k;
    if (d->rcv_kick) return 0;                             /* serve the queued reads at once */
    for (i = 0; i < d->ntcp; i++) {
        const MtStream *s = &d->tcp[i];
        if (s->used != LIVE) continue;
        if (s->open_timed) {
            uint32_t r = due(s->open_dl, d->now) ? 0 : s->open_dl - d->now;
            if (r < best) best = r;
        }
        for (k = 0; k < s->rq_n; k++)
            if (s->rq[k].timed) {
                uint32_t r = due(s->rq[k].dl, d->now) ? 0 : s->rq[k].dl - d->now;
                if (r < best) best = r;
            }
    }
    return best;
}
