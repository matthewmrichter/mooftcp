/* mt_enet_wd.c - transmit slot ownership and the stuck-write watchdog (see mt_enet.h). Plain C,
 * no Toolbox: the host tests (tests/test_enet_watchdog.c) build it against tests/pbfake.c. */
#include <stddef.h>
#include "mt_enet.h"
#include "mt_os.h"

#define NO_DEADLINE 0xFFFFFFFFu

/* A finished slot (ioResult <= 0) that was started: count its error once, mark it free. */
static void reap(MtEnet *e, MtEnetTx *c)
{
    if (c->pb.ioResult < 0) { e->tx_err++; e->last_err = c->pb.ioResult; }
    if (c->wd_stage < 2 && e->tx_started) e->tx_started--;   /* quarantined ones left the count */
    c->started = 0;
    c->wd_stage = 0;
    c->wd_seen = 0;
}

MtEnetTx *mt_enet_tx_claim(MtEnet *e)
{
    unsigned i;
    for (i = 0; i < MT_ENET_TXSLOTS; i++) {
        unsigned k = (e->tx_next + i) % MT_ENET_TXSLOTS;
        MtEnetTx *c = &e->tx[k];
        if (c->pb.ioResult > 0) continue;        /* the driver owns it: never touched */
        if (c->started) reap(e, c);
        e->tx_next = (uint8_t)((k + 1u) % MT_ENET_TXSLOTS);
        return c;
    }
    return NULL;
}

int mt_enet_tx_busy(const MtEnet *e)
{
    unsigned i;
    int n = 0;
    for (i = 0; i < MT_ENET_TXSLOTS; i++) if (e->tx[i].pb.ioResult > 0) n++;
    return n;
}

int mt_enet_tx_quarantined(const MtEnet *e)
{
    unsigned i;
    int n = 0;
    for (i = 0; i < MT_ENET_TXSLOTS; i++)
        if (e->tx[i].pb.ioResult > 0 && e->tx[i].wd_stage == 2) n++;
    return n;
}

uint32_t mt_enet_wd_deadline(const MtEnet *e, uint32_t d)
{
    if (!e->tx_started) return d;
    return (d == NO_DEADLINE || d > MT_ENET_WD_SCAN_MS) ? MT_ENET_WD_SCAN_MS : d;
}

void mt_enet_init(MtEnet *e)
{
    uint8_t *p = (uint8_t *)e;
    size_t n = sizeof *e;
    while (n--) *p++ = 0;
}

void mt_enet_link_reset(MtEnet *e)
{
    unsigned i;
    e->gate = NULL;
    e->refnum = 0;
    for (i = 0; i < 6; i++) e->mac[i] = 0;
    e->attached = 0;
    e->opened = 0;
    e->reattach_req = 0;
    e->reattach_tries = 0;
    e->blocked_tries = 0;
    /* tx[], tx_started, wd_* and the counters stay: the driver may still own PBs in tx[] */
}

void mt_enet_watchdog(MtEnet *e, uint32_t now)
{
    unsigned i;
    if (!e->tx_started) return;                  /* idle: nothing handed to the driver */
    if (!e->wd_fresh && (int32_t)(now - e->wd_next) < 0) return;
    e->wd_fresh = 0;
    e->wd_next = now + MT_ENET_WD_SCAN_MS;
    for (i = 0; i < MT_ENET_TXSLOTS; i++) {
        MtEnetTx *c = &e->tx[i];
        uint32_t age;
        if (!c->started) continue;
        if (c->pb.ioResult <= 0) { reap(e, c); continue; }
        if (!c->wd_seen) { c->wd_seen = 1; c->wd_t0 = now; continue; }
        age = now - c->wd_t0;                    /* modular: right across the 2^32 wrap */
        if (age < MT_ENET_TX_STUCK_MS) continue;
        if (c->wd_stage == 0) {
            e->tx_stuck++;
            e->kill_req = 1;                     /* performed by mt_enet_service at task time */
            c->wd_stage = 1;
            c->wd_t0 = now;
        } else if (c->wd_stage == 1) {
            c->wd_stage = 2;                     /* quarantined: stays started until it completes */
            if (e->tx_started) e->tx_started--;
            if (e->opened && !e->reattach_req) {
                e->reattach_req = 1;
                e->reattach_tries = 0;
                e->reattach_at = now;
            }
        }
    }
}

void mt_enet_service(MtEnet *e, uint32_t now)
{
    int16_t r;
    if (e->kill_req) {
        e->kill_req = 0;
        e->tx_killed++;
        r = mt_os_enet_kill(e->refnum, NULL);
        if (r) e->last_err = r;
        if (e->reattach_req) { e->reattach_at = now + MT_ENET_TX_STUCK_MS; return; }   /* let the kill work */
    }
    if (!e->reattach_req || (int32_t)(now - e->reattach_at) < 0) return;
    if (e->opened && !mt_enet_tx_quarantined(e)) {      /* the kill (or the driver) freed them: nothing to repair */
        e->reattach_req = 0;
        e->reattach_tries = 0;
        e->blocked_tries = 0;
        return;
    }
    if (mt_os_enet_blocked(e->refnum)) {                /* a sync call would wait behind the head request */
        e->reattach_blocked++;
        e->reattach_at = now + MT_ENET_BLOCKED_RETRY_MS;
        if (++e->blocked_tries >= MT_ENET_BLOCKED_TRIES && e->opened) {
            e->reattach_req = 0;                        /* link intact: give up for now */
            e->reattach_abandoned++;
            e->blocked_tries = 0;
        }
        return;
    }
    e->blocked_tries = 0;
    r = (int16_t)mt_enet_reattach_io(e);
    if (r == 0) {
        e->reattach++;
        e->reattach_req = 0;
        e->reattach_tries = 0;
        return;
    }
    e->last_err = r;
    e->reattach_fail++;
    if (e->reattach_tries < 255) e->reattach_tries++;
    {
        uint32_t d = e->reattach_tries > 4u ? MT_ENET_REATTACH_BACKOFF_MAX_MS
                                            : (MT_ENET_REATTACH_BACKOFF_MS << (e->reattach_tries - 1u));
        if (d > MT_ENET_REATTACH_BACKOFF_MAX_MS) d = MT_ENET_REATTACH_BACKOFF_MAX_MS;
        e->reattach_at = now + d;                       /* opened is 0 now: kept trying, from Control entries */
    }
}
