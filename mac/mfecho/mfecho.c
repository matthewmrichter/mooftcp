/* mfecho.c - MfEcho: a small TCP echo client for the MultiFinder per-application tests (Plan 3 Task 12).
 *
 * Copies of the application with different file names (MfEchoA, MfEchoB) run side by side under
 * MultiFinder. Each one writes "<name>.out" beside itself and reads "<name>.cfg" (tokens, any order):
 *   T<n>  run n seconds (default 60), then quit (ExitToShell)
 *   R     release the stream before quitting (default: quit with the stream live, so the driver's
 *         per-application cleanup must forget it)
 *   B<n>  testnet.py port base (echo at n+7; default 17000)
 *   U     also create a UDP stream (it stays live until the quit)
 *   X     quit from INSIDE a completion routine (Task 12 round 3): the last call is an async TCPStatus whose
 *         completion routine (run inline by the driver at task time) calls ExitToShell, so the driver's
 *         frames for that routine never return
 *   P     quit with work pending (Task 12 fix round 1): the TCP stream (and the UDP stream with U) gets an
 *         ASR (notifyProc), and just before the quit the app queues an async TCPRcv (and UDPRead) with a
 *         completion routine and sends "LATE": testnet.py answers 3 s later (TCP: 4000 bytes, UDP: the
 *         datagram), i.e. after the quit. The driver must never call those routines or fill those PBs.
 *   Y     async reader (Task 12 round 4): every echo is read with an async TCPRcv whose completion routine
 *         re-issues an async TCPStatus (the ordinary async-client pattern), and each echo is followed by a
 *         1 s probe read that times out (its completion comes from the driver's timer tick). When the reply or
 *         the tick is drained from the OTHER copy's gate leave at task time, the routine runs with the other
 *         copy current (CurApName differs, SR mask 0): counted as "cross-app"; the re-issued call is then
 *         "another application calling at depth 1", which round 3 took for an abandoned routine.
 *   Z     like Y, and the first time the routine runs cross-app at task time it calls _ExitToShell: that
 *         ends the CURRENT process, i.e. the other copy, inside its own driver call; this copy must keep
 *         echoing (its own frames were never involved) and the driver must abandon the dead frames.
 *   S     spin: bursts of sync UDPWrites (512 bytes to the host's discard port; sync TCPStatus without U) every
 *         loop: the Ethernet driver delivers received frames from inside EWrite, i.e. inside this copy's
 *         calls, so the other copy's replies wait in the gate's ring and are drained at this copy's gate
 *         leave with this copy current (the partner of Y / Z)
 * It connects to 172.17.0.1 (Snow's host) echo port, sends "<name> <seq>" every 2 s and checks the
 * echo; it logs the process low-memory globals (mf_os.c), the driver's goodbye counters and the
 * driver's TCP/UDP stream tables (through the private Status call) at start, every 10 s and at the end.
 * Last line: "RESULT PASS echoes N" when every echo came back, else "RESULT FAIL ...". */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mt_pb.h"
#include "mf_os.h"
#pragma GCC diagnostic ignored "-Warray-bounds"   /* CurApName at 0x910 */

#define HOST 0xAC110001u
#define P32(p) ((MacPtr32)(uintptr_t)(p))

static char name[40];
static TCPiopb tpb;
static UDPiopb upb;
static char rbuf[8192];
static char ubuf[4096];
static char line[64], back[64];
static TCPiopb lrcv;                      /* option P: left pending at the quit */
static UDPiopb lurd, luw;
static char lbuf[1024];
static wdsEntry lw[2];
volatile long g_tasr_n, g_uasr_n, g_compl_n;
/* options Y / Z: the async read, its completion's counters and the call it re-issues */
static TCPiopb rpb, ypb;
static int yre, zkill;
static UDPiopb spb;                       /* option S: the burst's UDPWrite */
static wdsEntry sw[2];
static char sbuf[512];
volatile long g_rd_n, g_xapp, g_xapp_irq, g_xkill;
volatile int g_rd_done;

/* Option P's routines. The ASRs are Pascal (callee pops: TCP 16 bytes, UDP 14 bytes); the completion
 * routine is entered with A0 = PB and returns with RTS. The driver runs them with this app's A5. */
