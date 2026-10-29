/* enetspike.c - EnetSpike: Task 9 spike of the .ENET0 (DaynaPORT) interface in Snow.
 *
 * Every number the interface doc lists as "obs" for the Ethernet driver comes from here:
 * csCodes and PB layout (EGetInfo, EAttachPH, EWrite), the handler's register convention and
 * interrupt level (spike_ph.s logs SR/SP/A3/A4/D1 into a ring), delivery of whole packets with
 * ReadRest, bursts, a 1514-byte frame, EWrite timing, a send from inside the handler, and
 * the Time Manager (version, a 1 ms self-priming task: accuracy and CPU cost).
 * Output: EnetSpike.out beside the app (CR separated). Host side: scripts/testnet.py running. */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>
#include <Files.h>
#include <Devices.h>
#include <Gestalt.h>
#include <Timer.h>
#include <OSUtils.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

QDGlobals qd;

/* ---- ENET parameter block (csParam at offset 28) ---- */
#pragma pack(push, 2)
typedef struct EPB {
    uint32_t qLink; int16_t qType; int16_t ioTrap; uint32_t ioCmdAddr;
    uint32_t ioCompletion; int16_t ioResult; uint32_t ioNamePtr; int16_t ioVRefNum;
    int16_t ioRefNum; int16_t csCode;
    int16_t eProtType; uint32_t ePointer; int16_t eBuffSize; int16_t eDataSize;
    int16_t pad[10];
} EPB;
typedef struct WDS { uint16_t len; uint32_t ptr; } WDS;
#pragma pack(pop)

enum { E_ATTACH = 247, E_DETACH = 248, E_WRITE = 249, E_READ = 250, E_RDCANCEL = 251,
       E_GETINFO = 252, E_SETGENERAL = 253 };

/* ---- ring shared with spike_ph.s (offsets there) ---- */
#define NSLOT 8
#define SLOTSZ 1572
typedef struct Slot {
    uint16_t len, sr, type, rr;
    uint32_t ticks, a3, a4, d1, sp, us0, us1;
    uint8_t buf[1536];
} Slot;
typedef struct Ring {
    volatile uint16_t head, tail, over, hook;
    Slot s[NSLOT];
    EPB hpb;                 /* used by the send-from-handler hook */
    WDS hwds[2];
    uint8_t hframe[64];
    volatile int16_t hres;
    volatile uint32_t hticks0, hticks1;
    volatile int16_t hran;
    volatile int16_t hasync;
    volatile int16_t hdt;                 /* 1: install a deferred task from the hook */
    struct { uint32_t qLink; int16_t qType; int16_t dtFlags; uint32_t dtAddr; uint32_t dtParm;
             uint32_t dtReserved; } dt;
    volatile uint16_t dsr; volatile int16_t dran, dres, dinst;
    volatile uint32_t dtk0, dtk1;
} Ring;

/* _DTInstall (A082): A0 = task, D0 = result */
static int16_t dt_install(void *t)
{
    register void *a0 __asm__("a0") = t;
    register int16_t d0 __asm__("d0");
    __asm__ volatile(".short 0xA082" : "=d"(d0) : "a"(a0) : "d1", "a1", "cc", "memory");
    return d0;
}

extern void spike_ph(void);
extern void spike_ph_set_ctx(void *ring);
extern void spike_tm(void);
extern void spike_dt(void);

static Ring ring;
static int16_t drv = 0;
static uint8_t mac[6];

/* ---- output ---- */
static short outref = -1;
static void P(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    long n;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    n = (long)strlen(b);
    if (outref >= 0) FSWrite(outref, &n, b);
}

/* ---- Time Manager ---- */
typedef struct TmRec {
    TMTask t; volatile uint32_t count; uint32_t period;
    volatile int16_t mode;            /* 0 count, 1 sync EWrite once, 2 async EWrite once */
    volatile uint16_t sr; volatile int16_t ran, res;
    volatile uint32_t tk0, tk1;
    EPB pb; WDS w[2]; uint8_t frame[64];
} TmRec;
void spike_tm_c(TmRec *r);
void spike_tm_c(TmRec *r)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    r->sr = sr;
    r->count++;
    if (r->mode) {
        r->tk0 = TickCount();
        if (r->mode == 1) r->res = (int16_t)PBControlSync((ParmBlkPtr)&r->pb);
        else r->res = (int16_t)PBControlAsync((ParmBlkPtr)&r->pb);
        r->tk1 = TickCount();
        r->ran = 1;
        r->mode = 0;
        return;
    }
    PrimeTime((QElemPtr)r, (long)r->period);
}

