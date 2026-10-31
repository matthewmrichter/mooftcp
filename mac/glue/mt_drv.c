/* mt_drv.c - the .IPP driver: boot (called once by the INIT), the five Device Manager entries,
 * the gate callbacks and the mt_os.h services that call application code.
 *
 * Code layout (mac/moof/moof.r): the DRVR resource is a 48-byte header + entry stubs written in
 * Rez, followed by this flat Retro68 code block; mt_drv_stubs.s holds `_start`, the dispatcher
 * the stubs reach (selector in D0), and the routines that call completion routines and ASRs.
 * The INIT (mac/init/mt_init.c) detaches and locks the DRVR resource in the system heap and calls
 * the boot stub, which relocates the code in place (it never moves again); mt_drv_boot allocates
 * everything, and installs the driver in the unit table as a pointer-based driver (refnum -49).
 *
 * Boot order (binding): stack (mt_pb_init) -> gate (mt_gate_init, mt_os_mac_set_gate) -> timer
 * -> unit table. The first OpenDriver(".IPP") then opens .ENET0 (frames may arrive from there on),
 * applies the configuration and arms the timer (link_up: .ENET0 cannot run at INIT time).
 *
 * Control entry: the PB (and any PB queued behind it) is taken off the driver queue here; the
 * rest (routing, queued and refused PBs, goodbye, KillIO, the mt_pb.h result contract) is
 * mt_pb_entry.c, host-tested. */
#include <string.h>
#include "mt_pb.h"
#include "mt_gate.h"
#include "mt_enet.h"
#include "mt_timer.h"
#include "mt_os_mac.h"
#include "mt_cfg.h"
#include "mt_drv.h"
#include "mt_drv_logic.h"
#include "mt_pb_entry.h"
#include "mt_drv_mac.h"

#pragma GCC diagnostic ignored "-Warray-bounds"   /* low-memory globals */
#define LM_CURRENT_A5 (*(volatile uint32_t *)0x904)
#define LM_TICKS      (*(volatile uint32_t *)0x16A)

/* mt_drv_stubs.s */
extern void mt_drv_call_completion(uint32_t proc, void *pb, uint32_t a5);
extern void mt_drv_call_asr_tcp(uint32_t proc, uint32_t a5, uint32_t stream, uint32_t ev,
                                uint32_t user, uint32_t reason, uint32_t icmp);
extern void mt_drv_call_asr_udp(uint32_t proc, uint32_t a5, uint32_t stream, uint32_t ev,
                                uint32_t user, uint32_t icmp);

static MtRes *g_res;               /* the one resident block (system heap) */

MtRes *mt_drv_res(void) { return g_res; }

/* ---- small helpers ---- */

static inline int sr_irq(void) {
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    return (sr & 0x0700u) != 0;
}

static uint32_t stashed_a5(const MtAnyPb *pb) {
    uint32_t a5 = mt_entry_stashed_a5(pb);
    return a5 ? a5 : LM_CURRENT_A5;
}

/* ---- mt_os.h: calls into application code (glue side: mt_pb.c) ---- */

MacPtr32 mt_os_a5(void) { return LM_CURRENT_A5; }

void mt_os_complete(MtAnyPb *pb, int16_t err) {
    MacPtr32 proc;
    pb->hdr.ioResult = err;              /* final before the routine runs (matrix 150) */
    proc = pb->hdr.ioCompletion;
    if (proc) mt_drv_call_completion(proc, pb, stashed_a5(pb));
}

void mt_os_asr(const MtStreamAsr *a, uint16_t event, uint16_t reason, MacPtr32 icmp) {
    uint32_t a5 = a->a5 ? a->a5 : LM_CURRENT_A5;
    if (a->kind == MT_ASR_UDP)
        mt_drv_call_asr_udp(a->proc, a5, a->stream, event, a->user, icmp);
    else
        mt_drv_call_asr_tcp(a->proc, a5, a->stream, event, a->user, reason, icmp);
}

/* ---- gate callbacks (the owner runs them; see mt_gate.h) ---- */

static void rx_cb(void *ctx, const uint8_t *frame, uint16_t len) {
    mt_pb_input(((MtRes *)ctx)->d, frame, len);
}

static void tick_cb(void *ctx) {
    MtRes *r = (MtRes *)ctx;
    mt_entry_drain(&r->entry);
    mt_pb_tick(r->d);
}

static void after_cb(void *ctx) {
    MtRes *r = (MtRes *)ctx;
    mt_entry_drain(&r->entry);
    mt_timer_arm_deadline(&r->timer, mt_pb_next_deadline_ms(r->d));
}

static void tm_cb(void *ctx) { mt_gate_tick(&((MtRes *)ctx)->gate); }

/* ---- boot (from the INIT, once) ---- */

static long boot(void *drvr);

long mt_drv_boot(void *drvr) {
    void *z = mt_mac_zone_sys();                 /* everything the boot causes lives in the system heap */
    long r = boot(drvr);
    mt_mac_zone_restore(z);
    return r;
}