extern void tasr_stub(void);
extern void uasr_stub(void);
extern void compl_stub(void);
__asm__(
".text\n"
".align 2\n"
".globl tasr_stub\n"
"tasr_stub:\n"
"  addq.l #1,g_tasr_n\n"
"  move.l (%sp)+,%a0\n"
"  lea 16(%sp),%sp\n"
"  jmp (%a0)\n"
".globl uasr_stub\n"
"uasr_stub:\n"
"  addq.l #1,g_uasr_n\n"
"  move.l (%sp)+,%a0\n"
"  lea 14(%sp),%sp\n"
"  jmp (%a0)\n"
".globl compl_stub\n"
"compl_stub:\n"
"  addq.l #1,g_compl_n\n"
"  rts\n"
".globl exit_stub\n"
"exit_stub:\n"
"  .short 0xA9F4\n"                 /* _ExitToShell: never returns */
".globl rd_stub\n"                  /* options Y / Z: A0 = PB -> rd_done(pb); the driver saved D2-D7/A2-A6 */
"rd_stub:\n"
"  move.l %a0,-(%sp)\n"
"  jsr rd_done\n"
"  addq.l #4,%sp\n"
"  rts\n"
".globl cur_sr\n"
"cur_sr:\n"
"  move.w %sr,%d0\n"
"  rts\n"
);
extern void exit_stub(void);
extern void rd_stub(void);
extern unsigned short cur_sr(void);
static TCPiopb xpb;
static short ref;

/* The async read's completion (Y / Z), run by the driver with this app's A5. Cross-app = the current
 * application (CurApName) is not this one AND we run at task time (SR mask 0: the gate owner's drain,
 * not an interrupt-time delivery while the other copy happened to be interrupted). */
void rd_done(void *vpb)
{
    const unsigned char *cap = (const unsigned char *)0x910;
    int n = cap[0] < 31 ? cap[0] : 31, other, task;
    (void)vpb;
    other = n != (int)strlen(name) || memcmp(cap + 1, name, (size_t)n) != 0;
    task = (cur_sr() & 0x0700u) == 0;
    g_rd_n++;
    if (other && task) g_xapp++;
    else if (other) g_xapp_irq++;
    g_rd_done = 1;
    if (other && task && zkill && !g_xkill) {
        g_xkill++;
        exit_stub();                              /* ends the CURRENT process: the other copy */
    }
    if (yre) {                                    /* the re-issue: an async TCPStatus with a completion */
        memset(&ypb, 0, sizeof ypb);
        ypb.ioCRefNum = ref; ypb.csCode = TCPStatus; ypb.tcpStream = rpb.tcpStream;
        ypb.ioCompletion = P32(compl_stub);
        (void)os_control_async(&ypb);
    }
}

static void tables(const char *tag)
{
    uint32_t gb = 0, gf = 0, dr = 0;
    MtDrv *d = (MtDrv *)os_drv_status(ref, &gb, &gf, &dr);
    uint8_t i;
    if (!d) { os_out("%s: no driver block\r", tag); return; }
    os_out("%s: goodbye %lu forced %lu dropped %lu owners %u gone %lu full %lu dead-calls %lu\r", tag,
           (unsigned long)gb, (unsigned long)gf, (unsigned long)dr, d->n_own, (unsigned long)d->n_own_gone,
           (unsigned long)d->n_own_full, (unsigned long)d->n_dead_calls);
    os_out("%s: swept call %lu timer %lu rx %lu dead-drops %lu abandoned %lu postponed %lu depth %u\r", tag,
           (unsigned long)d->n_gone_call, (unsigned long)d->n_gone_timer, (unsigned long)d->n_gone_rx,
           (unsigned long)d->n_dead_drops, (unsigned long)d->n_unstuck, (unsigned long)d->n_unstick_wait, d->depth);
    if (yre || zkill)
        os_out("%s: async reads %ld cross-app %ld (at interrupt time %ld) xkill %ld re-issued completions %ld\r", tag,
               g_rd_n, g_xapp, g_xapp_irq, g_xkill, g_compl_n);
    for (i = 0; i < MT_PB_OWNERS; i++)
        if (d->own[i].lo)
            os_out("  owner[%u] %08lx-%08lx hdr %08lx\r", i, (unsigned long)d->own[i].lo,
                   (unsigned long)d->own[i].hi, (unsigned long)d->own[i].h0);
    for (i = 0; i < d->ntcp; i++) {
        const MtStream *s = &d->tcp[i];
        if (!s->used) continue;
        os_out("  tcp[%u] used %u handle %08lx rbuf %08lx a5 %08lx asr %08lx conn %u state %d\r", i, s->used,
               (unsigned long)s->handle, (unsigned long)s->rbuf, (unsigned long)s->a5, (unsigned long)s->asr,
               s->conn, s->t ? 1 : 0);
    }
    for (i = 0; i < d->nudp; i++) {
        const MtUdpStream *u = &d->udp[i];
        if (!u->used) continue;
        os_out("  udp[%u] used %u rbuf %08lx a5 %08lx\r", i, u->used, (unsigned long)u->rbuf, (unsigned long)u->a5);
    }
}

