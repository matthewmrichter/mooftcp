/* ApiProbe - runs a fixed list of MacTCP API cases against whichever stack answers to
 * ".IPP" and writes one text line per case to ApiProbe.out (default directory = the
 * application's folder).
 *
 * Line format (fields separated by one space; everything after '|' is the event log):
 *   <id> <name> <S|A> cs=<csCode> res=<ioResult> <key=value>... | <events>
 * S = synchronous call, A = asynchronous call (polling ioResult, ioCompletion set).
 * Values written "@N" are tick counts and are timing dependent; the determinism check
 * strips them. Events (in occurrence order, the order is a global sequence number):
 *   A:s<label>:e<event>:t<reason>:w<ioResult of the watched PB>@tick   TCP ASR
 *   U:s<label>:e<event>:w<res>@tick                                   UDP ASR
 *   C:k<slot>:r<ioResult at entry>:v<0 a0, 1 stack>@tick              ioCompletion routine
 *     (k-1 = the block is not one of ours: the ICMP echo notify proc gets MacTCP's own copy)
 *   P:k<slot>:r<ioResult>@tick                                        poll loop saw completion
 * Tick values in events are relative to the start of the case.
 *
 * Only mt_mactcp.h and probe_os.h are included: Toolbox headers live in probe_os.c.
 */
#include <stdint.h>
#include <string.h>
#include "mt_mactcp.h"
#include "probe_os.h"

#define P32(x) ((MacPtr32)(uintptr_t)(x))
#define TK(s)  ((long)(s) * 60)

#define HOST       0xAC110001UL   /* 172.17.0.1 : the Snow host, testnet.py */
#define GATEWAY    0x0A000001UL   /* 10.0.0.1 */
#define DEADLINK   0x0A630063UL   /* 10.99.0.99 : on-link (mask 255.0.0.0), nobody home */
#define DEADROUTED 0xC0000201UL   /* 192.0.2.1 : off-link, via the gateway */
#define P_ECHO     17007
#define P_DISCARD  17009
#define P_BANNER   17023
#define P_ONESHOT  17013
#define P_BURST    17019
#define P_RESET    17037
#define P_CLOSED   17001

/* ------------------------------------------------------------------ globals */

typedef union {
    TCPiopb t; UDPiopb u; ICMPParamBlock i; IPParamBlock p; GetAddrParamBlock g;
    uint8_t raw[112];
} Slot;
#define NSLOT 8
static Slot slots[NSLOT];
static Slot *volatile g_watch;

static short drv = -49;          /* .IPP refnum seen when the open succeeds; used if it fails */
static uint32_t myip, mymask;
static int configured;

static char rbuf[5][16384];      /* TCP/UDP receive buffers by label 1..4 */
static char big[40000];
static char tmpbuf[2048];
static wdsEntry wds[4][8];
static rdsEntry rds[8];

/* register snapshots filled by the asm stubs */
uint32_t g_c_a5, g_c_a0, g_c_d0, g_c_arg; uint16_t g_c_sr;
uint32_t g_a_a5, g_a_sp; uint16_t g_a_sr;
uint32_t g_u_a5; uint16_t g_u_sr;
uint32_t g_asr_target, g_uasr_target;
volatile int g_inpoll;

extern void compl_stub(void);
extern void asr_stub(void);
extern void uasr_stub(void);

__asm__(
".text\n"
".align 2\n"
".globl compl_stub\n"
"compl_stub:\n"
"  move.l %a5,g_c_a5\n"
"  move.l %a0,g_c_a0\n"
"  move.l %d0,g_c_d0\n"
"  move.w %sr,g_c_sr\n"
"  move.l 4(%sp),g_c_arg\n"
"  movem.l %d0-%d2/%a0-%a1,-(%sp)\n"
"  move.l 24(%sp),-(%sp)\n"
"  move.l %a0,-(%sp)\n"
"  jsr compl_c\n"
"  addq.l #8,%sp\n"
"  movem.l (%sp)+,%d0-%d2/%a0-%a1\n"
"  rts\n"
".globl asr_stub\n"
"asr_stub:\n"
"  move.l %a5,g_a_a5\n"
"  move.l %sp,g_a_sp\n"
"  move.w %sr,g_a_sr\n"
"  move.l g_asr_target,-(%sp)\n"
"  rts\n"
".globl uasr_stub\n"
"uasr_stub:\n"
"  move.l %a5,g_u_a5\n"
"  move.w %sr,g_u_sr\n"
"  move.l g_uasr_target,-(%sp)\n"
"  rts\n"
);

/* ------------------------------------------------------------------ events */

typedef struct { uint32_t seq, tick; char kind; int16_t a, b, c, d; } Ev;
#define NEV 100
static Ev evs[NEV];
static volatile int ev_n;
static volatile uint32_t ev_seq, ev_drop;
static uint32_t case_t0;

static inline uint16_t ints_off(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}
static inline void ints_restore(uint16_t sr)
{
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc");
}

static void ev_add(char kind, int a, int b, int c, int d)
{
    uint16_t sr = ints_off();
    if (ev_n < NEV) {
        Ev *e = &evs[ev_n];
        e->seq = ++ev_seq; e->tick = os_ticks() - case_t0; e->kind = kind;
        e->a = (int16_t)a; e->b = (int16_t)b; e->c = (int16_t)c; e->d = (int16_t)d;
        ev_n++;
    } else ev_drop++;
    ints_restore(sr);
}

static int slot_idx(const void *p)
{
    long d = (const char *)p - (const char *)slots;
    if (d < 0 || d >= (long)sizeof slots) return -1;
    return (int)(d / (long)sizeof(Slot));
}
static short pbres(const Slot *s) { return *(volatile const int16_t *)&s->raw[16]; }

int32_t g_n_res, g_n_out, g_n_in; uint16_t g_n_len; uint32_t g_n_calls;   /* ICMP notify snapshot */

void compl_c(void *a0, void *stk)
{
    int k = slot_idx(a0), via = 0;
    if (k < 0) {
        k = slot_idx(stk); via = 1;
        if (k < 0 && a0 && !((uintptr_t)a0 & 1)) {   /* ICMP notify: MacTCP's own block */
            const uint8_t *q = (const uint8_t *)a0;
            g_n_res = *(const int16_t *)(q + 16);
            g_n_out = (int32_t)*(const uint32_t *)(q + 50);
            g_n_in = (int32_t)*(const uint32_t *)(q + 54);
            g_n_len = *(const uint16_t *)(q + 58);
            g_n_calls++;
        }
    }
    ev_add('C', k, k >= 0 ? pbres(&slots[k]) : 0, via, 0);
}

static pascal void asr_c(uint32_t stream, uint16_t evt, uint32_t ud, uint16_t term, const ICMPReport *icmp)
{
    int t = term;
    if (evt == TCPICMPReceived && icmp) t = 1000 + icmp->reportType;
    (void)stream;
    ev_add('A', (int)ud, evt, t, g_watch ? pbres(g_watch) : 99);
}

static pascal void uasr_c(uint32_t stream, uint16_t evt, uint32_t ud, const ICMPReport *icmp)
{
    (void)stream; (void)icmp;
    ev_add('U', (int)ud, evt, 0, g_watch ? pbres(g_watch) : 99);
}


/* ------------------------------------------------------------------ output */

typedef struct { char s[900]; int n; } B;
static B ex, hd, evb;
static B *cb = &ex;

static void ap(const char *s) { while (*s && cb->n < (int)sizeof cb->s - 1) cb->s[cb->n++] = *s++; cb->s[cb->n] = 0; }
static void apd(long v)
{
    char t[14], o[16]; int n = 0, k = 0; unsigned long u;
    if (v < 0) { ap("-"); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) o[k++] = t[--n];
    o[k] = 0; ap(o);
}
static void apx(unsigned long v)
{
    static const char hx[] = "0123456789abcdef"; char o[12]; int i;
    for (i = 0; i < 8; i++) o[i] = hx[(v >> (28 - 4 * i)) & 15];
    o[8] = 0; ap(o);
}
static void kv(const char *k, long v) { ap(" "); ap(k); ap("="); apd(v); }
static void kt(const char *k, long v) { ap(" "); ap(k); ap("=@"); apd(v); }
static void kh(const char *k, unsigned long v) { ap(" "); ap(k); ap("=0x"); apx(v); }
static void ks(const char *k, const char *v) { ap(" "); ap(k); ap("="); ap(v); }
static void kip(const char *k, uint32_t ip)
{
    ap(" "); ap(k); ap("=");
    apd((ip >> 24) & 255); ap("."); apd((ip >> 16) & 255); ap("."); apd((ip >> 8) & 255); ap("."); apd(ip & 255);
}

static char cur_id[24], cur_name[32]; static int cur_async;
static int line_count;

static void BEGIN(const char *id, const char *name, int async)
{
    strncpy(cur_id, id, sizeof cur_id - 1); cur_id[sizeof cur_id - 1] = 0;
    strncpy(cur_name, name, sizeof cur_name - 1); cur_name[sizeof cur_name - 1] = 0;
    cur_async = async; ex.n = 0; ex.s[0] = 0;
    ev_n = 0; ev_drop = 0; case_t0 = os_ticks(); g_watch = 0;
}

static void emit(int cs, long res)
{
    int i;
    cb = &hd; hd.n = 0; hd.s[0] = 0;
    ap(cur_id); ap(" "); ap(cur_name); ap(cur_async ? " A" : " S");
    kv("cs", cs); kv("res", res);
    cb = &evb; evb.n = 0; evb.s[0] = 0;
    for (i = 0; i < ev_n; i++) {
        const Ev *e = &evs[i];
        ap(" "); { char c[2]; c[0] = e->kind; c[1] = 0; ap(c); }
        switch (e->kind) {
        case 'A': ap(":s"); apd(e->a); ap(":e"); apd(e->b); ap(":t"); apd(e->c); ap(":w"); apd(e->d); break;
        case 'U': ap(":s"); apd(e->a); ap(":e"); apd(e->b); ap(":w"); apd(e->d); break;
        case 'C': ap(":k"); apd(e->a); ap(":r"); apd(e->b); ap(":v"); apd(e->c); break;
        default:  ap(":k"); apd(e->a); ap(":r"); apd(e->b); break;
        }
        ap("@"); apd(e->tick);
    }
    if (ev_drop) { ap(" dropped="); apd(ev_drop); }
    cb = &ex;
    os_out_write(hd.s, hd.n);
    os_out_write(ex.s, ex.n);
    os_out_write(" |", 2);
    os_out_write(evb.s, evb.n);
    os_out_write("\r", 1);
    os_out_flush();
    line_count++;
}

