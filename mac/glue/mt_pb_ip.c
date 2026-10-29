/* mt_pb_ip.c - MacTCP IP-level calls: GetMyIPAddr (15), ICMPEcho (17), LAPStats (19).
 *
 * ICMPEcho (matrix A00..A03, "ICMP echo notify"): the app's PB completes with 0 as soon as the
 * request has been handed to the core (MacTCP completes it when the request left the machine,
 * which for an unresolved host is after the ARP wait; ours does not wait). The real outcome comes
 * through the PB's icmpCompletion routine, called like an ioCompletion routine (mt_os_complete)
 * with the driver's OWN copy of the block (MtEcho.blk): ioResult 0 for a reply, -23035 when no
 * reply came within `timeout` seconds; icmpEchoInfo holds the request and reply times in ticks
 * (1/60 s; 0 reply time for a timeout) and echoedData the first MT_PB_ECHO_DATA reply bytes.
 * The slot stays reserved while the routine runs, so the routine may issue another ICMPEcho.
 * Replies are matched on (id, seq, source address); id carries the slot, seq a driver counter. */
#include <string.h>
#include <stddef.h>
#include "mt_pb.h"

#define ECHO_WAIT 1
#define ECHO_NOTIFY 2

/* Milliseconds -> ticks (60 Hz): floor(ms * 3 / 50) without overflowing 32 bits. */
static uint32_t ms_ticks(uint32_t ms) { return ms / 50u * 3u + (ms % 50u) * 3u / 50u; }

static int due(uint32_t dl, uint32_t now) { return (int32_t)(dl - now) <= 0; }

/* ---- ICMPEcho ---- */

static void notify(MtDrv *d, MtEcho *e, int16_t err, uint32_t reply_ms, const uint8_t *data,
                   uint16_t len) {
    ICMPEchoInfo *info = &e->blk.icmpEchoInfo;
    MacPtr32 proc = e->blk.ioCompletion;      /* the app's icmpCompletion (copied at the request) */
    e->state = ECHO_NOTIFY;
    if (!proc) {
        e->state = 0;
        return;
    }
    info->echoRequestOut = ms_ticks(e->t0);
    info->echoReplyIn = err == 0 ? ms_ticks(reply_ms) : 0;
    info->echoedData.length = 0;
    info->echoedData.ptr = 0;
    info->options = 0;
    if (err == 0 && len) {
        if (len > MT_PB_ECHO_DATA) len = MT_PB_ECHO_DATA;
        memcpy(d->echo_data, data, len);       /* the frame is only valid during the core callback */
        info->echoedData.length = len;
        info->echoedData.ptr = mt_os_addr(d->echo_data);
    }
    mt_pb_call_complete(d, (MtAnyPb *)&e->blk, err);
    e->state = 0;
}

static void echo_cb(void *user, mt_ip4 from, uint16_t id, uint16_t seq, const uint8_t *data,
                    uint16_t len, uint32_t now) {
    MtDrv *d = (MtDrv *)user;
    uint8_t i;
    for (i = 0; i < MT_PB_ECHOQ; i++) {
        MtEcho *e = &d->echo[i];
        if (e->state == ECHO_WAIT && e->id == id && e->seq == seq && e->dest == from) {
            notify(d, e, 0, now, data, len);
            return;
        }
    }
}

