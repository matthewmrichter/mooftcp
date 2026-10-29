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

T_MAIN(data_arrival_after_partial_read, pending_small_read_then_data_arrival,
       small_reads_from_asr_drain_without_nesting, unconfigured_through_driver_entry_no_completion)