static void note(const char *id, const char *text)
{
    os_out_write("# ", 2); os_out_write(id, (long)strlen(id)); os_out_write(" ", 1);
    os_out_write(text, (long)strlen(text)); os_out_write("\r", 1);
    os_out_flush();
}

/* ------------------------------------------------------------------ calls */

static long last_imm, last_dt;
#define DEFWAIT TK(25)

static void pb_init(Slot *s, short cs)
{
    memset(s, 0, sizeof *s);
    s->t.ioCRefNum = drv; s->t.csCode = cs; s->t.ioResult = 1;
}

/* issue the call; for sync it completes here. returns immediate OSErr */
static short issue(Slot *s, int async)
{
    short e;
    s->t.ioResult = 1;
    if (async) { s->t.ioCompletion = P32(compl_stub); e = os_control_async(s); }
    else { s->t.ioCompletion = 0; e = os_control_sync(s); }
    return e;
}
static short waitdone(Slot *s, long tmo)
{
    long t0 = (long)os_ticks();
    g_inpoll = 1;
    while (pbres(s) == 1 && (long)os_ticks() - t0 < tmo) { }
    g_inpoll = 0;
    if (pbres(s) != 1) ev_add('P', slot_idx(s), pbres(s), 0, 0);
    return pbres(s);
}
static short run(Slot *s, int async, long tmo)
{
    long t0 = (long)os_ticks(); short r;
    last_imm = issue(s, async);
    if (async) r = waitdone(s, tmo); else r = pbres(s);
    last_dt = (long)os_ticks() - t0;
    return r;
}
static void delay(long ticks)
{
    long t0 = (long)os_ticks();
    while ((long)os_ticks() - t0 < ticks) { }
}

/* TCP helpers: slot 7 = sync helper PB, slot 6 = status helper */
static StreamPtr mk(int label, long len, short *res)
{
    Slot *s = &slots[7]; short r;
    pb_init(s, TCPCreate);
    s->t.csParam.create.rcvBuff = P32(rbuf[label]);
    s->t.csParam.create.rcvBuffLen = (uint32_t)len;
    s->t.csParam.create.notifyProc = P32(asr_stub);
    s->t.csParam.create.userDataPtr = (MacPtr32)label;
    r = run(s, 0, DEFWAIT);
    if (res) *res = r;
    return r == 0 ? s->t.tcpStream : 0;
}
static short tcall(short cs, StreamPtr sp)       /* close/abort/release style, sync */
{
    Slot *s = &slots[7];
    pb_init(s, cs); s->t.tcpStream = sp;
    return run(s, 0, DEFWAIT);
}
static short rel(StreamPtr sp) { return tcall(TCPRelease, sp); }

static short tstat(StreamPtr sp, TCPStatusPB *out)
{
    Slot *s = &slots[6]; short r;
    pb_init(s, TCPStatus); s->t.tcpStream = sp;
    r = run(s, 0, DEFWAIT);
    *out = s->t.csParam.status;
    return r;
}
static void pst(const char *tag, StreamPtr sp)
{
    TCPStatusPB st; short r = tstat(sp, &st);
    char k[24]; int n = (int)strlen(tag);
    memcpy(k, tag, (size_t)n);
    #define KK(sfx) (k[n] = 0, strcat(k, sfx), k)
    kv(KK(".res"), r);
    if (r == 0) {
        kv(KK(".state"), st.connectionState);
        kv(KK(".lport"), st.localPort); kv(KK(".rport"), st.remotePort);
        kv(KK(".sndw"), st.sendWindow); kv(KK(".rcvw"), st.rcvWindow);
        kv(KK(".unack"), st.amtUnackedData); kv(KK(".unread"), st.amtUnreadData);
        kv(KK(".mss"), (long)st.sendMaxSegSize);
    }
    #undef KK
}

static short aopen(Slot *s, StreamPtr sp, uint32_t host, uint16_t port, uint16_t lport, int tmo_s, int async, long wait)
{
    TCPOpenPB *o;
    pb_init(s, TCPActiveOpen); s->t.tcpStream = sp;
    o = &s->t.csParam.open;
    o->remoteHost = host; o->remotePort = port; o->localPort = lport;
    if (tmo_s) {   /* ULP timeout (abort action) bounds the connect; commandTimeoutValue is also set */
        o->validityFlags = (int8_t)(timeoutValue | timeoutAction);
        o->ulpTimeoutValue = (int8_t)tmo_s; o->ulpTimeoutAction = 1;
        o->commandTimeoutValue = (int8_t)tmo_s;
    }
    return run(s, async, wait);
}
static short popen_(Slot *s, StreamPtr sp, uint16_t lport, int tmo_s, int async, long wait)
{
    TCPOpenPB *o;
    pb_init(s, TCPPassiveOpen); s->t.tcpStream = sp;
    o = &s->t.csParam.open;
    o->localPort = lport;
    if (tmo_s) { o->validityFlags = (int8_t)timeoutValue; o->commandTimeoutValue = (int8_t)tmo_s; }
    return run(s, async, wait);
}
static short rcv_issue(Slot *s, StreamPtr sp, char *buf, int len, int tmo_s, int async)
{
    TCPReceivePB *r;
    pb_init(s, TCPRcv); s->t.tcpStream = sp;
    r = &s->t.csParam.receive;
    r->commandTimeoutValue = (int8_t)tmo_s; r->rcvBuff = P32(buf); r->rcvBuffLen = (uint16_t)len;
    return issue(s, async);
}
static short rcv(Slot *s, StreamPtr sp, char *buf, int len, int tmo_s, int async)
{
    long t0 = (long)os_ticks(); short r;
    last_imm = rcv_issue(s, sp, buf, len, tmo_s, async);
    r = async ? waitdone(s, TK(tmo_s ? tmo_s + 10 : 30)) : pbres(s);
    last_dt = (long)os_ticks() - t0;
    return r;
}
static int rcv_len(const Slot *s) { return s->t.csParam.receive.rcvBuffLen; }
static short snd_issue(Slot *s, StreamPtr sp, wdsEntry *w, int push, int async)
{
    pb_init(s, TCPSend); s->t.tcpStream = sp;
    s->t.csParam.send.wdsPtr = P32(w); s->t.csParam.send.pushFlag = (uint8_t)push;
    return issue(s, async);
}
static short snd(Slot *s, StreamPtr sp, wdsEntry *w, int push, int async)
{
    long t0 = (long)os_ticks(); short r;
    last_imm = snd_issue(s, sp, w, push, async);
    r = async ? waitdone(s, TK(30)) : pbres(s);
    last_dt = (long)os_ticks() - t0;
    return r;
}
/* one-entry wds helper */
static wdsEntry *w1(int k, const void *p, int len)
{
    wds[k][0].length = (uint16_t)len; wds[k][0].ptr = P32(p);
    wds[k][1].length = 0; wds[k][1].ptr = 0;
    return wds[k];
}

/* connect helper: returns open result; fills stream via *sp */
static short connect_to(int label, uint32_t host, uint16_t port, StreamPtr *sp, int async)
{
    short r; Slot *s = &slots[0];
    int tries;
    *sp = mk(label, 4096, &r);
    if (!*sp) return r;
    /* Snow's NAT can swallow a SYN whose 4-tuple matches a still-open host socket from an
     * earlier case (it never forwards our FIN/RST); retry with a fresh local port. */
    for (tries = 0; tries < 3; tries++) {
        r = aopen(s, *sp, host, port, 0, 10, async, TK(20));
        if (r == 0) break;
        if (pbres(s) == 1) tcall(TCPAbort, *sp);
    }
    if (tries) { kv("connectTries", tries + 1); }
    return r;
}

static void hexn(const char *k, const uint8_t *p, int n)   /* first bytes as hex */
{
    static const char hx[] = "0123456789abcdef"; char o[40]; int i, m = n > 12 ? 12 : n;
    for (i = 0; i < m; i++) { o[2 * i] = hx[p[i] >> 4]; o[2 * i + 1] = hx[p[i] & 15]; }
    o[2 * m] = 0; ks(k, o);
}

/* ================================================================== cases */

/* -- 100 driver / address -- */
static void c_getaddr(int async)
{
    Slot *s = &slots[0]; short r;
    BEGIN("110", "getmyip", async);
    pb_init(s, ipctlGetAddr);
    r = run(s, async, DEFWAIT);
    kip("addr", s->g.ourAddress); kip("mask", (uint32_t)s->g.ourNetMask);
    kv("imm", last_imm);
    emit(ipctlGetAddr, r);
}

static void c_lapstats(int async)
{
    static LAPStats lbuf;
    Slot *s = &slots[0]; short r; LAPStats *l;
    BEGIN("160", "lapstats", async);
    pb_init(s, ipctlLAPStats);
    memset(&lbuf, 0, sizeof lbuf);
    s->p.csParam.LAPStatsPB.lapStatsPtr = P32(&lbuf);   /* output block: ours, or the driver replaces it */
    r = run(s, async, DEFWAIT);
    l = (LAPStats *)(uintptr_t)s->p.csParam.LAPStatsPB.lapStatsPtr;
    kv("ptrIsOurs", l == &lbuf);
    if (r == 0 && l) {
        kv("ifType", l->ifType); kv("maxMTU", l->ifMaxMTU); kv("speed", l->ifSpeed);
        kv("phyLen", l->ifPhyAddrLength);
        if (l->ifPhysicalAddress) hexn("mac", (const uint8_t *)(uintptr_t)l->ifPhysicalAddress, 6);
        kv("slot", l->slotNumber);
        kv("arpTab", l->AddrXlation != 0);
    }
    emit(ipctlLAPStats, r);
}

