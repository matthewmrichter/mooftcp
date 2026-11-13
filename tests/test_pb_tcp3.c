/* PB dispatcher, TCP calls (part 3): TCPDataArrival after a read that leaves data (matrix 500),
 * and the driver-entry result contract for an unconfigured driver. Matrix case ids:
 * docs/dev/MACTCP-REFERENCE-MATRIX.md (raw: docs/dev/probe/mactcp-configured.out). */
#include "t.h"
#include "pbfake.h"

static const char banner[] = "abcdefghijklmnopqrstuvwxyz";

static void asr_nop(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)user; (void)reason; (void)icmp;
}

/* Matrix 500 A: the 26-byte banner raised A:e4:t26; an immediate 10-byte read then completed
 * (C:k1:r0) and TCPDataArrival(16) followed it (A:s1:e4:t16); the 16-byte read left nothing. */
static void data_arrival_after_partial_read(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r;
    int n0, ci;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, banner, 26);
    T_EQ(pbf_nlog, n0 + 1);
    T_EQ(pbf_log[n0].reason, 26);                         /* matrix 500: A:e4:t26 */
    n0 = pbf_nlog;
    r = pbt_rcv(&x, s, 10, 0);
    T_EQ(r->ioResult, 0);                                 /* matrix 500: rcv10 0, n1 10 */
    T_EQ(r->csParam.receive.rcvBuffLen, 10);
    T_EQ(pbf_nlog, n0 + 2);
    ci = pbf_find('C', pbf_a(r), n0);
    T_EQ(ci, n0);                                         /* matrix 500: C:k1:r0 first, */
    T_EQ(pbf_log[n0 + 1].kind, 'A');                      /* then A:e4:t16 */
    T_EQ(pbf_log[n0 + 1].ev, TCPDataArrival);
    T_EQ(pbf_log[n0 + 1].reason, 16);
    T_EQ(pbf_log[n0 + 1].stream, s);
    n0 = pbf_nlog;
    r = pbt_rcv(&x, s, 100, 0);
    T_EQ(r->ioResult, 0);                                 /* matrix 500: rcvRest 0, n2 16 */
    T_EQ(r->csParam.receive.rcvBuffLen, 16);
    T_EQ(pbf_nlog, n0 + 1);                               /* nothing left: no DataArrival */
}

/* A pending read smaller than the data: it completes first, then DataArrival for the rest
 * (no DataArrival while it was pending, matrix 400/502). */
static void pending_small_read_then_data_arrival(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *r2;
    int n0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r = pbt_rcv(&x, s, 10, 0);
    T_EQ(r->ioResult, inProgress);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, banner, 26);
    T_EQ(pbf_nlog, n0 + 2);
    T_EQ(pbf_log[n0].kind, 'C');
    T_EQ(pbf_log[n0].pb, pbf_a(r));
    T_EQ(pbf_log[n0 + 1].kind, 'A');
    T_EQ(pbf_log[n0 + 1].reason, 16);
    /* two pending reads that together take everything: no DataArrival at all (matrix 502) */
    r = pbt_rcv(&x, s, 100, 0);                           /* takes the 16 left at once */
    T_EQ(r->csParam.receive.rcvBuffLen, 16);
    r = pbt_rcv(&x, s, 3, 0);
    r2 = pbt_rcv(&x, s, 100, 0);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps + 26, c.ms, TF_ACK | TF_PSH, 8192, "12345", 5);
    T_EQ(r->ioResult, 0);
    T_EQ(r->csParam.receive.rcvBuffLen, 3);
    T_EQ(r2->ioResult, 0);
    T_EQ(r2->csParam.receive.rcvBuffLen, 2);
    T_EQ(pbf_nlog, n0 + 2);
    T_EQ(pbf_count('A'), 1);                              /* only the t16 after the first read */
}

/* An app that does one small read per DataArrival: every read that leaves data gets another
 * ASR, but the ASRs run one after the other, never nested (bounded stack on the 68k). */
static pbt *g_x;
static int g_depth, g_max_depth, g_asrs;
static uint8_t g_got[64];
static int g_ngot;
static void asr_one_small_read(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    TCPiopb *r;
    (void)user; (void)reason; (void)icmp;
    if (ev != TCPDataArrival) return;
    g_asrs++;
    if (++g_depth > g_max_depth) g_max_depth = g_depth;
    r = pbt_rcv(g_x, s, 5, 0);
    if (r->ioResult == 0 && g_ngot + r->csParam.receive.rcvBuffLen <= 64) {
        memcpy(g_got + g_ngot, pbf_p(r->csParam.receive.rcvBuff), r->csParam.receive.rcvBuffLen);
        g_ngot += r->csParam.receive.rcvBuffLen;
    }
    g_depth--;
}

