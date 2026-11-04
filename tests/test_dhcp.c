/* DHCP client (core/mt_dhcp.c, docs/dev/DHCP-DESIGN.md) against a scripted server on the fake
 * link. The server is this file: it reads the client's frames from fake_link and answers with
 * frames built here (UDP checksum valid unless a test breaks it). */
#include "t.h"
#include "net.h"
#include "mt_util.h"
#include "mt_int.h"                /* mt_dhcp_deadline: the client's own deadline */

#include "dhcpb.h"

static void deliver(mt_stack *s, const drep *r, mt_ip4 ipdst, uint32_t now) {
    static uint8_t f[1514];
    uint16_t n = mk_reply(f, SRV, ipdst, r);
    mt_input(s, f, n, now);
}

/* ---- time ---- */
/* Run the stack's timers up to now + ms, ticking at every deadline. */
static void advance(mt_stack *s, uint32_t *now, uint32_t ms) {
    uint32_t end = *now + ms;
    int guard = 0;
    for (;;) {
        uint32_t d = mt_next_deadline(s, *now);
        if (d == MT_NO_DEADLINE || d > end - *now) { *now = end; return; }
        *now += d;
        mt_tick(s, *now);
        if (++guard > 100000) { T_CHECK(!"advance: runaway deadlines"); return; }
    }
}

/* ---- events ---- */
typedef struct { int n; int ev[32]; uint8_t st[32]; mt_ip4 a[32]; uint32_t lease[32]; } evrec;
static void ev_cb(void *user, int ev, uint8_t st, mt_ip4 a, uint32_t lease) {
    evrec *r = (evrec *)user;
    if (r->n < 32) { r->ev[r->n] = ev; r->st[r->n] = st; r->a[r->n] = a; r->lease[r->n] = lease; r->n++; }
}
static int count_ev(const evrec *r, int ev) {
    int i, n = 0;
    for (i = 0; i < r->n; i++) n += r->ev[i] == ev;
    return n;
}

static uint8_t state_of(mt_stack *s, uint32_t now) {
    mt_dhcp_info i;
    mt_dhcp_get_info(s, &i, now);
    return i.state;
}
static mt_ip4 addr_of(mt_stack *s) {
    mt_ifconfig c;
    mt_get_config(s, &c);
    return c.addr;
}

/* Start on an unconfigured stack and run the first DISCOVER out; *m is that DISCOVER. */
static mt_stack *started(fake_link *fl, evrec *ev, uint32_t *now, mt_ip4 hint, dmsg *m) {
    mt_stack *s = tstack_new(fl, 0);
    memset(ev, 0, sizeof *ev);
    mt_dhcp_set_cb(s, ev_cb, ev);
    T_EQ(mt_dhcp_start(s, hint, *now), MT_OK);
    T_EQ(mt_next_deadline(s, *now), 0);
    mt_tick(s, *now);
    T_CHECK(nth_dhcp(fl, 0, m));
    return s;
}

/* Full exchange to BOUND: r->xid must be the DISCOVER's xid; r's type is replaced. Returns the
 * REQUEST's xid. */
static uint32_t bind(mt_stack *s, fake_link *fl, uint32_t *now, drep *r) {
    dmsg q;
    fake_clear(fl);
    r->type = 2;
    deliver(s, r, r->yiaddr, *now);
    T_CHECK(nth_dhcp(fl, 0, &q));
    T_EQ(q.type, 3);
    fake_clear(fl);
    r->type = 5; r->xid = q.xid;
    deliver(s, r, r->yiaddr, *now);
    return q.xid;
}

/* ---------------------------------------------------------------- tests */

static void dhcp_discover_on_start(void) {
    fake_link fl; evrec ev; dmsg m; uint32_t now = 1000;
    mt_stack *s = started(&fl, &ev, &now, 0xC0A83277u, &m);
    T_EQ(fl.n, 1);
    T_CHECK(m.ok); T_CHECK(m.cksum_ok); T_CHECK(m.eth_bcast);
    T_EQ(m.ipsrc, 0); T_EQ(m.ipdst, BCAST);
    T_EQ(m.type, 1);
    T_EQ(m.flags, 0);                               /* broadcast flag clear (ruling) */
    T_MEMEQ(m.chaddr, T_ME_MAC, 6);
    T_CHECK(m.has_cid); T_CHECK(m.has_maxsz);
    T_CHECK(m.has_req); T_EQ(m.req, 0xC0A83277u);   /* the hint */
    T_CHECK(!m.has_sid);
    T_CHECK(prl_has(&m, 1) && prl_has(&m, 3) && prl_has(&m, 6) && prl_has(&m, 15) && prl_has(&m, 51) &&
            prl_has(&m, 58) && prl_has(&m, 59));
    T_CHECK(m.blen >= 300);                         /* BOOTP minimum */
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    T_EQ(addr_of(s), 0);
    T_EQ(mt_get_stats(s)->udp_out, 1);
    T_EQ(mt_get_stats(s)->ip_out, 1);
    T_EQ(mt_get_stats(s)->eth_out, 1);
    T_EQ(count_ev(&ev, MT_DHCP_EV_STATE), 2);       /* -> INIT, -> SELECTING */
}

