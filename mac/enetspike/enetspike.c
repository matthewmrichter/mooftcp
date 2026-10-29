/* enetspike.c - EnetSpike, second form: the Task 9 glue test. The Ethernet/timer/gate glue
 * (mac/glue/mt_enet.c, mt_enet_ph.s, mt_gate.c, mt_timer.c, mt_os_mac.c) runs the portable core
 * (a real mt_stack with the address 10.0.0.77) against Snow's NAT gateway 10.0.0.1:
 *
 *   A  ARP + ICMP echo through the stack (the stack ARPs for the gateway by itself)
 *   B  1000 sequential echo round trips with a pattern payload; each must come back intact
 *   C  20 echoes of 1472 bytes (1514-byte frames both ways)
 *   D  a burst of 60 echoes without waiting (4 transmit slots: drops are counted, nothing hangs)
 *   E  the Time Manager timer: requested deadlines vs ticks, re-arm replaces, cancel, periodic
 *   G  the core's own timers through the Time Manager task (ARP retries of an unanswered address)
 *   F  gate / driver counters and the stack space seen at interrupt time
 *
 * The first form of this app (git history, "Ethernet driver spike") drove the driver by hand to
 * find the interface; docs/dev/MACTCP-INTERFACE.md "Ethernet driver calls" records what it found.
 * Output: EnetSpike.out (CR separated), last line "RESULT PASS" or "RESULT FAIL". */
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
#include <OSUtils.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "mt.h"
#include "mt_gate.h"
#include "mt_enet.h"
#include "mt_timer.h"
#include "mt_os_mac.h"

uint32_t mt_os_ms(void);

QDGlobals qd;

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

#pragma GCC diagnostic ignored "-Warray-bounds"   /* low-memory Ticks at 0x16A, CurrentA5 at 0x904 */
static unsigned long now_ticks(void) { return *(volatile unsigned long *)0x16A; }

/* ---- the application: everything the callbacks reach through one pointer ---- */
typedef struct App {
    mt_stack *s;
    MtGate   *g;
    MtEnet   *e;
    MtTimer   tm;                 /* drives mt_gate_tick */
    volatile uint16_t got_seq;    /* last echo reply seq */
    volatile uint32_t replies, bad_payload, bad_from;
    volatile uint32_t pat_len;
    volatile uint32_t min_stack_irq, min_stack_task;
    volatile uint32_t after_calls, tick_calls;
    volatile uint32_t a5_irq_n, a5_irq_bad, a5_task_n, a5_task_bad;   /* A5 seen by echo_cb vs CurrentA5 */
} App;

#define PAT(i) ((uint8_t)((i) * 7u + 3u))

static void note_stack(volatile uint32_t *min)
{
    uint32_t sp = (uint32_t)StackSpace();
    if (sp < *min) *min = sp;
}

static void rx_cb(void *ctx, const uint8_t *f, uint16_t len)
{
    App *a = (App *)ctx;
    note_stack(mt_gate_in_irq(a->g) ? &a->min_stack_irq : &a->min_stack_task);
    mt_input(a->s, f, len, mt_os_ms());
}
static void tick_cb(void *ctx)
{
    App *a = (App *)ctx;
    note_stack(mt_gate_in_irq(a->g) ? &a->min_stack_irq : &a->min_stack_task);
    a->tick_calls++;
    mt_tick(a->s, mt_os_ms());
}
static void after_cb(void *ctx)
{
    App *a = (App *)ctx;
    uint32_t d = mt_next_deadline(a->s, mt_os_ms());
    a->after_calls++;
    mt_timer_arm_deadline(&a->tm, d);
}
static void tm_cb(void *ctx) { mt_gate_tick(((App *)ctx)->g); }

static void echo_cb(void *user, mt_ip4 from, uint16_t id, uint16_t seq, const uint8_t *data,
                    uint16_t len, uint32_t now)
{
    App *a = (App *)user;
    uint16_t i;
    uint32_t a5, cur = *(volatile uint32_t *)0x904;      /* CurrentA5: the application's A5 */
    __asm__ volatile("move.l %%a5,%0" : "=d"(a5));
    if (mt_gate_in_irq(a->g)) { a->a5_irq_n++; if (a5 != cur) a->a5_irq_bad++; }
    else { a->a5_task_n++; if (a5 != cur) a->a5_task_bad++; }
    (void)now;
    if (from != 0x0A000001u || id != 0x4D54) { a->bad_from++; return; }
    if (len != a->pat_len) { a->bad_payload++; return; }
    for (i = 0; i < len; i++)
        if (data[i] != PAT(i)) { a->bad_payload++; break; }
    a->replies++;
    a->got_seq = seq;
}

