/* Plan 3 Task 12: per-application cleanup under MultiFinder.
 *
 * MultiFinder sends the driver no goodbye kiss when an application quits (measured in Snow,
 * docs/dev/EMULATOR.md "MultiFinder"), and BufPtr is global there, so ApplZone..BufPtr would cover
 * other partitions. The glue records each task-time caller's partition (mt_os_app_mem) and sweeps the
 * owners whose partition is gone (mt_os_app_alive) at every Control entry and from the timer. The fake
 * OS (tests/pbfake.h "MultiFinder model") gives each application a partition in fake memory with its own
 * header words, switches ApplZone / A5 between them and disposes of a partition without a goodbye. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "t.h"
#include "pbfake.h"
#include "dnsb.h"
#include "mt_util.h"
#include "mt_gate.h"
#include "mt_drv_logic.h"
#include "mt_pb_entry.h"

#define TRAP_SYNC  0xA004u
#define TRAP_ASYNC (0xA004u | MT_DRV_TRAP_ASYNC)
#define PART       0x10000u                     /* 64K partitions */

static pbt g_x;
static MtEntry g_e;
static MtGate g_g;
static MacPtr32 g_compl[PBF_PARTS], g_asr[PBF_PARTS];
static int g_asr_n[PBF_PARTS];
static int g_cur;

static void compl_rec(MtAnyPb *pb) { (void)pb; }
static void asr_rec(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)reason; (void)icmp;
    if (user < PBF_PARTS) g_asr_n[user]++;
}

static void msetup(void) {
    mt_ifconfig c;
    pbt_init(&g_x, 1);
    mt_get_config(g_x.d->s, &c);
    c.dns = T_DNS;
    mt_pb_set_config(g_x.d, &c);
    mt_arp_insert(g_x.d->s, T_DNS, T_PEER_MAC, 0);
    memset(&g_g, 0, sizeof g_g);
    mt_entry_init(&g_e, g_x.d, &g_g);
    pbf_gate = &g_g;
    pbf_leave_hook = NULL;
    memset(g_asr_n, 0, sizeof g_asr_n);
}

/* An application: a partition with its own completion routine and ASR inside it. */
static int launch(void) {
    int a = pbf_mf_launch(PART);
    g_compl[a] = pbf_mf_compl(a, compl_rec);
    g_asr[a] = pbf_mf_asr(a, asr_rec);
    g_cur = a;
    return a;
}
static void to(int a) { pbf_mf_switch(a); g_cur = a; }

/* A PB of the current application (in its partition, its completion routine). */
static void *apb(int16_t cs, StreamPtr s) {
    TCPiopb *pb = (TCPiopb *)pbf_mf_alloc(g_cur, sizeof(MtAnyPb));
    pb->ioCompletion = g_compl[g_cur];
    pb->csCode = cs;
    pb->tcpStream = s;
    return pb;
}
/* A task-time call of the current application through the driver entry. */
static int16_t ecall(void *pb, int async) {
    ((MtAnyPb *)pb)->hdr.ioResult = inProgress;          /* as the Device Manager does */
    return mt_entry_control(&g_e, (MtAnyPb *)pb, async ? TRAP_ASYNC : TRAP_SYNC, 0);
}
static int ncompl(const void *pb) {
    int i, n = 0;
    for (i = 0; i < pbf_nlog && i < PBF_LOG; i++) n += pbf_log[i].kind == 'C' && pbf_log[i].pb == pbf_a(pb);
    return n;
}
/* The timer tick of the driver (mt_drv.c tick_cb) after `ms`. A gate held by someone else (on the Mac:
 * frames a quit process left inside the gate) defers the Time Manager task: nothing runs. */
static void tick(uint32_t ms) {
    pbf_now += ms;
    if (g_g.depth) return;
    g_g.depth++;
    mt_entry_sweep(&g_e, 0, MT_SWEEP_TIMER);
    if (!(g_x.d->n_own || g_x.d->depth) || mt_entry_core_ok(&g_e, MT_SWEEP_TIMER)) {
        mt_entry_drain(&g_e);
        mt_pb_tick(g_x.d);
    }
    g_g.depth--;
}
/* The running routine's context as the glue records it (co_ctx): the current application. */
static void co_ctx_is(int part) {
    g_x.d->co_ctx[0].lo = pbf_parts[part].lo;
    g_x.d->co_ctx[0].hi = pbf_parts[part].hi;
    g_x.d->co_ctx[0].h0 = pbf_parts[part].h0;
    g_x.d->co_ctx[0].name = pbf_parts[part].name;
    g_x.d->co_depth[0] = 0;
    g_x.d->co_n = 1;
    g_x.d->co_over = 0;
}
/* The input path's check (mt_drv.c rx_cb), optionally while an application routine runs (glue depth
 * 1, gate free: an Ethernet interrupt took the gate). 1 = the frame may be delivered. */
static int rx_check(int depth) {
    int ok;
    uint8_t sv = g_x.d->depth;
    if (sv) depth = -1;                          /* frames abandoned by a quit routine: keep them as they are */
    g_g.depth++;
    if (depth >= 0) g_x.d->depth = (uint8_t)depth;   /* -1: as it is */
    if (depth > 0) co_ctx_is(g_cur);                 /* a routine runs on the current (live) app's stack */
    ok = !(g_x.d->n_own || g_x.d->depth) || mt_entry_core_ok(&g_e, MT_SWEEP_RX);
    if (depth >= 0) g_x.d->depth = sv;
    g_g.depth--;
    return ok;
}

/* TCPCreate (buffer, ASR in the current partition) + ActiveOpen to the peer + SYN-ACK. */
static StreamPtr est(pbt_conn *c) {
    TCPiopb *pb = (TCPiopb *)apb(TCPCreate, 0), *op;
    tcpv v;
    StreamPtr s;
    pb->csParam.create.rcvBuff = pbf_a(pbf_mf_alloc(g_cur, 8192));
    pb->csParam.create.rcvBuffLen = 8192;
    pb->csParam.create.notifyProc = g_asr[g_cur];
    pb->csParam.create.userDataPtr = (MacPtr32)g_cur;
    if (ecall(pb, 0) != 0) return 0;
    s = pb->tcpStream;
    fake_clear(&g_x.fl);
    op = (TCPiopb *)apb(TCPActiveOpen, s);
    op->csParam.open.remoteHost = T_PEER;
    op->csParam.open.remotePort = PBT_PEER_PORT;
    if (ecall(op, 1) != 0 || op->ioResult != inProgress) return 0;
    if (!pbt_last_tcp(&g_x, &v) || v.flags != TF_SYN) return 0;
    c->lport = v.sport;
    c->ps = 5000;
    c->ms = v.seq + 1;
    pbt_peer(&g_x, c->lport, c->ps, c->ms, TF_SYN | TF_ACK, 8192, NULL, 0);
    c->ps++;
    fake_clear(&g_x.fl);
    return op->ioResult == 0 ? s : 0;
}
static TCPiopb *rcv(StreamPtr s, uint16_t len) {
    TCPiopb *pb = (TCPiopb *)apb(TCPRcv, s);
    pb->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(g_cur, len));
    pb->csParam.receive.rcvBuffLen = len;
    ecall(pb, 1);
    return pb;
}
static int16_t status(StreamPtr s) {
    TCPiopb *pb = (TCPiopb *)apb(TCPStatus, s);
    return ecall(pb, 0);
}
/* The peer sends `data` on c and the application reads it back with a pending TCPRcv. */
static int echoes(StreamPtr s, pbt_conn *c, const char *data) {
    uint16_t n = (uint16_t)strlen(data);
    TCPiopb *r = rcv(s, 64);
    if (r->ioResult != inProgress) return 0;
    pbt_peer(&g_x, c->lport, c->ps, c->ms, TF_ACK | TF_PSH, 8192, data, n);
    c->ps += n;
    return r->ioResult == 0 && r->csParam.receive.rcvBuffLen == n &&
           memcmp(pbf_p(r->csParam.receive.rcvBuff), data, n) == 0;
}
static int rst_to(uint16_t lport) {
    int i;
    for (i = 0; i < g_x.fl.n; i++) {
        tcpv v;
        if (view_tcp(g_x.fl.f[i], g_x.fl.len[i], &v) && v.sport == lport && (v.flags & TF_RST)) return 1;
    }
    return 0;
}

