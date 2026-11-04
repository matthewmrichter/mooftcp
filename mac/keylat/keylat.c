/* keylat.c - KeyLat: keypress-to-wire latency probe (real Mac Classic, System 6, MoofTCP or MacTCP).
 *
 * A minimal interactive TCP client. It connects to scripts/keylatsrv.py on the host and, for every keyDown
 * event, sends ONE small pushed segment (async TCPSend) "KL1 K <seq> <when> <seen>\n":
 *   when  EventRecord.when of the key (the tick at which the keyboard event was posted)
 *   seen  TickCount() when the app got the event back from WaitNextEvent/GetNextEvent
 * The host timestamps the arrival on its own clock. The only clock on the Mac is TickCount (60.15 Hz, one tick
 * = 16.6 ms), so every Mac-side time is a whole number of ticks. To put Mac ticks and the host clock on one
 * time base the app also sends "KL1 S <seq> <tick>\n" sync pings, each sent the instant TickCount changes to
 * <tick> (busy-wait for the tick edge), so the ping leaves within a fraction of a millisecond of tick <tick>
 * starting; the host uses the best case of those pings as the baseline (scripts/keylatsrv.py, README in
 * docs/dev/HARDWARE.md "KeyLat"). No VIA timer or Time Manager is used: reading VIA1's T1 counter clears its
 * interrupt flag and would eat a system tick, and the extended Time Manager (Microseconds) is not on every
 * System 6 Mac, so the resolution is stated honestly instead.
 *
 * Per sent packet the app also logs to "KeyLat.out" (beside the app; CR lines):
 *   K seq when seen call ret done res ch      call = tick before PBControlAsync, ret = tick after it returned,
 *                                              done = tick the app noticed the TCPSend complete (= acked), res = ioResult
 *   S seq tick call ret done res
 * Everything runs in the app's main loop; no interrupt-time code; nothing is written to disk between the
 * keystroke and the send (the log is written when the app has been idle). Cmd-Q or the close box quits. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mt_mactcp.h"
#include "kl_os.h"

#define P32(p) ((MacPtr32)(uintptr_t)(p))
#define NSLOT 4
#define MAXREC 900
#define NSYNC_START 20
#define NSYNC_END 12
#define SYNC_GAP 12                 /* ticks between calibration pings */
#define IDLE_PING 180               /* ticks of silence before a drift-check ping */
#define KEY_QUIET 60                /* no ping within this many ticks after a key */

typedef struct Rec {
    uint32_t seq, a, b, call, ret, done;    /* K: a = when, b = seen;  S: a = tick */
    int16_t res;
    char kind, ch;
} Rec;

typedef struct Slot {
    TCPiopb pb;
    wdsEntry w[2];
    char buf[64];
    int busy, rec;
} Slot;

static Rec rec[MAXREC];
static int nrec, nflushed;
static Slot slot[NSLOT];
static short ref;
static StreamPtr stream;
static char rbuf[4096];
static TCPiopb cpb;
static int dead;                    /* the connection failed: no more sends */
static long nkeys, nsync, ndrop;
static uint32_t seq;

static void tinit(TCPiopb *pb, int16_t cs)
{
    memset(pb, 0, sizeof *pb);
    pb->ioCRefNum = ref;
    pb->csCode = cs;
    pb->tcpStream = stream;
}

static int free_slot(void)
{
    int i;
    for (i = 0; i < NSLOT; i++) if (!slot[i].busy) return i;
    return -1;
}

/* Queue the formatted text in slot s as one pushed segment. Returns the PB's immediate result. */
static short do_send(Slot *s, int n, uint32_t *call, uint32_t *ret)
{
    short r;
    tinit(&s->pb, TCPSend);
    s->w[0].length = (uint16_t)n; s->w[0].ptr = P32(s->buf);
    s->w[1].length = 0; s->w[1].ptr = 0;
    s->pb.csParam.send.wdsPtr = P32(s->w);
    s->pb.csParam.send.pushFlag = 1;
    *call = os_ticks();
    r = os_control_async(&s->pb);
    *ret = os_ticks();
    return r;
}

static void begin_rec(Slot *s, char kind, uint32_t a, uint32_t b, uint32_t call, uint32_t ret, short r, char ch)
{
    Rec *e;
    s->busy = 0; s->rec = -1;
    if (nrec >= MAXREC) return;
    e = &rec[nrec];
    e->kind = kind; e->seq = seq; e->a = a; e->b = b; e->call = call; e->ret = ret; e->done = 0;
    e->res = r; e->ch = ch;
    if (r == 0) { s->busy = 1; s->rec = nrec; }
    nrec++;
}