static void c_globalinfo(int async)
{
    Slot *s = &slots[0]; short r; static TCPParam tp; static TCPStats ts; static StreamPtr tbl[40];
    const TCPParam *pp; const TCPStats *ps; int i, nz = 0;
    BEGIN("800", "tcp_globalinfo", async);
    pb_init(s, TCPGlobalInfo);
    s->t.csParam.globalInfo.tcpParamPtr = P32(&tp);
    s->t.csParam.globalInfo.tcpStatsPtr = P32(&ts);
    s->t.csParam.globalInfo.tcpCDBTable[0] = P32(tbl);
    r = run(s, async, DEFWAIT);
    kv("paramPtrIsOurs", s->t.csParam.globalInfo.tcpParamPtr == P32(&tp));
    kv("statsPtrIsOurs", s->t.csParam.globalInfo.tcpStatsPtr == P32(&ts));
    kv("cdbIsOurs", s->t.csParam.globalInfo.tcpCDBTable[0] == P32(tbl));
    kv("maxConn", s->t.csParam.globalInfo.maxTCPConnections);
    pp = (const TCPParam *)(uintptr_t)s->t.csParam.globalInfo.tcpParamPtr;
    ps = (const TCPStats *)(uintptr_t)s->t.csParam.globalInfo.tcpStatsPtr;
    if (r == 0 && pp) {
        kv("rtoA", (long)pp->tcpRtoA); kv("rtoMin", (long)pp->tcpRtoMin); kv("rtoMax", (long)pp->tcpRtoMax);
        kv("mss", (long)pp->tcpMaxSegSize); kv("maxConnP", (long)pp->tcpMaxConn); kv("maxWin", (long)pp->tcpMaxWindow);
    }
    if (r == 0 && ps) {
        kv("connAttempts", (long)ps->tcpConnAttempts); kv("connOpened", (long)ps->tcpConnOpened);
        kv("connAccepted", (long)ps->tcpConnAccepted); kv("connClosed", (long)ps->tcpConnClosed);
        kv("connAborted", (long)ps->tcpConnAborted); kv("octIn", (long)ps->tcpOctetsIn); kv("octOut", (long)ps->tcpOctetsOut);
        kv("inPkts", (long)ps->tcpInputPkts); kv("outPkts", (long)ps->tcpOutputPkts);
    }
    for (i = 0; i < 40; i++) if (tbl[i]) nz++;
    kv("ourTblNonzero", nz);
    emit(TCPGlobalInfo, r);
}

/* -- 200 TCPCreate -- */
static void c_create_sizes(int async)
{
    static const long sizes[] = { 0, 1, 64, 128, 256, 300, 512, 1023, 1024, 1025, 2048, 2500, 3000, 3500, 4095, 4096, 8192, 16384 };
    int i; char id[24];
    for (i = 0; i < (int)(sizeof sizes / sizeof sizes[0]); i++) {
        Slot *s = &slots[0]; short r, rr = 0; StreamPtr sp = 0;
        strcpy(id, "200."); { B *sv = cb; cb = &hd; hd.n = 0; apd(sizes[i]); cb = sv; strcat(id, hd.s); }
        BEGIN(id, "create_size", async);
        pb_init(s, TCPCreate);
        s->t.csParam.create.rcvBuff = P32(rbuf[1]);
        s->t.csParam.create.rcvBuffLen = (uint32_t)sizes[i];
        s->t.csParam.create.notifyProc = P32(asr_stub);
        s->t.csParam.create.userDataPtr = 1;
        r = run(s, async, DEFWAIT);
        kv("len", sizes[i]);
        if (r == 0) { sp = s->t.tcpStream; kv("streamNZ", sp != 0); }
        if (sp) rr = rel(sp);
        kv("rel", rr);
        emit(TCPCreate, r);
    }
}

static void c_create_null(int async)
{
    Slot *s = &slots[0]; short r, rr = 0; StreamPtr sp = 0;
    BEGIN("201", "create_nullbuf", async);
    pb_init(s, TCPCreate);
    s->t.csParam.create.rcvBuff = 0; s->t.csParam.create.rcvBuffLen = 4096;
    r = run(s, async, DEFWAIT);
    if (r == 0) sp = s->t.tcpStream;
    if (sp) rr = rel(sp);
    kv("rel", rr); emit(TCPCreate, r);
}

static void c_create_odd(int async)
{
    Slot *s = &slots[0]; short r, rr = 0; StreamPtr sp = 0;
    BEGIN("202", "create_oddbuf", async);
    pb_init(s, TCPCreate);
    s->t.csParam.create.rcvBuff = P32(rbuf[1] + 1); s->t.csParam.create.rcvBuffLen = 4096;
    r = run(s, async, DEFWAIT);
    sp = 0; rr = 0; if (r == 0) sp = s->t.tcpStream;
    if (sp) rr = rel(sp);
    kv("rel", rr); emit(TCPCreate, r);

    BEGIN("202b", "create_nonotify", async);
    pb_init(s, TCPCreate);
    s->t.csParam.create.rcvBuff = P32(rbuf[1]); s->t.csParam.create.rcvBuffLen = 4096;
    r = run(s, async, DEFWAIT);
    sp = 0; rr = 0; if (r == 0) sp = s->t.tcpStream;
    if (sp) rr = rel(sp);
    kv("rel", rr); emit(TCPCreate, r);
}

static void c_release_twice(int async)
{
    Slot *s = &slots[0]; short r, r2, r3, c; StreamPtr sp;
    BEGIN("203", "release_twice", async);
    sp = mk(1, 4096, &c); kv("create", c);
    pb_init(s, TCPRelease); s->t.tcpStream = sp;
    r = run(s, async, DEFWAIT);
    pb_init(s, TCPRelease); s->t.tcpStream = sp;
    r2 = run(s, async, DEFWAIT); kv("second", r2);
    pb_init(s, TCPRelease); s->t.tcpStream = 0;
    r3 = run(s, async, DEFWAIT); kv("stream0", r3);
    emit(TCPRelease, r);
}

/* -- 300 TCPActiveOpen -- */
static void c_aopen_banner(int async)
{
    Slot *s = &slots[0]; short r, c; StreamPtr sp; long t0;
    BEGIN("300", "aopen_banner", async);
    sp = mk(1, 4096, &c); kv("create", c); kh("streamAligned", (unsigned long)(sp & 3));
    g_watch = s;
    t0 = (long)os_ticks();
    r = aopen(s, sp, HOST, P_BANNER, 0, 10, async, TK(20));
    kt("dt", (long)os_ticks() - t0); kv("imm", last_imm);
    kip("lhost", s->t.csParam.open.localHost); kv("lport", s->t.csParam.open.localPort);
    kip("rhost", s->t.csParam.open.remoteHost); kv("rport", s->t.csParam.open.remotePort);
    pst("st", sp);
    g_watch = 0;
    { short rr = rcv(&slots[1], sp, tmpbuf, 100, 5, async);
      kv("rcv", rr); kv("n", rcv_len(&slots[1])); kt("rcvdt", last_dt);
      kv("mark", slots[1].t.csParam.receive.markFlag); kv("urg", slots[1].t.csParam.receive.urgentFlag);
      kv("secTS", slots[1].t.csParam.receive.secondTimeStamp); }
    kv("close", tcall(TCPClose, sp));
    pst("afterClose", sp);
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);
}

static void c_aopen_fail(int async)
{
    Slot *s = &slots[0]; short r, c; StreamPtr sp; long t0;
    BEGIN("301", "aopen_refused", async);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    r = aopen(s, sp, HOST, P_CLOSED, 0, 10, async, TK(20));
    kt("dt", (long)os_ticks() - t0); pst("st", sp); kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("302", "aopen_deadhost_onlink", async);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    r = aopen(s, sp, DEADLINK, 23, 0, 8, async, TK(30));
    kt("dt", (long)os_ticks() - t0); pst("st", sp);
    if (pbres(s) == 1) kv("abort", tcall(TCPAbort, sp));
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("302c", "aopen_deadhost_cmdtimeout_only", 1);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    pb_init(s, TCPActiveOpen); s->t.tcpStream = sp;
    s->t.csParam.open.remoteHost = DEADLINK; s->t.csParam.open.remotePort = 23;
    s->t.csParam.open.validityFlags = (int8_t)timeoutValue; s->t.csParam.open.commandTimeoutValue = 5;
    kv("imm", issue(s, 1));
    r = waitdone(s, TK(20));
    kt("dt", (long)os_ticks() - t0);
    if (pbres(s) == 1) kv("abort", tcall(TCPAbort, sp));
    delay(20); kv("afterAbort", pbres(s));
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("303", "aopen_deadhost_routed", async);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    r = aopen(s, sp, DEADROUTED, 23, 0, 8, async, TK(30));
    kt("dt", (long)os_ticks() - t0); pst("st", sp);
    if (pbres(s) == 1) kv("abort", tcall(TCPAbort, sp));
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("304", "aopen_nullhost", async);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    r = aopen(s, sp, 0, P_BANNER, 0, 8, async, TK(20));
    kt("dt", (long)os_ticks() - t0);
    if (pbres(s) == 1) kv("abort", tcall(TCPAbort, sp));
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("304b", "aopen_nullport", async);
    sp = mk(1, 4096, &c); t0 = (long)os_ticks();
    r = aopen(s, sp, HOST, 0, 0, 8, async, TK(20));
    kt("dt", (long)os_ticks() - t0);
    if (pbres(s) == 1) kv("abort", tcall(TCPAbort, sp));
    kv("rel", rel(sp));
    emit(TCPActiveOpen, r);

    BEGIN("304c", "aopen_unconfigured_stream0", async);
    r = aopen(s, 0, HOST, P_BANNER, 0, 8, async, TK(20));
    emit(TCPActiveOpen, r);
}