static void dhcp_start_needs_mac_and_drops_address(void) {
    fake_link fl; mt_ifconfig c; uint32_t now = 0;
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_get_config(s, &c);
    memset(c.mac, 0, 6);
    mt_set_config(s, &c);
    T_EQ(mt_dhcp_start(s, 0, now), MT_ERR_INVAL);
    T_EQ(state_of(s, now), MT_DHCP_OFF);
    T_EQ(addr_of(s), T_ME);
    s = tstack_new(&fl, T_ME);
    T_EQ(mt_dhcp_start(s, 0, now), MT_OK);
    T_EQ(addr_of(s), 0);                            /* unconfigured until the first ACK */
    T_EQ(state_of(s, now), MT_DHCP_INIT);
}

static void dhcp_full_exchange(void) {
    fake_link fl; evrec ev; dmsg d, q; drep r; uint32_t now = 5000;
    mt_ifconfig c; mt_dhcp_info i; arpv a;
    static uint8_t f[100];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    T_CHECK(!d.has_req);
    fake_clear(&fl);
    rep_init(&r, 2, d.xid);                         /* OFFER, unicast to the offered address */
    now += 30;
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    T_EQ(fl.n, 1);
    T_CHECK(nth_dhcp(&fl, 0, &q));
    T_EQ(q.type, 3); T_EQ(q.xid, d.xid);
    T_EQ(q.ipsrc, 0); T_EQ(q.ipdst, BCAST); T_CHECK(q.eth_bcast); T_CHECK(q.cksum_ok);
    T_CHECK(q.has_req); T_EQ(q.req, OFFERED);
    T_CHECK(q.has_sid); T_EQ(q.sid, SRV);
    T_EQ(q.ciaddr, 0);
    fake_clear(&fl);
    rep_init(&r, 5, q.xid);
    now += 20;
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    mt_get_config(s, &c);
    T_EQ(c.addr, OFFERED); T_EQ(c.mask, 0xFFFFFF00u); T_EQ(c.gateway, T_GW); T_EQ(c.dns, DNS1);
    T_CHECK(strcmp(c.domain, "home.lan") == 0);
    T_EQ(c.mtu, 1500);
    T_EQ(count_ev(&ev, MT_DHCP_EV_BOUND), 1);
    T_EQ(ev.ev[ev.n - 1], MT_DHCP_EV_BOUND); T_EQ(ev.a[ev.n - 1], OFFERED); T_EQ(ev.lease[ev.n - 1], 3600);
    mt_dhcp_get_info(s, &i, now);
    T_EQ(i.addr, OFFERED); T_EQ(i.server, SRV); T_EQ(i.lease_s, 3600); T_EQ(i.left_s, 3600);
    T_EQ(i.offers, 1); T_EQ(i.acks, 1); T_EQ(i.naks, 0); T_EQ(i.tx, 2); T_EQ(i.bad, 0);
    T_EQ(fl.n, 0);                                  /* no ARP probe, no DECLINE (rulings) */
    /* the new address answers ARP */
    mt_input(s, f, mk_arp(f, 1, T_PEER_MAC, T_PEER, (const uint8_t *)"\0\0\0\0\0\0", OFFERED), now);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &a));
    T_EQ(a.op, 2); T_EQ(a.sip, OFFERED);
    /* T1 = 1800 s after the REQUEST was sent (20 ms before the ACK): the next deadline */
    T_EQ(mt_dhcp_deadline(s, now), 1800000u - 20u);
}

static void dhcp_broadcast_replies_accepted(void) {
    fake_link fl; evrec ev; dmsg d; drep r; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    bind(s, &fl, &now, &r);                         /* bind() uses yiaddr; redo with broadcast */
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    s = started(&fl, &ev, &now, 0, &d);
    fake_clear(&fl);
    rep_init(&r, 2, d.xid);
    deliver(s, &r, BCAST, now);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    T_CHECK(nth_dhcp(&fl, 0, &d));
    rep_init(&r, 5, d.xid);
    deliver(s, &r, BCAST, now);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    T_EQ(addr_of(s), OFFERED);
}