/* The carry-over RC item 12 case: B quits (no goodbye under MultiFinder), A's connection lives on. */
static void quit_one_app_keeps_other_apps_streams(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    TCPiopb *rb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    T_EQ(g_x.d->n_own, 2);
    rb = rcv(sb, 32);                                     /* B quits with a read pending */
    T_EQ(rb->ioResult, inProgress);
    pbf_mf_quit(b);
    to(a);                                                /* MultiFinder makes A current */
    fake_clear(&g_x.fl);
    T_EQ(status(sa), 0);                                  /* A's next call notices B is gone */
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(g_x.d->n_gone_call, 1);                          /* ...through the Control entry */
    T_EQ(g_x.d->n_gone_timer, 0);
    T_EQ(g_x.d->n_gone_rx, 0);
    T_EQ(g_x.d->n_own, 1);
    T_CHECK(rst_to(cb.lport));                            /* B's connection is reset */
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(ncompl(rb), 0);                                  /* silently: nothing of B's runs */
    T_EQ(rb->ioResult, inProgress);
    T_CHECK(echoes(sa, &ca, "still here"));               /* A keeps working */
    T_EQ(g_asr_n[b], 0);
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* A goodbye kiss under MultiFinder (if one comes, e.g. at shutdown) takes only the current partition:
 * BufPtr is global there, so the old ApplZone..BufPtr range would have covered A. */
static void goodbye_under_multifinder_spares_other_partitions(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    msetup();
    b = launch();                                   /* B below A, as in Snow (MfEchoB under MfEchoA): */
    sb = est(&cb);                                        /* B's ApplZone..BufPtr would contain A */
    a = launch();
    sa = est(&ca);
    T_CHECK(sa && sb);
    to(b);
    T_EQ(ecall(apb(-1, 0), 0), 0);                        /* B's goodbye kiss */
    T_EQ(g_e.n_goodbye, 1);
    T_EQ(g_x.d->n_own, 1);                                /* B's record went with it */
    to(a);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(status(sa), 0);
    T_CHECK(echoes(sa, &ca, "after goodbye"));
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* No call at all after B quits: the timer's periodic sweep finds it (the deadline asks for a tick). */
static void sweep_by_timer_when_nobody_calls(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    T_CHECK(mt_pb_next_deadline_ms(g_x.d) <= MT_PB_SWEEP_MS);
    pbf_mf_quit(b);
    to(a);
    tick(MT_PB_SWEEP_MS);
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(g_x.d->n_gone_timer, 1);                         /* ...through the timer */
    T_EQ(g_x.d->n_gone_call, 0);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(status(sa), 0);
    pbf_mf_quit(a);                                       /* the last one goes: no more periodic sweeps */
    tick(MT_PB_SWEEP_MS);
    T_EQ(g_x.d->n_own, 0);
    T_EQ(g_x.d->n_own_gone, 2);
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* Between the quit and the sweep the glue may still hold the dead application's PBs and routines;
 * data that arrives then must not run its completion routine or ASR (its code is gone). */
static void gone_apps_routines_never_run_before_the_sweep(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    TCPiopb *rb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    rb = rcv(sb, 32);
    pbf_mf_quit(b);
    to(a);
    pbt_peer(&g_x, cb.lport, cb.ps, cb.ms, TF_ACK | TF_PSH, 8192, "late data", 9);   /* input path: no sweep */
    T_EQ(ncompl(rb), 0);
    T_EQ(g_asr_n[b], 0);
    T_CHECK(g_x.d->n_dead_calls > 0);
    T_EQ(pbf_dead_calls, 0);
    tick(MT_PB_SWEEP_MS);
    T_EQ(status(sb), invalidStreamPtr);
    T_CHECK(echoes(sa, &ca, "fine"));
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* UDP with loans (Task 4), a DNR lookup and an ICMPEcho per application: B's go, A's keep working. */
static int g_lax;                               /* the fuzzer: a full table is a legal answer */
static UDPiopb *ucreate(StreamPtr *s, uint16_t *port) {
    UDPiopb *pb = (UDPiopb *)apb(UDPCreate, 0);
    int16_t r;
    pb->csParam.create.rcvBuff = pbf_a(pbf_mf_alloc(g_cur, 4096));
    pb->csParam.create.rcvBuffLen = 4096;
    pb->csParam.create.notifyProc = g_asr[g_cur];
    pb->csParam.create.userDataPtr = (MacPtr32)g_cur;
    r = ecall(pb, 0);
    if (!g_lax) T_EQ(r, 0);
    *s = r == 0 ? pb->udpStream : 0;
    *port = pb->csParam.create.localPort;
    return pb;
}
static UDPiopb *uread(StreamPtr s) {
    UDPiopb *pb = (UDPiopb *)apb(UDPRead, s);
    ecall(pb, 1);
    return pb;
}
static MtDnrPb *dnr(const char *name, uint8_t **hi) {
    MtDnrPb *p = (MtDnrPb *)apb(MT_CS_DNR, 0);
    char *n = (char *)pbf_mf_alloc(g_cur, 64);
    *hi = (uint8_t *)pbf_mf_alloc(g_cur, 280);
    strcpy(n, name);
    p->op = MT_DNR_A;
    p->name = pbf_a(n);
    p->out = pbf_a(*hi);
    p->hasproc = 1;
    T_EQ(ecall(p, 1), 0);
    T_EQ(p->ioResult, inProgress);
    return p;
}
static IPParamBlock *echo(mt_ip4 dest) {
    IPParamBlock *pb = (IPParamBlock *)apb(ipctlEchoICMP, 0);
    pb->csParam.IPEchoPB.dest = dest;
    pb->csParam.IPEchoPB.timeout = 5;
    pb->csParam.IPEchoPB.icmpCompletion = g_compl[g_cur];
    if (g_lax) (void)ecall(pb, 1); else T_EQ(ecall(pb, 1), 0);
    return pb;
}
static void last_echo(uint16_t *id, uint16_t *seq) {
    int i;
    for (i = g_x.fl.n - 1; i >= 0; i--) {
        ipv v;
        if (view_ip(g_x.fl.f[i], g_x.fl.len[i], &v) && v.proto == 1 && v.len >= 8 && v.payload[0] == 8) {
            *id = mt_rd16(v.payload + 4);
            *seq = mt_rd16(v.payload + 6);
            return;
        }
    }
    *id = *seq = 0;
}
static int ncompl_cs(int16_t cs, int from) {
    int i, n = 0;
    for (i = from; i < pbf_nlog && i < PBF_LOG; i++) n += pbf_log[i].kind == 'C' && pbf_log[i].cs == cs;
    return n;
}

static void sweep_with_loans_dns_echo_of_both_apps(void) {
    int a, b, n0;
    StreamPtr ua, ub;
    uint16_t pa, pb_, ida, seqa, idb, seqb;
    UDPiopb *ra1, *ra2, *rb1, *rb2, *ret;
    MtDnrPb *da, *db;
    uint8_t *ha, *hb, snap[280];
    qv qa, qb;
    pkt ans;
    msetup();
    a = launch();
    ucreate(&ua, &pa);
    b = launch();
    ucreate(&ub, &pb_);
    pbt_udp_in(&g_x, 9, pa, "a1", 2); pbt_udp_in(&g_x, 9, pa, "a2", 2);
    pbt_udp_in(&g_x, 9, pb_, "b1", 2); pbt_udp_in(&g_x, 9, pb_, "b2", 2);
    to(a); ra1 = uread(ua); ra2 = uread(ua);
    to(b); rb1 = uread(ub); rb2 = uread(ub);
    T_EQ(ra1->ioResult, 0); T_EQ(ra2->ioResult, 0); T_EQ(rb1->ioResult, 0); T_EQ(rb2->ioResult, 0);
    to(a); da = dnr("a.example.com", &ha); get_q(&g_x.fl, g_x.fl.n - 1, &qa);
    to(b); db = dnr("b.example.com", &hb); get_q(&g_x.fl, g_x.fl.n - 1, &qb);
    to(a); echo(T_PEER); last_echo(&ida, &seqa);
    to(b); echo(T_PEER); last_echo(&idb, &seqb);
    pbf_mf_quit(b);
    to(a);
    T_EQ(status(0), invalidStreamPtr);                    /* any task-time call of A sweeps */
    T_EQ(g_x.d->n_own_gone, 1);
    /* B: stream and loans gone, lookup and echo forgotten, nothing written or called afterwards */
    ret = (UDPiopb *)apb(UDPBfrReturn, ub);
    ret->csParam.receive.rcvBuff = rb1->csParam.receive.rcvBuff;
    T_EQ(ecall(ret, 0), invalidStreamPtr);
    memcpy(snap, hb, sizeof snap);
    n0 = pbf_nlog;
    begin(&ans, qb.id, 0x8180, qb.name, 1); count_an(&ans, 1); rr_a(&ans, 300, 0x0A000002u);
    reply(g_x.d->s, &qb, &ans, pbf_now + 5);
    T_MEMEQ(hb, snap, sizeof snap);
    T_EQ(ncompl(db), 0);
    pbt_echo_reply(&g_x, T_PEER, idb, seqb, NULL, 0);
    T_EQ(pbf_nlog, n0);                                   /* no notify for B's echo */
    pbt_udp_in(&g_x, 9, pb_, "b3", 2);                    /* the port is closed now */
    /* A: loans returned in any order, lookup answered, echo notified */
    ret = (UDPiopb *)apb(UDPBfrReturn, ua);
    ret->csParam.receive.rcvBuff = ra2->csParam.receive.rcvBuff;
    T_EQ(ecall(ret, 0), 0);
    ret = (UDPiopb *)apb(UDPBfrReturn, ua);
    ret->csParam.receive.rcvBuff = ra1->csParam.receive.rcvBuff;
    T_EQ(ecall(ret, 0), 0);
    begin(&ans, qa.id, 0x8180, qa.name, 1); count_an(&ans, 1); rr_a(&ans, 300, 0x0A000001u);
    reply(g_x.d->s, &qa, &ans, pbf_now + 5);
    T_EQ(da->ioResult, 0);
    T_EQ(mt_rd32(ha + 260), 0x0A000001u);
    n0 = pbf_nlog;
    pbt_echo_reply(&g_x, T_PEER, ida, seqa, NULL, 0);
    T_EQ(ncompl_cs(ipctlEchoICMP, n0), 1);                /* A's icmpCompletion with the driver's block */
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* A new application in the dead one's memory before any sweep: a different header (or a different
 * start inside the old range) proves the old owner gone; the new one's objects are its own. */
static void new_app_in_dead_apps_partition_is_not_the_old_owner(void) {
    int a, b, c, d;
    pbt_conn ca, cb, cc, cd;
    StreamPtr sa, sb, sc, sd;
    MacPtr32 blo;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    blo = pbf_parts[b].lo;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART - 0x1000u, 0);         /* same place, another size (header word) */
    g_compl[c] = pbf_mf_compl(c, compl_rec);
    g_asr[c] = pbf_mf_asr(c, asr_rec);
    g_cur = c;
    sc = est(&cc);                                        /* C's first call sweeps B first */
    T_CHECK(sc != 0);
    T_EQ(g_x.d->n_own_gone, 1);
    to(a);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(status(sc), 0);
    tick(MT_PB_SWEEP_MS);                                 /* C stays: it is alive */
    T_EQ(status(sc), 0);
    /* C quits; D starts 0x100 bytes into C's old range (its header lies inside C's memory) */
    pbf_mf_quit(c);
    pbf_mf_forget(c);
    d = pbf_mf_launch_at(blo + 0x100u, PART - 0x100u, 0);
    g_compl[d] = pbf_mf_compl(d, compl_rec);
    g_asr[d] = pbf_mf_asr(d, asr_rec);
    g_cur = d;
    sd = est(&cd);
    T_CHECK(sd != 0);
    T_EQ(g_x.d->n_own_gone, 2);
    T_EQ(status(sc), invalidStreamPtr);
    T_EQ(status(sd), 0);
    to(a);
    T_CHECK(echoes(sa, &ca, "A"));
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* The documented limit (Ruling in the Task 12 report): an application relaunched into exactly the same
 * block (same start and size) before any sweep ran is indistinguishable from the old
 * one; the old objects are kept (never a live application's connection dropped by mistake). */
static void identical_relaunch_before_a_sweep_is_not_detected(void) {
    int b, c;
    pbt_conn cb;
    StreamPtr sb;
    MacPtr32 blo;
    uint32_t bname;
    msetup();
    b = launch();
    sb = est(&cb);
    T_CHECK(sb != 0);
    blo = pbf_parts[b].lo;
    bname = pbf_parts[b].name;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART, bname);               /* the same application again */
    g_compl[c] = pbf_mf_compl(c, compl_rec);
    g_cur = c;
    T_EQ(status(sb), 0);                                  /* still there (the known limit) */
    T_EQ(g_x.d->n_own_gone, 0);
    T_EQ(pbf_bad, 0);
}

/* Fix round M2: a DIFFERENT application of the same size in exactly the same block differs by its
 * CurApName hash: its first call proves the old owner gone. */
static void different_app_in_identical_block_is_told_apart(void) {
    int b, c;
    pbt_conn cb, cc;
    StreamPtr sb, sc;
    MacPtr32 blo;
    msetup();
    b = launch();
    sb = est(&cb);
    T_CHECK(sb != 0);
    blo = pbf_parts[b].lo;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART, 0);                   /* same place and size, another name */
    g_compl[c] = pbf_mf_compl(c, compl_rec);
    g_asr[c] = pbf_mf_asr(c, asr_rec);
    g_cur = c;
    sc = est(&cc);
    T_CHECK(sc != 0);
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(status(sc), 0);
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* Fix round M4: an ICMPEcho without a notify routine holds its slot at most MT_PB_ECHO_NONOTIFY_S
 * (no goodbye drops it any more), whatever timeout it asked for, in both modes. */
static void echo_without_notify_is_capped(void) {
    int mode, k;
    for (mode = 0; mode < 2; mode++) {
        IPParamBlock *pb;
        msetup();
        if (mode) launch();
        for (k = 0; k < MT_PB_ECHOQ; k++) {
            pb = (IPParamBlock *)(mode ? apb(ipctlEchoICMP, 0) : (void *)pbt_pb(ipctlEchoICMP, 0));
            pb->ioCompletion = 0;
            pb->csParam.IPEchoPB.dest = T_PEER;
            pb->csParam.IPEchoPB.timeout = 30000;
            pb->csParam.IPEchoPB.icmpCompletion = 0;
            T_EQ(ecall(pb, 1), 0);
        }
        pb = (IPParamBlock *)pbt_pb(ipctlEchoICMP, 0);
        pb->csParam.IPEchoPB.dest = T_PEER;
        T_EQ(ecall(pb, 1), insufficientResources);        /* all four slots held */
        tick(MT_PB_ECHO_NONOTIFY_S * 1000u - 100u);
        T_EQ(ecall(pb, 1), insufficientResources);
        tick(200);
        T_EQ(ecall(pb, 1), 0);                            /* freed after the cap */
        T_EQ(pbf_bad, 0);
    }
}

/* Fix round M6: a timer sweep that finds a glue frame active pushes the periodic deadline, so the
 * timer does not re-fire at deadline 0; the next tick does the sweep. */
static void timer_sweep_bailing_does_not_spin(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    pbf_mf_quit(b);
    to(a);
    pbf_now += MT_PB_SWEEP_MS;
    g_x.d->depth = 1;
    T_EQ(mt_pb_sweep(g_x.d, 0, NULL, NULL), 0);
    g_x.d->depth = 0;
    T_EQ(g_x.d->n_own_gone, 0);
    T_CHECK(mt_pb_next_deadline_ms(g_x.d) >= MT_PB_SWEEP_RETRY_MS - 1u);
    T_CHECK(mt_pb_next_deadline_ms(g_x.d) <= MT_PB_SWEEP_RETRY_MS);
    tick(MT_PB_SWEEP_RETRY_MS);
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(pbf_bad, 0);
}

/* Without MultiFinder nothing is recorded: no sweep, no periodic deadline, the goodbye as before. */
static void classic_system6_records_no_owner(void) {
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    msetup();
    pbf_app_lo = pbf_a(pbf_alloc(8));                     /* the app range starts here (classic mode) */
    pb = pbt_pb(TCPCreate, 0);
    pb->csParam.create.rcvBuff = pbf_a(pbf_alloc(8192));
    pb->csParam.create.rcvBuffLen = 8192;
    T_EQ(ecall(pb, 0), 0);
    s = pb->tcpStream;
    T_EQ(g_x.d->n_own, 0);
    T_CHECK(pbt_connect(&g_x, s, &c) == 0);
    T_CHECK(mt_pb_next_deadline_ms(g_x.d) > MT_PB_SWEEP_MS);   /* idle: no sweep tick asked */
    tick(1000);
    T_EQ(pbt_simple(&g_x, TCPStatus, s), 0);
    T_EQ(ecall(pbt_pb(-1, 0), 0), 0);                     /* goodbye: ApplZone..BufPtr */
    T_EQ(pbt_simple(&g_x, TCPStatus, s), invalidStreamPtr);
    T_EQ(pbf_bad, 0);
}

/* More applications than owner slots: the unrecorded one is never swept (a leak, documented), the
 * recorded ones are; a system-heap client created by an application's call survives its quit. */
static void owner_table_full_and_system_heap_clients(void) {
    int apps[MT_PB_OWNERS + 1], i;
    pbt_conn c[MT_PB_OWNERS + 1], cs;
    StreamPtr s[MT_PB_OWNERS + 1], sys;
    TCPiopb *pb;
    msetup();
    for (i = 0; i <= MT_PB_OWNERS; i++) {
        apps[i] = launch();
        s[i] = est(&c[i]);
        T_CHECK(s[i] != 0);
    }
    T_EQ(g_x.d->n_own, MT_PB_OWNERS);
    T_CHECK(g_x.d->n_own_full > 0);
    to(apps[0]);                                          /* a system-heap stream made by app 0's call */
    pb = (TCPiopb *)apb(TCPCreate, 0);
    pb->ioCompletion = 0;
    pb->csParam.create.rcvBuff = pbf_a(pbf_alloc(8192));
    pb->csParam.create.rcvBuffLen = 8192;
    T_EQ(ecall(pb, 0), 0);
    sys = pb->tcpStream;
    T_CHECK(pbt_connect(&g_x, sys, &cs) == 0);
    for (i = 0; i <= MT_PB_OWNERS; i++) pbf_mf_quit(apps[i]);
    tick(MT_PB_SWEEP_MS);
    T_EQ(g_x.d->n_own_gone, MT_PB_OWNERS);
    T_EQ(pbt_simple(&g_x, TCPStatus, s[0]), invalidStreamPtr);
    T_EQ(pbt_simple(&g_x, TCPStatus, s[MT_PB_OWNERS]), 0);   /* never recorded: kept */
    T_EQ(pbt_simple(&g_x, TCPStatus, sys), 0);
    T_EQ(pbf_bad, 0);
}

/* The sweep never runs inside a call-out (a stream of the gone app may be mid-callback); it is
 * retried at the next call or tick. */
static void sweep_waits_for_glue_frames(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    pbf_mf_quit(b);
    to(a);
    g_x.d->depth = 1;
    T_EQ(mt_pb_sweep(g_x.d, 1, NULL, NULL), 0);
    T_EQ(g_x.d->n_own_gone, 0);
    g_x.d->depth = 0;
    T_EQ(status(sa), 0);
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(pbf_bad, 0);
}

/* Fix round 2 (1): a frame that arrives while an application routine runs (glue depth 1, gate free)
 * when an owner is gone cannot sweep; it is dropped, never delivered into the dead partition. The next
 * frame at depth 0 sweeps and is delivered. The timer skips its core work the same way. */
static void frame_dropped_when_dead_owner_cannot_be_swept(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    pbf_mf_quit(b);
    to(a);
    T_EQ(rx_check(1), 0);                                 /* dropped */
    T_EQ(g_x.d->n_dead_drops, 1);
    T_EQ(g_x.d->n_own_gone, 0);
    g_x.d->depth = 1;                                     /* the timer during that routine: no core work */
    co_ctx_is(a);                                         /* (a routine on A's stack; A is alive) */
    g_g.depth++;
    T_EQ(mt_entry_core_ok(&g_e, MT_SWEEP_TIMER), 0);
    g_g.depth--;
    g_x.d->depth = 0;
    T_EQ(g_x.d->n_dead_drops, 2);
    T_EQ(g_x.d->n_unstuck, 0);                            /* A is alive: its routine is never abandoned */
    T_EQ(rx_check(0), 1);                                 /* routine done: swept, delivered */
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(g_x.d->n_gone_rx, 1);
    T_EQ(status(sb), invalidStreamPtr);
    T_CHECK(echoes(sa, &ca, "ok"));
    T_EQ(pbf_bad, 0);
}

/* Fix round 2 (2): an interrupt sweep that drops owners while a gateless reader (quiet() at task
 * time, the input path's check) walks the table: the readers stop at MT_PB_OWNERS and never read a
 * torn slot (fails on the loops bounded only by n_own). */
static void concurrent_drop(void) { mt_pb_sweep(g_x.d, 0, NULL, NULL); }
static void readers_survive_concurrent_owner_drop(void) {
    int a, b, c, pass;
    pbt_conn ca, cb, cc;
    for (pass = 0; pass < 2; pass++) {
        msetup();
        a = launch();
        T_CHECK(est(&ca) != 0);
        b = launch();
        T_CHECK(est(&cb) != 0);
        c = launch();
        T_CHECK(est(&cc) != 0);
        T_EQ(g_x.d->n_own, 3);
        pbf_mf_quit(b);
        pbf_mf_quit(c);
        to(a);
        pbf_own_tab = g_x.d->own;
        pbf_alive_hook = concurrent_drop;                 /* fires inside the first alive check */
        if (pass == 0) {
            T_EQ(mt_pb_sweep(g_x.d, 1, NULL, NULL), 0);   /* quiet() at task time, then the full path */
        } else {
            T_EQ(mt_pb_owners_alive(g_x.d), 1);           /* the input path's check */
        }
        T_CHECK(pbf_alive_hook == NULL);                  /* the "interrupt" did run */
        T_EQ(g_x.d->n_own, 1);
        T_EQ(pbf_bad, 0);
        pbf_own_tab = NULL;
    }
}

/* Fix round 2 (3): the same application relaunched into the identical block before a sweep (the
 * documented limit) has overwritten the old app's pending TCPRcv PB; data for the old stream must not
 * make the glue fill a PB that no longer looks like a read (it was a NULL memcpy). */
static void rewritten_pending_pb_is_never_filled(void) {
    int b, c;
    pbt_conn cb;
    StreamPtr sb;
    TCPiopb *rb;
    MacPtr32 blo;
    uint32_t bname;
    msetup();
    b = launch();
    sb = est(&cb);
    T_CHECK(sb != 0);
    rb = rcv(sb, 32);
    T_EQ(rb->ioResult, inProgress);
    blo = pbf_parts[b].lo;
    bname = pbf_parts[b].name;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART, bname);               /* zeroes the old PB: csCode 0, no buffer */
    (void)c;
    pbt_peer(&g_x, cb.lport, cb.ps, cb.ms, TF_ACK | TF_PSH, 8192, "late", 4);
    T_EQ(ncompl(rb), 0);
    T_EQ(pbf_bad, 0);
}

/* Fix round 3: an application that quits from inside its own completion routine (ExitToShell, a
 * MultiFinder force quit) never returns into the glue; depth (and, from inside a core call, the gate)
 * stays set. The next frame / tick / Control call sees that the routine's owner is gone, abandons
 * those frames and sweeps: the survivors' network keeps working, nothing of the quit app is written. */
static jmp_buf g_quitjmp;
static int g_quitpart;
static const void *g_quit_pb;                    /* the fuzzer arms one PB at a time: a completion that did not
                                                  * fire inline (deferred, pending) is an ordinary one later */
static void quit_in_routine(MtAnyPb *pb) {
    if (g_quit_pb && pb != g_quit_pb) return;
    pbf_mf_quit(g_quitpart);
    memset(pbf_p(pbf_parts[g_quitpart].lo), 0xC5, pbf_parts[g_quitpart].hi - pbf_parts[g_quitpart].lo);
    longjmp(g_quitjmp, 1);                               /* never returns */
}
static int canary_ok(int part) {
    const uint8_t *m = (const uint8_t *)pbf_p(pbf_parts[part].lo);
    uint32_t i, n = pbf_parts[part].hi - pbf_parts[part].lo;
    for (i = 0; i < n; i++) if (m[i] != 0xC5) return 0;
    return 1;
}
static void quit_inside_own_routine_keeps_network_alive(void) {
    int a, b, pass;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    for (pass = 0; pass < 2; pass++) {
        msetup();
        a = launch();
        sa = est(&ca);
        b = launch();
        sb = est(&cb);
        T_CHECK(sa && sb);
        g_quitpart = b;
        if (pass == 0) {                                  /* the entry's inline completion: gate free */
            TCPiopb *pb = (TCPiopb *)apb(TCPStatus, sb);
            pb->ioCompletion = pbf_mf_compl(b, quit_in_routine);
            if (!setjmp(g_quitjmp)) { ecall(pb, 1); T_CHECK(0); }
            T_EQ(g_x.d->depth, 1);
            T_EQ(g_g.depth, 0);
        } else {                                          /* inside a core call: the gate is held too */
            TCPiopb *pb;
            pbt_peer(&g_x, cb.lport, cb.ps, cb.ms, TF_ACK | TF_PSH, 8192, "abcdef", 6);
            cb.ps += 6;
            pb = (TCPiopb *)apb(TCPRcv, sb);              /* data there, some left: completed inside the call */
            pb->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(b, 2));
            pb->csParam.receive.rcvBuffLen = 2;
            pb->ioCompletion = pbf_mf_compl(b, quit_in_routine);
            if (!setjmp(g_quitjmp)) { ecall(pb, 1); T_CHECK(0); }
            T_EQ(g_x.d->depth, 1);
            T_CHECK(g_g.depth > 0);                       /* abandoned frames hold the gate */
            pbf_irq = 0;                                  /* (the fake's own nesting counters for those */
            pbf_in_int = 0;                               /*  dead frames; the Mac has none) */
        }
        to(a);
        if (pass == 0) {
            T_EQ(rx_check(-1), 1);                        /* the next frame: abandoned, swept, delivered */
            T_EQ(g_x.d->n_gone_rx, 1);
            T_EQ(g_g.depth, 0);                           /* the detector's own gate holding was left alone */
        } else {
            T_EQ(status(sa), 0);                          /* a Control call clears the gate as well */
            T_EQ(g_g.depth, 0);
            T_EQ(g_x.d->n_gone_call, 1);
        }
        T_EQ(g_x.d->n_unstuck, 1);
        T_EQ(g_x.d->depth, 0);
        T_EQ(g_x.d->n_own_gone, 1);
        T_EQ(status(sb), invalidStreamPtr);
        T_CHECK(echoes(sa, &ca, "alive"));
        tick(MT_PB_SWEEP_MS);
        T_CHECK(canary_ok(b));
        T_EQ(pbf_dead_calls, 0);
        T_EQ(pbf_bad, 0);
    }
}

/* Fix round 3 / 4: B quits inside its routine and a DIFFERENT application of the same size lands in B's
 * block before any check: the header looks alive. Its first task-time Control call proves the routine
 * abandoned because its partition OVERLAPS the recorded context under another identity (the block was
 * reused: two live partitions cannot overlap); NOT because "a different application is calling" (round
 * 4: that happens legitimately, see xapp_routine_during_other_apps_drain). */
static void reused_block_unsticks_abandoned_routine(void) {
    int a, b, c;
    pbt_conn ca, cb, cc;
    StreamPtr sa, sb, sc;
    TCPiopb *pb;
    MacPtr32 blo;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    g_quitpart = b;
    pb = (TCPiopb *)apb(TCPStatus, sb);
    pb->ioCompletion = pbf_mf_compl(b, quit_in_routine);
    if (!setjmp(g_quitjmp)) { ecall(pb, 1); T_CHECK(0); }
    T_EQ(g_x.d->depth, 1);
    blo = pbf_parts[b].lo;
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART, 0);                   /* same place and size, another app */
    g_compl[c] = pbf_mf_compl(c, compl_rec);
    g_asr[c] = pbf_mf_asr(c, asr_rec);
    g_cur = c;
    T_EQ(rx_check(-1), 1);                                /* header alive: nothing proves it yet */
    T_EQ(g_x.d->n_unstuck, 0);
    sc = est(&cc);                                        /* C's first call */
    T_CHECK(sc != 0);
    T_EQ(g_x.d->n_unstuck, 1);
    T_EQ(g_x.d->depth, 0);
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(status(sb), invalidStreamPtr);
    to(a);
    T_CHECK(echoes(sa, &ca, "A"));
    T_EQ(pbf_bad, 0);
}

/* Fix round 3 (2): the after-callback drains like the timer: while a gone owner cannot be swept (a
 * live app's routine runs), its PBs queued in the entry are neither run nor completed; the sweep later
 * drops them. */
static void queued_pbs_of_gone_owner_not_drained(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    TCPiopb *q;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    q = (TCPiopb *)apb(TCPStatus, sb);                    /* queued by preemption (gate owned) */
    g_g.depth = 1;
    q->ioResult = inProgress;
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)q, TRAP_ASYNC, 1), 0);
    g_g.depth = 0;
    T_EQ(g_e.pend.queued, 1);
    pbf_mf_quit(b);
    to(a);
    g_x.d->depth = 1;                                     /* a routine runs on A's stack (A is alive) */
    co_ctx_is(a);
    g_g.depth++;
    mt_entry_drain_ok(&g_e, MT_SWEEP_TIMER);
    g_g.depth--;
    T_EQ(ncompl(q), 0);
    T_EQ(g_e.n_pend_run, 0);
    g_x.d->depth = 0;
    g_g.depth++;
    mt_entry_drain_ok(&g_e, MT_SWEEP_TIMER);              /* now: swept, B's queued PB dropped */
    g_g.depth--;
    T_EQ(ncompl(q), 0);
    T_EQ(g_e.n_pend_dropped, 1);
    T_EQ(g_x.d->n_gone_timer, 1);
    T_EQ(pbf_bad, 0);
}