static void c_aopen_twice(int async)
{
    Slot *s = &slots[0], *s2 = &slots[1]; short r, r2, c; StreamPtr sp;
    BEGIN("305", "aopen_twice", async);
    sp = mk(1, 4096, &c);
    r = aopen(s, sp, HOST, P_ECHO, 0, 10, async, TK(20));
    r2 = aopen(s2, sp, HOST, P_ECHO, 0, 10, async, TK(20));
    kv("second", r2); kv("secondImm", last_imm);
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPActiveOpen, r);
}

static void c_aopen_localport(int async)
{
    Slot *s = &slots[0], *s2 = &slots[1]; short r, r2, c; StreamPtr a, b;
    BEGIN("306", "aopen_localport", async);
    a = mk(1, 4096, &c); b = mk(2, 4096, &c);
    r = aopen(s, a, HOST, P_ECHO, (uint16_t)(33333 + async), 10, async, TK(20));
    kv("lport", s->t.csParam.open.localPort);
    r2 = aopen(s2, b, HOST, P_ECHO, (uint16_t)(33333 + async), 10, async, TK(20));
    kv("sameQuad", r2);
    if (r2 == 0) kv("closeB", tcall(TCPClose, b));
    kv("closeA", tcall(TCPClose, a));
    kv("relA", rel(a)); kv("relB", rel(b));
    emit(TCPActiveOpen, r);
}

/* -- 400 TCPSend -- */
static void c_send_echo(int async)
{
    Slot *s = &slots[0], *rs = &slots[1]; short r, rr; StreamPtr sp;
    BEGIN("400", "send_push_pendingrcv", async);
    r = connect_to(1, HOST, P_ECHO, &sp, async); kv("open", r);
    memcpy(tmpbuf, "hello", 5);
    last_imm = rcv_issue(rs, sp, rbuf[2], 100, 10, 1);   /* always async: pending */
    g_watch = rs;
    delay(10);
    r = snd(s, sp, w1(0, tmpbuf, 5), 1, async);
    kt("sendDt", last_dt);
    pst("atSendDone", sp);
    rr = waitdone(rs, TK(10));
    kv("rcv", rr); kv("n", rcv_len(rs)); kt("rcvDt", last_dt);
    g_watch = 0;
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPSend, r);
}

static void c_send_nopush(int async)
{
    Slot *s = &slots[0]; short r; StreamPtr sp; long t0; int i, seen = -1;
    BEGIN("401", "send_nopush", async);
    r = connect_to(1, HOST, P_ECHO, &sp, async); kv("open", r);
    memcpy(tmpbuf, "abcde", 5);
    t0 = (long)os_ticks();
    r = snd(s, sp, w1(0, tmpbuf, 5), 0, async);
    kt("sendDt", last_dt);
    pst("atSendDone", sp);
    for (i = 0; i < 360 && seen < 0; i++) {   /* up to 6 s for the echo */
        int k; for (k = 0; k < ev_n; k++) if (evs[k].kind == 'A' && evs[k].b == TCPDataArrival) seen = (int)evs[k].tick;
        if (seen < 0) delay(1);
    }
    kt("echoArrivalTick", seen); kt("total", (long)os_ticks() - t0);
    pst("later", sp);
    { short rr = rcv(&slots[1], sp, rbuf[2], 100, 3, async); kv("rcv", rr); kv("n", rcv_len(&slots[1])); }
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPSend, r);
}

static void c_send_variants(int async)
{
    Slot *s = &slots[0]; short r; StreamPtr sp; int n;
    BEGIN("402", "send_zero_len_entry", async);
    r = connect_to(1, HOST, P_ECHO, &sp, async); kv("open", r);
    r = snd(s, sp, w1(0, tmpbuf, 0), 1, async);
    delay(30); pst("st", sp);
    emit(TCPSend, r);

    BEGIN("402b", "send_terminator_only", async);
    wds[0][0].length = 0; wds[0][0].ptr = 0;
    r = snd(s, sp, wds[0], 1, async);
    delay(30); pst("st", sp);
    emit(TCPSend, r);

    BEGIN("403", "send_odd_address", async);
    memcpy(tmpbuf + 1, "oddbyte", 7);
    r = snd(s, sp, w1(0, tmpbuf + 1, 7), 1, async);
    delay(60);
    { short rr = rcv(&slots[1], sp, rbuf[2], 100, 2, async); n = rcv_len(&slots[1]); kv("rcv", rr); kv("n", n); }
    hexn("data", (const uint8_t *)rbuf[2], n);
    emit(TCPSend, r);

    BEGIN("404", "send_multi_entry", async);
    memcpy(tmpbuf, "ab", 2); memcpy(tmpbuf + 10, "cde", 3); memcpy(tmpbuf + 20, "fghi", 4);
    wds[0][0].length = 2; wds[0][0].ptr = P32(tmpbuf);
    wds[0][1].length = 3; wds[0][1].ptr = P32(tmpbuf + 10);
    wds[0][2].length = 4; wds[0][2].ptr = P32(tmpbuf + 20);
    wds[0][3].length = 0; wds[0][3].ptr = 0;
    r = snd(s, sp, wds[0], 1, async);
    delay(60);
    { short rr = rcv(&slots[1], sp, rbuf[2], 100, 2, async); n = rcv_len(&slots[1]); kv("rcv", rr); kv("n", n); }
    hexn("data", (const uint8_t *)rbuf[2], n);
    emit(TCPSend, r);

    BEGIN("404b", "send_sendLength_field", async);
    memcpy(tmpbuf, "0123456789", 10);
    pb_init(s, TCPSend); s->t.tcpStream = sp;
    s->t.csParam.send.wdsPtr = P32(w1(0, tmpbuf, 10)); s->t.csParam.send.pushFlag = 1;
    s->t.csParam.send.sendLength = 3;
    r = run(s, async, DEFWAIT);
    kv("sendLengthOut", s->t.csParam.send.sendLength); kv("sendFreeOut", (long)s->t.csParam.send.sendFree);
    delay(60);
    { short rr = rcv(&slots[1], sp, rbuf[2], 100, 2, async); n = rcv_len(&slots[1]); kv("rcv", rr); kv("n", n); }
    emit(TCPSend, r);

    BEGIN("408", "send_after_close", async);
    kv("close", tcall(TCPClose, sp));
    r = snd(s, sp, w1(0, tmpbuf, 3), 1, async);
    pst("st", sp);
    kv("rel", rel(sp));
    emit(TCPSend, r);
}

static void c_send_unconnected(int async)
{
    Slot *s = &slots[0]; short r, c; StreamPtr sp;
    BEGIN("405", "send_before_open", async);
    sp = mk(1, 4096, &c);
    r = snd(s, sp, w1(0, tmpbuf, 5), 1, async);
    kv("rel", rel(sp));
    emit(TCPSend, r);
}

static void c_send_large(int async)
{
    Slot *s = &slots[0]; short r; StreamPtr sp; long t0;
    BEGIN("407", "send_large_40000", async);
    r = connect_to(1, HOST, P_DISCARD, &sp, async); kv("open", r);
    wds[0][0].length = 20000; wds[0][0].ptr = P32(big);
    wds[0][1].length = 20000; wds[0][1].ptr = P32(big + 20000);
    wds[0][2].length = 0; wds[0][2].ptr = 0;
    t0 = (long)os_ticks();
    r = snd(s, sp, wds[0], 1, async);
    kt("sendDt", (long)os_ticks() - t0);
    pst("atSendDone", sp);
    delay(120); pst("later", sp);
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPSend, r);
}

static void c_send_double(int async)
{
    Slot *a = &slots[0], *b = &slots[1]; short ra, rb; StreamPtr sp; long t0; int i;
    (void)async;
    BEGIN("406", "send_two_pending", 1);
    kv("open", connect_to(1, HOST, P_DISCARD, &sp, 1));
    wds[0][0].length = 20000; wds[0][0].ptr = P32(big); wds[0][1].length = 20000; wds[0][1].ptr = P32(big + 20000);
    wds[0][2].length = 0; wds[0][2].ptr = 0;
    wds[1][0].length = 20000; wds[1][0].ptr = P32(big); wds[1][1].length = 20000; wds[1][1].ptr = P32(big + 20000);
    wds[1][2].length = 0; wds[1][2].ptr = 0;
    t0 = (long)os_ticks();
    kv("immA", snd_issue(a, sp, wds[0], 1, 1));
    kv("immB", snd_issue(b, sp, wds[1], 1, 1));
    kv("resA_afterIssue", pbres(a)); kv("resB_afterIssue", pbres(b));
    for (i = 0; i < TK(30) && (pbres(a) == 1 || pbres(b) == 1); i++) delay(1);
    ra = pbres(a); rb = pbres(b);
    kt("both", (long)os_ticks() - t0);
    kv("resB", rb);
    pst("st", sp);
    if (pbres(a) == 1 || pbres(b) == 1) kv("abort", tcall(TCPAbort, sp));
    delay(30);
    kv("finalA", pbres(a)); kv("finalB", pbres(b));
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPSend, ra);
}