static void dhcp_foreign_replies_ignored(void) {
    fake_link fl; evrec ev; dmsg d; drep r; mt_dhcp_info i; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    int k;
    fake_clear(&fl);
    for (k = 0; k < 9; k++) {
        rep_init(&r, 2, d.xid);
        switch (k) {
        case 0: r.xid ^= 1; break;
        case 1: r.chaddr[5] ^= 1; break;
        case 2: r.cookie ^= 1; break;
        case 3: r.op = 1; break;
        case 4: r.no_type = 1; break;
        case 5: r.has_sid = 0; break;              /* OFFER without server id */
        case 6: r.yiaddr = 0x7F000001u; break;     /* unusable address */
        case 7: r.yiaddr = 0; break;
        case 8: r.bad_cksum = 1; break;
        }
        deliver(s, &r, OFFERED, now);
        T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    }
    T_EQ(fl.n, 0);
    mt_dhcp_get_info(s, &i, now);
    T_EQ(i.bad, 8);                                 /* the bad checksum is a UDP drop, not seen */
    T_EQ(i.offers, 0);
    T_EQ(mt_get_stats(s)->udp_drops, 1);
    /* a reply type that does not fit the state (ACK while selecting) is ignored too */
    rep_init(&r, 5, d.xid);
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    T_EQ(addr_of(s), 0);
    /* no UDP checksum is fine */
    rep_init(&r, 2, d.xid);
    r.no_cksum = 1;
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
}

static void dhcp_malformed_never_crash(void) {
    fake_link fl; evrec ev; dmsg d; drep r; uint32_t now = 0, seed = 12345;
    static uint8_t f[1514];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    uint16_t n;
    int k, j;
    rep_init(&r, 2, d.xid);
    n = mk_reply(f, SRV, OFFERED, &r);
    /* every truncation of a valid OFFER (IP and UDP lengths patched to match, so it is parsed) */
    for (k = 34 + 8; k < n; k++) {
        static uint8_t g[1514];
        uint16_t ul = (uint16_t)(k - 34);
        memcpy(g, f, (size_t)k);
        mt_wr16(g + 16, (uint16_t)(k - 14));
        mt_wr16(g + 24, 0);
        mt_wr16(g + 24, mt_cksum_fold(mt_cksum_add(0, g + 14, 20)));
        mt_wr16(g + 38, ul);
        mt_wr16(g + 40, 0);                         /* no UDP checksum */
        mt_input(s, g, (uint16_t)k, now);
        T_EQ(state_of(s, now), MT_DHCP_SELECTING);  /* the end option is required (ruling) */
    }
    mt_input(s, f, n, now);                         /* the whole OFFER still works */
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    /* random option bytes, random lengths */
    s = started(&fl, &ev, &now, 0, &d);
    for (k = 0; k < 4000; k++) {
        static uint8_t opt[64];
        uint8_t no = 0;
        for (j = 0; j < 40; j++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            opt[j] = (uint8_t)seed;
        }
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        no = (uint8_t)(seed % 41);
        rep_init(&r, 2, d.xid);
        r.has_sid = (k & 1); r.no_type = (k % 7) == 0;
        r.domain = (k % 3) ? NULL : "x";
        r.extra = opt; r.nextra = no;
        deliver(s, &r, OFFERED, now);
        if (state_of(s, now) != MT_DHCP_SELECTING) {
            T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
            s = started(&fl, &ev, &now, 0, &d);
        }
    }
}

static void dhcp_selecting_backoff(void) {
    fake_link fl; evrec ev; dmsg d, m; uint32_t now = 0, last = 0;
    static const uint32_t base[] = {4000, 8000, 16000, 32000, 64000, 64000, 64000};
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    int k;
    for (k = 0; k < 7; k++) {
        uint32_t dl = mt_next_deadline(s, now);
        T_CHECK(dl >= base[k] - 1000 && dl <= base[k] + 1000);
        fake_clear(&fl);
        advance(s, &now, dl);
        T_EQ(count_dhcp(&fl), 1);
        T_CHECK(nth_dhcp(&fl, 0, &m));
        T_EQ(m.type, 1);
        T_EQ(m.xid, d.xid);                         /* same xid within one SELECTING round */
        T_CHECK(m.secs >= last); last = m.secs;
        T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    }
    T_CHECK(last >= 180);                           /* secs = seconds since the round began */
}