static long boot(void *drvr) {
    mt_limits lim;
    mt_link link;
    mt_ifconfig cfg;
    uint8_t mode = 0;
    uint32_t len, off, ea, eb;
    uint8_t *raw;
    MtRes *r;
    int16_t ref;
    int cr, e;

    /* the boot stub (mt_drv_stubs.s) has already relocated the code in place */
    if (g_res) return MT_BOOT_AGAIN;
    if (mt_mac_unit_find((const uint8_t *)"\p.ipp")) return MT_BOOT_TAKEN;   /* MacTCP is there */

    cr = mt_cfg_load(&cfg, &mode);
    mt_limits_default(&lim);
    off = ((uint32_t)sizeof(MtRes) + 7u) & ~7u;
    len = off + mt_pb_size(&lim);
    raw = (uint8_t *)mt_mac_alloc_sys(len + 4u);
    if (!raw) return MT_BOOT_NOMEM;
    r = (MtRes *)(((uintptr_t)raw + 3u) & ~(uintptr_t)3u);   /* NewPtr is only 2-aligned (obs) */
    r->raw = raw;
    r->size = len + 4u;
    r->cfg_result = (int16_t)cr;
    r->mode = mode;

    link.ctx = &r->enet;
    link.tx = mt_enet_write;
    r->d = mt_pb_init((uint8_t *)r + off, len - off, &lim, &link, LM_TICKS);
    if (!r->d) { mt_mac_free(raw); return MT_BOOT_NOMEM; }
    mt_mac_entropy(&ea, &eb);
    mt_set_entropy(r->d->s, ea, eb);

    g_res = r;
    mt_gate_init(&r->gate, r, rx_cb, tick_cb, after_cb);
    mt_entry_init(&r->entry, r->d, &r->gate);
    mt_os_mac_set_gate(&r->gate);
    mt_timer_init(&r->timer, tm_cb, r);

    if (cr == MT_CFG_OK && cfg.addr) r->cfg = cfg;  /* applied at the first open (MAC known) */

    e = mt_mac_unit_install(drvr, MT_DRV_UNIT, &ref);
    if (e) {
        r->enet_err = (int16_t)e;
        mt_timer_close(&r->timer);
        mt_os_mac_set_gate(0);
        g_res = 0;
        mt_mac_free(raw);
        return MT_BOOT_NOUNIT;
    }
    r->refnum = ref;
    return r->cfg.addr ? MT_BOOT_OK : MT_BOOT_UNCONFIGURED;
}

/* The first successful open brings the link up: .ENET0 cannot be opened at INIT time (obs, Task 10:
 * the DaynaPORT driver's poll reads through CurStackBase, which is -1 until the first application
 * runs: address error at its offset 0x19F8). MacTCP also opens its LAP at driver open. */
static int16_t link_up(MtRes *r) {
    void *z;
    int e;
    if (r->enet.opened) return 0;
    z = mt_mac_zone_sys();                       /* the Ethernet driver allocates in the current zone */
    e = mt_enet_open(&r->enet, &r->gate);
    mt_mac_zone_restore(z);
    r->enet_err = (int16_t)e;
    if (e) return ipBadLapErr;
    memcpy(r->cfg.mac, r->enet.mac, 6);
    mt_pb_set_config(r->d, &r->cfg);             /* gate enter/leave: arms the timer too */
    mt_timer_arm_deadline(&r->timer, mt_pb_next_deadline_ms(r->d));
    return 0;
}

/* ---- Device Manager entries (from mt_drv_stubs.s: A0 = PB, A1 = DCE) ---- */

int16_t mt_drv_open(void *pb, void *dce) {
    (void)pb; (void)dce;
    /* matrix "Unconfigured run": with no address the open fails with ipBadAddr and the driver
     * stays closed (every call then gets the Device Manager's -28) */
    if (!g_res || !g_res->cfg.addr) return ipBadAddr;
    return link_up(g_res);
}

int16_t mt_drv_close(void *pb, void *dce) {
    (void)pb; (void)dce;
    return -24;                                  /* closeErr: matrix 999, the driver stays open */
}

int16_t mt_drv_prime(void *pb, void *dce) {
    (void)pb; (void)dce;
    return -36;                                  /* ioErr: .IPP has no read/write */
}

int16_t mt_drv_status(void *vpb, void *dce) {
    MtAnyPb *pb = (MtAnyPb *)vpb;
    (void)dce;
    if (pb->hdr.csCode == MT_DRV_STATUS_RES && g_res) {   /* private: diagnostics (MoofDiag) */
        uint32_t p = (uint32_t)(uintptr_t)g_res;
        memcpy((uint8_t *)pb + 28, &p, 4);
        return 0;
    }
    return -18;                                  /* statusErr: .IPP answers only Control */
}

int16_t mt_drv_control(void *vpb, void *dce) {
    MtAnyPb *pb = (MtAnyPb *)vpb;
    MtRes *r = g_res;
    void *extra[MT_DRV_PENDQ];
    int n = 0, i;
    int16_t res;

    /* contract 1: several PBs pending at once. Every queued call is taken off, whatever its
     * csCode (a PBControl with csCode 1 left queued kept drvrActive set and hung every later call:
     * ApiProbe sweep 120.1, Task 10); an immediate call (KillIO) was never queued. PBs the Device
     * Manager queued behind ours while drvrActive was set are taken off as well and run after
     * ours, as calls made at interrupt time (they can only have come from there). */
    if (!(mt_entry_trap(pb) & MT_DRV_TRAP_IMMED))
        n = mt_mac_dequeue(pb, dce, extra, (int)MT_DRV_PENDQ);
    if (!r) { pb->hdr.ioResult = MT_PB_NOTOPEN; return MT_PB_NOTOPEN; }
    res = mt_entry_control(&r->entry, pb, mt_entry_trap(pb), sr_irq());
    for (i = 0; i < n; i++) {
        MtAnyPb *x = (MtAnyPb *)extra[i];
        r->entry.n_adopted++;
        (void)mt_entry_control(&r->entry, x, mt_entry_trap(x), 1);
    }
    return res;
}