/* -- 500 TCPRcv -- */
static void c_rcv_cases(int async)
{
    Slot *rs = &slots[1]; short r; StreamPtr sp;
    BEGIN("500", "rcv_partial_and_timeout", async);
    r = connect_to(1, HOST, P_BANNER, &sp, async); kv("open", r);
    delay(60);
    r = rcv(rs, sp, rbuf[2], 10, 5, async); kv("rcv10", r); kv("n1", rcv_len(rs));
    kv("push?", rs->t.csParam.receive.markFlag);
    r = rcv(rs, sp, rbuf[2], 100, 5, async); kv("rcvRest", r); kv("n2", rcv_len(rs));
    r = rcv(rs, sp, rbuf[2], 100, 2, async); kv("rcvNone", r); kt("noneDt", last_dt);
    emit(TCPRcv, r);

    BEGIN("501", "rcv_zero_length", async);
    r = rcv(rs, sp, rbuf[2], 0, 2, async); kv("n", rcv_len(rs));
    emit(TCPRcv, r);

    BEGIN("501b", "rcv_notimeout_forever_abort", async);
    last_imm = rcv_issue(rs, sp, rbuf[2], 100, 0, 1);
    delay(TK(3)); kv("stillPending", pbres(rs) == 1);
    kv("abort", tcall(TCPAbort, sp));
    r = waitdone(rs, TK(5)); kv("afterAbort", r);
    kv("rel", rel(sp));
    emit(TCPRcv, r);
}

static void c_rcv_unconnected(int async)
{
    Slot *rs = &slots[1]; short r, c; StreamPtr sp;
    BEGIN("503", "rcv_before_open", async);
    sp = mk(1, 4096, &c);
    r = rcv(rs, sp, rbuf[2], 100, 2, async);
    kv("rel", rel(sp));
    emit(TCPRcv, r);
}

static void c_rcv_double(int async)
{
    Slot *r1 = &slots[1], *r2 = &slots[2], *s = &slots[0]; short ra, rb; StreamPtr sp;
    (void)async;
    BEGIN("502", "rcv_two_pending", 1);
    kv("open", connect_to(1, HOST, P_ECHO, &sp, 1));
    kv("imm1", rcv_issue(r1, sp, rbuf[2], 100, 10, 1));
    kv("imm2", rcv_issue(r2, sp, rbuf[3], 100, 10, 1));
    delay(30);
    kv("res1_early", pbres(r1)); kv("res2_early", pbres(r2));
    memcpy(tmpbuf, "ab", 2);
    snd(s, sp, w1(0, tmpbuf, 2), 1, 1);
    delay(TK(2));
    kv("res1", pbres(r1)); kv("n1", rcv_len(r1)); kv("res2", pbres(r2)); kv("n2", rcv_len(r2));
    memcpy(tmpbuf, "cd", 2);
    snd(s, sp, w1(0, tmpbuf, 2), 1, 1);
    delay(TK(2));
    kv("res1b", pbres(r1)); kv("res2b", pbres(r2)); kv("n2b", rcv_len(r2));
    ra = pbres(r1); rb = pbres(r2); (void)rb;
    if (pbres(r1) == 1 || pbres(r2) == 1) kv("abort", tcall(TCPAbort, sp));
    delay(30);
    kv("rel", rel(sp));
    emit(TCPRcv, ra);
}

static void c_nocopy(int async)
{
    Slot *rs = &slots[1], *bs = &slots[2]; short r, rr; StreamPtr sp; int i;
    BEGIN("504", "nocopy_rcv_and_return", async);
    kv("open", connect_to(1, HOST, P_BANNER, &sp, async));
    delay(60);
    memset(rds, 0, sizeof rds);
    pb_init(rs, TCPNoCopyRcv); rs->t.tcpStream = sp;
    rs->t.csParam.receive.commandTimeoutValue = 5;
    rs->t.csParam.receive.rdsPtr = P32(rds); rs->t.csParam.receive.rdsLength = 6;
    r = run(rs, async, TK(10));
    kv("rdsLengthOut", rs->t.csParam.receive.rdsLength);
    for (i = 0; i < 3 && (i == 0 || rds[i - 1].length); i++) {
        kv("len", rds[i].length);
        if (!rds[i].length) break;
        kv("inRbuf", rds[i].ptr >= P32(rbuf[1]) && rds[i].ptr < P32(rbuf[1]) + 4096);
        kv("off", (long)(rds[i].ptr - P32(rbuf[1])));
    }
    pb_init(bs, TCPRcvBfrReturn); bs->t.tcpStream = sp;
    bs->t.csParam.receive.rdsPtr = P32(rds);
    rr = run(bs, async, DEFWAIT); kv("return", rr);
    pb_init(bs, TCPRcvBfrReturn); bs->t.tcpStream = sp;
    bs->t.csParam.receive.rdsPtr = P32(rds);
    rr = run(bs, async, DEFWAIT); kv("returnAgain", rr);
    rds[0].length = 5; rds[0].ptr = P32(rbuf[1] + 3); rds[1].length = 0; rds[1].ptr = 0;
    pb_init(bs, TCPRcvBfrReturn); bs->t.tcpStream = sp;
    bs->t.csParam.receive.rdsPtr = P32(rds);
    rr = run(bs, async, DEFWAIT); kv("returnBogus", rr);
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPNoCopyRcv, r);
}

/* -- 600 close / abort / release -- */
static void c_close_pending(int async)
{
    Slot *rs = &slots[1], *cs = &slots[2]; short r; StreamPtr sp;
    BEGIN("600", "close_with_pending_rcv", async);
    kv("open", connect_to(1, HOST, P_ECHO, &sp, async));
    last_imm = rcv_issue(rs, sp, rbuf[2], 100, 10, 1);
    g_watch = rs;
    delay(20);
    pb_init(cs, TCPClose); cs->t.tcpStream = sp;
    r = run(cs, async, TK(15));
    kt("closeDt", last_dt);
    kv("rcvRes", pbres(rs)); kv("rcvN", rcv_len(rs));
    delay(TK(2)); kv("rcvResLater", pbres(rs));
    pst("st", sp);
    g_watch = 0;
    if (pbres(rs) == 1) tcall(TCPAbort, sp);
    delay(20);
    kv("rel", rel(sp));
    emit(TCPClose, r);
}

static void c_close_idle(int async)
{
    Slot *cs = &slots[0]; short r, r2, c; StreamPtr sp;
    BEGIN("602", "close_unconnected", async);
    sp = mk(1, 4096, &c);
    pb_init(cs, TCPClose); cs->t.tcpStream = sp;
    r = run(cs, async, DEFWAIT);
    pb_init(cs, TCPClose); cs->t.tcpStream = sp;
    r2 = run(cs, async, DEFWAIT); kv("second", r2);
    pst("st", sp);
    kv("abort", tcall(TCPAbort, sp));
    kv("rel", rel(sp));
    emit(TCPClose, r);
}

static void c_remote_close(int async)
{
    Slot *rs = &slots[1]; short r; StreamPtr sp;
    BEGIN("601", "remote_close_oneshot", async);
    kv("open", connect_to(1, HOST, P_ONESHOT, &sp, async));
    g_watch = rs;
    delay(TK(3));
    pst("afterPeerClose", sp);
    r = rcv(rs, sp, rbuf[2], 100, 3, async); kv("rcv1", r); kv("n1", rcv_len(rs));
    r = rcv(rs, sp, rbuf[2], 100, 3, async); kv("rcv2", r); kv("n2", rcv_len(rs));
    pst("afterRcv", sp);
    g_watch = 0;
    r = tcall(TCPClose, sp); kv("close", r);
    pst("afterClose", sp);
    kv("rel", rel(sp));
    emit(TCPClose, r);
}

static void c_abort(int async)
{
    Slot *rs = &slots[1], *as = &slots[2]; short r, c; StreamPtr sp;
    BEGIN("603", "abort_connected_pending_rcv", async);
    kv("open", connect_to(1, HOST, P_ECHO, &sp, async));
    last_imm = rcv_issue(rs, sp, rbuf[2], 100, 10, 1);
    g_watch = rs;
    delay(20);
    pb_init(as, TCPAbort); as->t.tcpStream = sp;
    r = run(as, async, DEFWAIT);
    kv("rcvRes", pbres(rs)); delay(30); kv("rcvResLater", pbres(rs));
    pst("st", sp);
    g_watch = 0;
    kv("rel", rel(sp));
    emit(TCPAbort, r);

    BEGIN("604", "abort_unconnected", async);
    sp = mk(1, 4096, &c);
    pb_init(as, TCPAbort); as->t.tcpStream = sp;
    r = run(as, async, DEFWAIT);
    pb_init(as, TCPAbort); as->t.tcpStream = 0;
    kv("stream0", run(as, async, DEFWAIT));
    kv("rel", rel(sp));
    emit(TCPAbort, r);
}

static void c_release_connected(int async)
{
    Slot *rs = &slots[1]; short r; StreamPtr sp; TCPStatusPB st;
    BEGIN("605", "release_connected_pending_rcv", async);
    kv("open", connect_to(1, HOST, P_ECHO, &sp, async));
    last_imm = rcv_issue(rs, sp, rbuf[2], 100, 10, 1);
    delay(20);
    r = rel(sp);
    delay(TK(2));
    kv("rcvRes", pbres(rs));
    kv("statusAfter", tstat(sp, &st));
    emit(TCPRelease, r);
}

static void c_remote_rst(int async)
{
    Slot *rs = &slots[1], *s = &slots[0]; short r; StreamPtr sp;
    BEGIN("606", "remote_reset", async);
    kv("open", connect_to(1, HOST, P_RESET, &sp, async));
    g_watch = rs;
    delay(TK(3));
    pst("afterRst", sp);
    r = rcv(rs, sp, rbuf[2], 100, 3, async); kv("rcv1", r); kv("n1", rcv_len(rs));
    r = rcv(rs, sp, rbuf[2], 100, 3, async); kv("rcv2", r);
    kv("send", snd(s, sp, w1(0, tmpbuf, 3), 1, async));
    g_watch = 0;
    kv("close", tcall(TCPClose, sp));
    kv("rel", rel(sp));
    emit(TCPRcv, r);
}

static void c_burst(int async)
{
    Slot *rs = &slots[1]; short r = 0; StreamPtr sp; long total = 0; int calls = 0;
    BEGIN("608", "burst_then_close", async);
    kv("open", connect_to(1, HOST, P_BURST, &sp, async));
    g_watch = rs;
    delay(TK(2));
    while (calls < 30) {
        r = rcv(rs, sp, rbuf[2], 1024, 3, async);
        calls++;
        if (r != 0) break;
        total += rcv_len(rs);
    }
    kv("calls", calls); kv("total", total); kv("lastRes", r);
    pst("st", sp);
    g_watch = 0;
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPRcv, r);
}