static void spin(uint32_t n) { volatile uint32_t i; for (i = 0; i < n; i++) { } }

/* ---- ENET calls ---- */
static int16_t ecall(EPB *pb)
{
    pb->ioRefNum = drv;
    pb->ioCompletion = 0;
    return (int16_t)PBControlSync((ParmBlkPtr)pb);
}

static int16_t e_attach(uint16_t type, void (*h)(void))
{
    EPB pb;
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_ATTACH; pb.eProtType = (int16_t)type; pb.ePointer = (uint32_t)h;
    return ecall(&pb);
}

static int16_t e_write(const uint8_t *frame, uint16_t len)
{
    EPB pb;
    WDS w[2];
    memset(&pb, 0, sizeof pb);
    w[0].len = len; w[0].ptr = (uint32_t)frame; w[1].len = 0; w[1].ptr = 0;
    pb.csCode = E_WRITE; pb.ePointer = (uint32_t)w;
    return ecall(&pb);
}

/* ---- frame builders ---- */
static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
static uint8_t gwmac[6];
static int have_gw;
static uint32_t cks(const uint8_t *p, int n)
{
    uint32_t s = 0;
    int i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (~s) & 0xFFFF;
}

static int arp_req(uint8_t *f)
{
    memset(f, 0, 60);
    memcpy(f, bcast, 6); memcpy(f + 6, mac, 6);
    f[12] = 0x08; f[13] = 0x06; f[15] = 1; f[16] = 0x08; f[18] = 6; f[19] = 4; f[21] = 1;
    memcpy(f + 22, mac, 6); f[28] = 10; f[31] = 77; f[38] = 10; f[41] = 1;
    return 60;
}

static int ip_hdr(uint8_t *f, int total, int proto, const uint8_t *dst)
{
    uint32_t c;
    memcpy(f, gwmac, 6); memcpy(f + 6, mac, 6); f[12] = 8; f[13] = 0;
    f[14] = 0x45; f[16] = (uint8_t)(total >> 8); f[17] = (uint8_t)total; f[19] = 1;
    f[22] = 64; f[23] = (uint8_t)proto;
    f[26] = 10; f[29] = 77; memcpy(f + 30, dst, 4);
    c = cks(f + 14, 20);
    f[24] = (uint8_t)(c >> 8); f[25] = (uint8_t)c;
    return 34;
}

static int icmp_echo(uint8_t *f, int seq)
{
    static const uint8_t gw[4] = { 10, 0, 0, 1 };
    int i, n = 34;
    uint32_t c;
    memset(f, 0, 100);
    ip_hdr(f, 20 + 8 + 32, 1, gw);
    f[34] = 8; f[38] = 0x12; f[39] = 0x34; f[41] = (uint8_t)seq; f[40] = (uint8_t)(seq >> 8);
    for (i = 0; i < 32; i++) f[42 + i] = (uint8_t)(i + 1);
    c = cks(f + 34, 40);
    f[36] = (uint8_t)(c >> 8); f[37] = (uint8_t)c;
    n = 14 + 20 + 8 + 32;
    return n;
}

static int udp_big(uint8_t *f, int payload)
{
    static const uint8_t host[4] = { 172, 17, 0, 1 };
    int i;
    memset(f, 0, 14 + 20 + 8 + payload);
    ip_hdr(f, 20 + 8 + payload, 17, host);
    f[34] = 0x0F; f[35] = 0xA0; f[36] = 17007 >> 8; f[37] = 17007 & 255;
    f[38] = (uint8_t)((8 + payload) >> 8); f[39] = (uint8_t)(8 + payload);
    for (i = 0; i < payload; i++) f[42 + i] = (uint8_t)(i * 7);
    return 14 + 20 + 8 + payload;
}

/* ---- ring drain ---- */
static unsigned n_rx, n_arp_reply, n_ip, n_icmp_reply, n_udp;
static uint32_t sr_or, sr_min = 0xFFFF;