static void small_reads_from_asr_drain_without_nesting(void) {
    pbt x;
    pbt_conn c;
    g_x = &x;
    g_depth = g_max_depth = g_asrs = g_ngot = 0;
    pbt_init(&x, 1);
    pbt_est(&x, pbf_asr(asr_one_small_read), 0, &c);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, banner, 26);
    T_EQ(g_ngot, 26);                                     /* the app got everything */
    T_MEMEQ(g_got, banner, 26);
    T_EQ(g_asrs, 6);                                      /* 26, 21, 16, 11, 6, 1 */
    T_EQ(g_max_depth, 1);
    T_EQ(x.d->tcp[0].in_data_asr, 0);
    T_EQ(pbf_bad, 0);
}

/* Result contract 4 (mt_pb.h): through the driver-entry model an unconfigured call returns -28
 * and runs no completion routine (matrix "Unconfigured run": U01-U12 async, no C event). */
static int g_compl_runs;
static void compl_count(MtAnyPb *pb) { (void)pb; g_compl_runs++; }

static void unconfigured_through_driver_entry_no_completion(void) {
    pbt x;
    TCPiopb *pb;
    uint8_t *buf;
    pbt_init(&x, 0);
    g_compl_runs = 0;
    buf = (uint8_t *)pbf_alloc(4096);
    pb = pbt_pb(TCPCreate, 0);
    pb->ioCompletion = pbf_compl(compl_count);
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 4096;
    T_EQ(pbt_call(&x, pb), MT_PB_NOTOPEN);                /* matrix U01: -28 */
    T_EQ(pb->ioResult, MT_PB_NOTOPEN);
    pb = pbt_pb(TCPStatus, 0x12345678u);
    pb->ioCompletion = pbf_compl(compl_count);
    T_EQ(pbt_call(&x, pb), MT_PB_NOTOPEN);                /* matrix U04 */
    T_EQ(g_compl_runs, 0);
    T_EQ(pbf_count('C'), 0);
    T_EQ(pbf_nlog, 0);
}

/* 1.1d1 F2: tcp_hi / udp_hi are one past the highest slot in use (live or released with the core release deferred);
 * the glue's per-call and per-tick stream loops stop there, and a stream in the top slot is still served. */
static void stream_scans_bounded_by_highest_used_slot(void) {
    pbt x;
    StreamPtr a, b, c, u1, u2;
    TCPiopb *r;
    pbt_conn cc;
    pbt_init(&x, 1);
    T_EQ(x.d->tcp_hi, 0);
    T_EQ(x.d->udp_hi, 0);
    a = pbt_create(&x, 4096, 0, 0);
    b = pbt_create(&x, 4096, 0, 0);
    c = pbt_create(&x, 4096, 0, 0);
    T_CHECK(a && b && c);
    T_EQ(x.d->tcp_hi, 3);
    T_EQ(pbt_simple(&x, TCPRelease, c), 0);
    T_EQ(x.d->tcp_hi, 2);
    T_EQ(pbt_simple(&x, TCPRelease, a), 0);
    T_EQ(x.d->tcp_hi, 2);                                 /* b is still in slot 1 */
    T_EQ(pbt_simple(&x, TCPRelease, b), 0);
    T_EQ(x.d->tcp_hi, 0);
    a = pbt_create(&x, 4096, 0, 0);                       /* the slots rotate: slot 3 now */
    T_CHECK(a == mt_os_addr(&x.d->tcp[3]) + ((uint32_t)(x.d->tcp[3].gen & 7u) << 1));
    T_EQ(x.d->tcp_hi, 4);
    T_EQ(pbt_simple(&x, TCPRelease, a), 0);
    T_EQ(x.d->tcp_hi, 0);
    a = pbt_est(&x, 0, 0, &cc);                           /* slot 4, connected */
    T_CHECK(a != 0);
    T_EQ(x.d->tcp_hi, 5);
    r = pbt_rcv(&x, a, 10, 1);                            /* a timed read in the top slot */
    T_EQ(r->ioResult, inProgress);
    T_CHECK(mt_pb_next_deadline_ms(x.d) <= 1000);         /* its deadline is seen */
    pbf_now += 1000;
    mt_pb_tick(x.d);
    T_EQ(r->ioResult, commandTimeout);                    /* and it times out */
    u1 = pbt_ucreate(&x, 0, 4096, 0, 0, NULL);
    u2 = pbt_ucreate(&x, 0, 4096, 0, 0, NULL);
    T_CHECK(u1 && u2);
    T_EQ(x.d->udp_hi, 2);
    T_EQ(pbt_simple(&x, UDPRelease, u2), 0);
    T_EQ(x.d->udp_hi, 1);
    T_EQ(pbt_simple(&x, UDPRelease, u1), 0);
    T_EQ(x.d->udp_hi, 0);
}