/* -- 700 TCPPassiveOpen -- */
static void c_passive(int async)
{
    Slot *ps = &slots[0], *p2 = &slots[1]; short r, c; StreamPtr a, b; long t0;
    BEGIN("700", "popen_port0_timeout", async);
    a = mk(1, 4096, &c);
    g_watch = ps;
    last_imm = 0; t0 = (long)os_ticks();
    if (async) {
        pb_init(ps, TCPPassiveOpen); ps->t.tcpStream = a;
        ps->t.csParam.open.localPort = 0; ps->t.csParam.open.validityFlags = (int8_t)timeoutValue;
        ps->t.csParam.open.commandTimeoutValue = 4;
        kv("imm", issue(ps, 1));
        delay(30);
        pst("pending", a);
        r = waitdone(ps, TK(10));
    } else r = popen_(ps, a, 0, 4, 0, TK(10));
    kt("dt", (long)os_ticks() - t0);
    kv("lport", ps->t.csParam.open.localPort); kip("lhost", ps->t.csParam.open.localHost);
    kip("rhost", ps->t.csParam.open.remoteHost);
    pst("after", a); g_watch = 0;
    kv("rel", rel(a));
    emit(TCPPassiveOpen, r);

    BEGIN("701", "popen_port_in_use", 1);
    a = mk(1, 4096, &c); b = mk(2, 4096, &c);
    pb_init(ps, TCPPassiveOpen); ps->t.tcpStream = a;
    ps->t.csParam.open.localPort = 17777; ps->t.csParam.open.validityFlags = (int8_t)timeoutValue;
    ps->t.csParam.open.commandTimeoutValue = 6;
    kv("immA", issue(ps, 1));
    delay(20);
    r = popen_(p2, b, 17777, 3, 0, TK(10));
    kv("secondPassive", r);
    pst("bState", b);
    kv("activeFromSamePort", aopen(p2, b, HOST, async ? P_DISCARD : P_ECHO, 17777, 5, 0, TK(10)));
    if (pbres(p2) == 0) tcall(TCPAbort, b);
    kv("abortA", tcall(TCPAbort, a));
    delay(20); kv("aResAfterAbort", pbres(ps));
    kv("relA", rel(a)); kv("relB", rel(b));
    emit(TCPPassiveOpen, r);

    BEGIN("702", "popen_abort_pending", 1);
    a = mk(1, 4096, &c);
    pb_init(ps, TCPPassiveOpen); ps->t.tcpStream = a;
    ps->t.csParam.open.localPort = 17778;
    kv("imm", issue(ps, 1));
    delay(20);
    pst("pending", a);
    r = tcall(TCPAbort, a);
    delay(20); kv("openRes", pbres(ps));
    pst("after", a);
    kv("rel", rel(a));
    emit(TCPAbort, r);

    BEGIN("703", "connect_to_self", 1);
    a = mk(1, 4096, &c); b = mk(2, 4096, &c);
    pb_init(ps, TCPPassiveOpen); ps->t.tcpStream = a;
    ps->t.csParam.open.localPort = 17779; ps->t.csParam.open.validityFlags = (int8_t)timeoutValue;
    ps->t.csParam.open.commandTimeoutValue = 8;
    kv("immP", issue(ps, 1));
    delay(20);
    r = aopen(p2, b, myip, 17779, 0, 6, 1, TK(15));
    kv("passiveRes", pbres(ps));
    pst("a", a); pst("b", b);
    tcall(TCPAbort, b); tcall(TCPAbort, a); delay(20);
    kv("relA", rel(a)); kv("relB", rel(b));
    emit(TCPActiveOpen, r);
}

/* -- 900 UDP -- */
static StreamPtr ucreate(Slot *s, int label, long len, uint16_t port, short *res, int async)
{
    short r;
    pb_init(s, UDPCreate);
    s->u.csParam.create.rcvBuff = P32(rbuf[label]); s->u.csParam.create.rcvBuffLen = (uint32_t)len;
    s->u.csParam.create.notifyProc = P32(uasr_stub);
    s->u.csParam.create.localPort = port; s->u.csParam.create.userDataPtr = (MacPtr32)label;
    r = run(s, async, DEFWAIT);
    if (res) *res = r;
    return r == 0 ? s->u.udpStream : 0;
}
static short urelease(StreamPtr sp)
{
    Slot *s = &slots[7]; pb_init(s, UDPRelease); s->u.udpStream = sp;
    return run(s, 0, DEFWAIT);
}

static void c_udp_create(int async)
{
    static const long sizes[] = { 0, 1, 255, 256, 511, 512, 575, 576, 1023, 1024, 1500, 1600, 1800, 2000, 2047, 2048, 4096 };
    Slot *s = &slots[0]; int i; char id[24]; short r, c; StreamPtr sp, sp2;
    for (i = 0; i < (int)(sizeof sizes / sizeof sizes[0]); i++) {
        strcpy(id, "900."); { B *sv = cb; cb = &hd; hd.n = 0; apd(sizes[i]); cb = sv; strcat(id, hd.s); }
        BEGIN(id, "udp_create_size", async);
        sp = ucreate(s, 1, sizes[i], 0, &r, async);
        kv("len", sizes[i]); kv("lport", s->u.csParam.create.localPort);
        kv("rel", sp ? urelease(sp) : 0);
        emit(UDPCreate, r);
    }
    BEGIN("901", "udp_create_port0_twice", async);
    sp = ucreate(s, 1, 2048, 0, &r, async); kv("lport1", s->u.csParam.create.localPort);
    sp2 = ucreate(&slots[1], 2, 2048, 0, &c, async); kv("create2", c); kv("lport2", slots[1].u.csParam.create.localPort);
    kv("rel1", sp ? urelease(sp) : 0); kv("rel2", sp2 ? urelease(sp2) : 0);
    emit(UDPCreate, r);

    BEGIN("901b", "udp_create_same_port", async);
    sp = ucreate(s, 1, 2048, 7777, &r, async); kv("lport1", s->u.csParam.create.localPort);
    sp2 = ucreate(&slots[1], 2, 2048, 7777, &c, async); kv("create2", c);
    kv("rel1", sp ? urelease(sp) : 0); kv("rel2", sp2 ? urelease(sp2) : 0);
    emit(UDPCreate, r);

    BEGIN("901c", "udp_create_endingPort", async);
    pb_init(s, UDPCreate);
    s->u.csParam.create.rcvBuff = P32(rbuf[1]); s->u.csParam.create.rcvBuffLen = 2048;
    s->u.csParam.create.localPort = 7780; s->u.csParam.create.endingPort = 7790;
    r = run(s, async, DEFWAIT); sp = r == 0 ? s->u.udpStream : 0;
    kv("lport", s->u.csParam.create.localPort); kv("ending", s->u.csParam.create.endingPort);
    kv("rel", sp ? urelease(sp) : 0);
    emit(UDPCreate, r);

    BEGIN("902", "udp_release_twice", async);
    sp = ucreate(s, 1, 2048, 0, &c, async);
    pb_init(s, UDPRelease); s->u.udpStream = sp; r = run(s, async, DEFWAIT);
    pb_init(s, UDPRelease); s->u.udpStream = sp; kv("second", run(s, async, DEFWAIT));
    pb_init(s, UDPRelease); s->u.udpStream = 0; kv("stream0", run(s, async, DEFWAIT));
    emit(UDPRelease, r);
}

static short uwrite(Slot *s, StreamPtr sp, uint32_t host, uint16_t port, wdsEntry *w, int async, int cksum)
{
    pb_init(s, UDPWrite); s->u.udpStream = sp;
    s->u.csParam.send.remoteHost = host; s->u.csParam.send.remotePort = port;
    s->u.csParam.send.wdsPtr = P32(w); s->u.csParam.send.checkSum = (uint8_t)cksum;
    return run(s, async, DEFWAIT);
}
static short uread(Slot *s, StreamPtr sp, int tmo, int async, int issue_only)
{
    pb_init(s, UDPRead); s->u.udpStream = sp;
    s->u.csParam.receive.timeOut = (uint16_t)tmo;
    if (issue_only) { last_imm = issue(s, 1); return 1; }
    return run(s, async, TK(tmo ? tmo + 10 : 30));
}