static void drain(const char *tag, int verbose)
{
    while (ring.tail != ring.head) {
        Slot *s = &ring.s[ring.tail];
        uint8_t *b = s->buf;
        int i, n = s->len;
        n_rx++;
        sr_or |= s->sr;
        if ((s->sr & 0x0700) < sr_min) sr_min = s->sr & 0x0700;
        if (s->type == 0x0806 && n >= 42 && b[21] == 2 && b[28] == 10 && b[31] == 1) {
            n_arp_reply++;
            if (!have_gw) { memcpy(gwmac, b + 22, 6); have_gw = 1; }
        } else if (s->type == 0x0800) {
            n_ip++;
            if (n >= 35 && b[23] == 1 && b[34] == 0) n_icmp_reply++;
            if (n >= 35 && b[23] == 17) n_udp++;
        }
        if (verbose) {
            P("RX %s #%u tick=%lu sr=%04x type=%04x len=%d rr=%d a3-sp=%ld a4=%08lx d1=%lu sp=%08lx dst=%02x%02x%02x%02x%02x%02x [",
              tag, n_rx, (unsigned long)s->ticks, s->sr, s->type, n, (int)(int16_t)s->rr,
              (long)(s->a3 - s->sp), (unsigned long)s->a4, (unsigned long)s->d1,
              (unsigned long)s->sp, b[0], b[1], b[2], b[3], b[4], b[5]);
            for (i = 14; i < 14 + 16 && i < n; i++) P("%02x", b[i]);
            P("]\r");
        }
        ring.tail = (uint16_t)((ring.tail + 1) & 7);
    }
}

static void wait_ticks(int t, const char *tag, int verbose)
{
    unsigned long end = TickCount() + (unsigned long)t;
    while (TickCount() < end) drain(tag, verbose);
    drain(tag, verbose);
}

/* hook: runs inside the handler, tries an EWrite from interrupt level */
void spike_hook_c(Ring *r);
void spike_hook_c(Ring *r)
{
    if (r->hdt) { r->dinst = dt_install(&r->dt); r->hran = 1; return; }
    r->hticks0 = TickCount();
    if (r->hasync) r->hres = (int16_t)PBControlAsync((ParmBlkPtr)&r->hpb);
    else r->hres = (int16_t)PBControlSync((ParmBlkPtr)&r->hpb);
    r->hticks1 = TickCount();
    r->hran = 1;
}

void spike_dt_c(Ring *r);
void spike_dt_c(Ring *r)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    r->dsr = sr;
    r->dtk0 = TickCount();
    r->dres = (int16_t)PBControlSync((ParmBlkPtr)&r->hpb);
    r->dtk1 = TickCount();
    r->dran = 1;
}

