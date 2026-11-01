/* Plan 3 Task 12: per-application cleanup under MultiFinder.
 *
 * MultiFinder sends the driver no goodbye kiss when an application quits (measured in Snow,
 * docs/dev/EMULATOR.md "MultiFinder"), and BufPtr is global there, so ApplZone..BufPtr would cover
 * other partitions. The glue records each task-time caller's partition (mt_os_app_mem) and sweeps the
 * owners whose partition is gone (mt_os_app_alive) at every Control entry and from the timer. The fake
 * OS (tests/pbfake.h "MultiFinder model") gives each application a partition in fake memory with its own
 * header words, switches ApplZone / A5 between them and disposes of a partition without a goodbye. */
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
/* The timer tick of the driver (mt_drv.c tick_cb) after `ms`. */
static void tick(uint32_t ms) {
    pbf_now += ms;
    g_g.depth++;
    mt_entry_sweep(&g_e, 0);
    mt_entry_drain(&g_e);
    mt_pb_tick(g_x.d);
    g_g.depth--;
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
    c = pbf_mf_launch_at(blo, PART - 0x1000u);            /* same place, another size (header word) */
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
    d = pbf_mf_launch_at(blo + 0x100u, PART - 0x100u);
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
    msetup();
    b = launch();
    sb = est(&cb);
    T_CHECK(sb != 0);
    blo = pbf_parts[b].lo;
    pbf_mf_quit(b);
    pbf_mf_forget(b);
    c = pbf_mf_launch_at(blo, PART);
    g_compl[c] = pbf_mf_compl(c, compl_rec);
    g_cur = c;
    T_EQ(status(sb), 0);                                  /* still there (the known limit) */
    T_EQ(g_x.d->n_own_gone, 0);
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

/* ---- randomized: up to three applications launching, calling, quitting without a goodbye, a new one
 * reusing a dead one's memory, data for every connection (also the dead ones' before the sweep), ticks.
 * Invariants: no routine or PB of a quit partition is ever run (pbf_dead_calls), after a sweep tick no
 * glue object refers to a quit partition, every object of a live application is still there, and
 * nothing writes into a quit partition after its sweep (canary). FZ_SEED=n replays one seed,
 * FZ_SEEDS=n runs seeds 1..n (default 40). */
#define FA 3
#define FS 3
typedef struct {
    int part, live, swept;
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

/* The driver's input path (mt_drv.c rx_cb) checks the owners before each frame, once per clock step. */
static void rx(void) {
    g_g.depth++;
    if (g_x.d->n_own) mt_entry_sweep(&g_e, 0);
    g_g.depth--;
}

static unsigned long g_steps, g_gone, g_deadc, g_apps;
static void fuzz_one(uint32_t seed) {
    int step, i, j;
    msetup();
    g_rng = seed * 2654435761u + 1u;
    g_nf = 0;
    memset(F, 0, sizeof F);
    for (step = 0; step < 300 && !g_fail; step++) {
        int a = pick_live(), r = (int)rn(12);
        g_steps++;
        if (a < 0 && g_nf >= FMAX) break;
        if (a < 0 || (r == 0 && live_apps() < FA && g_nf < FMAX)) {   /* launch */
            int reuse = -1;
            for (i = 0; i < g_nf; i++) if (!F[i].live && F[i].part >= 0 && rn(2)) { reuse = i; break; }
            if (reuse >= 0) {
                MacPtr32 lo = pbf_parts[F[reuse].part].lo + (rn(2) ? 0 : 0x100u);
                uint32_t sz = pbf_parts[F[reuse].part].hi - lo;
                if (lo == pbf_parts[F[reuse].part].lo) sz -= 0x800u;   /* same start: another size (else the
                                                                           identical-block limit applies) */
                pbf_mf_forget(F[reuse].part);
                F[reuse].part = -1;                      /* its memory is the new one's now */
                a = g_nf++;
                memset(&F[a], 0, sizeof F[a]);
                F[a].part = pbf_mf_launch_at(lo, sz);
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
        switch (r) {
        case 1: case 2:
            if (!F[a].s[j]) F[a].s[j] = est(&F[a].c[j]);
            break;
        case 3:
            if (F[a].s[j]) (void)rcv(F[a].s[j], (uint16_t)(1 + rn(40)));
            break;
        case 4: {                                          /* data for any connection, dead ones too */
            int b = (int)rn((uint32_t)g_nf), k = (int)rn(FS);
            if (F[b].s[k]) {
                rx();
                pbt_peer(&g_x, F[b].c[k].lport, F[b].c[k].ps, F[b].c[k].ms, TF_ACK | TF_PSH, 8192, "datadata", 8);
                F[b].c[k].ps += 8;
            }
            break;
        }
        case 5:
            if (!F[a].u) ucreate(&F[a].u, &F[a].uport);
            else if (rn(2)) (void)uread(F[a].u);
            else { rx(); pbt_udp_in(&g_x, 9, F[a].uport, "u", 1); }
            break;
        case 6:
            (void)echo(T_PEER);
            if (rn(2)) {
                uint16_t id, sq;
                last_echo(&id, &sq);
                rx();
                pbt_echo_reply(&g_x, T_PEER, id, sq, NULL, 0);
            }
            break;
        case 7:
            if (F[a].s[j] && rn(3) == 0) {
                T_EQ(ecall(apb(TCPRelease, F[a].s[j]), 0), 0);
                F[a].s[j] = 0;
            }
            break;
        case 8:                                            /* quit: no goodbye under MultiFinder */
            pbf_mf_quit(F[a].part);
            F[a].live = 0;
            break;
        case 9:
            if (rn(6) == 0) {                              /* a goodbye kiss, then the quit */
                T_EQ(ecall(apb(-1, 0), 0), 0);
                pbf_mf_quit(F[a].part);
                F[a].live = 0;
            }
            break;
        default:
            tick(rn(300));
            break;
        }
        if (pbf_dead_calls) { fbad(seed, step, "a quit application's routine or PB was run"); return; }
        if (pbf_bad) { fbad(seed, step, "misuse seen by the fake OS"); return; }
        if (rn(3) == 0) {                                  /* a sweep tick, then the strict checks */
            tick(MT_PB_SWEEP_MS);
            for (i = 0; i < g_nf; i++) {
                if (F[i].live || F[i].part < 0) continue;
                if (refers(&pbf_parts[F[i].part])) { fbad(seed, step, "object of a quit partition kept"); return; }
                if (!F[i].swept) {                         /* canary: nothing writes here any more */
                    memset(pbf_p(pbf_parts[F[i].part].lo), 0xC5, pbf_parts[F[i].part].hi - pbf_parts[F[i].part].lo);
                    F[i].swept = 1;
                }
            }
        }
        for (i = 0; i < g_nf; i++) {
            if (F[i].live) {
                for (j = 0; j < FS; j++)
                    if (F[i].s[j] && !tcp_live(F[i].s[j])) { fbad(seed, step, "live app's TCP stream dropped"); return; }
                if (F[i].u && !udp_live(F[i].u)) { fbad(seed, step, "live app's UDP stream dropped"); return; }
            } else if (F[i].swept && F[i].part >= 0 && ((step & 15) == 0 || step == 299)) {
                static uint8_t ref[PART];
                if (!ref[0]) memset(ref, 0xC5, sizeof ref);
                if (memcmp(pbf_p(pbf_parts[F[i].part].lo), ref, pbf_parts[F[i].part].hi - pbf_parts[F[i].part].lo))
                    { fbad(seed, step, "write into a quit partition after its sweep"); return; }
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
        g_apps += (unsigned long)g_nf;
    }
    printf("fuzz_apps: %lu steps, %lu applications, %lu swept, %lu routines of quit applications skipped\n",
           g_steps, g_apps, g_gone, g_deadc);
    g_lax = 0;
    T_EQ(g_fail, 0);
}

T_MAIN(quit_one_app_keeps_other_apps_streams, goodbye_under_multifinder_spares_other_partitions,
       sweep_by_timer_when_nobody_calls, gone_apps_routines_never_run_before_the_sweep,
       sweep_with_loans_dns_echo_of_both_apps, new_app_in_dead_apps_partition_is_not_the_old_owner,
       identical_relaunch_before_a_sweep_is_not_detected, classic_system6_records_no_owner,
       owner_table_full_and_system_heap_clients, sweep_waits_for_glue_frames, fuzz_apps)