static void reap(void)
{
    int i;
    for (i = 0; i < NSLOT; i++) {
        Slot *s = &slot[i];
        if (s->busy && s->pb.ioResult != inProgress) {
            Rec *e = &rec[s->rec];
            e->done = os_ticks();
            e->res = s->pb.ioResult;
            if (e->res != 0 && !dead) { dead = 1; os_show(4, "Connection lost: the host closed or stopped answering. Cmd-Q quits."); }
            s->busy = 0;
        }
    }
}

static int send_key(const KlKey *k)
{
    int i = free_slot(), n;
    uint32_t call, ret;
    short r;
    if (dead) return -1;
    if (nrec >= MAXREC) { os_show(4, "Log full (900 packets): quit and start again."); return -1; }
    if (i < 0) { ndrop++; return -1; }
    seq++;
    n = sprintf(slot[i].buf, "KL1 K %lu %lu %lu\n", (unsigned long)seq, (unsigned long)k->when, (unsigned long)k->seen);
    r = do_send(&slot[i], n, &call, &ret);
    begin_rec(&slot[i], 'K', k->when, k->seen, call, ret, r, k->ch >= 32 && k->ch < 127 ? k->ch : '.');
    if (r) { dead = 1; os_show(4, "TCPSend failed."); }
    nkeys++;
    return nrec - 1;
}

/* A calibration ping sent right at a tick edge. Skipped (0) when the tick moved while formatting. */
static int send_sync(void)
{
    int i = free_slot(), n;
    uint32_t t, now, call, ret;
    short r;
    if (dead || i < 0 || nrec >= MAXREC) return 0;
    t = os_ticks();
    seq++;
    n = sprintf(slot[i].buf, "KL1 S %lu %lu\n", (unsigned long)seq, (unsigned long)(t + 1));
    while ((now = os_ticks()) == t) { }
    if (now != t + 1) { seq--; return 0; }
    r = do_send(&slot[i], n, &call, &ret);
    begin_rec(&slot[i], 'S', t + 1, 0, call, ret, r, 0);
    if (r) { dead = 1; os_show(4, "TCPSend failed."); }
    nsync++;
    return 1;
}

static void flush_log(void)
{
    char line[128];
    while (nflushed < nrec) {
        const Rec *e = &rec[nflushed];
        int n;
        if (e->kind == 'K')
            n = sprintf(line, "K %lu %lu %lu %lu %lu %lu %d %c\r", (unsigned long)e->seq, (unsigned long)e->a,
                        (unsigned long)e->b, (unsigned long)e->call, (unsigned long)e->ret, (unsigned long)e->done,
                        e->res, e->ch);
        else
            n = sprintf(line, "S %lu %lu %lu %lu %lu %d\r", (unsigned long)e->seq, (unsigned long)e->a,
                        (unsigned long)e->call, (unsigned long)e->ret, (unsigned long)e->done, e->res);
        if (e->kind == 'K' && e->done == 0 && e->res == 0) break;      /* not complete yet: next time */
        if (e->kind == 'S' && e->done == 0 && e->res == 0) break;
        os_out_write(line, n);
        nflushed++;
    }
}

static int parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    int part, d;
    for (part = 0; part < 4; part++) {
        long n = 0;
        d = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; d++; if (n > 255) return 0; }
        if (!d) return 0;
        v = (v << 8) | (uint32_t)n;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s && *s != ' ') return 0;
    *ip = v;
    return 1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
}

static void status(const char *state)
{
    char l[96];
    os_show(1, state);
    sprintf(l, "keys %ld  sync pings %ld  dropped %ld", nkeys, nsync, ndrop);
    os_show(2, l);
}