int main(void)
{
    char line[200];
    Str255 ps;
    size_t n;
    long gv;
    int i, got;
    OSErr err;
    EPB pb;
    uint8_t frame[1536];
    uint8_t info[128];
    unsigned long t0, t1;
    unsigned long iter, iter1;
    int16_t r;
    static TmRec tm;
    int test = 0;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0);
    InitCursor();
    Create((ConstStringPtr)"\pEnetSpike.out", 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)"\pEnetSpike.out", 0, &outref)) outref = -1;
    else SetEOF(outref, 0);
    P("EnetSpike\r");
    {
        short cr;
        long cn = 1;
        char cc = '0';
        if (FSOpen((ConstStringPtr)"\pEnetSpike.cfg", 0, &cr) == 0) { FSRead(cr, &cn, &cc); FSClose(cr); }
        test = cc - '0';
        P("test mode %d\r", test);
    }

    /* --- environment --- */
    err = Gestalt(gestaltTimeMgrVersion, &gv);
    P("gestalt tmgr err=%d val=%ld\r", (int)err, gv);
    P("sizeof EPB=%u WDS=%u Slot=%u\r", (unsigned)sizeof(EPB), (unsigned)sizeof(WDS),
      (unsigned)sizeof(Slot));

    /* --- open --- */
    {
        short ref = 0;
        err = OpenDriver((ConstStringPtr)"\p.ENET0", &ref);
        P("OpenDriver .ENET0 err=%d ref=%d\r", (int)err, (int)ref);
        drv = ref;
        err = OpenDriver((ConstStringPtr)"\p.ENET", &ref);
        P("OpenDriver .ENET err=%d ref=%d\r", (int)err, (int)ref);
        err = OpenDriver((ConstStringPtr)"\p.ENET1", &ref);
        P("OpenDriver .ENET1 err=%d ref=%d\r", (int)err, (int)ref);
    }
    if (drv == 0) { P("no driver\r"); goto fin; }

    /* --- EGetInfo --- */
    memset(info, 0xEE, sizeof info);
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_GETINFO; pb.ePointer = (uint32_t)info; pb.eBuffSize = 18;
    t0 = TickCount();
    r = ecall(&pb);
    P("EGetInfo(buf 18) res=%d eDataSize=%d eBuffSize=%d ticks=%lu\r", r, pb.eDataSize,
      pb.eBuffSize, (unsigned long)(TickCount() - t0));
    for (i = 0; i < 24; i++) P("%02x%s", info[i], i == 23 ? "\r" : " ");
    memcpy(mac, info, 6);
    memset(&pb, 0, sizeof pb);
    memset(info, 0xEE, sizeof info);
    pb.csCode = E_GETINFO; pb.ePointer = (uint32_t)info; pb.eBuffSize = 6;
    r = ecall(&pb);
    P("EGetInfo(buf 6) res=%d bytes beyond 6: %02x %02x\r", r, info[6], info[7]);

    /* --- attach --- */
    spike_ph_set_ctx(&ring);
    r = e_attach(0x0806, spike_ph); P("EAttachPH 0806 res=%d\r", r);
    r = e_attach(0x0800, spike_ph); P("EAttachPH 0800 res=%d\r", r);
    r = e_attach(0x0800, spike_ph); P("EAttachPH 0800 again res=%d\r", r);
    FlushVol(0, 0);

    /* --- one ARP request --- */
    n = (size_t)arp_req(frame);
    t0 = TickCount();
    r = e_write(frame, (uint16_t)n);
    t1 = TickCount();
    P("EWrite arp 60 res=%d ticks=%lu\r", r, t1 - t0);
    wait_ticks(120, "arp1", 1);
    P("after arp1: rx=%u arp_reply=%u gw=%d\r", n_rx, n_arp_reply, have_gw);
    if (have_gw)
        P("gwmac %02x:%02x:%02x:%02x:%02x:%02x\r", gwmac[0], gwmac[1], gwmac[2], gwmac[3],
          gwmac[4], gwmac[5]);
    FlushVol(0, 0);

    /* --- EWrite shorter than 60 and WDS in two parts --- */
    if (test == 0) {
        EPB pb2; WDS w[3];
        n = (size_t)arp_req(frame);
        memset(&pb2, 0, sizeof pb2);
        w[0].len = 14; w[0].ptr = (uint32_t)frame;
        w[1].len = 28; w[1].ptr = (uint32_t)(frame + 14);   /* 42 bytes total: runt */
        w[2].len = 0; w[2].ptr = 0;
        pb2.csCode = E_WRITE; pb2.ePointer = (uint32_t)w;
        n_arp_reply = 0;
        r = ecall(&pb2);
        P("EWrite arp 42 bytes in 2 WDS pieces res=%d\r", r);
        wait_ticks(60, "arp42", 1);
        P("reply to the 42-byte request: %u\r", n_arp_reply);
        /* 1-byte write: error? */
        w[0].len = 10; w[0].ptr = (uint32_t)frame; w[1].len = 0;
        r = ecall(&pb2);
        P("EWrite 10 bytes res=%d\r", r);
        w[0].len = 1515;
        r = ecall(&pb2);
        P("EWrite 1515 bytes res=%d\r", r);
    }

    if (test == 0) {
    /* --- burst of 20 --- */
    n_arp_reply = 0;
    t0 = TickCount();
    for (i = 0; i < 20; i++) {
        n = (size_t)arp_req(frame);
        r = e_write(frame, (uint16_t)n);
        if (r) P("burst write %d res=%d\r", i, r);
    }
    t1 = TickCount();
    P("burst 20 writes took %lu ticks\r", t1 - t0);
    wait_ticks(180, "burst", 1);
    P("burst: replies=%u of 20, over=%u\r", n_arp_reply, ring.over);
    FlushVol(0, 0);

    /* --- ICMP echo to 10.0.0.1 --- */
    if (have_gw) {
        n_icmp_reply = 0;
        n = (size_t)icmp_echo(frame, 1);
        r = e_write(frame, (uint16_t)n);
        P("EWrite icmp echo len=%u res=%d\r", (unsigned)n, r);
        wait_ticks(120, "icmp", 1);
        P("icmp replies=%u (0800 frames=%u)\r", n_icmp_reply, n_ip);

        /* 1514-byte UDP echo via the host (testnet.py UDP echo 17007) */
        n_udp = 0;
        n = (size_t)udp_big(frame, 1472);
        r = e_write(frame, (uint16_t)n);
        P("EWrite udp len=%u res=%d\r", (unsigned)n, r);
        wait_ticks(240, "udp", 1);
        P("udp frames=%u\r", n_udp);
        FlushVol(0, 0);
    }

    /* --- sequential round trips (ARP) --- */
    {
        unsigned ok = 0, lost = 0;
        unsigned long maxrt = 0, sumrt = 0;
        t0 = TickCount();
        for (i = 0; i < 200; i++) {
            unsigned before = n_arp_reply;
            unsigned long a = TickCount(), b;
            n = (size_t)arp_req(frame);
            e_write(frame, (uint16_t)n);
            while (n_arp_reply == before && TickCount() - a < 60) drain("rt", 0);
            b = TickCount() - a;
            if (n_arp_reply != before) { ok++; sumrt += b; if (b > maxrt) maxrt = b; }
            else lost++;
        }
        P("200 sequential ARP round trips: ok=%u lost=%u max=%lu ticks sum=%lu total=%lu ticks\r",
          ok, lost, maxrt, sumrt, TickCount() - t0);
    }
    P("handler SR values seen: or=%04x min-mask=%04x\r", (unsigned)sr_or, (unsigned)sr_min);
    FlushVol(0, 0);

    }   /* test == 0 */

    if (test == 1) {
        /* gate level: EWrite under mask L, then spin masked: does the handler still run? */
        int L;
        for (L = 0; L <= 7; L++) {
            uint16_t sr, sr2, h0, h1, h2;
            int16_t wr;
            uint32_t ta, tb;
            P("mask L=%d: trying EWrite under the mask ...\r", L);
            FlushVol(0, 0);
            drain("m", 0);
            h0 = ring.head;
            __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
            sr2 = (uint16_t)((sr & ~0x0700) | (L << 8));
            ta = TickCount();
            __asm__ volatile("move.w %0,%%sr" : : "d"(sr2) : "memory");
            n = (size_t)arp_req(frame);
            wr = e_write(frame, (uint16_t)n);
            h1 = ring.head;
            spin(300000);
            h2 = ring.head;
            __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory");
            tb = TickCount();
            P("  L=%d: ewrite=%d records during write=%d, after spin=%d; ticks advanced %lu (0 = VBL masked); ",
              L, wr, (int)((h1 - h0) & 7), (int)((h2 - h0) & 7), (unsigned long)(tb - ta));
            wait_ticks(30, "m", 0);
            P("after unmask: %d (total rx %u)\r", (int)((ring.head - h0) & 7) , n_rx);
            FlushVol(0, 0);
        }
    }
    if (test == 7) {
        /* which interrupt mask keeps the protocol handler out? ASYNC EWrite (returns at once),
         * then mask L and spin: does a record appear while masked? */
        static EPB apb;
        static WDS aw[2];
        static uint8_t af[64];
        int L;
        for (L = 0; L <= 7; L++) {
            uint16_t sr, sr2, h0, h1, h2;
            uint32_t ta, tb;
            drain("m", 0);
            h0 = ring.head;
            arp_req(af);
            memset(&apb, 0, sizeof apb);
            aw[0].len = 60; aw[0].ptr = (uint32_t)af; aw[1].len = 0; aw[1].ptr = 0;
            apb.csCode = E_WRITE; apb.ePointer = (uint32_t)aw; apb.ioRefNum = drv;
            __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
            sr2 = (uint16_t)((sr & ~0x0700) | (L << 8));
            ta = TickCount();
            __asm__ volatile("move.w %0,%%sr" : : "d"(sr2) : "memory");
            PBControlAsync((ParmBlkPtr)&apb);
            h1 = ring.head;
            spin(300000);
            h2 = ring.head;
            __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory");
            tb = TickCount();
            P("mask L=%d: records right after async write=%d, after masked spin=%d, ioResult=%d, ticks advanced %lu; ",
              L, (int)((h1 - h0) & 7), (int)((h2 - h0) & 7), (int)apb.ioResult, (unsigned long)(tb - ta));
            wait_ticks(40, "m", 0);
            P("after unmask: records=%d ioResult=%d\r", (int)((ring.head - h0) & 7), (int)apb.ioResult);
            FlushVol(0, 0);
        }
    }
    if (test == 6) {
        P("JDTInstall=%08lx DTInstall trap %08lx unimplemented %08lx\r", *(unsigned long *)0x0D9C,
          (unsigned long)NGetTrapAddress(0x82, (TrapType)0), (unsigned long)NGetTrapAddress(0x89F, (TrapType)1));
        P("Microseconds trap %08lx\r", (unsigned long)NGetTrapAddress(0x93, (TrapType)0));
        FlushVol(0, 0);
    }
    if (test == 2 || test == 5 || test == 6) {
        memset(&ring.hpb, 0, sizeof ring.hpb);
        arp_req(ring.hframe);
        ring.hwds[0].len = 60; ring.hwds[0].ptr = (uint32_t)ring.hframe;
        ring.hwds[1].len = 0; ring.hwds[1].ptr = 0;
        ring.hpb.csCode = E_WRITE; ring.hpb.ePointer = (uint32_t)ring.hwds; ring.hpb.ioRefNum = drv;
        ring.hran = 0;
        ring.hasync = (test == 2);
        ring.hdt = (test == 6);
        ring.dt.qType = 7; ring.dt.dtAddr = (uint32_t)spike_dt; ring.dt.dtParm = (uint32_t)&ring;
        P("hook test: %s EWrite from inside the handler ...\r", test == 6 ? "DEFERRED TASK" : ring.hasync ? "ASYNC" : "SYNC");
        FlushVol(0, 0);
        n_arp_reply = 0;
        ring.hook = 1;
        n = (size_t)arp_req(frame);
        e_write(frame, (uint16_t)n);
        wait_ticks(120, "hook", 1);
        P("hook ran=%d res=%d ioResult now=%d ticks %lu..%lu; replies now %u\r", ring.hran, ring.hres,
          ring.hpb.ioResult, (unsigned long)ring.hticks0, (unsigned long)ring.hticks1, n_arp_reply);
        P("deferred task: installed=%d ran=%d sr=%04x res=%d ticks %lu..%lu\r", ring.dinst, ring.dran, ring.dsr, ring.dres, (unsigned long)ring.dtk0, (unsigned long)ring.dtk1);
    }
    if (test == 3 || test == 4) {
        memset(&tm, 0, sizeof tm);
        tm.t.tmAddr = (ProcPtr)spike_tm;
        arp_req(tm.frame);
        tm.w[0].len = 60; tm.w[0].ptr = (uint32_t)tm.frame; tm.w[1].len = 0; tm.w[1].ptr = 0;
        tm.pb.csCode = E_WRITE; tm.pb.ePointer = (uint32_t)tm.w; tm.pb.ioRefNum = drv;
        tm.mode = (int16_t)(test == 3 ? 1 : 2);
        P("TM test: %s EWrite from a Time Manager task ...\r", test == 3 ? "SYNC" : "ASYNC");
        FlushVol(0, 0);
        n_arp_reply = 0;
        InsTime((QElemPtr)&tm);
        PrimeTime((QElemPtr)&tm, 5);
        wait_ticks(120, "tm", 1);
        P("TM task ran=%d sr=%04x res=%d ioResult=%d ticks %lu..%lu; replies %u\r", tm.ran, tm.sr,
          tm.res, tm.pb.ioResult, (unsigned long)tm.tk0, (unsigned long)tm.tk1, n_arp_reply);
        RmvTime((QElemPtr)&tm);
    }

    if (test == 0) {
    /* --- Time Manager: 1 ms self-priming task, accuracy and cost --- */
    iter = 0;
    t0 = TickCount();
    while (TickCount() - t0 < 120) iter++;
    memset(&tm, 0, sizeof tm);
    tm.t.tmAddr = (ProcPtr)spike_tm;
    tm.period = 1;
    InsTime((QElemPtr)&tm);
    PrimeTime((QElemPtr)&tm, 1);
    iter1 = 0;
    t0 = TickCount();
    while (TickCount() - t0 < 120) iter1++;
    t1 = tm.count;
    RmvTime((QElemPtr)&tm);
    P("TM 1ms task: count=%lu in 120 ticks (%lu ms expected %lu); busy-loop iterations %lu without, %lu with\r",
      t1, 120UL * 1000 / 60, 2000UL, iter, iter1);
    FlushVol(0, 0);


    }

    /* detach */
    memset(&pb, 0, sizeof pb);
    pb.csCode = E_DETACH; pb.eProtType = 0x0806;
    r = ecall(&pb); P("EDetachPH 0806 res=%d\r", r);
    pb.eProtType = 0x0800;
    r = ecall(&pb); P("EDetachPH 0800 res=%d\r", r);
    P("RESULT DONE\r");

fin:
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
    got = 0; (void)got;
    sprintf(line, "EnetSpike finished. Results are in EnetSpike.out");
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