/* send one echo from the application thread (the gated path every PBControl takes) */
static int send_echo_to(App *a, mt_ip4 dst, uint16_t seq, uint16_t len)
{
    uint8_t d[1472];
    uint16_t i;
    int r;
    for (i = 0; i < len; i++) d[i] = PAT(i);
    a->pat_len = len;
    MT_GATE_ENTER(a->g);
    r = mt_icmp_echo(a->s, dst, 0x4D54, seq, d, len, mt_os_ms());
    MT_GATE_LEAVE(a->g);
    return r;
}
static int send_echo(App *a, uint16_t seq, uint16_t len)
{
    return send_echo_to(a, 0x0A000001u, seq, len);
}

/* wait for reply `seq`; returns elapsed ticks or -1 on timeout */
static long wait_reply(App *a, uint16_t seq, unsigned long max_ticks)
{
    unsigned long t0 = now_ticks();
    while (now_ticks() - t0 < max_ticks) {
        if (a->replies && a->got_seq == seq) return (long)(now_ticks() - t0);
    }
    return -1;
}

/* ---- timer test ---- */
typedef struct TT { MtTimer t; volatile uint32_t fired, at; volatile uint32_t period_left; uint32_t period; } TT;
static void tt_cb(void *ctx)
{
    TT *x = (TT *)ctx;
    x->fired++;
    x->at = (uint32_t)now_ticks();
    if (x->period_left) {                     /* re-arm from inside the callback */
        x->period_left--;
        mt_timer_arm(&x->t, x->period);
    }
}

static int fails;
static void check(int ok, const char *what)
{
    if (!ok) { fails++; P("FAIL %s\r", what); }
}