/* Fix round 4 (the round-3 re-review's finding): the glue runs app A's completion routine inline while
 * app B is the CURRENT process (B's task-time call leaves the gate, own_and_run drains the ring, rx_cb ->
 * mt_pb_input -> A's TCPRcv completion; only A5 is A's, the low-memory globals stay B's). A's routine
 * re-issues an async call, the ordinary pattern: that arrives as "B calling at depth 1". Round 3 took a
 * different current application as proof of an abandoned routine and reset depth, co_gate, in_core and
 * the gate under B's running drain: depth wrapped to 255 when A's routine returned and every later sweep
 * bailed forever. Now the context recorded is the process running the chain (B), and B is alive. */
static int g_xa, g_xkillb = -1;                  /* A; pass 1: the routine ends the current process (B) */
static StreamPtr g_xs;
static pbt_conn *g_xc;
static int g_xdeliver;
static TCPiopb *g_xre;
static void reissue_compl(MtAnyPb *pb) {         /* A's completion: an async TCPStatus with A's completion */
    TCPiopb *st;
    (void)pb;
    if (g_xkillb >= 0) {                         /* ExitToShell from A's routine on B's stack: B ends */
        g_quitpart = g_xkillb;
        quit_in_routine(pb);
    }
    st = (TCPiopb *)pbf_mf_alloc(g_xa, sizeof(MtAnyPb));
    st->ioCompletion = g_compl[g_xa];
    st->csCode = TCPStatus;
    st->tcpStream = g_xs;
    st->ioResult = inProgress;
    (void)mt_entry_control(&g_e, (MtAnyPb *)st, TRAP_ASYNC, 0);
    g_xre = st;
}
static void drain_hook(void) {                   /* the caller's outermost gate leave: own_and_run drains a
                                                  * frame for A's connection (task time, caller current) */
    if (!g_xdeliver || g_g.depth) return;
    g_xdeliver = 0;
    g_g.depth = 1;
    if (!(g_x.d->n_own || g_x.d->depth) || mt_entry_core_ok(&g_e, MT_SWEEP_RX)) {
        uint32_t seq = g_xc->ps;
        g_xc->ps += 4;                           /* before the send: pass 1 longjmps out of it */
        pbt_peer(&g_x, g_xc->lport, seq, g_xc->ms, TF_ACK | TF_PSH, 8192, "xapp", 4);
    }
    g_g.depth = 0;
}
static void xapp_routine_during_other_apps_drain(void) {
    int a, b, pass;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    TCPiopb *ra;
    for (pass = 0; pass < 2; pass++) {
        msetup();
        a = launch();
        sa = est(&ca);
        b = launch();
        sb = est(&cb);
        T_CHECK(sa && sb);
        to(a);
        ra = (TCPiopb *)apb(TCPRcv, sa);                  /* A's pending read, completion in A */
        ra->ioCompletion = pbf_mf_compl(a, reissue_compl);
        ra->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(a, 64));
        ra->csParam.receive.rcvBuffLen = 64;
        T_EQ(ecall(ra, 1), 0);
        T_EQ(ra->ioResult, inProgress);
        g_xa = a; g_xs = sa; g_xc = &ca; g_xre = NULL; g_xdeliver = 1;
        g_xkillb = pass == 1 ? b : -1;
        to(b);
        pbf_leave_hook = drain_hook;
        if (pass == 0) {
            T_EQ(status(sb), 0);                          /* B's call: its gate leave delivers A's data */
            pbf_leave_hook = NULL;
            T_EQ(g_xdeliver, 0);
            T_EQ(ra->ioResult, 0);                        /* A's read completed inside B's drain... */
            T_CHECK(g_xre != NULL);
            T_EQ(ncompl(g_xre), 1);                       /* ...and its re-issued call ran as a call-out */
            T_EQ(g_xre->ioResult, 0);
            T_EQ(g_x.d->depth, 0);                        /* accounting intact: nothing abandoned */
            T_EQ(g_x.d->n_unstuck, 0);
            T_EQ(g_x.d->co_gate, 0);
            T_EQ(g_g.depth, 0);
            pbf_mf_quit(b);                               /* later B quits: A's network must survive */
            to(a);
            T_EQ(rx_check(0), 1);
            T_EQ(g_x.d->n_unstuck, 0);
        } else {
            /* A's routine, run from B's drain, calls ExitToShell: the CURRENT process (B) ends inside
             * its own driver call; the context recorded for the chain is B, so B's death frees the
             * frames and the gate levels B's drain held (own_and_run + mt_pb_input) */
            if (!setjmp(g_quitjmp)) { status(sb); T_CHECK(0); }
            pbf_leave_hook = NULL;
            T_EQ(g_x.d->depth, 1);
            T_EQ(g_g.depth, 2);                           /* B's drain (1) + its core call (1): dead */
            T_EQ(g_x.d->co_gate, 2);
            T_EQ(g_x.d->co_n, 1);
            T_EQ(g_x.d->co_ctx[0].lo, pbf_parts[b].lo);  /* B's stack, not A's partition */
            pbf_irq = 0;
            pbf_in_int = 0;
            to(a);
            T_EQ(status(sa), 0);                          /* A's next call: abandoned, gate released, swept */
            T_EQ(g_x.d->n_unstuck, 1);
            T_EQ(g_g.depth, 0);
            T_EQ(g_x.d->n_gone_call, 1);
        }
        T_EQ(g_x.d->n_own_gone, 1);
        T_EQ(g_x.d->n_dead_drops, 0);
        T_EQ(status(sb), invalidStreamPtr);
        T_CHECK(echoes(sa, &ca, "A lives"));
        T_EQ(g_x.d->depth, 0);
        T_EQ(g_g.depth, 0);
        tick(MT_PB_SWEEP_MS);
        if (pass == 1) T_CHECK(canary_ok(b));
        T_EQ(pbf_dead_calls, 0);
        T_EQ(pbf_bad, 0);
    }
    g_xkillb = -1;
}