static void tinit(int16_t cs, StreamPtr sp)
{
    memset(&tpb, 0, sizeof tpb);
    tpb.ioCRefNum = ref;
    tpb.csCode = cs;
    tpb.tcpStream = sp;
}

/* One receive of up to n bytes into buf: sync TCPRcv, or (Y / Z) an async one waited for by yielding, so
 * the reply can arrive while the other copy is current. *got = bytes received. */
static short rcv_async(StreamPtr sp, char *buf, uint16_t n, int8_t tmo, uint16_t *got);
static short rcv_once(StreamPtr sp, char *buf, uint16_t n, uint16_t *got)
{
    short r;
    if (!yre && !zkill) {
        tinit(TCPRcv, sp);
        tpb.csParam.receive.commandTimeoutValue = 5;
        tpb.csParam.receive.rcvBuff = P32(buf);
        tpb.csParam.receive.rcvBuffLen = n;
        r = os_control_sync(&tpb);
        *got = tpb.csParam.receive.rcvBuffLen;
        return r;
    }
    return rcv_async(sp, buf, n, 5, got);
}
static short rcv_async(StreamPtr sp, char *buf, uint16_t n, int8_t tmo, uint16_t *got)
{
    short r;
    memset(&rpb, 0, sizeof rpb);
    rpb.ioCRefNum = ref; rpb.csCode = TCPRcv; rpb.tcpStream = sp;
    rpb.ioCompletion = P32(rd_stub);
    rpb.csParam.receive.commandTimeoutValue = tmo;
    rpb.csParam.receive.rcvBuff = P32(buf);
    rpb.csParam.receive.rcvBuffLen = n;
    g_rd_done = 0;
    r = os_control_async(&rpb);
    if (r) { *got = 0; return r; }
    {
        uint32_t t1 = os_ticks();
        while (!g_rd_done && os_ticks() - t1 < (uint32_t)(tmo + 2) * 60u) (void)os_yield(1);
    }
    if (!g_rd_done) { *got = 0; return -1; }
    *got = rpb.csParam.receive.rcvBuffLen;
    return rpb.ioResult;
}
static short rcv_probe(StreamPtr sp, uint16_t *got)
{
    static char one[2];
    return rcv_async(sp, one, 1, 1, got);
}