static void dhcp_requesting_gives_up(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    int k;
    fake_clear(&fl);
    rep_init(&r, 2, d.xid);
    deliver(s, &r, OFFERED, now);
    for (k = 0; k < 3; k++) {                       /* 3 resends of the REQUEST */
        fake_clear(&fl);
        advance(s, &now, mt_next_deadline(s, now));
        T_CHECK(nth_dhcp(&fl, 0, &m));
        T_EQ(m.type, 3); T_EQ(m.xid, d.xid);
        T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    }
    fake_clear(&fl);
    advance(s, &now, mt_next_deadline(s, now));     /* gives up: a new DISCOVER round at once */
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 1);
    T_CHECK(m.xid != d.xid);
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
}

static void dhcp_nak_while_requesting(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; mt_dhcp_info i; uint32_t now = 0, dl;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    fake_clear(&fl);
    rep_init(&r, 2, d.xid);
    deliver(s, &r, OFFERED, now);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    fake_clear(&fl);
    rep_init(&r, 6, m.xid);                         /* NAK, from the chosen server */
    r.has_mask = r.has_router = r.has_dns = r.has_lease = 0; r.domain = NULL; r.yiaddr = 0;
    deliver(s, &r, BCAST, now);
    T_EQ(state_of(s, now), MT_DHCP_INIT);
    T_EQ(fl.n, 0);
    dl = mt_next_deadline(s, now);
    T_CHECK(dl >= 2000 && dl <= 3000);
    advance(s, &now, dl);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 1);
    mt_dhcp_get_info(s, &i, now);
    T_EQ(i.naks, 1);
    T_EQ(count_ev(&ev, MT_DHCP_EV_LOST), 0);        /* nothing was held */
}

static void dhcp_nak_from_other_server_ignored(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    deliver(s, &r, OFFERED, now);
    T_CHECK(nth_dhcp(&fl, 1, &m));
    rep_init(&r, 6, m.xid);
    r.sid = T_GW;                                   /* not the server we chose */
    deliver(s, &r, BCAST, now);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    rep_init(&r, 5, m.xid);
    r.sid = T_GW;                                   /* an ACK from it is not ours either */
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
}

/* Lease 120 s, T1 60, T2 105: renew (unicast, after ARP), rebind (broadcast), expire. */
static void dhcp_renew_rebind_expire(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 7, t0; arpv a;
    static uint8_t f[100];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.lease = 120; r.t1 = 60; r.t2 = 105; r.has_t1 = r.has_t2 = 1;
    bind(s, &fl, &now, &r);
    t0 = now;
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    T_EQ(mt_next_deadline(s, now), 60000u);
    fake_clear(&fl);
    advance(s, &now, 60000);
    T_EQ(state_of(s, now), MT_DHCP_RENEWING);
    /* the server's MAC is unknown: an ARP request, the REQUEST itself is too big to hold */
    T_EQ(count_dhcp(&fl), 0);
    T_CHECK(fl.n == 1 && view_arp(fl.f[0], fl.len[0], &a) && a.op == 1 && a.tip == SRV);
    mt_input(s, f, mk_arp(f, 2, T_PEER_MAC, SRV, T_ME_MAC, OFFERED), now);
    fake_clear(&fl);
    T_EQ(mt_next_deadline(s, now), 4000u);          /* the one quick resend */
    advance(s, &now, 4000);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 3); T_EQ(m.ipsrc, OFFERED); T_EQ(m.ipdst, SRV); T_CHECK(!m.eth_bcast);
    T_EQ(m.ciaddr, OFFERED); T_CHECK(!m.has_req); T_CHECK(!m.has_sid); T_CHECK(m.cksum_ok);
    T_CHECK(m.xid != d.xid);
    /* next resend: half the time to T2 (41 s left -> 20.5 s), at least 60 s: T2 comes first */
    fake_clear(&fl);
    T_EQ(mt_next_deadline(s, now), 41000u);
    advance(s, &now, 41000);
    T_EQ(now - t0, 105000u);
    T_EQ(state_of(s, now), MT_DHCP_REBINDING);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 3); T_EQ(m.ipsrc, OFFERED); T_EQ(m.ipdst, BCAST); T_CHECK(m.eth_bcast);
    T_EQ(m.ciaddr, OFFERED);
    T_EQ(addr_of(s), OFFERED);
    /* no answer: the lease ends at 120 s; the address goes, a DISCOVER asks for it again */
    fake_clear(&fl);
    T_EQ(mt_next_deadline(s, now), 15000u);
    advance(s, &now, 15000);
    T_EQ(addr_of(s), 0);
    T_EQ(count_ev(&ev, MT_DHCP_EV_LOST), 1);
    T_EQ(ev.ev[ev.n - 1] == MT_DHCP_EV_STATE || ev.ev[ev.n - 1] == MT_DHCP_EV_LOST, 1);
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 1); T_EQ(m.ipsrc, 0); T_CHECK(m.has_req); T_EQ(m.req, OFFERED);
}

