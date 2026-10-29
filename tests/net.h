#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "mt.h"

/* Fake wire: tx() copies every transmitted frame into f[]/len[] (drops, and
 * returns nonzero, once 64 are queued). */
typedef struct { uint8_t f[64][1514]; uint16_t len[64]; int n; } fake_link;
void fake_init(fake_link *fl, mt_link *link);
void fake_clear(fake_link *fl);

#define T_ME   0xC0A832C8u   /* 192.168.50.200 */
#define T_GW   0xC0A83201u
#define T_PEER 0xC0A8320Au
#define T_FAR  0x08080808u
extern const uint8_t T_ME_MAC[6], T_PEER_MAC[6], T_GW_MAC[6];

/* Stack on a static arena (pool of 4, reused round-robin): /24, gateway .1,
 * MAC 02:00:00:00:00:01, mtu 1500, seed 1. addr==0 gives an unconfigured stack. */
mt_stack *tstack_new(fake_link *fl, mt_ip4 addr);

/* Frame builders; each returns the total frame length. For ARP, eth dst is
 * broadcast when op==1 and tmac otherwise; eth src is smac. */
uint16_t mk_arp(uint8_t *f, uint16_t op, const uint8_t smac[6], mt_ip4 sip,
                const uint8_t tmac[6], mt_ip4 tip);
/* Eth+IPv4 (IHL 5, TTL 64, valid checksum). eth dst = our MAC; eth src =
 * T_GW_MAC if src is T_GW or off-subnet, else T_PEER_MAC. frag_off_flags is
 * the raw 16-bit flags/offset field. */
uint16_t mk_ip(uint8_t *f, mt_ip4 src, mt_ip4 dst, uint8_t proto, uint16_t id,
               uint16_t frag_off_flags, const uint8_t *pl, uint16_t plen);

typedef struct {
    uint16_t op; uint8_t smac[6]; mt_ip4 sip; uint8_t tmac[6]; mt_ip4 tip;
    uint8_t eth_dst[6], eth_src[6];
} arpv;
typedef struct {
    mt_ip4 src, dst; uint8_t proto; uint16_t id;
    uint16_t flags;   /* top 3 bits of the frag field, unshifted (0x4000 = DF, 0x2000 = MF) */
    uint16_t frag;    /* fragment offset in 8-byte units */
    const uint8_t *payload; uint16_t len;
    uint8_t eth_dst[6], eth_src[6];
} ipv;
/* Return 1 if f is a well-formed Ethernet+ARP/IPv4 frame, filling *out. */
int view_arp(const uint8_t *f, uint16_t len, arpv *out);
int view_ip(const uint8_t *f, uint16_t len, ipv *out);


/* ---- TCP helpers ---- */
#define TF_FIN 1
#define TF_SYN 2
#define TF_RST 4
#define TF_PSH 8
#define TF_ACK 16
#define TF_URG 32
/* Eth+IPv4+TCP with a valid checksum; opt/optlen (multiple of 4) go after the 20-byte header. */
uint16_t mk_tcp(uint8_t *f, mt_ip4 src, uint16_t sport, mt_ip4 dst, uint16_t dport, uint32_t seq,
                uint32_t ack, uint8_t flags, uint16_t win, const uint8_t *opt, uint8_t optlen,
                const uint8_t *data, uint16_t dlen);
typedef struct {
    mt_ip4 src, dst; uint16_t sport, dport;
    uint32_t seq, ack; uint8_t flags; uint16_t win;
    uint16_t mss;                   /* MSS option value, 0 if absent */
    const uint8_t *data; uint16_t dlen;
    int checksum_ok;
} tcpv;
/* 1 if f is a well-formed Ethernet+IPv4+TCP frame (checksum_ok says whether it verifies). */
int view_tcp(const uint8_t *f, uint16_t len, tcpv *v);

/* Send helper over mt_tcp_send_segs: p[0..len) as one request (split into 65535-byte segments,
 * at most MT_SEGS_MAX), done = NULL. The mt_seg array lives in a per-TCB pool here so it outlives
 * the call; p must stay valid until the peer ACKs it. Returns len on success, else the
 * mt_tcp_send_segs error (MT_ERR_INVAL for len 0 or len > 8 * 65535). */
int32_t tsend(mt_tcp *t, const uint8_t *p, uint32_t len, int push, int urgent, uint32_t now);
/* Same with a done callback. */
int32_t tsend_cb(mt_tcp *t, const uint8_t *p, uint32_t len, int push, int urgent,
                 mt_send_done_cb done, void *ud, uint32_t now);

/* Records TCP events: (ev, arg) pairs in order. */
typedef struct { int n; struct { int ev; uint32_t arg; } e[32]; } rec;
void rec_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user);
/* Scripted active open at now=0 to T_PEER:23 (ARP for T_PEER pre-resolved; peer MSS 1460,
 * window 8192, peer ISN 5000). Returns the ESTABLISHED TCB (8192-byte buffer from a pool
 * of 8); *peer_seq = next peer seq (5001), *my_seq = our next seq. Leaves fl empty and rec
 * holding the ESTABLISHED event. */
mt_tcp *tconn_established(mt_stack *s, fake_link *fl, rec *events, uint32_t *peer_seq,
                          uint32_t *my_seq);

#endif