/* Fix round 4 (safety net co_gen): a routine whose frames were abandoned, but which resumes anyway (a
 * wrongful unstick: here a frame's check sees the running process's header as changed for a moment),
 * must not wrap depth to 255 nor restore a stale co_gate; the driver keeps working afterwards. */
static int g_resumed;
static void resume_compl(MtAnyPb *pb) {
    int live = pbf_parts[g_quitpart].live;
    (void)pb;
    pbf_parts[g_quitpart].live = 0;                       /* the header "changes" while the routine runs */
    T_EQ(rx_check(-1), 1);                                /* a frame: abandons the chain (and sweeps B) */
    pbf_parts[g_quitpart].live = live;
    T_EQ(g_x.d->n_unstuck, 1);
    T_EQ(g_x.d->depth, 0);
    g_resumed++;                                          /* ...and the routine returns into the glue */
}
static void routine_resuming_after_unstick_keeps_accounting(void) {
    int a, b;
    pbt_conn ca, cb;
    StreamPtr sa, sb;
    TCPiopb *pb;
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    g_quitpart = b;
    g_resumed = 0;
    pb = (TCPiopb *)apb(TCPStatus, sb);
    pb->ioCompletion = pbf_mf_compl(b, resume_compl);
    T_EQ(ecall(pb, 1), 0);
    T_EQ(g_resumed, 1);
    T_EQ(g_x.d->depth, 0);                                /* not 255 */
    T_EQ(g_x.d->co_gate, 0);
    T_EQ(g_x.d->in_core, 0);
    T_EQ(g_g.depth, 0);
    T_EQ(g_x.d->n_unstuck, 1);
    to(a);
    T_EQ(status(sa), 0);                                  /* call-outs nest and return as before */
    T_EQ(g_x.d->depth, 0);
    T_CHECK(echoes(sa, &ca, "after a resume"));
    T_EQ(g_x.d->depth, 0);
    tick(MT_PB_SWEEP_MS);
    T_EQ(g_x.d->depth, 0);
    T_EQ(pbf_bad, 0);
}