static void dhcp_renew_ack_extends(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; mt_dhcp_info i; uint32_t now = 0;
    static uint8_t f[100];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    int bound_events;
    rep_init(&r, 2, d.xid);
    r.lease = 1000;
    bind(s, &fl, &now, &r);
    mt_input(s, f, mk_arp(f, 2, T_PEER_MAC, SRV, T_ME_MAC, OFFERED), now);   /* server's MAC known */
    bound_events = count_ev(&ev, MT_DHCP_EV_BOUND);
    fake_clear(&fl);
    advance(s, &now, 500000);                       /* T1 = 500 s */
    T_EQ(state_of(s, now), MT_DHCP_RENEWING);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.ipdst, SRV);
    now += 700;
    rep_init(&r, 5, m.xid);
    r.lease = 2000; r.dns = 0x08080808u;
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    T_EQ(count_ev(&ev, MT_DHCP_EV_BOUND), bound_events);   /* same address: no BOUND event */
    mt_dhcp_get_info(s, &i, now);
    T_EQ(i.lease_s, 2000);
    T_EQ(i.left_s, 2000);                           /* from the REQUEST's send time, 0.7 s ago */
    { mt_ifconfig c; mt_get_config(s, &c); T_EQ(c.dns, 0x08080808u); }
    T_EQ(mt_dhcp_deadline(s, now), 1000000u - 700u);
}

static void dhcp_nak_while_renewing_drops_address(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0;
    static uint8_t f[100];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    bind(s, &fl, &now, &r);
    advance(s, &now, 1800000);                      /* T1: the server's ARP entry expired long ago */
    mt_input(s, f, mk_arp(f, 2, T_PEER_MAC, SRV, T_ME_MAC, OFFERED), now);
    fake_clear(&fl);
    advance(s, &now, 4000);
    T_CHECK(nth_dhcp(&fl, 0, &m));
    rep_init(&r, 6, m.xid);
    r.yiaddr = 0;
    deliver(s, &r, OFFERED, now);
    T_EQ(addr_of(s), 0);
    T_EQ(count_ev(&ev, MT_DHCP_EV_LOST), 1);
    T_EQ(state_of(s, now), MT_DHCP_INIT);
}

static void dhcp_new_address_after_loss(void) {
    fake_link fl; evrec ev; dmsg d; drep r; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.lease = 60;
    bind(s, &fl, &now, &r);
    advance(s, &now, 60000);                        /* nobody answers: lost */
    T_EQ(addr_of(s), 0);
    T_CHECK(nth_dhcp(&fl, count_dhcp(&fl) - 1, &d));
    T_EQ(d.type, 1);
    T_EQ(d.req, OFFERED);
    rep_init(&r, 2, d.xid);
    r.yiaddr = OTHER;
    bind(s, &fl, &now, &r);
    T_EQ(addr_of(s), OTHER);
    T_EQ(count_ev(&ev, MT_DHCP_EV_BOUND), 2);
}

static void dhcp_ack_defaults(void) {
    fake_link fl; evrec ev; dmsg d; drep r; mt_ifconfig c; uint32_t now = 0;
    mt_stack *s = tstack_new(&fl, 0);
    mt_get_config(s, &c);
    c.dns = 0x09090909u; strcpy(c.domain, "fallback"); c.gateway = 0x0A000001u;
    mt_set_config(s, &c);                           /* the embedder's fallbacks */
    memset(&ev, 0, sizeof ev);
    mt_dhcp_set_cb(s, ev_cb, &ev);
    mt_dhcp_start(s, 0, now);
    mt_tick(s, now);
    T_CHECK(nth_dhcp(&fl, 0, &d));
    rep_init(&r, 2, d.xid);
    r.yiaddr = 0x0A000064u;                         /* 10.0.0.100, class A */
    r.has_mask = r.has_router = r.has_dns = r.has_lease = 0; r.domain = NULL;
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_EQ(c.addr, 0x0A000064u); T_EQ(c.mask, 0xFF000000u);
    T_EQ(c.gateway, 0x0A000001u); T_EQ(c.dns, 0x09090909u);
    T_CHECK(strcmp(c.domain, "fallback") == 0);
    T_EQ(mt_next_deadline(s, now), 1800000u);       /* 3600 s default lease */
    /* a non-contiguous mask is replaced by the class mask; a short lease is raised to 20 s */
    s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.mask = 0xFF00FF00u; r.lease = 3;
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_EQ(c.mask, 0xFFFFFF00u);
    T_EQ(mt_next_deadline(s, now), 10000u);
}

