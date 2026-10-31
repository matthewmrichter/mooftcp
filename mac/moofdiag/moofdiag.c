/* moofdiag.c - MoofDiag: what the MoofTCP driver looks like from an application (Task 10).
 *
 * Writes MoofDiag.out (CR separated) beside the app:
 *   - the unit table: size, every installed driver with its unit, refnum, name and DCE flags;
 *   - OpenDriver(".IPP"): result and refnum;
 *   - if the driver is open, its resident block through the private Status call 'MT'
 *     (mt_drv.h MT_DRV_STATUS_RES): boot results, configuration, entry counters, gate, Ethernet
 *     and core counters, and the system-heap figures (FreeMem / zone size).
 * Last line: "RESULT PASS" when .IPP opened with refnum -49 and the block was readable, else
 * "RESULT FAIL" (or "RESULT UNCONFIGURED" when the open failed with -23004). */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Files.h>
#include <Devices.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "mt.h"
#include "mt_drv.h"

QDGlobals qd;

#pragma GCC diagnostic ignored "-Warray-bounds"
#define UTABLE_BASE (*(uint8_t ***volatile *)0x11C)
#define UNIT_COUNT  (*(volatile int16_t *)0x1D2)
#define SYS_ZONE    (*(THz *)0x2A6)

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

static uint32_t s24(const void *p) { return (uint32_t)(uintptr_t)p & 0x00FFFFFFu; }
static void ip(char *b, uint32_t a)
{
    sprintf(b, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24), (unsigned long)(a >> 16 & 255),
            (unsigned long)(a >> 8 & 255), (unsigned long)(a & 255));
}

static void units(void)
{
    int16_t i, n = UNIT_COUNT;
    P("unit table: %d entries at %08lx\r", n, (unsigned long)s24(UTABLE_BASE));
    for (i = 0; i < n; i++) {
        uint8_t **h = UTABLE_BASE[i];
        const uint8_t *dce, *drv;
        uint16_t fl;
        char name[40];
        if (!h || !*h) continue;
        dce = (const uint8_t *)s24(*h);
        fl = (uint16_t)(dce[4] << 8 | dce[5]);
        drv = (const uint8_t *)s24(*(void *const *)dce);
        if ((fl & 0x40) && drv) drv = *(void *const *)drv ? (const uint8_t *)s24(*(void *const *)drv) : NULL;
        name[0] = 0;
        if (drv) {
            uint8_t k = drv[18] < 39 ? drv[18] : 39;
            memcpy(name, drv + 19, k);
            name[k] = 0;
        }
        P("  unit %2d refnum %4d flags %04x driver %08lx %s\r", i, -(i + 1), fl,
          (unsigned long)s24(drv), name);
    }
}