/* Fix round 4: B quits inside a completion routine fired INSIDE the gate (a TCPRcv served inline from
 * mt_pb_control), so the dead frames hold the gate: frames and ticks are deferred (nothing can enter),
 * only a Control call can rescue. Pass 0: B's header changed, A's call abandons the chain and releases
 * exactly the gate level the chain held. Pass 1: D (same size, another name) sits in B's block before
 * any call (header alive); D's first call proves the chain dead by the overlap and releases the gate,
 * then D works. Pass 2: the SAME application relaunched identically (the documented limit) nests under
 * the dead chain and proves nothing; when it quits, A's call releases the whole gate. */
static void gate_held_while_abandoned_routines_process_quits(void) {
    int a, b, c, pass;
    pbt_conn ca, cb, cc;
    StreamPtr sa, sb, sc;
    TCPiopb *pb;
    MacPtr32 blo;
    uint32_t bname;
    for (pass = 0; pass < 3; pass++) {
        msetup();
        a = launch();
        sa = est(&ca);
        b = launch();
        sb = est(&cb);
        T_CHECK(sa && sb);
        g_quitpart = b;
        pbt_peer(&g_x, cb.lport, cb.ps, cb.ms, TF_ACK | TF_PSH, 8192, "abcdef", 6);
        cb.ps += 6;
        pb = (TCPiopb *)apb(TCPRcv, sb);
        pb->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(b, 2));
        pb->csParam.receive.rcvBuffLen = 2;
        pb->ioCompletion = pbf_mf_compl(b, quit_in_routine);
        if (!setjmp(g_quitjmp)) { ecall(pb, 1); T_CHECK(0); }
        T_EQ(g_x.d->depth, 1);
        T_EQ(g_g.depth, 1);                               /* the dead chain holds the gate once */
        T_EQ(g_x.d->co_gate, 1);
        pbf_irq = 0;
        pbf_in_int = 0;
        blo = pbf_parts[b].lo;
        bname = pbf_parts[b].name;
        tick(MT_PB_SWEEP_MS);                             /* deferred: the gate is held */
        T_EQ(g_x.d->n_unstuck, 0);
        T_EQ(g_g.depth, 1);
        if (pass == 0) {
            to(a);
            T_EQ(status(sa), 0);                          /* the rescue: header changed */
            T_EQ(g_x.d->n_unstuck, 1);
            T_EQ(g_x.d->n_gone_call, 1);
        } else {
            pbf_mf_forget(b);
            c = pbf_mf_launch_at(blo, PART, pass == 2 ? bname : 0);
            g_compl[c] = pbf_mf_compl(c, compl_rec);
            g_asr[c] = pbf_mf_asr(c, asr_rec);
            g_cur = c;
            sc = est(&cc);                                /* the new tenant's first calls */
            T_CHECK(sc != 0);
            if (pass == 1) {
                T_EQ(g_x.d->n_unstuck, 1);                /* overlap under another identity: abandoned */
                T_EQ(g_x.d->n_own_gone, 1);               /* ...and B's objects swept by the same rule */
                T_EQ(g_g.depth, 0);
                T_EQ(status(sc), 0);
            } else {
                T_EQ(g_x.d->n_unstuck, 0);                /* indistinguishable: nothing proven */
                T_EQ(g_x.d->depth, 1);
                T_EQ(g_g.depth, 1);                       /* its calls nested under the dead chain */
                T_EQ(status(sc), 0);
                T_EQ(g_x.d->depth, 1);
                T_EQ(g_g.depth, 1);
                pbf_mf_quit(c);                           /* when it quits the header changes at last */
                to(a);
                T_EQ(status(sa), 0);
                T_EQ(g_x.d->n_unstuck, 1);
                T_EQ(status(sc), invalidStreamPtr);
            }
        }
        T_EQ(g_x.d->depth, 0);
        T_EQ(g_g.depth, 0);
        T_EQ(g_g.in_irq, 0);
        T_EQ(g_x.d->in_core, 0);
        T_EQ(status(sb), invalidStreamPtr);
        to(a);
        T_CHECK(echoes(sa, &ca, "A"));
        T_EQ(g_x.d->depth, 0);
        T_EQ(g_g.depth, 0);
        tick(MT_PB_SWEEP_MS);
        if (pass == 0) T_CHECK(canary_ok(b));
        T_EQ(pbf_dead_calls, 0);
        T_EQ(pbf_bad, 0);
    }
}