static void dhcp_ack_unusable_address_ignored(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0;
    static const mt_ip4 bad[] = {0, 0x7F000001u, 0xE0000001u, 0xFFFFFFFFu, 0xC0A832FFu, 0xC0A83200u, 0x00000005u};
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    unsigned k;
    rep_init(&r, 2, d.xid);
    deliver(s, &r, OFFERED, now);
    T_CHECK(nth_dhcp(&fl, 1, &m));
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        rep_init(&r, 5, m.xid);
        r.yiaddr = bad[k];
        deliver(s, &r, BCAST, now);
        T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
        T_EQ(addr_of(s), 0);
    }
}

static void dhcp_domain_option_sanitised(void) {
    fake_link fl; evrec ev; dmsg d; drep r; mt_ifconfig c; uint32_t now = 0;
    static uint8_t ext[80];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.domain = "abc";
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_CHECK(strcmp(c.domain, "abc") == 0);
    /* a domain with a trailing NUL (some servers) */
    s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.domain = NULL;
    ext[0] = 15; ext[1] = 4; memcpy(ext + 2, "lan\0", 4);
    r.extra = ext; r.nextra = 6;
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_CHECK(strcmp(c.domain, "lan") == 0);
    /* a 70-character domain: cut to 63 */
    s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.domain = NULL;
    ext[0] = 15; ext[1] = 70; memset(ext + 2, 'a', 70);
    r.extra = ext; r.nextra = 72;
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_EQ(strlen(c.domain), 63);
    /* a control character: the domain is not applied (the old one stays) */
    s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.domain = NULL;
    ext[0] = 15; ext[1] = 3; memcpy(ext + 2, "a\nb", 3);
    r.extra = ext; r.nextra = 5;
    bind(s, &fl, &now, &r);
    mt_get_config(s, &c);
    T_EQ(c.addr, OFFERED);
    T_CHECK(strcmp(c.domain, "a\nb") != 0);
}

static void dhcp_infinite_lease(void) {
    fake_link fl; evrec ev; dmsg d; drep r; mt_dhcp_info i; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.lease = 0xFFFFFFFFu;
    bind(s, &fl, &now, &r);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    T_EQ(mt_next_deadline(s, now), MT_NO_DEADLINE);
    advance(s, &now, 0x7FFFFFFFu);
    mt_tick(s, now);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    mt_dhcp_get_info(s, &i, now);
    T_EQ(i.lease_s, 0xFFFFFFFFu); T_EQ(i.left_s, 0xFFFFFFFFu);
}

/* A 40-day lease across the ms clock wrap: T1 lands 20 days later to the second. */
static void dhcp_long_lease_clock_wrap(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0xFFFF0000u, t0, ticks = 0;
    const uint32_t lease = 40u * 86400u;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    r.lease = lease;
    bind(s, &fl, &now, &r);
    t0 = now;
    /* run in deadline steps; every step must be <= 2^30 ms and the state stays BOUND until T1 */
    while (state_of(s, now) == MT_DHCP_BOUND) {
        uint32_t dl = mt_next_deadline(s, now);
        T_CHECK(dl != MT_NO_DEADLINE && dl <= 0x40000000u);
        if (dl == MT_NO_DEADLINE || ++ticks > 10) break;
        now += dl;
        mt_tick(s, now);
    }
    T_EQ(state_of(s, now), MT_DHCP_RENEWING);
    T_EQ((uint32_t)(now - t0), 20u * 86400u * 1000u);   /* T1 = lease / 2, across the wrap */
    T_CHECK(ticks <= 3);
    T_CHECK(nth_dhcp(&fl, count_dhcp(&fl) - 1, &m) || fl.n == 1);   /* REQUEST or its ARP */
}