static int16_t do_echo(MtDrv *d, MtAnyPb *pb) {
    mt_ip4 dest = pb->ip.csParam.IPEchoPB.dest;
    wdsEntry data = pb->ip.csParam.IPEchoPB.data;
    int16_t tmo = pb->ip.csParam.IPEchoPB.timeout;
    MtEcho *e = NULL;
    uint8_t i;
    int r;
    for (i = 0; i < MT_PB_ECHOQ; i++)
        if (d->echo[i].state == 0) { e = &d->echo[i]; break; }
    if (!e) return insufficientResources;
    if (data.length && !data.ptr) return invalidBufPtr;
    if (!mt_pb_dest_ok(dest)) return ipBadAddr;
    d->echo_seq++;
    r = mt_icmp_echo(d->s, dest, (uint16_t)(0x4D00u + i), d->echo_seq,
                     (const uint8_t *)mt_os_ptr(data.ptr), data.length, d->now);
    if (r == MT_ERR_INVAL) return invalidLength;             /* more than one frame */
    if (r == MT_ERR_NOMEM) return insufficientResources;
    if (r != MT_OK) return ipRouteErr;
    memset(e, 0, sizeof *e);
    /* MacTCP's own copy of the block: only the IPParamBlock part (28-byte header + IPEchoPB = 54
     * bytes) is read from the app's PB, which may be exactly that big; the rest stays zero
     * (icmpEchoInfo is the driver's to fill; the app's own copy of it stays 0, matrix ICMP echo notify). */
    memcpy(&e->blk, pb, offsetof(IPParamBlock, csParam) + sizeof(IPEchoPB));
    e->blk.ioCompletion = pb->ip.csParam.IPEchoPB.icmpCompletion;
    e->id = (uint16_t)(0x4D00u + i);
    e->seq = d->echo_seq;
    e->dest = dest;
    e->t0 = d->now;
    e->dl = d->now + (tmo > 0 ? (uint32_t)tmo : (uint32_t)MT_PB_ECHO_TMO_S) * 1000u;
    e->state = ECHO_WAIT;
    return 0;                                                /* matrix A00: our PB completes 0 */
}

void mt_pb_ip_timers(MtDrv *d) {
    uint8_t i;
    for (i = 0; i < MT_PB_ECHOQ;) {
        MtEcho *e = &d->echo[i];
        if (e->state == ECHO_WAIT && due(e->dl, d->now)) {
            notify(d, e, icmpEchoTimeoutErr, 0, NULL, 0);    /* matrix A02, A03: -23035 */
            i = 0;                                           /* the routine may have changed anything */
        } else {
            i++;
        }
    }
}

uint32_t mt_pb_ip_deadline(MtDrv *d) {
    uint32_t best = MT_NO_DEADLINE;
    uint8_t i;
    for (i = 0; i < MT_PB_ECHOQ; i++) {
        const MtEcho *e = &d->echo[i];
        if (e->state == ECHO_WAIT) {
            uint32_t r = due(e->dl, d->now) ? 0 : e->dl - d->now;
            if (r < best) best = r;
        }
    }
    return best;
}

void mt_pb_ip_init(MtDrv *d) {
    mt_set_echo_cb(d->s, echo_cb, d);
    d->lap_if[0] = 8;                         /* a Pascal string; the NUL after it also reads as C */
    memcpy(d->lap_if + 1, "Ethernet", 8);
}

/* ---- GetMyIPAddr, LAPStats ---- */

static int16_t do_getaddr(MtDrv *d, MtAnyPb *pb) {
    mt_ifconfig c;
    mt_get_config(d->s, &c);
    pb->addr.ourAddress = c.addr;                            /* matrix 110 */
    pb->addr.ourNetMask = (int32_t)c.mask;
    return 0;
}

static int16_t do_lapstats(MtDrv *d, MtAnyPb *pb) {
    uint8_t *dst = (uint8_t *)mt_os_ptr(pb->ip.csParam.LAPStatsPB.lapStatsPtr);
    mt_ifconfig c;
    LAPStats ls;
    if (!dst) return invalidBufPtr;
    mt_get_config(d->s, &c);
    memcpy(d->lap_mac, c.mac, 6);
    memset(&ls, 0, sizeof ls);                               /* matrix 160 (Environment): */
    ls.ifType = 6;                                           /* Ethernet */
    ls.ifString = mt_os_addr(d->lap_if);                     /* (a guess: the matrix has no string) */
    ls.ifMaxMTU = (int16_t)c.mtu;
    ls.ifSpeed = 10000000;
    ls.ifPhyAddrLength = 6;
    ls.ifPhysicalAddress = mt_os_addr(d->lap_mac);
    ls.AddrXlation = mt_os_addr(d->arp);
    ls.slotNumber = -1;
    memcpy(dst, &ls, sizeof ls);                             /* the buffer may be unaligned */
    return 0;
}

int16_t mt_pb_ip_control(MtDrv *d, MtAnyPb *pb) {
    switch (pb->hdr.csCode) {
    case ipctlGetAddr:  return do_getaddr(d, pb);
    case ipctlEchoICMP: return do_echo(d, pb);
    case ipctlLAPStats: return do_lapstats(d, pb);
    default:            return MT_PB_UNIMP;
    }
}