/* Fix round 4 (rule 3): a call-out that begins at interrupt time records no context. The low-memory
 * globals then belong to whichever process was interrupted, possibly one in the middle of quitting
 * (partition already freed): A's ASR runs from a frame delivered at interrupt time while B (never a
 * caller) is current and gone; the ASR calls the driver. Nothing may be abandoned: the chain is live. */
static int g_asr_calls;
static void asr_calls_driver(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    TCPiopb *st = (TCPiopb *)pbf_mf_alloc((int)user, sizeof(MtAnyPb));
    (void)ev; (void)reason; (void)icmp;
    st->csCode = TCPStatus;
    st->tcpStream = s;
    st->ioResult = inProgress;
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)st, TRAP_SYNC, 1), 0);
    g_asr_calls++;
}
static void interrupt_time_routine_is_never_abandoned(void) {
    int a, b;
    pbt_conn ca;
    StreamPtr sa;
    msetup();
    a = launch();
    g_asr[a] = pbf_mf_asr(a, asr_calls_driver);
    sa = est(&ca);
    T_CHECK(sa != 0);
    b = pbf_mf_launch(PART);                              /* B: current, never calls the driver... */
    pbf_mf_quit(b);                                       /* ...and mid-quit when the frame interrupts */
    g_asr_calls = 0;
    g_g.depth++;                                          /* the protocol handler owns the core */
    pbf_in_int++;
    T_EQ(mt_entry_core_ok(&g_e, MT_SWEEP_RX), 1);
    pbt_peer(&g_x, ca.lport, ca.ps, ca.ms, TF_ACK | TF_PSH, 8192, "data", 4);
    ca.ps += 4;
    pbf_in_int--;
    g_g.depth--;
    T_EQ(g_asr_calls, 1);                                 /* A's ASR ran and called the driver */
    T_EQ(g_x.d->n_unstuck, 0);                            /* no context: never abandoned */
    T_EQ(g_x.d->co_n, 0);
    T_EQ(g_x.d->depth, 0);
    T_EQ(g_g.depth, 0);
    to(a);
    T_EQ(status(sa), 0);
    T_EQ(g_x.d->n_unstuck, 0);
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
}

/* Fix round 4 (found by fuzz_apps): while a dead owner cannot be swept (a routine of live app A runs),
 * a new application D is launched into the dead one's block and calls: it is recorded anyway, and when
 * the sweep finally runs, the dead owner's objects go while D's (created later, inside the same address
 * range) stay. The classic case B's objects are told from D's by age (MtDrv.own_ser / born). */
static void tenant_of_unswept_block_keeps_its_objects(void) {
    int a, b, d, pass;
    pbt_conn ca, cb, cd;
    StreamPtr sa, sb, sd;
    TCPiopb *rb;
    MacPtr32 blo;
    for (pass = 0; pass < 2; pass++) {                    /* pass 1: that routine fired inside the gate, so the
                                                           * gate is held too (D's calls nest as call-outs) */
    msetup();
    a = launch();
    sa = est(&ca);
    b = launch();
    sb = est(&cb);
    T_CHECK(sa && sb);
    rb = rcv(sb, 32);                                     /* B quits with a read pending */
    blo = pbf_parts[b].lo;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    g_x.d->depth = 1;                                     /* a routine runs on A's stack: no forgetting now */
    co_ctx_is(a);
    if (pass == 1) { g_g.depth = 1; g_x.d->co_gate = 1; }
    d = pbf_mf_launch_at(blo, PART - 0x1000u, 0);         /* D in B's block (another size: B's header gone) */
    g_compl[d] = pbf_mf_compl(d, compl_rec);
    g_asr[d] = pbf_mf_asr(d, asr_rec);
    g_cur = d;
    {                                                     /* D creates and opens while the sweep must wait... */
        TCPiopb *pb = (TCPiopb *)apb(TCPCreate, 0), *op;
        tcpv v;
        pb->csParam.create.rcvBuff = pbf_a(pbf_mf_alloc(d, 8192));
        pb->csParam.create.rcvBuffLen = 8192;
        pb->csParam.create.notifyProc = g_asr[d];
        T_EQ(ecall(pb, 0), 0);                            /* sync: ioResult is set by the entry */
        sd = pb->tcpStream;
        fake_clear(&g_x.fl);
        op = (TCPiopb *)apb(TCPActiveOpen, sd);
        op->csParam.open.remoteHost = T_PEER;
        op->csParam.open.remotePort = PBT_PEER_PORT;
        T_EQ(ecall(op, 1), 0);
        T_CHECK(pbt_last_tcp(&g_x, &v) && v.flags == TF_SYN);
        cd.lport = v.sport; cd.ps = 5000; cd.ms = v.seq + 1;
        pbt_peer(&g_x, cd.lport, cd.ps, cd.ms, TF_SYN | TF_ACK, 8192, NULL, 0);
        cd.ps++;
        T_EQ(op->ioResult, inProgress);                   /* the documented cost of the limbo: its async PB in
                                                           * the dead owner's range is withheld (mt_pb_gone cannot
                                                           * tell it from a stale one by address); the stream
                                                           * itself is connected */
        T_CHECK(g_x.d->n_dead_calls > 0);
    }
    T_EQ(g_x.d->n_own_gone, 0);
    T_EQ(g_x.d->n_own, 3);                                /* ...and is recorded nevertheless */
    T_EQ(status(sd), 0);
    g_x.d->depth = 0;                                     /* A's routine returned */
    if (pass == 1) { g_g.depth = 0; g_x.d->co_gate = 0; }
    to(a);
    T_EQ(status(sa), 0);                                  /* the sweep: B goes, D stays */
    T_EQ(g_x.d->n_own_gone, 1);
    T_EQ(g_x.d->n_own, 2);
    T_EQ(status(sb), invalidStreamPtr);
    T_EQ(ncompl(rb), 0);
    T_CHECK(rst_to(cb.lport));
    to(d);
    T_EQ(status(sd), 0);
    T_CHECK(echoes(sd, &cd, "D works"));
    to(a);
    T_CHECK(echoes(sa, &ca, "A works"));
    pbf_mf_quit(d);                                       /* and when D quits, its record (made while the sweep
                                                           * had to wait) is what lets its objects go */
    T_EQ(status(sa), 0);
    T_EQ(g_x.d->n_own_gone, 2);
    T_EQ(status(sd), invalidStreamPtr);
    T_CHECK(rst_to(cd.lport));
    T_EQ(pbf_dead_calls, 0);
    T_EQ(pbf_bad, 0);
    }
}

/* ---- randomized: up to three applications launching, calling, quitting without a goodbye, a new one
 * reusing a dead one's memory, data for every connection (also the dead ones' before the sweep), ticks.
 * Invariants: no routine or PB of a quit partition is ever run (pbf_dead_calls), after a sweep tick no
 * glue object refers to a quit partition, every object of a live application is still there, and
 * nothing writes into a quit partition after its sweep (canary). FZ_SEED=n replays one seed,
 * FZ_SEEDS=n runs seeds 1..n (default 40). */