static void dhcp_port68_reserved(void) {
    fake_link fl; uint8_t buf[1024]; uint32_t now = 0;
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_udp *u = mt_udp_create(s, 68, buf, sizeof buf, NULL, NULL);
    T_CHECK(u != NULL);                             /* DHCP off: an app may have it */
    mt_udp_release(u);
    mt_dhcp_start(s, 0, now);
    T_CHECK(mt_udp_create(s, 68, buf, sizeof buf, NULL, NULL) == NULL);
    T_CHECK(mt_udp_create(s, 69, buf, sizeof buf, NULL, NULL) != NULL);
    mt_dhcp_stop(s);
    T_CHECK(mt_udp_create(s, 68, buf, sizeof buf, NULL, NULL) != NULL);
}

static void dhcp_unconfigured_accepts_only_port68(void) {
    fake_link fl; evrec ev; dmsg d; uint32_t now = 0;
    static uint8_t f[1514], pl[64];
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    uint32_t drops;
    uint16_t n;
    fake_clear(&fl);
    drops = mt_get_stats(s)->ip_in_drops;
    /* UDP to another port, TCP, ICMP echo, a fragment of a port-68 datagram: all dropped, silent */
    memset(pl, 0, sizeof pl);
    mt_wr16(pl, 67); mt_wr16(pl + 2, 69); mt_wr16(pl + 4, 16);
    n = mk_ip(f, SRV, BCAST, 17, 1, 0, pl, 16); memset(f, 0xFF, 6);
    mt_input(s, f, n, now);
    n = mk_tcp(f, SRV, 80, OFFERED, 1234, 1, 0, TF_SYN, 1000, NULL, 0, NULL, 0);
    mt_input(s, f, n, now);
    pl[0] = 8; pl[1] = 0; mt_wr16(pl + 2, 0); mt_wr16(pl + 2, mt_cksum_fold(mt_cksum_add(0, pl, 8)));
    n = mk_ip(f, SRV, BCAST, 1, 1, 0, pl, 8); memset(f, 0xFF, 6);
    mt_input(s, f, n, now);
    memset(pl, 0, sizeof pl);
    mt_wr16(pl, 67); mt_wr16(pl + 2, 68); mt_wr16(pl + 4, 16);
    n = mk_ip(f, SRV, BCAST, 17, 1, 0x2000, pl, 16); memset(f, 0xFF, 6);
    mt_input(s, f, n, now);
    T_EQ(mt_get_stats(s)->ip_in_drops, drops + 4);
    T_EQ(fl.n, 0);
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
}

/* DHCP off: the static path is byte-for-byte what it was: port 68 is an ordinary closed port. */
static void dhcp_off_static_unchanged(void) {
    fake_link fl; drep r; uint32_t now = 0; ipv ip;
    static uint8_t f[1514];
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n;
    T_EQ(mt_next_deadline(s, now), MT_NO_DEADLINE);
    rep_init(&r, 2, 1);
    n = mk_reply(f, SRV, T_ME, &r);
    mt_input(s, f, n, now);
    T_EQ(mt_get_stats(s)->udp_noport, 1);
    T_EQ(fl.n, 1);                                  /* ICMP port unreachable (after no ARP: held) */
    T_CHECK(fl.n == 1 && (view_ip(fl.f[0], fl.len[0], &ip) == 0 || ip.proto == 1));
    T_EQ(addr_of(s), T_ME);
    /* unconfigured and DHCP off: the same frame is dropped at IP */
    s = tstack_new(&fl, 0);
    n = mk_reply(f, SRV, BCAST, &r);
    memset(f, 0xFF, 6);
    mt_input(s, f, n, now);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
    T_EQ(fl.n, 0);
}

static void dhcp_stop_keeps_config(void) {
    fake_link fl; evrec ev; dmsg d; drep r; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &d);
    rep_init(&r, 2, d.xid);
    bind(s, &fl, &now, &r);
    mt_dhcp_stop(s);
    T_EQ(state_of(s, now), MT_DHCP_OFF);
    T_EQ(addr_of(s), OFFERED);
    T_EQ(mt_next_deadline(s, now), MT_NO_DEADLINE);
    /* a late reply is not the client's any more: port 68 is closed */
    rep_init(&r, 5, d.xid);
    fake_clear(&fl);
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_OFF);
    T_EQ(mt_get_stats(s)->udp_noport, 1);
}

