/* mfecho.c - MfEcho: a small TCP echo client for the MultiFinder per-application tests (Plan 3 Task 12).
 *
 * Copies of the application with different file names (MfEchoA, MfEchoB) run side by side under
 * MultiFinder. Each one writes "<name>.out" beside itself and reads "<name>.cfg" (tokens, any order):
 *   T<n>  run n seconds (default 60), then quit (ExitToShell)
 *   R     release the stream before quitting (default: quit with the stream live, so the driver's
 *         per-application cleanup must forget it)
 *   B<n>  testnet.py port base (echo at n+7; default 17000)
 *   U     also create a UDP stream (it stays live until the quit)
 *   P     quit with work pending (Task 12 fix round 1): the TCP stream (and the UDP stream with U) gets an
 *         ASR (notifyProc), and just before the quit the app queues an async TCPRcv (and UDPRead) with a
 *         completion routine and sends "LATE": testnet.py answers 3 s later (TCP: 4000 bytes, UDP: the
 *         datagram), i.e. after the quit. The driver must never call those routines or fill those PBs.
 * It connects to 172.17.0.1 (Snow's host) echo port, sends "<name> <seq>" every 2 s and checks the
 * echo; it logs the process low-memory globals (mf_os.c), the driver's goodbye counters and the
 * driver's TCP/UDP stream tables (through the private Status call) at start, every 10 s and at the end.
 * Last line: "RESULT PASS echoes N" when every echo came back, else "RESULT FAIL ...". */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mt_pb.h"
#include "mf_os.h"

#define HOST 0xAC110001u
#define P32(p) ((MacPtr32)(uintptr_t)(p))

static short ref;
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
);

static void tables(const char *tag)
{
    uint32_t gb = 0, gf = 0, dr = 0;
    MtDrv *d = (MtDrv *)os_drv_status(ref, &gb, &gf, &dr);
    uint8_t i;
    if (!d) { os_out("%s: no driver block\r", tag); return; }
    os_out("%s: goodbye %lu forced %lu dropped %lu owners %u gone %lu full %lu dead-calls %lu\r", tag,
           (unsigned long)gb, (unsigned long)gf, (unsigned long)dr, d->n_own, (unsigned long)d->n_own_gone,
           (unsigned long)d->n_own_full, (unsigned long)d->n_dead_calls);
    os_out("%s: swept call %lu timer %lu rx %lu dead-drops %lu\r", tag, (unsigned long)d->n_gone_call,
           (unsigned long)d->n_gone_timer, (unsigned long)d->n_gone_rx, (unsigned long)d->n_dead_drops);
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

int main(void)
{
    char cfg[64];
    int n, i, release = 0, udp = 0, quit = 0, pend = 0;
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
    }
    os_out("MfEcho '%s' T%ld B%ld R%d U%d P%d\r", name, secs, base, release, udp, pend);
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
                    tinit(TCPRcv, sp);
                    tpb.csParam.receive.commandTimeoutValue = 5;
                    tpb.csParam.receive.rcvBuff = P32(back + got);
                    tpb.csParam.receive.rcvBuffLen = (uint16_t)(len - got);
                    r = os_control_sync(&tpb);
                    if (r) break;
                    got += tpb.csParam.receive.rcvBuffLen;
                }
                if (r == 0 && memcmp(back, line, (size_t)len) == 0) ok++;
                else { bad++; os_out("t %lu seq %ld echo FAIL rcv %d\r", (unsigned long)(os_ticks() - t0), seq, r); }
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
        quit = os_yield(10);
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
    os_out_close();
    return 0;
}