#define FA 3
#define FS 3
typedef struct {
    int part, live, swept, qstep;
    StreamPtr s[FS];
    pbt_conn c[FS];
    StreamPtr u;
    uint16_t uport;
} fapp;
#define FMAX 24
static fapp F[FMAX];
static int g_nf;
static uint32_t g_rng;
static uint32_t rn(uint32_t n) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 8) % n; }
static int g_fail;
static void fbad(uint32_t seed, int step, const char *what) {
    if (g_fail++ < 5) printf("fuzz_apps: seed %u step %d: %s\n", seed, step, what);
}

static int in_part(const pbf_part *p, MacPtr32 a) { return a && a >= p->lo && a < p->hi; }
static int pb_in(const pbf_part *p, const MtAnyPb *pb) {
    return pb && (in_part(p, pbf_a(pb)) || in_part(p, pb->hdr.ioCompletion));
}
/* Does any glue object still refer to partition p? */
static int refers(const pbf_part *p) {
    MtDrv *d = g_x.d;
    uint8_t i, k;
    for (i = 0; i < d->ntcp; i++) {
        const MtStream *s = &d->tcp[i];
        if (!s->used) continue;
        if (in_part(p, s->rbuf) || in_part(p, s->asr) || pb_in(p, s->open_pb) || pb_in(p, s->close_pb)) return 1;
        for (k = 0; k < s->rq_n; k++) if (pb_in(p, s->rq[k].pb)) return 1;
        for (k = 0; k < s->sq_n; k++) if (pb_in(p, s->sq[k].pb)) return 1;
    }
    for (i = 0; i < d->nudp; i++) {
        const MtUdpStream *u = &d->udp[i];
        if (!u->used) continue;
        if (in_part(p, u->rbuf) || in_part(p, u->asr)) return 1;
        for (k = 0; k < u->rq_n; k++) if (pb_in(p, u->rq[k].pb)) return 1;
    }
    for (i = 0; i < MT_PB_ECHOQ; i++)
        if (d->echo[i].state && in_part(p, d->echo[i].blk.ioCompletion)) return 1;
    return 0;
}
static int tcp_live(StreamPtr h) {
    uint8_t i;
    for (i = 0; i < g_x.d->ntcp; i++) if (g_x.d->tcp[i].used == 1 && g_x.d->tcp[i].handle == h) return 1;
    return 0;
}
static int udp_live(StreamPtr h) {
    uint8_t i;
    for (i = 0; i < g_x.d->nudp; i++) if (g_x.d->udp[i].used == 1 && g_x.d->udp[i].handle == h) return 1;
    return 0;
}
static int live_apps(void) {
    int i, n = 0;
    for (i = 0; i < g_nf; i++) n += F[i].live;
    return n;
}
static int pick_live(void) {
    int i, k;
    if (!g_nf) return -1;
    k = (int)rn((uint32_t)g_nf);
    for (i = 0; i < g_nf; i++, k = (k + 1) % g_nf) if (F[k].live) return k;
    return -1;
}

/* The driver's input path (mt_drv.c rx_cb): before every frame while owners exist; half of the
 * frames arrive while some application routine runs (depth 1: the sweep cannot run, the frame must
 * then be dropped). rx() checks; rx_end() restores depth after the delivery. */
static int g_rxdepth, g_rxirq;
static uint8_t g_rxsv;
static int rx(void) {
    int ok;
    if (g_g.depth) return 0;                     /* the gate is held (by frames a quit process left): the
                                                  * frame waits in the ring; here it is lost to the model */
    g_rxdepth = (int)rn(2);
    ok = rx_check(g_rxdepth);
    g_rxsv = g_x.d->depth;
    if (ok && g_rxdepth && !g_rxsv) g_x.d->depth = 1;   /* the frame is delivered inside that routine's time */
    g_rxirq = ok && rn(2);                       /* from the protocol handler (interrupt time) or a task-time drain */
    if (g_rxirq) pbf_in_int++;
    return ok;
}
static void rx_end(void) {
    g_x.d->depth = g_rxsv;
    if (g_rxirq) pbf_in_int--;
}
/* Round 4: frames that wait in the gate's ring and are delivered by the gate owner's drain, i.e. at the
 * outermost gate leave of whichever application's task-time call comes next (own_and_run in mt_gate.c):
 * the routines they fire (another app's completion, which may re-issue a call) then run inline with
 * THAT application current. pbf_leave_hook plays own_and_run. */
typedef struct { int b, k; } fring_ent;
static fring_ent g_ring[8];
static int g_ring_n, g_in_hook;
static unsigned long g_drained, g_reissued;
static void fring_push(int b, int k) { if (g_ring_n < 8) { g_ring[g_ring_n].b = b; g_ring[g_ring_n].k = k; g_ring_n++; } }
static void fdeliver(int b, int k) {             /* the caller holds the gate (depth 1) */
    if (!(g_x.d->n_own || g_x.d->depth) || mt_entry_core_ok(&g_e, MT_SWEEP_RX)) {
        pbt_peer(&g_x, F[b].c[k].lport, F[b].c[k].ps, F[b].c[k].ms, TF_ACK | TF_PSH, 8192, "datadata", 8);
        F[b].c[k].ps += 8;
    }
}
static void fhook(void) {
    if (g_in_hook || g_g.depth) return;          /* nested leaves are not the outermost one */
    g_in_hook = 1;
    g_g.depth = 1;
    while (g_ring_n) {
        fring_ent r = g_ring[0];
        memmove(g_ring, g_ring + 1, (size_t)(--g_ring_n) * sizeof r);
        g_drained++;
        fdeliver(r.b, r.k);
    }
    g_g.depth = 0;
    g_in_hook = 0;
}
/* Data for connection (b, k): delivered now (depth 0 or 1, interrupt or task time), or left in the ring
 * for the next drain. 1 = delivered now. */
static int rx_tcp(int b, int k) {
    if (g_g.depth || rn(4) == 0) { fring_push(b, k); return 0; }
    if (!rx()) return 0;
    pbt_peer(&g_x, F[b].c[k].lport, F[b].c[k].ps, F[b].c[k].ms, TF_ACK | TF_PSH, 8192, "datadata", 8);
    rx_end();
    F[b].c[k].ps += 8;
    return 1;
}
/* A completion routine (of the app whose partition holds the PB) that re-issues an async TCPStatus with
 * the same app's ordinary completion: the pattern of every async client. */
static void compl_reissue(MtAnyPb *pb) {
    int i;
    MacPtr32 at = pbf_a(pb);
    for (i = 0; i < PBF_PARTS; i++)
        if (pbf_parts[i].lo && pbf_parts[i].live && at >= pbf_parts[i].lo && at < pbf_parts[i].hi) break;
    if (i == PBF_PARTS || pbf_mf_left(i) < 0x400u) return;
    {
        TCPiopb *st = (TCPiopb *)pbf_mf_alloc(i, sizeof(MtAnyPb));
        st->ioCompletion = g_compl[i];
        st->csCode = TCPStatus;
        st->tcpStream = pb->tcp.tcpStream;
        st->ioResult = inProgress;
        (void)mt_entry_control(&g_e, (MtAnyPb *)st, TRAP_ASYNC, 0);
        g_reissued++;
    }
}
/* The quit: MultiFinder frees the partition (no goodbye); from now on nothing may write into it. */
static void fquit(int a, int step) {
    pbf_mf_quit(F[a].part);
    F[a].live = 0;
    memset(pbf_p(pbf_parts[F[a].part].lo), 0xC5, pbf_parts[F[a].part].hi - pbf_parts[F[a].part].lo);
    F[a].swept = 1;                                        /* canary armed at the quit */
    F[a].qstep = step;
}