int main(void)
{
    char host[48] = "10.0.0.1", port[12] = "17061", cfg[96], l[240];
    uint32_t ip = 0, t_last = 0, t_key = 0, t_flush = 0;
    long pn;
    int n, i, ok = 0, ending = 0, endn = 0;
    short r;
    KlKey k;

    os_init();
    n = os_read_cfg(cfg, sizeof cfg - 1);
    if (n > 0) {
        char *p;
        cfg[n] = 0;
        p = strpbrk(cfg, " \t\r\n");
        if (p) {
            *p++ = 0;
            while (*p == ' ' || *p == '\t') p++;
            trim(cfg); trim(p);
            if (cfg[0] && p[0] && strlen(cfg) < sizeof host && strlen(p) < sizeof port) { strcpy(host, cfg); strcpy(port, p); }
        }
    }
    while (!ok) {
        if (!os_setup_dialog(host, sizeof host, port, sizeof port)) return 0;
        trim(host); trim(port);
        pn = strtol(port, 0, 10);
        if (parse_ip(host, &ip) && pn >= 1 && pn <= 65535) ok = 1;
        else os_alert("The host address must be four numbers like 10.0.0.1 and the port a number from 1 to 65535.");
    }
    n = sprintf(cfg, "%s %s\r", host, port);
    os_write_cfg(cfg, n);

    os_window();
    sprintf(l, "KeyLat 1.0   host %s port %s", host, port);
    os_show(0, l);
    os_show(3, "Tick = 16.6 ms: every Mac-side time is a whole number of ticks.");
    os_show(1, "Connecting ...");
    os_out_open();
    sprintf(l, "KeyLat 1 %s %s\r", host, port);
    os_out_write(l, (long)strlen(l));

    r = os_open_ipp(&ref);
    if (r) {
        sprintf(l, "The network driver .IPP could not be opened (error %d). Is MoofTCP or MacTCP installed and configured with an address?", r);
        os_alert(l);
        goto out;
    }
    tinit(&cpb, TCPCreate);
    cpb.csParam.create.rcvBuff = P32(rbuf);
    cpb.csParam.create.rcvBuffLen = sizeof rbuf;
    r = os_control_sync(&cpb);
    stream = cpb.tcpStream;
    if (r) {
        sprintf(l, "TCPCreate failed (error %d).", r);
        os_alert(l);
        goto out;
    }
    tinit(&cpb, TCPActiveOpen);
    cpb.csParam.open.remoteHost = ip;
    cpb.csParam.open.remotePort = (tcp_port)pn;
    cpb.csParam.open.validityFlags = (int8_t)(timeoutValue | timeoutAction);
    cpb.csParam.open.ulpTimeoutValue = 15;
    cpb.csParam.open.ulpTimeoutAction = 1;
    cpb.csParam.open.commandTimeoutValue = 15;
    r = os_control_sync(&cpb);
    if (r) {
        sprintf(l, "Could not connect to %s port %s (error %d). Is scripts/keylatsrv.py running on that host, and is the address right?", host, port, r);
        os_alert(l);
        sprintf(l, "connect failed %d\r", r);
        os_out_write(l, (long)strlen(l));
        goto release;
    }
    t_last = t_key = t_flush = os_ticks();
    status("Calibrating: do not type yet ...");
    for (;;) {
        uint32_t now;
        int ev = os_poll(&k);
        reap();
        now = os_ticks();
        if (ev == 2) {
            if (!ending && !dead) { ending = 1; endn = 0; os_show(1, "Finishing (sync pings) ..."); }
            else if (!ending) break;
        }
        if (ev == 1 && !ending) {
            int s = send_key(&k);
            t_key = t_last = os_ticks();
            if (s >= 0 && rec[s].res == 0) {
                const Rec *e = &rec[s];
                sprintf(l, "key %ld: age %ld, send call %ld ticks", nkeys, (long)(e->b - e->a), (long)(e->ret - e->call));
                os_show(5, l);
                status(nsync >= NSYNC_START ? "Ready: type keys. Cmd-Q quits." : "Calibrating ...");
            }
            continue;
        }
        if (os_event_pending() || dead) {
            if (ending && dead) break;
            continue;
        }
        if (ending) {                                   /* closing burst, then done */
            if (endn >= NSYNC_END) {
                int busy = 0;
                for (i = 0; i < NSLOT; i++) busy |= slot[i].busy;
                if (!busy || now - t_last > 240) break;
            } else if (now - t_last >= SYNC_GAP && send_sync()) { t_last = os_ticks(); endn++; }
            continue;
        }
        if (nsync < NSYNC_START) {
            if (now - t_last >= SYNC_GAP && now - t_key >= KEY_QUIET && send_sync()) {
                t_last = os_ticks();
                if (nsync == NSYNC_START) status("Ready: type keys. Cmd-Q quits.");
            }
        } else if (now - t_last >= IDLE_PING && now - t_key >= KEY_QUIET) {
            if (send_sync()) { t_last = os_ticks(); status("Ready: type keys. Cmd-Q quits."); }
        }
        if (now - t_key >= 90 && now - t_flush >= 90 && nflushed < nrec) { flush_log(); t_flush = os_ticks(); }
    }
    reap();
    if (!dead) {
        tinit(&cpb, TCPClose);
        cpb.csParam.close.ulpTimeoutValue = 5;
        cpb.csParam.close.validityFlags = (int8_t)timeoutValue;
        cpb.csParam.close.ulpTimeoutAction = 1;
        (void)os_control_sync(&cpb);
    }
release:
    if (stream) { tinit(&cpb, TCPRelease); (void)os_control_sync(&cpb); }
out:
    reap();
    flush_log();
    if (nflushed < nrec) {                              /* sends that never completed */
        for (i = nflushed; i < nrec; i++) rec[i].res = rec[i].res ? rec[i].res : (int16_t)inProgress;
        for (i = 0; i < NSLOT; i++) slot[i].busy = 0;
        flush_log();
    }
    sprintf(l, "END keys %ld sync %ld dropped %ld\r", nkeys, nsync, ndrop);
    os_out_write(l, (long)strlen(l));
    os_out_close();
    return 0;
}