int main(void)
{
    char cfg[64];
    int n, i, release = 0, udp = 0, quit = 0, pend = 0, xquit = 0, spin = 0;
    long secs = 60, base = 17000;
    uint32_t t0, next, nextdump;
    StreamPtr sp = 0, us = 0;
    short r;
    long seq = 0, ok = 0, bad = 0;
    wdsEntry w[2];

    os_init();
    os_app_name(name, sizeof name);
    os_out_open();
    n = os_read_cfg(cfg, sizeof cfg - 1);
    cfg[n > 0 ? n : 0] = 0;
    for (i = 0; cfg[i]; i++) {
        if (cfg[i] == 'T') secs = strtol(cfg + i + 1, 0, 10);
        else if (cfg[i] == 'B') base = strtol(cfg + i + 1, 0, 10);
        else if (cfg[i] == 'R') release = 1;
        else if (cfg[i] == 'U') udp = 1;
        else if (cfg[i] == 'P') pend = 1;
        else if (cfg[i] == 'X') xquit = 1;
        else if (cfg[i] == 'Y') yre = 1;
        else if (cfg[i] == 'Z') zkill = 1;
        else if (cfg[i] == 'S') spin = 1;
    }
    os_out("MfEcho '%s' T%ld B%ld R%d U%d P%d X%d Y%d Z%d S%d\r", name, secs, base, release, udp, pend, xquit,
           yre, zkill, spin);
    os_dump_lowmem("start");
    r = os_open_ipp(&ref);
    os_out("OpenDriver %d ref %d rbuf %08lx pb %08lx\r", r, ref, (unsigned long)P32(rbuf), (unsigned long)P32(&tpb));
    tables("open");
    if (r) goto out;

    tinit(TCPCreate, 0);
    tpb.csParam.create.rcvBuff = P32(rbuf);
    tpb.csParam.create.rcvBuffLen = sizeof rbuf;
    if (pend) tpb.csParam.create.notifyProc = P32(tasr_stub);
    r = os_control_sync(&tpb);
    sp = tpb.tcpStream;
    os_out("TCPCreate %d stream %08lx\r", r, (unsigned long)sp);
    if (r) goto out;
    tinit(TCPActiveOpen, sp);
    tpb.csParam.open.remoteHost = HOST;
    tpb.csParam.open.remotePort = (tcp_port)(base + 7);
    tpb.csParam.open.validityFlags = (int8_t)(timeoutValue | timeoutAction);
    tpb.csParam.open.ulpTimeoutValue = 20;
    tpb.csParam.open.ulpTimeoutAction = 1;
    tpb.csParam.open.commandTimeoutValue = 20;
    r = os_control_sync(&tpb);
    os_out("TCPActiveOpen %d lport %u\r", r, tpb.csParam.open.localPort);
    if (r) goto out;
    if (udp) {
        memset(&upb, 0, sizeof upb);
        upb.ioCRefNum = ref;
        upb.csCode = UDPCreate;
        upb.csParam.create.rcvBuff = P32(ubuf);
        upb.csParam.create.rcvBuffLen = sizeof ubuf;
        if (pend) upb.csParam.create.notifyProc = P32(uasr_stub);
        r = os_control_sync(&upb);
        us = upb.udpStream;
        os_out("UDPCreate %d stream %08lx\r", r, (unsigned long)us);
    }
    tables("connected");

    t0 = os_ticks();
    next = t0;
    nextdump = t0 + 600;
    while (os_ticks() - t0 < (uint32_t)secs * 60u && !quit) {
        if ((int32_t)(os_ticks() - next) >= 0) {
            int len;
            next += 120;
            seq++;
            len = sprintf(line, "%s %ld\r\n", name, seq);
            w[0].length = (uint16_t)len; w[0].ptr = P32(line);
            w[1].length = 0; w[1].ptr = 0;
            tinit(TCPSend, sp);
            tpb.csParam.send.wdsPtr = P32(w);
            tpb.csParam.send.pushFlag = 1;
            r = os_control_sync(&tpb);
            if (r == 0) {
                int got = 0;
                while (got < len) {
                    uint16_t g = 0;
                    r = rcv_once(sp, back + got, (uint16_t)(len - got), &g);
                    if (r) break;
                    got += g;
                }
                if (r == 0 && memcmp(back, line, (size_t)len) == 0) ok++;
                else { bad++; os_out("t %lu seq %ld echo FAIL rcv %d\r", (unsigned long)(os_ticks() - t0), seq, r); }
                if ((yre || zkill) && r == 0) {
                    /* a probe read that times out after 1 s: its completion comes from the driver's timer tick,
                     * which runs from whichever gate leave drains a deferred tick, often the other copy's (in
                     * Snow frames never arrive while the gate is held, ticks do: MoofDiag "ticks ... deferred") */
                    uint16_t g = 0;
                    (void)rcv_probe(sp, &g);
                }
            } else {
                bad++;
                os_out("t %lu seq %ld send FAIL %d\r", (unsigned long)(os_ticks() - t0), seq, r);
            }
        }
        if ((int32_t)(os_ticks() - nextdump) >= 0) {
            char tag[48];
            nextdump += 600;
            sprintf(tag, "t%lu ok%ld bad%ld", (unsigned long)((os_ticks() - t0) / 60u), ok, bad);
            os_dump_lowmem(tag);
            tables(tag);
        }
        if (spin) {                               /* S: bursts of sends (UDPWrite to the discard port): received
                                                   * frames are delivered from inside EWrite, i.e. inside OUR
                                                   * call, and drained at its gate leave with us current */
            int k;
            for (k = 0; k < 40; k++) {
                if (us) {
                    sw[0].length = sizeof sbuf; sw[0].ptr = P32(sbuf); sw[1].length = 0; sw[1].ptr = 0;
                    memset(&spb, 0, sizeof spb);
                    spb.ioCRefNum = ref; spb.csCode = UDPWrite; spb.udpStream = us;
                    spb.csParam.send.remoteHost = HOST;
                    spb.csParam.send.remotePort = (udp_port)(base + 9);
                    spb.csParam.send.wdsPtr = P32(sw);
                    spb.csParam.send.checkSum = 1;
                    (void)os_control_sync(&spb);
                } else {
                    tinit(TCPStatus, sp); (void)os_control_sync(&tpb);
                }
            }
            quit = os_yield(1);
        } else {
            quit = os_yield(10);
        }
    }
    os_dump_lowmem("end");
    tables("end");
    if (pend && !release) {
        memset(&lrcv, 0, sizeof lrcv);                   /* async TCPRcv with a completion routine */
        lrcv.ioCRefNum = ref; lrcv.csCode = TCPRcv; lrcv.tcpStream = sp;
        lrcv.ioCompletion = P32(compl_stub);
        lrcv.csParam.receive.rcvBuff = P32(lbuf);
        lrcv.csParam.receive.rcvBuffLen = sizeof lbuf;
        r = os_control_async(&lrcv);
        os_out("pending TCPRcv %d ioResult %d\r", r, lrcv.ioResult);
        if (us) {
            memset(&lurd, 0, sizeof lurd);               /* async UDPRead, no timeout */
            lurd.ioCRefNum = ref; lurd.csCode = UDPRead; lurd.udpStream = us;
            lurd.ioCompletion = P32(compl_stub);
            r = os_control_async(&lurd);
            os_out("pending UDPRead %d ioResult %d\r", r, lurd.ioResult);
            lw[0].length = 4; lw[0].ptr = P32("LATE"); lw[1].length = 0; lw[1].ptr = 0;
            memset(&luw, 0, sizeof luw);
            luw.ioCRefNum = ref; luw.csCode = UDPWrite; luw.udpStream = us;
            luw.csParam.send.remoteHost = HOST;
            luw.csParam.send.remotePort = (udp_port)(base + 7);
            luw.csParam.send.wdsPtr = P32(lw);
            luw.csParam.send.checkSum = 1;
            r = os_control_sync(&luw);
            os_out("UDPWrite LATE %d\r", r);
        }
        lw[0].length = 6; lw[0].ptr = P32("LATE\r\n"); lw[1].length = 0; lw[1].ptr = 0;
        tinit(TCPSend, sp);
        tpb.csParam.send.wdsPtr = P32(lw);
        tpb.csParam.send.pushFlag = 1;
        r = os_control_sync(&tpb);
        os_out("TCPSend LATE %d; quitting with the reads pending (asr %ld/%ld compl %ld so far)\r", r,
               g_tasr_n, g_uasr_n, g_compl_n);
    }
    if (release) {
        tinit(TCPRelease, sp);
        r = os_control_sync(&tpb);
        os_out("TCPRelease %d\r", r);
        if (us) {
            memset(&upb, 0, sizeof upb);
            upb.ioCRefNum = ref; upb.csCode = UDPRelease; upb.udpStream = us;
            r = os_control_sync(&upb);
            os_out("UDPRelease %d\r", r);
        }
    }
out:
    os_out("RESULT %s echoes %ld bad %ld\r", (bad == 0 && ok > 0) ? "PASS" : "FAIL", ok, bad);
    if (xquit && sp) {
        os_out("quitting from inside a completion routine (ExitToShell)\r");
        os_out_close();
        memset(&xpb, 0, sizeof xpb);
        xpb.ioCRefNum = ref; xpb.csCode = TCPStatus; xpb.tcpStream = sp;
        xpb.ioCompletion = P32(exit_stub);
        (void)os_control_async(&xpb);                /* the driver runs exit_stub inline: no return */
        return 0;
    }
    os_out_close();
    return 0;
}