static void dhcp_restart_and_entropy(void) {
    fake_link fl; evrec ev; dmsg a, b; uint32_t now = 0;
    mt_stack *s = started(&fl, &ev, &now, 0, &a);
    fake_clear(&fl);
    mt_dhcp_start(s, 0, now);                       /* restart: a new round, a new xid */
    mt_tick(s, now);
    T_CHECK(nth_dhcp(&fl, 0, &b));
    T_CHECK(a.xid != b.xid);
    /* entropy changes the xid sequence */
    s = tstack_new(&fl, 0);
    mt_set_entropy(s, 0x12345678u, 0x9ABCDEF0u);
    mt_dhcp_start(s, 0, now);
    mt_tick(s, now);
    T_CHECK(nth_dhcp(&fl, 0, &b));
    T_CHECK(a.xid != b.xid);
}

static void dhcp_log_records_state_changes(void) {
    fake_link fl; evrec ev; dmsg d; drep r; uint32_t now = 0;
    mt_logrec rec[32];
    mt_stack *s = tstack_new(&fl, 0);
    uint16_t n, k, seen = 0;
    mt_log_enable(s, 1);
    memset(&ev, 0, sizeof ev);
    mt_dhcp_start(s, 0, now);
    mt_tick(s, now);
    T_CHECK(nth_dhcp(&fl, 0, &d));
    rep_init(&r, 2, d.xid);
    bind(s, &fl, &now, &r);
    n = mt_log_read(s, rec, 32);
    for (k = 0; k < n; k++)
        if (rec[k].code == MT_LOG_DHCP) {
            seen++;
            if (rec[k].a == MT_DHCP_BOUND) T_EQ(rec[k].b, (uint16_t)OFFERED);
        }
    T_EQ(seen, 4);                                  /* INIT, SELECTING, REQUESTING, BOUND */
}

/* A link that refuses frames (busy DaynaPORT): nothing is counted as sent, the client's own
 * retransmissions recover (DISCOVER, then REQUEST), and the exchange completes. */
static void dhcp_refusing_link_recovers(void) {
    fake_link fl; evrec ev; dmsg d, m; drep r; uint32_t now = 0;
    mt_stack *s = tstack_new(&fl, 0);
    memset(&ev, 0, sizeof ev);
    mt_dhcp_set_cb(s, ev_cb, &ev);
    mt_dhcp_start(s, 0, now);
    fl.refuse = 1;                                  /* the first DISCOVER is refused */
    mt_tick(s, now);
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->eth_tx_err, 1);
    T_EQ(mt_get_stats(s)->eth_out, 0);
    T_EQ(state_of(s, now), MT_DHCP_SELECTING);
    advance(s, &now, mt_next_deadline(s, now));     /* ~4 s: the retransmission goes out */
    T_CHECK(nth_dhcp(&fl, 0, &d));
    T_EQ(d.type, 1);
    fake_clear(&fl);
    rep_init(&r, 2, d.xid);
    fl.refuse = 1;                                  /* the REQUEST answering the OFFER is refused */
    deliver(s, &r, OFFERED, now);
    T_EQ(fl.n, 0);
    T_EQ(state_of(s, now), MT_DHCP_REQUESTING);
    advance(s, &now, mt_next_deadline(s, now));
    T_CHECK(nth_dhcp(&fl, 0, &m));
    T_EQ(m.type, 3);
    rep_init(&r, 5, m.xid);
    deliver(s, &r, OFFERED, now);
    T_EQ(state_of(s, now), MT_DHCP_BOUND);
    T_EQ(addr_of(s), OFFERED);
    T_EQ(mt_get_stats(s)->eth_tx_err, 2);
}

T_MAIN(dhcp_discover_on_start, dhcp_refusing_link_recovers, dhcp_start_needs_mac_and_drops_address, dhcp_full_exchange,
       dhcp_broadcast_replies_accepted, dhcp_foreign_replies_ignored, dhcp_malformed_never_crash,
       dhcp_selecting_backoff, dhcp_requesting_gives_up, dhcp_nak_while_requesting,
       dhcp_nak_from_other_server_ignored, dhcp_renew_rebind_expire, dhcp_renew_ack_extends,
       dhcp_nak_while_renewing_drops_address, dhcp_new_address_after_loss, dhcp_ack_defaults,
       dhcp_ack_unusable_address_ignored, dhcp_domain_option_sanitised, dhcp_infinite_lease,
       dhcp_long_lease_clock_wrap, dhcp_port68_reserved, dhcp_unconfigured_accepts_only_port68,
       dhcp_off_static_unchanged, dhcp_stop_keeps_config, dhcp_restart_and_entropy,
       dhcp_log_records_state_changes)