static void c_udp_io(int async)
{
    Slot *s = &slots[0], *rs = &slots[1]; short r, c; StreamPtr sp; int n;
    BEGIN("903", "udp_echo_pending_read", async);
    sp = ucreate(s, 1, 2048, 0, &c, async); kv("create", c); kv("lport", s->u.csParam.create.localPort);
    g_watch = rs;
    uread(rs, sp, 10, 1, 1); kv("immRead", last_imm);
    delay(10);
    memcpy(tmpbuf, "udp-hello", 9);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, tmpbuf, 9), async, 1);
    kt("writeDt", last_dt);
    waitdone(rs, TK(8));
    kv("readRes", pbres(rs)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    kip("from", rs->u.csParam.receive.remoteHost); kv("fromPort", rs->u.csParam.receive.remotePort);
    kv("inRbuf", rs->u.csParam.receive.rcvBuff >= P32(rbuf[1]) && rs->u.csParam.receive.rcvBuff < P32(rbuf[1]) + 2048);
    g_watch = 0;
    { Slot *bs = &slots[2]; pb_init(bs, UDPBfrReturn); bs->u.udpStream = sp; bs->u.csParam.receive.rcvBuff = rs->u.csParam.receive.rcvBuff;
      kv("bfrReturn", run(bs, async, DEFWAIT));
      pb_init(bs, UDPBfrReturn); bs->u.udpStream = sp; bs->u.csParam.receive.rcvBuff = rs->u.csParam.receive.rcvBuff;
      kv("bfrReturnAgain", run(bs, async, DEFWAIT));
      pb_init(bs, UDPBfrReturn); bs->u.udpStream = sp; bs->u.csParam.receive.rcvBuff = P32(rbuf[1] + 7);
      kv("bfrReturnBogus", run(bs, async, DEFWAIT)); }
    kv("rel", urelease(sp));
    emit(UDPWrite, r);

    BEGIN("903b", "udp_data_before_read", async);
    sp = ucreate(s, 1, 2048, 0, &c, async);
    g_watch = rs;
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, tmpbuf, 9), async, 1);
    delay(TK(2));
    n = uread(rs, sp, 3, async, 0);
    kv("readRes", n); kv("n", rs->u.csParam.receive.rcvBuffLen); kt("readDt", last_dt);
    g_watch = 0;
    kv("rel", urelease(sp));
    emit(UDPWrite, r);

    BEGIN("904", "udp_write_zero_len", async);
    sp = ucreate(s, 1, 2048, 0, &c, async);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, tmpbuf, 0), async, 1);
    kv("rd", uread(rs, sp, 2, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904b", "udp_write_odd_addr", async);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, tmpbuf + 1, 9), async, 1);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904c", "udp_write_multi_entry", async);
    wds[0][0].length = 2; wds[0][0].ptr = P32(tmpbuf); wds[0][1].length = 3; wds[0][1].ptr = P32(tmpbuf + 10);
    wds[0][2].length = 0; wds[0][2].ptr = 0;
    r = uwrite(s, sp, HOST, P_ECHO, wds[0], async, 1);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904d", "udp_write_nochecksum", async);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, tmpbuf, 9), async, 0);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904e", "udp_write_1472", async);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, big, 1472), async, 1);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904f", "udp_write_1473_fragment", async);
    r = uwrite(s, sp, HOST, P_ECHO, w1(0, big, 1473), async, 1);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("904g", "udp_write_3000", async);
    wds[0][0].length = 3000; wds[0][0].ptr = P32(big); wds[0][1].length = 0; wds[0][1].ptr = 0;
    r = uwrite(s, sp, HOST, P_ECHO, wds[0], async, 1);
    kv("rd", uread(rs, sp, 3, async, 0)); kv("n", rs->u.csParam.receive.rcvBuffLen);
    emit(UDPWrite, r);

    BEGIN("905", "udp_read_timeout", async);
    r = uread(rs, sp, 2, async, 0); kt("dt", last_dt);
    emit(UDPRead, r);

    BEGIN("906", "udp_two_pending_reads_release", 1);
    uread(rs, sp, 0, 1, 1); kv("imm1", last_imm);
    uread(&slots[2], sp, 0, 1, 1); kv("imm2", last_imm);
    delay(30);
    kv("res1", pbres(rs)); kv("res2", pbres(&slots[2]));
    kv("rel", urelease(sp));
    delay(30);
    kv("res1AfterRelease", pbres(rs)); kv("res2AfterRelease", pbres(&slots[2]));
    emit(UDPRelease, pbres(rs));

    BEGIN("907", "udp_read_stale_stream", async);
    r = uread(rs, sp, 2, async, 0);
    emit(UDPRead, r);
}

static void c_udp_misc(int async)
{
    Slot *s = &slots[0]; short r, c; StreamPtr sp; int cs;
    BEGIN("910", "udp_max_mtu", async);
    sp = ucreate(s, 1, 2048, 0, &c, async);
    pb_init(s, UDPMaxMTUSize); s->u.udpStream = sp; s->u.csParam.mtu.remoteHost = HOST;
    r = run(s, async, DEFWAIT); kv("mtu", s->u.csParam.mtu.mtuSize);
    emit(UDPMaxMTUSize, r);
    for (cs = 26; cs <= 29; cs++) {
        char id[8] = "911.00"; id[4] = (char)('0' + cs / 10); id[5] = (char)('0' + cs % 10);
        BEGIN(id, "udp_other_cs", async);
        pb_init(s, (short)cs); s->u.udpStream = sp;
        r = run(s, 1, TK(4));      /* async: UDPMultiRead may wait forever */
        kv("pending", r == 1);
        if (r == 1) { kv("rel", urelease(sp)); delay(20); kv("afterRel", pbres(s)); sp = ucreate(&slots[2], 1, 2048, 0, &c, 0); }
        emit(cs, r);
    }
    kv("rel", 0); (void)urelease(sp);
}

/* -- ICMP -- (icmpCompletion = our notify proc; 0 = none) */
#ifndef ICMP_PROC
#define ICMP_PROC P32(compl_stub)   /* register convention: A0 = block, same as an ioCompletion routine */
#endif
static void c_icmp(int async)
{
    static const struct { const char *id, *name; uint32_t dest; } t[] = {
        { "A00", "echo_gateway", GATEWAY }, { "A01", "echo_host", HOST },
        { "A02", "echo_dead_onlink", DEADLINK }, { "A03", "echo_self", 0 }
    };
    Slot *s = &slots[0]; int i; short r;
    static uint8_t data[32];
    for (i = 0; i < 4; i++) {
        uint32_t dest = t[i].dest ? t[i].dest : myip;
        long t0;
        memset(data, 'x', sizeof data);
        BEGIN(t[i].id, t[i].name, async);
        pb_init(s, ipctlEchoICMP);
        s->p.csParam.IPEchoPB.dest = dest;
        s->p.csParam.IPEchoPB.data.length = 32; s->p.csParam.IPEchoPB.data.ptr = P32(data);
        s->p.csParam.IPEchoPB.timeout = 3;
        s->p.csParam.IPEchoPB.icmpCompletion = ICMP_PROC;
        note(t[i].id, "about to call ICMPEcho (a crash here leaves this line last)");
        t0 = (long)os_ticks();
        r = run(s, async, TK(15));
        kt("dt", (long)os_ticks() - t0); kv("imm", last_imm);
        /* the reply (or timeout) is reported through icmpCompletion: wait for it to be called.
         * ioCompletion (async only) accounts for one 'C' event, icmpCompletion for another. */
        {
            int want = async ? 2 : 1, k, nc = 0, spin; long tw = (long)os_ticks();
            for (spin = 0; (long)os_ticks() - tw < TK(8); spin++) {
                nc = 0; for (k = 0; k < ev_n; k++) if (evs[k].kind == 'C') nc++;
                if (nc >= want) break;
            }
            kt("notifyWait", (long)os_ticks() - tw); kv("notifyCalls", nc - (async ? 1 : 0));
        }
        kh("notifyPBisOurs", slot_idx((void *)(uintptr_t)g_c_a0) >= 0);
        if (g_n_calls) { kv("nRes", g_n_res); kt("nOut", g_n_out); kt("nIn", g_n_in); kv("nLen", g_n_len); }
        g_n_calls = 0; g_n_res = 0; g_n_out = 0; g_n_in = 0; g_n_len = 0;
        kv("resNow", pbres(s));
        kt("echoOut", (long)s->i.icmpEchoInfo.echoRequestOut); kt("echoIn", (long)s->i.icmpEchoInfo.echoReplyIn);
        kv("echoedLen", s->i.icmpEchoInfo.echoedData.length);
        kip("dest", dest);
        emit(ipctlEchoICMP, r);
    }
}

/* -- register / context report -- */
static void c_regs(int async)
{
    Slot *s = &slots[0]; short r; StreamPtr sp; long a5 = (long)os_cur_a5(); (void)async;
    g_c_a5 = g_a_a5 = g_u_a5 = 0; g_c_sr = g_a_sr = 0;
    BEGIN("150", "entry_context", 1);
    sp = mk(1, 4096, &r);
    g_watch = s;
    r = aopen(s, sp, HOST, P_ECHO, 0, 10, 1, TK(20));
    delay(10);
    memcpy(tmpbuf, "x", 1);
    snd(&slots[1], sp, w1(0, tmpbuf, 1), 1, 1);
    delay(TK(2));
    kh("appA5", (unsigned long)a5);
    kh("complA5", g_c_a5); kh("complSR", g_c_sr); kh("complD0", g_c_d0);
    kv("complA0isPB", slot_idx((void *)(uintptr_t)g_c_a0) >= 0);
    kv("complStackArgIsPB", slot_idx((void *)(uintptr_t)g_c_arg) >= 0);
    kh("asrA5", g_a_a5); kh("asrSR", g_a_sr);
    kv("asrIntLevel", (g_a_sr >> 8) & 7); kv("complIntLevel", (g_c_sr >> 8) & 7);
    g_watch = 0;
    kv("close", tcall(TCPClose, sp)); kv("rel", rel(sp));
    emit(TCPActiveOpen, r);
}

/* ------------------------------------------------------------------ sweeps */

static void sweep(const char *idp, const char *name, int stale_tcp, int stale_udp)
{
    int cs; Slot *s = &slots[0]; char id[24]; short r, c; StreamPtr tsp = 0, usp = 0;
    if (stale_tcp) { tsp = mk(1, 4096, &c); rel(tsp); }
    if (stale_udp) { usp = ucreate(s, 2, 2048, 0, &c, 0); urelease(usp); }
    for (cs = 0; cs <= 60; cs++) {
        if (cs == 17 || cs == 43 || cs == 19) continue;
        strcpy(id, idp); strcat(id, ".");
        { B *sv = cb; cb = &hd; hd.n = 0; apd(cs); cb = sv; strcat(id, hd.s); }
        BEGIN(id, name, 0);
        pb_init(s, (short)cs);
        if (cs >= 20 && cs <= 29) s->u.udpStream = usp; else s->t.tcpStream = tsp;
        r = run(s, 0, TK(5));
        if (r == 1) { kv("pending", 1); }
        if (r == 0 && (cs == 20 || cs == 27)) urelease(s->u.udpStream);
        if (r == 0 && cs == 30) rel(s->t.tcpStream);
        emit(cs, r);
    }
}

