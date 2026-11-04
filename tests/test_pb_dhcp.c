/* The driver side of DHCP (mac/glue/mt_pb.c mt_pb_dhcp_start / mt_pb_dhcp_event; DHCP-DESIGN.md
 * section 4): unconfigured (-28) until the first lease, configured from then on for the boot, the
 * status view, GetMyIPAddr, streams surviving a lost lease. */
#include "t.h"
#include "pbfake.h"
#include "dhcpb.h"

static int16_t call(pbt *x, void *pb) { return pbt_call(x, (TCPiopb *)pb); }

/* Run the driver's timers to pbf_now + ms (mt_pb_tick at every deadline, as the Time Manager would). */
static void run(pbt *x, uint32_t ms) {
    uint32_t end = pbf_now + ms;
    int guard = 0;
    for (;;) {
        uint32_t d = mt_pb_next_deadline_ms(x->d);
        if (d == MT_NO_DEADLINE || d > end - pbf_now) { pbf_now = end; return; }
        pbf_now += d;
        mt_pb_tick(x->d);
        if (++guard > 100000) { T_CHECK(!"run: runaway deadlines"); return; }
    }
}

static void reply(pbt *x, drep *r, mt_ip4 dst) {
    static uint8_t f[1514];
    uint16_t n = mk_reply(f, SRV, dst, r);
    mt_pb_input(x->d, f, n);
}

static mt_ip4 my_addr(pbt *x, int16_t *res) {
    GetAddrParamBlock *pb = (GetAddrParamBlock *)pbt_pb(ipctlGetAddr, 0);
    *res = call(x, pb);
    return pb->ourAddress;
}

/* Unconfigured driver in DHCP mode, first DISCOVER out; returns its xid. */
static uint32_t dhcp_up(pbt *x, MtStatusView *v, uint32_t lease) {
    mt_ifconfig c;
    dmsg m;
    drep r;
    pbt_init(x, 0);
    memset(v, 0, sizeof *v);
    mt_get_config(x->d->s, &c);
    c.addr = OFFERED;                               /* the prefs address: a hint only */
    c.dns = 0x09090909u;
    T_EQ(mt_pb_dhcp_start(x->d, &c, OFFERED, v), MT_OK);
    T_EQ(x->d->configured, 0);
    { mt_ifconfig k; mt_get_config(x->d->s, &k); T_EQ(k.addr, 0); }
    run(x, 0);
    T_CHECK(nth_dhcp(&x->fl, 0, &m));
    T_EQ(m.type, 1); T_EQ(m.req, OFFERED);
    T_EQ(v->state, MT_DHCP_SELECTING);
    if (!lease) return m.xid;
    fake_clear(&x->fl);
    rep_init(&r, 2, m.xid);
    r.lease = lease;
    r.has_dns = 0;                                  /* the prefs DNS stays */
    reply(x, &r, OFFERED);
    T_CHECK(nth_dhcp(&x->fl, 0, &m));
    T_EQ(m.type, 3);
    rep_init(&r, 5, m.xid);
    r.lease = lease;
    r.has_dns = 0;
    reply(x, &r, OFFERED);
    fake_clear(&x->fl);
    return m.xid;
}

static void dhcp_unconfigured_until_lease(void) {
    pbt x; MtStatusView v; int16_t res; mt_ip4 a; mt_ifconfig c;
    dhcp_up(&x, &v, 0);
    a = my_addr(&x, &res);
    T_EQ(res, MT_PB_NOTOPEN);                       /* matrix "Unconfigured run" kept for the wait */
    T_EQ(pbf_count('C'), 0);                        /* -28: no completion */
    (void)a;
    /* the lease arrives */
    pbt_init(&x, 0);
    dhcp_up(&x, &v, 7200);
    T_EQ(x.d->configured, 1);
    T_EQ(v.state, MT_DHCP_BOUND);
    T_EQ(v.addr, OFFERED);
    T_EQ(v.lease_s, 7200);
    a = my_addr(&x, &res);
    T_EQ(res, 0);
    T_EQ(a, OFFERED);
    mt_get_config(x.d->s, &c);
    T_EQ(c.dns, 0x09090909u);                       /* fallback kept: the server sent none */
    T_EQ(c.gateway, T_GW);
}

static void dhcp_view_tracks_renewal(void) {
    pbt x; MtStatusView v; dmsg m; drep r; uint32_t t0;
    static uint8_t f[100];
    dhcp_up(&x, &v, 100);
    t0 = v.t_bound;
    mt_pb_input(x.d, f, mk_arp(f, 2, T_PEER_MAC, SRV, T_ME_MAC, OFFERED));
    run(&x, 50000);                                 /* T1 */
    T_EQ(v.state, MT_DHCP_RENEWING);
    T_EQ(v.addr, OFFERED);
    T_CHECK(nth_dhcp(&x.fl, 0, &m));
    rep_init(&r, 5, m.xid);
    r.lease = 100;
    reply(&x, &r, OFFERED);
    T_EQ(v.state, MT_DHCP_BOUND);
    T_CHECK(v.t_bound - t0 >= 50u * 60u - 1u);      /* Ticks: the renewal restarted the lease */
}

/* After a loss the driver stays configured: GetMyIPAddr answers (0.0.0.0), streams can be
 * released, nothing gets -28. */
static void dhcp_lost_lease_keeps_driver_usable(void) {
    pbt x; MtStatusView v; int16_t res; mt_ip4 a;
    StreamPtr u;
    UDPiopb *up;
    dhcp_up(&x, &v, 30);
    u = pbt_ucreate(&x, 4000, 4096, 0, 0, NULL);
    T_CHECK(u != 0);
    run(&x, 31000);                                 /* nobody renews: lost */
    T_EQ(v.state, MT_DHCP_SELECTING);
    T_EQ(v.addr, 0);
    T_EQ(x.d->configured, 1);
    a = my_addr(&x, &res);
    T_EQ(res, 0);
    T_EQ(a, 0);
    up = (UDPiopb *)pbt_pb(UDPRelease, u);
    T_EQ(call(&x, up), 0);
    /* and a DISCOVER asks for the old address back */
    { dmsg m; T_CHECK(nth_dhcp(&x.fl, count_dhcp(&x.fl) - 1, &m)); T_EQ(m.type, 1); T_EQ(m.req, OFFERED); }
}

static void dhcp_manual_mode_untouched(void) {
    pbt x; int16_t res;
    pbt_init(&x, 1);
    T_EQ(x.d->view == NULL, 1);
    T_EQ(my_addr(&x, &res), T_ME);
    T_EQ(res, 0);
    { mt_dhcp_info i; mt_dhcp_get_info(x.d->s, &i, pbf_now); T_EQ(i.state, MT_DHCP_OFF); T_EQ(i.tx, 0); }
}

T_MAIN(dhcp_unconfigured_until_lease, dhcp_view_tracks_renewal, dhcp_lost_lease_keeps_driver_usable,
       dhcp_manual_mode_untouched)