int main(void)
{
    char line[160];
    Str255 ps;
    size_t n;
    int i, r;
    mt_limits lim;
    mt_link link;
    mt_ifconfig cfg;
    uint32_t arena_len;
    void *arena;
    static App app;
    static TT tt;
    MtGate *g;
    MtEnet *e;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0);
    InitCursor();
    Create((ConstStringPtr)"\pEnetSpike.out", 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)"\pEnetSpike.out", 0, &outref)) outref = -1;
    else SetEOF(outref, 0);
    P("EnetSpike (glue test)\r");

    mt_limits_default(&lim);
    lim.max_tcp = 2; lim.max_udp = 1; lim.arp_entries = 4; lim.dns_cache = 1;
    arena_len = mt_stack_size(&lim);
    g = (MtGate *)NewPtrClear(sizeof(MtGate));
    e = (MtEnet *)NewPtrClear(sizeof(MtEnet));
    /* obs: NewPtr blocks are only 2-byte aligned on the 68000 Mac; the core wants MT_ARENA_ALIGN (4) */
    arena = NewPtrClear((long)arena_len + 4);
    if (arena) arena = (void *)(((uintptr_t)arena + 3u) & ~(uintptr_t)3u);
    P("sizeof: MtGate %lu MtEnet %lu arena %lu\r", (unsigned long)sizeof(MtGate),
      (unsigned long)sizeof(MtEnet), (unsigned long)arena_len);
    if (!g || !e || !arena) { P("out of memory\rRESULT FAIL\r"); fails++; goto fin; }

    memset(&app, 0, sizeof app);
    app.g = g; app.e = e;
    app.min_stack_irq = app.min_stack_task = 0xFFFFFFFFu;
    /* order matters: the stack exists before the gate can run it (the handler attaches inside
     * mt_enet_open), the address is set once the MAC is known */
    link.ctx = e; link.tx = mt_enet_write;
    app.s = mt_stack_init(arena, arena_len, &lim, &link, (uint32_t)now_ticks());
    if (!app.s) {
        P("mt_stack_init failed (alignment?) arena=%08lx\rRESULT FAIL\r", (unsigned long)arena);
        fails++;
        goto fin;
    }
    mt_gate_init(g, &app, rx_cb, tick_cb, after_cb);
    mt_os_mac_set_gate(g);
    mt_set_echo_cb(app.s, echo_cb, &app);
    mt_timer_init(&app.tm, tm_cb, &app);

    r = mt_enet_open(e, g);
    P("mt_enet_open = %d refnum=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\r", r, e->refnum, e->mac[0],
      e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    if (r) { P("RESULT FAIL\r"); fails++; goto fin; }

    memset(&cfg, 0, sizeof cfg);
    cfg.addr = 0x0A00004Du; cfg.mask = 0xFF000000u; cfg.gateway = 0x0A000001u; cfg.mtu = 1500;
    memcpy(cfg.mac, e->mac, 6);
    MT_GATE_ENTER(g);
    mt_set_config(app.s, &cfg);
    MT_GATE_LEAVE(g);

    /* A: ARP + the first echo */
    {
        unsigned long t0 = now_ticks();
        long rt;
        r = send_echo(&app, 0, 56);
        rt = wait_reply(&app, 0, 120);
        P("A: first echo (includes the ARP exchange): send=%d reply after %ld ticks (%lu ms)\r", r, rt,
          (unsigned long)((now_ticks() - t0) * 1000 / 60));
        check(rt >= 0, "A first echo");
    }

    /* B: 1000 sequential echoes */
    {
        unsigned ok = 0, lost = 0;
        long rt, maxrt = 0, sum = 0;
        unsigned long t0 = now_ticks();
        uint32_t bad0 = app.bad_payload;
        for (i = 1; i <= 1000; i++) {
            r = send_echo(&app, (uint16_t)i, 56);
            rt = wait_reply(&app, (uint16_t)i, 60);
            if (r == 0 && rt >= 0) { ok++; sum += rt; if (rt > maxrt) maxrt = rt; }
            else lost++;
        }
        P("B: 1000 echo round trips: ok=%u lost=%u max=%ld ticks, mean %lu.%02lu ticks, total %lu ticks; bad payloads %lu\r",
          ok, lost, maxrt, (unsigned long)(sum / (ok ? ok : 1)),
          (unsigned long)((sum * 100 / (ok ? ok : 1)) % 100), now_ticks() - t0,
          (unsigned long)(app.bad_payload - bad0));
        check(ok == 1000, "B all 1000 round trips");
        check(app.bad_payload == bad0, "B payload intact");
    }
    FlushVol(0, 0);

    /* C: big echoes */
    {
        unsigned ok = 0;
        for (i = 0; i < 20; i++) {
            r = send_echo(&app, (uint16_t)(2000 + i), 1472);
            if (r == 0 && wait_reply(&app, (uint16_t)(2000 + i), 60) >= 0) ok++;
        }
        P("C: 20 echoes of 1472 bytes (1514-byte frames): ok=%u, bad payloads %lu\r", ok,
          (unsigned long)app.bad_payload);
        check(ok == 20 && app.bad_payload == 0, "C big echoes");
    }

    /* D: burst */
    {
        uint32_t r0 = app.replies, ok0 = e->tx_ok, busy0 = e->tx_drop_busy;
        unsigned sent_ok = 0;
        unsigned long t0;
        for (i = 0; i < 60; i++) if (send_echo(&app, (uint16_t)(3000 + i), 56) == 0) sent_ok++;
        t0 = now_ticks();
        while (now_ticks() - t0 < 120) { }       /* let it settle: 2 s */
        P("D: burst of 60: core sends ok=%u, tx started %lu, tx dropped (all slots busy) %lu, replies %lu\r",
          sent_ok, (unsigned long)(e->tx_ok - ok0), (unsigned long)(e->tx_drop_busy - busy0),
          (unsigned long)(app.replies - r0));
        check(app.replies - r0 == e->tx_ok - ok0, "D every started echo came back");
    }
    FlushVol(0, 0);

    /* E: timer */
    {
        static const unsigned ms_list[5] = { 10, 50, 100, 250, 1000 };
        memset(&tt, 0, sizeof tt);
        mt_timer_init(&tt.t, tt_cb, &tt);
        for (i = 0; i < 5; i++) {
            unsigned long t0 = now_ticks();
            uint32_t f0 = tt.fired;
            long el;
            unsigned long want = (ms_list[i] * 60UL + 999) / 1000;
            mt_timer_arm(&tt.t, ms_list[i]);
            while (tt.fired == f0 && now_ticks() - t0 < 300) { }
            el = (long)(tt.at - t0);
            P("E: arm %u ms -> fired after %ld ticks (expect %lu..%lu)\r", ms_list[i], el, want, want + 1);
            check(tt.fired == f0 + 1 && el >= (long)want - 1 && el <= (long)want + 2,
                  "E deadline within a tick");
        }
        {   /* re-arm replaces the pending shot */
            uint32_t f0 = tt.fired;
            unsigned long t0 = now_ticks();
            mt_timer_arm(&tt.t, 500);
            mt_timer_arm(&tt.t, 50);
            while (tt.fired == f0 && now_ticks() - t0 < 100) { }
            while (now_ticks() - t0 < 80) { }     /* 1.3 s: the 500 ms shot must not fire */
            P("E: re-arm 500 -> 50: fired %lu time(s) at tick +%lu\r", (unsigned long)(tt.fired - f0),
              (unsigned long)(tt.at - t0));
            check(tt.fired == f0 + 1, "E re-arm replaces");
        }
        {   /* cancel */
            uint32_t f0 = tt.fired;
            unsigned long t0 = now_ticks();
            mt_timer_arm(&tt.t, 100);
            mt_timer_cancel(&tt.t);
            while (now_ticks() - t0 < 30) { }
            P("E: arm 100 then cancel: fired %lu time(s)\r", (unsigned long)(tt.fired - f0));
            check(tt.fired == f0, "E cancel");
        }
        {   /* periodic re-arm from the callback: 20 x 100 ms */
            uint32_t f0 = tt.fired;
            unsigned long t0 = now_ticks(), el;
            tt.period = 100; tt.period_left = 19;
            mt_timer_arm(&tt.t, 100);
            while (tt.fired - f0 < 20 && now_ticks() - t0 < 400) { }
            el = now_ticks() - t0;
            P("E: 20 periodic 100 ms re-arms from the callback took %lu ticks (ideal 120)\r", el);
            check(tt.fired - f0 == 20 && el >= 118 && el <= 130, "E periodic");
        }
    }

    /* G: the tick path through the real timer: an echo to an address nobody answers makes the
     * core ARP at +0, +1 s and +2 s (timer driven) and give up at +3 s */
    {
        uint32_t arp0 = mt_get_stats(app.s)->arp_out, t0n = g->n_tick;
        unsigned long t0 = now_ticks();
        send_echo_to(&app, 0x0A000063u, 9000, 56);
        while (now_ticks() - t0 < 240) { }
        P("G: unanswered ARP: %lu requests sent (expect 3), gate ticks run %lu\r",
          (unsigned long)(mt_get_stats(app.s)->arp_out - arp0), (unsigned long)(g->n_tick - t0n));
        check(mt_get_stats(app.s)->arp_out - arp0 == 3, "G three ARP requests from the timer");
        check(g->n_tick - t0n >= 2, "G timer ticks reached the core");
    }

    /* F: counters */
    {
        const mt_stats *st = mt_get_stats(app.s);
        P("F: gate: rx run %lu, deferred %lu, ticks run %lu (deferred %lu), after %lu, ring drops %u, max ring %lu, max depth %lu\r",
          (unsigned long)g->n_rx, (unsigned long)g->n_deferred, (unsigned long)g->n_tick,
          (unsigned long)g->n_tick_deferred, (unsigned long)g->n_after, g->drops,
          (unsigned long)g->max_ring, (unsigned long)g->max_depth);
        P("F: enet: tx ok %lu pad %lu busy-drop %lu len-drop %lu err %lu, in flight %d, last_err %d\r",
          (unsigned long)e->tx_ok, (unsigned long)e->tx_pad, (unsigned long)e->tx_drop_busy,
          (unsigned long)e->tx_drop_len, (unsigned long)e->tx_err, mt_enet_tx_busy(e), e->last_err);
        P("F: core: eth_in %lu eth_out %lu eth_tx_err %lu arp_in %lu icmp_in %lu ip_in_drops %lu\r",
          (unsigned long)st->eth_in, (unsigned long)st->eth_out, (unsigned long)st->eth_tx_err,
          (unsigned long)st->arp_in, (unsigned long)st->icmp_in, (unsigned long)st->ip_in_drops);
        P("F: stack space left (min): at interrupt time %lu, at task time %lu; ticks run %lu, after calls %lu\r",
          (unsigned long)app.min_stack_irq, (unsigned long)app.min_stack_task,
          (unsigned long)app.tick_calls, (unsigned long)app.after_calls);
        P("F: A5 in the echo callback vs CurrentA5: interrupt time %lu checks, %lu wrong; task time %lu checks, %lu wrong\r",
          (unsigned long)app.a5_irq_n, (unsigned long)app.a5_irq_bad, (unsigned long)app.a5_task_n,
          (unsigned long)app.a5_task_bad);
        P("F: enet: tx slots %u, sizeof MtEnet %lu, closed-drops %lu\r", (unsigned)MT_ENET_TXSLOTS,
          (unsigned long)sizeof(MtEnet), (unsigned long)e->tx_drop_closed);
        check(app.a5_irq_n > 0 && app.a5_irq_bad == 0 && app.a5_task_bad == 0, "F A5 intact in callbacks");
        check(e->tx_err == 0, "F no tx errors");
        check(g->drops == 0, "F no ring drops");
    }

    mt_timer_close(&tt.t);
    mt_timer_close(&app.tm);
    mt_enet_close(e);
    P("closed; handlers detached\r");
    P("RESULT %s\r", fails ? "FAIL" : "PASS");

fin:
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
    sprintf(line, "EnetSpike %s. Results are in EnetSpike.out", fails ? "FAIL" : "finished");
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