int main(void)
{
    char line[160], a1[20], a2[20], a3[20], a4[20];
    Str255 ps;
    size_t n;
    short ref = 0;
    OSErr r;
    int pass = 0, unconf = 0;
    THz save;
    long sysfree, syszone;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0);
    InitCursor();
    Create((ConstStringPtr)"\pMoofDiag.out", 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)"\pMoofDiag.out", 0, &outref)) outref = -1;
    else SetEOF(outref, 0);
    P("MoofDiag\r");

    save = GetZone();
    SetZone(SYS_ZONE);
    sysfree = FreeMem();
    syszone = (long)((char *)SYS_ZONE->bkLim - (char *)SYS_ZONE);
    SetZone(save);
    P("system zone: size %ld free %ld occupied %ld\r", syszone, sysfree, syszone - sysfree);

    units();
    r = OpenDriver((ConstStringPtr)"\p.IPP", &ref);
    P("OpenDriver(.IPP) = %d refnum %d\r", r, ref);
    save = GetZone();
    SetZone(SYS_ZONE);
    sysfree = FreeMem();
    SetZone(save);
    P("system zone after open: size %ld free %ld occupied %ld (no-network baseline: size 162100, occupied 149248)\r",
      syszone, sysfree, syszone - sysfree);
    if (r == -23004) unconf = 1;
    if (r == 0) {
        CntrlParam pb;
        MtRes *m;
        memset(&pb, 0, sizeof pb);
        pb.ioCRefNum = ref;
        pb.csCode = MT_DRV_STATUS_RES;
        r = PBStatusSync((ParmBlkPtr)&pb);
        memcpy(&m, &pb.csParam[0], 4);
        P("Status('MT') = %d block %08lx\r", r, (unsigned long)s24(m));
        if (r == 0 && m) {
            const mt_stats *st;
            mt_stack *s;
            P("boot: refnum %d cfg_result %d enet_err %d mode %u size %lu raw %08lx\r", m->refnum,
              m->cfg_result, m->enet_err, m->mode, (unsigned long)m->size, (unsigned long)s24(m->raw));
            ip(a1, m->cfg.addr); ip(a2, m->cfg.mask); ip(a3, m->cfg.gateway); ip(a4, m->cfg.dns);
            P("config: addr %s mask %s gateway %s dns %s domain '%s' mtu %u\r", a1, a2, a3, a4,
              m->cfg.domain, m->cfg.mtu);
            P("entries: calls %lu refused %lu pending-run %lu queued %lu queue-full %lu goodbye %lu\r",
              (unsigned long)m->entry.n_calls, (unsigned long)m->entry.n_refused,
              (unsigned long)m->entry.n_pend_run, (unsigned long)m->entry.pend.queued,
              (unsigned long)m->entry.pend.full, (unsigned long)m->entry.n_goodbye);
            P("entries: adopted %lu goodbye-forced %lu refusal-ioResult-only %lu dropped-at-goodbye %lu\r",
              (unsigned long)m->entry.n_adopted, (unsigned long)m->entry.n_goodbye_forced,
              (unsigned long)m->entry.n_compl_skipped, (unsigned long)m->entry.n_pend_dropped);
            P("gate: rx %lu deferred %lu ticks %lu (deferred %lu) after %lu drops %u max ring %lu max depth %lu\r",
              (unsigned long)m->gate.n_rx, (unsigned long)m->gate.n_deferred,
              (unsigned long)m->gate.n_tick, (unsigned long)m->gate.n_tick_deferred,
              (unsigned long)m->gate.n_after, m->gate.drops, (unsigned long)m->gate.max_ring,
              (unsigned long)m->gate.max_depth);
            P("enet: refnum %d mac %02x:%02x:%02x:%02x:%02x:%02x tx ok %lu pad %lu busy %lu len %lu closed %lu err %lu last_err %d\r",
              m->enet.refnum, m->enet.mac[0], m->enet.mac[1], m->enet.mac[2], m->enet.mac[3],
              m->enet.mac[4], m->enet.mac[5], (unsigned long)m->enet.tx_ok,
              (unsigned long)m->enet.tx_pad, (unsigned long)m->enet.tx_drop_busy,
              (unsigned long)m->enet.tx_drop_len, (unsigned long)m->enet.tx_drop_closed,
              (unsigned long)m->enet.tx_err, m->enet.last_err);
            P("timer: fired %lu armed %u\r", (unsigned long)m->timer.fired, m->timer.armed);
            memcpy(&s, m->d, sizeof s);                  /* MtDrv's first member: mt_stack *s */
            st = mt_get_stats(s);
            P("core: eth_in %lu eth_out %lu tx_err %lu arp_in %lu arp_out %lu ip_in %lu ip_out %lu tcp_in %lu tcp_out %lu rexmt %lu udp_in %lu udp_out %lu\r",
              (unsigned long)st->eth_in, (unsigned long)st->eth_out, (unsigned long)st->eth_tx_err,
              (unsigned long)st->arp_in, (unsigned long)st->arp_out, (unsigned long)st->ip_in,
              (unsigned long)st->ip_out, (unsigned long)st->tcp_in, (unsigned long)st->tcp_out,
              (unsigned long)st->tcp_rexmt, (unsigned long)st->udp_in, (unsigned long)st->udp_out);
            P("sizes: MtRes %lu MtGate %lu MtEnet %lu MtTimer %lu\r", (unsigned long)sizeof(MtRes),
              (unsigned long)sizeof(MtGate), (unsigned long)sizeof(MtEnet), (unsigned long)sizeof(MtTimer));
            pass = ref == -49;
        }
    }
    P("RESULT %s\r", pass ? "PASS" : unconf ? "UNCONFIGURED" : "FAIL");
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
    sprintf(line, "MoofDiag %s. Results are in MoofDiag.out", pass ? "finished" : "FAIL");
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