/* risky: null pointers in PBs */
static void c_risky(int async)
{
    Slot *s = &slots[0]; short r; StreamPtr sp;
    BEGIN("990", "send_wds_null", async);
    kv("open", connect_to(1, HOST, P_ECHO, &sp, async));
    pb_init(s, TCPSend); s->t.tcpStream = sp; s->t.csParam.send.wdsPtr = 0; s->t.csParam.send.pushFlag = 1;
    r = run(s, async, DEFWAIT);
    emit(TCPSend, r);

    BEGIN("991", "rcv_buffer_null", async);
    pb_init(s, TCPRcv); s->t.tcpStream = sp; s->t.csParam.receive.commandTimeoutValue = 2;
    s->t.csParam.receive.rcvBuff = 0; s->t.csParam.receive.rcvBuffLen = 100;
    r = run(s, async, TK(8));
    emit(TCPRcv, r);

    BEGIN("992", "nocopy_rds_null", async);
    pb_init(s, TCPNoCopyRcv); s->t.tcpStream = sp; s->t.csParam.receive.commandTimeoutValue = 2;
    s->t.csParam.receive.rdsPtr = 0; s->t.csParam.receive.rdsLength = 0;
    r = run(s, async, TK(8));
    emit(TCPNoCopyRcv, r);
    tcall(TCPAbort, sp); rel(sp);
}

/* ------------------------------------------------------------------ unconfigured suite */

static void unconfigured_suite(void)
{
    Slot *s = &slots[0]; short r, c; StreamPtr sp, up; int async;
    note("U", "unconfigured suite: no usable IP address");
    for (async = 0; async < 2; async++) {
        BEGIN("U01", "getmyip", async);
        pb_init(s, ipctlGetAddr); r = run(s, async, DEFWAIT);
        kip("addr", s->g.ourAddress); emit(ipctlGetAddr, r);

        BEGIN("U02", "tcp_create", async);
        sp = mk(1, 4096, &c); kv("create", c); emit(TCPCreate, c);

        BEGIN("U03", "tcp_active_open", async);
        r = aopen(s, sp, HOST, P_BANNER, 0, 6, async, TK(15));
        kt("dt", last_dt);
        emit(TCPActiveOpen, r);

        BEGIN("U04", "tcp_passive_open", async);
        r = popen_(s, sp, 0, 3, async, TK(10)); emit(TCPPassiveOpen, r);

        BEGIN("U05", "tcp_status", async);
        pst("st", sp); emit(TCPStatus, 0);

        BEGIN("U06", "tcp_send", async);
        r = snd(s, sp, w1(0, tmpbuf, 3), 1, async); emit(TCPSend, r);

        BEGIN("U07", "tcp_release", async);
        r = sp ? rel(sp) : 0; emit(TCPRelease, r);

        BEGIN("U08", "udp_create", async);
        up = ucreate(s, 1, 2048, 0, &c, async); kv("lport", s->u.csParam.create.localPort); emit(UDPCreate, c);
        BEGIN("U09", "udp_write", async);
        r = uwrite(s, up, HOST, P_ECHO, w1(0, tmpbuf, 3), async, 1); emit(UDPWrite, r);
        BEGIN("U10", "udp_release", async);
        r = up ? urelease(up) : 0; emit(UDPRelease, r);

        BEGIN("U11", "icmp_echo", async);
        pb_init(s, ipctlEchoICMP);
        s->p.csParam.IPEchoPB.dest = GATEWAY; s->p.csParam.IPEchoPB.data.length = 8;
        s->p.csParam.IPEchoPB.data.ptr = P32(tmpbuf); s->p.csParam.IPEchoPB.timeout = 120;
        s->p.csParam.IPEchoPB.icmpCompletion = P32(compl_stub);
        r = run(s, async, TK(8)); emit(ipctlEchoICMP, r);

        BEGIN("U12", "udp_max_mtu", async);
        pb_init(s, UDPMaxMTUSize); s->u.csParam.mtu.remoteHost = HOST; r = run(s, async, DEFWAIT);
        kv("mtu", s->u.csParam.mtu.mtuSize); emit(UDPMaxMTUSize, r);
    }
    c_globalinfo(0);
    c_lapstats(0);
}

/* ------------------------------------------------------------------ driver */

static void c_driver_open(void)
{
    short ref1 = 0, ref2 = 0, e1, e2;
    BEGIN("100", "driver_open_first", 0);
    e1 = os_open_ipp(&ref1);
    kv("refnum", ref1);
    kv("second", (e2 = os_open_ipp(&ref2), e2)); kv("secondRef", ref2);
    emit(0, e1);
    if (e1 == 0) drv = ref1;
}

static void c_driver_close(void)
{
    short e, ref = 0, e2; short refnow = drv;
    BEGIN("999", "driver_close_reopen", 0);
    e = os_close_drv(refnow);
    kv("closeRef", refnow);
    e2 = os_open_ipp(&ref); kv("reopen", e2); kv("reopenRef", ref);
    if (e2 == 0) drv = ref;
    { Slot *s = &slots[0]; pb_init(s, ipctlGetAddr); kv("getaddrAfter", run(s, 0, DEFWAIT)); kip("addr", s->g.ourAddress); }
    emit(0, e);
}

static void both(void (*f)(int)) { f(0); f(1); }

static unsigned grp_mask = 0x7FF;
static int grp(int g) { return (grp_mask >> g) & 1; }

int main(void)
{
    short r;
    int i;
    os_init();
    g_asr_target = (uint32_t)(uintptr_t)asr_c;
    g_uasr_target = (uint32_t)(uintptr_t)uasr_c;
    for (i = 0; i < (int)sizeof big; i++) big[i] = (char)('a' + i % 26);

    {   /* ApiProbe.cfg: digits 1-9 name the groups to run; missing = all */
        char cfg[16]; int n = os_read_cfg("\pApiProbe.cfg", cfg, (int)sizeof cfg), k;
        if (n > 0) {
            grp_mask = 0;
            for (k = 0; k < n; k++) if (cfg[k] >= '1' && cfg[k] <= '9') grp_mask |= 1u << (cfg[k] - '0');
            else if (cfg[k] == '0') grp_mask |= 1u << 10;
        }
    }
    r = os_out_open("\pApiProbe.out");
    if (r) { os_done_alert(); return 0; }
    note("0", "ApiProbe 1: format id name S|A cs= res= extras | events (A=ASR C=completion P=poll)");
    { char m[16]; int k, n = 0; for (k = 1; k <= 10; k++) if (grp(k)) m[n++] = (char)('0' + (k % 10)); m[n] = 0; note("0", m); }
    note("0", "values written =@N are tick counts (timing dependent)");

    c_driver_open();
    {   /* decide the configuration */
        Slot *s = &slots[0];
        pb_init(s, ipctlGetAddr);
        r = run(s, 0, DEFWAIT);
        myip = s->g.ourAddress; mymask = (uint32_t)s->g.ourNetMask;
        configured = (r == 0 && myip != 0);
    }
    if (!configured) {
        unconfigured_suite();
        sweep("120", "sweep_stream0", 0, 0);
        c_driver_close();
        os_out_close();
        os_done_alert();
        return 0;
    }

    /* Groups (ApiProbe.cfg selects them; no cfg = all). probe-run.sh runs one group set per
     * Snow boot: Snow's NAT stops accepting new TCP connections after about 16, because it
     * never closes the host side of a guest connection. */
    if (grp(1)) {
        note("G1", "driver, address, create sizes");
        both(c_getaddr);
        both(c_lapstats);
        both(c_globalinfo);
        c_regs(1);
        both(c_create_sizes);
        both(c_release_twice);
    }
    if (grp(2)) {
        note("G2", "active open");
        both(c_aopen_banner);
        both(c_aopen_fail);
        both(c_aopen_twice);
        both(c_aopen_localport);
        both(c_close_idle);
    }
    if (grp(3)) {
        note("G3", "send");
        both(c_send_echo);
        both(c_send_nopush);
        both(c_send_variants);
        both(c_send_unconnected);
        both(c_send_large);
        c_send_double(1);
    }
    if (grp(4)) {
        note("G4", "receive, close, abort");
        both(c_rcv_cases);
        both(c_rcv_unconnected);
        c_rcv_double(1);
        both(c_nocopy);
        both(c_close_pending);
        both(c_remote_close);
        both(c_abort);
    }
    if (grp(5)) {
        note("G5", "release, remote reset, burst, passive open");
        both(c_release_connected);
        both(c_remote_rst);
        both(c_burst);
        both(c_passive);
    }
    if (grp(6)) {
        note("G6", "UDP");
        both(c_udp_create);
        both(c_udp_io);
        both(c_udp_misc);
    }
    if (grp(7)) {
        note("G7", "ICMP echo");
        both(c_icmp);
    }
    if (grp(8)) {
        note("G8", "csCode sweeps, null pointers, driver close");
        sweep("120", "sweep_stream0", 0, 0);
        sweep("121", "sweep_stale_stream", 1, 1);
        both(c_risky);
        c_driver_close();
        /* completion marker (the host echo server logs it) */
        {
            Slot *s = &slots[0]; StreamPtr sp;
            memcpy(tmpbuf, "PROBE-DONE\n", 11);
            if (connect_to(1, HOST, P_ECHO, &sp, 0) == 0) {
                snd(s, sp, w1(0, tmpbuf, 11), 1, 0);
                delay(60);
                tcall(TCPAbort, sp); rel(sp);
            }
        }
    }
    if (grp(9)) {
        note("G9", "odd rcv buffer, no notify proc (a crash here leaves the file as is)");
        both(c_create_odd);
    }
    if (grp(10)) {
        /* last: an asynchronous TCPCreate with a null buffer crashes real MacTCP */
        note("G10", "null rcv buffer: sync, then async (a crash here leaves the file as is)");
        c_create_null(0);
        c_create_null(1);
    }
    os_out_close();
    os_done_alert();
    return 0;
}