static unsigned long g_steps, g_gone, g_deadc, g_apps, g_inroutine, g_ingate, g_unstuck, g_wait;
static void fuzz_one(uint32_t seed) {
    int step, i, j;
    msetup();
    g_rng = seed * 2654435761u + 1u;
    g_nf = 0;
    g_ring_n = 0;
    g_in_hook = 0;
    pbf_leave_hook = fhook;
    memset(F, 0, sizeof F);
    for (step = 0; step < 300 && !g_fail; step++) {
        int a = pick_live(), r = (int)rn(14), ticked = 0;
        g_steps++;
        if (a < 0 && g_nf >= FMAX) break;
        if (a < 0 || (r == 0 && live_apps() < FA && g_nf < FMAX)) {   /* launch */
            int reuse = -1;
            for (i = 0; i < g_nf; i++) if (!F[i].live && F[i].part >= 0 && rn(2)) { reuse = i; break; }
            if (reuse >= 0) {
                MacPtr32 lo = pbf_parts[F[reuse].part].lo + (rn(2) ? 0 : 0x100u);
                uint32_t sz = pbf_parts[F[reuse].part].hi - lo;
                if (lo == pbf_parts[F[reuse].part].lo && rn(2)) sz -= 0x800u;   /* same start: another size,
                                                      or the same size under another name (never the identical
                                                      application in the identical block: the documented limit) */
                pbf_mf_forget(F[reuse].part);
                F[reuse].part = -1;                      /* its memory is the new one's now */
                a = g_nf++;
                memset(&F[a], 0, sizeof F[a]);
                F[a].part = pbf_mf_launch_at(lo, sz, 0);
            } else {
                a = g_nf++;
                memset(&F[a], 0, sizeof F[a]);
                F[a].part = pbf_mf_launch(PART);
            }
            if (F[a].part < 0) { fbad(seed, step, "launch failed"); return; }
            F[a].live = 1;
            g_compl[F[a].part] = pbf_mf_compl(F[a].part, compl_rec);
            g_asr[F[a].part] = pbf_mf_asr(F[a].part, asr_rec);
            g_cur = F[a].part;
            continue;
        }
        to(F[a].part);
        j = (int)rn(FS);
        if (pbf_mf_left(F[a].part) < 0x5000u) r = 8;      /* its partition is nearly full: it quits */
        switch (r) {
        case 1: case 2:
            if (!F[a].s[j]) F[a].s[j] = est(&F[a].c[j]);
            break;
        case 3:
            if (F[a].s[j]) (void)rcv(F[a].s[j], (uint16_t)(1 + rn(40)));
            break;
        case 4: {                                          /* data for any connection, dead ones too */
            int b = (int)rn((uint32_t)g_nf), k = (int)rn(FS);
            if (F[b].s[k]) (void)rx_tcp(b, k);
            break;
        }
        case 11:                                           /* round 4: quit from inside a completion routine
                                                            * fired INSIDE the gate (a read served inline): the
                                                            * dead frames hold the gate */
            if (F[a].s[j] && !g_g.depth && rn(2)) {
                TCPiopb *pb;
                if (g_g.depth || !rx()) break;             /* data first, delivered now */
                pbt_peer(&g_x, F[a].c[j].lport, F[a].c[j].ps, F[a].c[j].ms, TF_ACK | TF_PSH, 8192, "datadata", 8);
                rx_end();
                F[a].c[j].ps += 8;
                if (!F[a].live) break;                     /* (cannot happen: a is live) */
                pb = (TCPiopb *)apb(TCPRcv, F[a].s[j]);
                pb->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(F[a].part, 4));
                pb->csParam.receive.rcvBuffLen = 4;
                pb->ioCompletion = pbf_mf_compl(F[a].part, quit_in_routine);
                g_quitpart = F[a].part;
                g_quit_pb = pb;
                if (!setjmp(g_quitjmp)) {
                    ecall(pb, 1);
                    g_quit_pb = NULL;                      /* the read did not complete inline (queued, the
                                                            * nesting limit, data already read): release the
                                                            * stream, its completion is then an ordinary one */
                    T_EQ(ecall(apb(TCPRelease, F[a].s[j]), 0), 0);
                    F[a].s[j] = 0;
                    break;
                }
                g_quit_pb = NULL;
                pbf_irq = 0;                               /* the fake's nesting counters of the dead frames */
                pbf_in_int = 0;
                F[a].live = 0;
                F[a].swept = 1;
                F[a].qstep = step;
                g_inroutine++;
                if (g_g.depth) g_ingate++;
            }
            break;
        case 12:                                           /* round 4: a read whose completion re-issues a call,
                                                            * answered from the next task-time caller's drain */
            if (F[a].s[j]) {
                TCPiopb *pb = (TCPiopb *)apb(TCPRcv, F[a].s[j]);
                pb->csParam.receive.rcvBuff = pbf_a(pbf_mf_alloc(F[a].part, 16));
                pb->csParam.receive.rcvBuffLen = 16;
                pb->ioCompletion = pbf_mf_compl(F[a].part, compl_reissue);
                ecall(pb, 1);
                fring_push(a, j);
            }
            break;
        case 5:
            if (!F[a].u) ucreate(&F[a].u, &F[a].uport);
            else if (rn(2)) (void)uread(F[a].u);
            else if (rx()) { pbt_udp_in(&g_x, 9, F[a].uport, "u", 1); rx_end(); }
            break;
        case 6:
            (void)echo(T_PEER);
            if (rn(2)) {
                uint16_t id, sq;
                last_echo(&id, &sq);
                if (rx()) { pbt_echo_reply(&g_x, T_PEER, id, sq, NULL, 0); rx_end(); }
            }
            break;
        case 7:
            if (F[a].s[j] && rn(3) == 0) {
                T_EQ(ecall(apb(TCPRelease, F[a].s[j]), 0), 0);
                F[a].s[j] = 0;
            }
            break;
        case 8:                                            /* quit: no goodbye under MultiFinder */
            fquit(a, step);
            break;
        case 10:
            if (rn(4) == 0) {                              /* quit from inside its own completion routine */
                TCPiopb *pb = (TCPiopb *)apb(TCPStatus, F[a].s[j]);
                g_quitpart = F[a].part;
                pb->ioCompletion = pbf_mf_compl(F[a].part, quit_in_routine);
                g_quit_pb = pb;
                if (!setjmp(g_quitjmp)) {
                    ecall(pb, 1);                          /* nested MT_PB_INLINE_MAX deep under stuck chains: the
                                                            * completion is deferred (ordinary later), no quit */
                    g_quit_pb = NULL;
                    if (pb->ioResult == inProgress) { fbad(seed, step, "status call left pending"); return; }
                    break;
                }
                g_quit_pb = NULL;
                pbf_irq = 0;                               /* the fake's nesting counters of the dead frames */
                pbf_in_int = 0;
                F[a].live = 0;
                F[a].swept = 1;                            /* quit_in_routine armed the canary */
                F[a].qstep = step;
                g_inroutine++;
            } else {
                tick(rn(300));
            }
            break;
        case 9:
            if (rn(6) == 0) {                              /* a goodbye kiss, then the quit */
                T_EQ(ecall(apb(-1, 0), 0), 0);
                fquit(a, step);
            }
            break;
        default:
            tick(rn(300));
            break;
        }
        if (pbf_dead_calls) { fbad(seed, step, "a quit application's routine or PB was run"); return; }
        if (pbf_bad) { fbad(seed, step, "misuse seen by the fake OS"); return; }
        /* At top level the only glue frames are ones a quit process left: at most a few, each chain with
         * its context recorded (a wrapped depth or a lost context would stall the driver for good). */
        if (g_x.d->depth > 6) { fbad(seed, step, "glue depth wrapped / grew without bound"); return; }
        if (g_x.d->depth && !g_x.d->co_n) { fbad(seed, step, "stuck frames without a recorded context"); return; }
        if (!g_x.d->depth && g_x.d->co_gate) { fbad(seed, step, "co_gate left set at depth 0"); return; }
        if (rn(3) == 0) {                                  /* a sweep tick, then the strict checks */
            tick(MT_PB_SWEEP_MS);
            ticked = !g_g.depth && !g_x.d->depth;          /* the sweep could run: nothing stuck (a dead chain
                                                            * still looking alive, i.e. its block reused by an
                                                            * app that has not called yet, is the documented
                                                            * limit until that app calls or quits) */
            for (i = 0; ticked && i < g_nf; i++) {
                if (F[i].live || F[i].part < 0) continue;
                if (refers(&pbf_parts[F[i].part])) { fbad(seed, step, "object of a quit partition kept"); return; }
            }
        }
        for (i = 0; i < g_nf; i++) {
            if (F[i].live) {
                for (j = 0; j < FS; j++)
                    if (F[i].s[j] && !tcp_live(F[i].s[j])) { fbad(seed, step, "live app's TCP stream dropped"); return; }
                if (F[i].u && !udp_live(F[i].u)) { fbad(seed, step, "live app's UDP stream dropped"); return; }
            } else if (F[i].swept && F[i].part >= 0 &&
                       (step - F[i].qstep < 48 || (step & 15) == 0 || step == 299)) {   /* every step soon after the quit */
                static uint8_t ref[PART];
                if (!ref[0]) memset(ref, 0xC5, sizeof ref);
                if (memcmp(pbf_p(pbf_parts[F[i].part].lo), ref, pbf_parts[F[i].part].hi - pbf_parts[F[i].part].lo))
                    { fbad(seed, step, "write into a quit partition (canary armed at the quit)"); return; }
            }
        }
    }
}

static void fuzz_apps(void) {
    uint32_t first = getenv("FZ_SEED") ? (uint32_t)atoi(getenv("FZ_SEED")) : 1;
    uint32_t last = getenv("FZ_SEED") ? first : getenv("FZ_SEEDS") ? (uint32_t)atoi(getenv("FZ_SEEDS")) : 40;
    uint32_t s;
    g_fail = 0;
    g_lax = 1;
    for (s = first; s <= last && !g_fail; s++) {
        fuzz_one(s);
        g_gone += g_x.d->n_own_gone;
        g_deadc += g_x.d->n_dead_calls;
        g_unstuck += g_x.d->n_unstuck;
        g_wait += g_x.d->n_unstick_wait;
        g_apps += (unsigned long)g_nf;
    }
    pbf_leave_hook = NULL;
    printf("fuzz_apps: %lu steps, %lu applications, %lu swept, %lu routines of quit applications skipped, "
           "%lu quits inside a routine (%lu of them inside the gate), %lu abandoned routine chains, %lu postponed, "
           "%lu frames drained by another app's call, %lu calls re-issued from a routine\n", g_steps, g_apps, g_gone,
           g_deadc, g_inroutine, g_ingate, g_unstuck, g_wait, g_drained, g_reissued);
    g_lax = 0;
    T_EQ(g_fail, 0);
}

T_MAIN(quit_one_app_keeps_other_apps_streams, goodbye_under_multifinder_spares_other_partitions,
       sweep_by_timer_when_nobody_calls, gone_apps_routines_never_run_before_the_sweep,
       sweep_with_loans_dns_echo_of_both_apps, new_app_in_dead_apps_partition_is_not_the_old_owner,
       identical_relaunch_before_a_sweep_is_not_detected, classic_system6_records_no_owner,
       different_app_in_identical_block_is_told_apart, echo_without_notify_is_capped,
       timer_sweep_bailing_does_not_spin, frame_dropped_when_dead_owner_cannot_be_swept,
       readers_survive_concurrent_owner_drop, rewritten_pending_pb_is_never_filled,
       quit_inside_own_routine_keeps_network_alive, queued_pbs_of_gone_owner_not_drained,
       reused_block_unsticks_abandoned_routine, xapp_routine_during_other_apps_drain,
       routine_resuming_after_unstick_keeps_accounting, gate_held_while_abandoned_routines_process_quits,
       interrupt_time_routine_is_never_abandoned, tenant_of_unswept_block_keeps_its_objects,
       owner_table_full_and_system_heap_clients, sweep_waits_for_glue_frames, fuzz_apps)