/* 1.1d2 F7: the glue's command timeouts are cached like the core's deadline. Queries rescan nothing until a timeout
 * changes: queuing a timed read and serving it cost one rescan each, the queries in between none; the answer always
 * equals the full scan (mt_pb_next_deadline_scan, test builds). */
static void glue_deadline_cached(void) {
    pbt x;
    StreamPtr a;
    TCPiopb *r;
    pbt_conn cc;
    uint32_t sc;
    pbt_init(&x, 1);
    a = pbt_est(&x, 0, 0, &cc);
    T_CHECK(a != 0);
    T_EQ(mt_pb_next_deadline_ms(x.d), mt_pb_next_deadline_scan(x.d));
    sc = x.d->gdl_scans;
    T_EQ(mt_pb_next_deadline_ms(x.d), mt_pb_next_deadline_scan(x.d));
    T_EQ(x.d->gdl_scans, sc);                             /* nothing changed: no rescan */
    r = pbt_rcv(&x, a, 10, 1);                            /* a read with a 1 s timeout */
    T_EQ(r->ioResult, inProgress);
    T_EQ(mt_pb_next_deadline_ms(x.d), 1000);
    T_EQ(mt_pb_next_deadline_scan(x.d), 1000);
    T_EQ(x.d->gdl_scans, sc + 1);                         /* one rescan for the new timeout */
    pbf_now += 100;
    T_EQ(mt_pb_next_deadline_ms(x.d), 900);
    T_EQ(x.d->gdl_scans, sc + 1);                         /* cached */
    pbt_peer(&x, cc.lport, cc.ps, cc.ms, TF_ACK | TF_PSH, 8192, "hi", 2);   /* serves the read */
    T_EQ(r->ioResult, 0);
    T_EQ(mt_pb_next_deadline_ms(x.d), mt_pb_next_deadline_scan(x.d));
    T_EQ(x.d->gdl_scans, sc + 2);                         /* its timeout went: one rescan */
    T_EQ(mt_pb_next_deadline_ms(x.d), mt_pb_next_deadline_scan(x.d));
    T_EQ(x.d->gdl_scans, sc + 2);
}

/* 1.1d2: stream_of finds the slot by a multiply (mt_pb_slot) instead of a division: exact for every offset a live
 * handle has (slot * size + gen bits 0..14) on every slot, never more than one slot off elsewhere. */
static void slot_by_multiply_equals_division(void) {
    static const uint32_t sizes[2] = { sizeof(MtStream), sizeof(MtUdpStream) };
    static const uint32_t slots[2] = { MT_PB_MAX_TCP, MT_PB_MAX_UDP };
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t size = sizes[k], n = slots[k], m = MT_PB_SLOT_M(sizes[k]), i, r, off, bad = 0;
        T_CHECK(MT_PB_SLOT_OK(slots[k], sizes[k]));
        for (i = 0; i < n; i++)
            for (r = 0; r <= 14; r += 2)
                if (mt_pb_slot(i * size + r, m) != i) bad++;
        T_EQ(bad, 0);
        for (off = 0; off < n * size; off++) {
            uint32_t q = mt_pb_slot(off, m);
            if (q != off / size && q != off / size + 1u) bad++;
        }
        T_EQ(bad, 0);
    }
}

/* 1.1d2 fix: a forged handle 15 bytes into the last slot (beyond any gen offset) is refused through stream_of, while
 * the live handle of that slot is accepted. */
static void forged_handle_in_last_slot_refused(void) {
    pbt x;
    StreamPtr h = 0, last;
    uint8_t i, n;
    pbt_init(&x, 1);
    n = x.d->ntcp;
    for (i = 0; i < n; i++) h = pbt_create(&x, 4096, 0, 0);   /* the slots fill in order: the last one is n - 1 */
    T_CHECK(h == x.d->tcp[n - 1].handle);
    last = mt_os_addr(&x.d->tcp[0]) + (uint32_t)(n - 1u) * (uint32_t)sizeof(MtStream);
    T_EQ(pbt_simple(&x, TCPStatus, h), connectionDoesntExist);   /* found (no connection yet) */
    T_EQ(pbt_simple(&x, TCPStatus, last + 15u), invalidStreamPtr);
    T_EQ(pbt_simple(&x, TCPStatus, last + (uint32_t)sizeof(MtStream) - 1u), invalidStreamPtr);
}

T_MAIN(data_arrival_after_partial_read, pending_small_read_then_data_arrival,
       small_reads_from_asr_drain_without_nesting, unconfigured_through_driver_entry_no_completion,
       stream_scans_bounded_by_highest_used_slot, glue_deadline_cached,
       slot_by_multiply_equals_division, forged_handle_in_last_slot_refused)
