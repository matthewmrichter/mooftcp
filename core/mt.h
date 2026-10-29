#ifndef MT_H
#define MT_H

#include <stdint.h>

/* MoofTCP portable core: public API. All state lives in one caller-supplied
 * arena; the core never allocates and has no mutable globals.
 *
 * Embedder contract (what the glue must guarantee):
 *  - One context at a time per stack. mt_input and mt_tick must never be re-entered, neither
 *    from a callback nor from link.tx; every other API call is legal from callbacks.
 *  - Timers: re-query mt_next_deadline after EVERY API call, not only after mt_input and
 *    mt_tick (almost any call can arm a timer: sends, opens, closes, mt_tcp_recv/consume window
 *    updates, mt_tcp_release RSTs, DNS and UDP calls, mt_set_config, and mt_tcp_set_ulp_timeout
 *    changes the deadline itself). Call mt_tick when it reaches 0, and at least once every
 *    2^31 ms. link.tx must not call back into the stack.
 *  - Clock: `now` (ms, uint32, wraps) comes from one monotonic source shared by every context
 *    that calls into the stack.
 *  - Lifetimes: the `frame` given to mt_input need only live for that call. Pointers handed to
 *    callbacks (echo data, UDP datagram data, TCP peek pointers) are valid only during the
 *    callback unless documented otherwise.
 *  - Callbacks run in the caller's context: at interrupt time if mt_input is called at interrupt
 *    time, so they must not call the Memory Manager (or anything else unsafe there).
 *  - link.tx receives complete Ethernet frames. ARP frames are padded to 60 bytes; IP frames are
 *    NOT, so a frame can be shorter than the 60-byte Ethernet minimum (a pure TCP ACK is 54
 *    bytes). The glue must pad to 60 in its ENET write path; the core does not. The pointer may
 *    point into the core's tx scratch buffer, the ARP scratch buffer or the pending-frame slot,
 *    and is valid only until tx returns.
 *  - TCP send buffers stay in caller memory (no copy): the mt_seg array given to
 *    mt_tcp_send_segs and every byte it points to must stay valid and unmodified until that
 *    request's `done` fires (retransmissions re-read them). The core never writes to them. `done`
 *    fires from inside mt_input (the ACK that completes it), mt_tick (a timeout that terminates the
 *    connection) or mt_tcp_abort / mt_tcp_close (termination), never from mt_tcp_send_segs, and
 *    never after mt_tcp_release (which drops queued requests silently: the caller completes them). */

typedef uint32_t mt_ip4;                         /* host byte order */
#define MT_NO_DEADLINE 0xFFFFFFFFu

typedef struct mt_stack mt_stack;

enum { MT_OK = 0, MT_ERR_NOROUTE = -1, MT_ERR_NOMEM = -2, MT_ERR_INVAL = -3,
       MT_ERR_STATE = -4, MT_ERR_INUSE = -5, MT_ERR_UNCONFIGURED = -6, MT_ERR_BUSY = -7,
       MT_ERR_ABORTED = -8 };

/* tx must finish with `frame` before returning (see the embedder contract); nonzero = dropped */
typedef struct { void *ctx; int (*tx)(void *ctx, const uint8_t *frame, uint16_t len); } mt_link;

typedef struct {
    mt_ip4 addr, mask, gateway, dns;
    char domain[64];
    uint8_t mac[6];
    uint16_t mtu;
} mt_ifconfig;

typedef struct { uint8_t max_tcp, max_udp, arp_entries, dns_cache; } mt_limits;

/* Counters (all uint32_t, in this order; tests index the struct as an array). tcp_conns_open is
 * a gauge, the rest only count up (wrapping at 2^32). */
typedef struct {
    uint32_t eth_in;      /* frames >= 14 bytes addressed to us or broadcast */
    uint32_t eth_out;     /* frames handed to link.tx successfully */
    uint32_t eth_tx_err;  /* link.tx returned nonzero (or no tx function) */
    uint32_t arp_in;      /* ARP frames received (accepted by the Ethernet filter) */
    uint32_t arp_out;     /* ARP requests/replies transmitted */
    uint32_t arp_drops;   /* unresolved next hops given up on + held frames discarded/replaced */
    uint32_t ip_in;         /* datagrams/fragments that passed IPv4 header validation (counted before
                             * reassembly and ICMP checks: some also appear in ip_in_drops) */
    uint32_t ip_out;        /* datagrams handed to IPv4 output (fragmented ones count once) */
    uint32_t ip_in_drops;   /* all IPv4-layer drops: header-validation failures (never in ip_in) AND later refusals
                             * (reassembly, bad ICMP checksum/size; those are also in ip_in), so
                             * ip_in - ip_in_drops is NOT the number delivered */
    uint32_t ip_frag_out;   /* fragment frames emitted */
    uint32_t ip_reasm_ok;   /* datagrams reassembled */
    uint32_t ip_reasm_fail; /* reassemblies abandoned (timeout, overlap, overflow, slot busy) */
    uint32_t icmp_in;       /* ICMP messages accepted (well-formed, checksum ok, from a unicast peer) */
    uint32_t icmp_out;      /* ICMP messages transmitted (echo requests/replies, errors) */
    uint32_t udp_in;        /* datagrams queued to an endpoint */
    uint32_t udp_out;       /* datagrams handed to IPv4 output */
    uint32_t udp_drops;     /* malformed, bad checksum, or receive buffer full */
    uint32_t udp_noport;    /* datagrams for a port nobody owns */
    uint32_t tcp_in;           /* well-formed segments (checksum, header, options ok) */
    uint32_t tcp_out;          /* segments handed to IPv4 output */
    uint32_t tcp_in_drops;     /* malformed / bad checksum / bad options */
    uint32_t tcp_rexmt;        /* retransmitted segments (includes fast retransmits) */
    uint32_t tcp_fast_rexmt;   /* fast retransmits (third duplicate ACK); also in tcp_rexmt */
    uint32_t tcp_active_opens;
    uint32_t tcp_passive_opens;
    uint32_t tcp_resets_in;    /* RSTs that tore a connection down */
    uint32_t tcp_resets_out;   /* RSTs transmitted */
    uint32_t tcp_conns_open;   /* connections established and not yet terminated (gauge) */
} mt_stats;

void      mt_limits_default(mt_limits *l);      /* 16, 8, 8, 8 */
uint32_t  mt_stack_size(const mt_limits *l);    /* arena bytes needed */
/* NULL if len < mt_stack_size(l) or mem is not aligned to MT_ARENA_ALIGN (mt_int.h: 8 bytes on
 * hosts with 64-bit pointers, else 4; an 8-aligned arena is always fine). */
mt_stack *mt_stack_init(void *mem, uint32_t len, const mt_limits *l, const mt_link *link,
                        uint32_t seed);
/* A stack with addr==0 sends nothing and answers nothing. */
void      mt_set_config(mt_stack *s, const mt_ifconfig *c);
void      mt_get_config(const mt_stack *s, mt_ifconfig *c);
const mt_stats *mt_get_stats(const mt_stack *s);
/* Connections in use for TCPGlobalInfo: TCBs past LISTEN (SYN_SENT .. TIME_WAIT). */
uint8_t mt_tcp_count_open(const mt_stack *s);

/* ---- Diagnostic log: fixed ring of 128 eight-byte records inside the arena (1024 bytes) ----
 * Off by default. A record is {t = the stack's clock (ms) when written, code, a, b}.
 * Writing is O(1), allocation-free and overwrites the oldest record; the core may be entered
 * from interrupt time provided calls into one stack are serialised (single writer context). A
 * reader in another context (mt_log_read) is safe against that writer: it never sees a torn
 * record (a record overwritten while being copied is skipped). Fields by code:
 *   TCP_STATE a = new mt_tcp_state,      b = local port
 *   TCP_TERM  a = mt_term_reason,        b = local port
 *   TCP_REXMT                            b = local port
 *   DROP      a = layer (MT_LOGL_xxx),     b = source line of the reason in the core sources
 *   ARP_FAIL  a = requests sent,         b = low 16 bits of the unresolved IP
 *   DNS_FAIL  a = mt_dns_status,         b = query id */
typedef struct { uint32_t t; uint8_t code, a; uint16_t b; } mt_logrec;
enum { MT_LOG_TCP_STATE = 1, MT_LOG_TCP_TERM, MT_LOG_TCP_REXMT, MT_LOG_DROP, MT_LOG_ARP_FAIL,
       MT_LOG_DNS_FAIL };
/* MT_LOGL_IP covers ICMP too. */
enum { MT_LOGL_ETH = 1, MT_LOGL_ARP, MT_LOGL_IP, MT_LOGL_UDP, MT_LOGL_TCP };
#define MT_LOG_RECS 128u
void     mt_log_enable(mt_stack *s, int on);   /* default off; records are kept when turned off */
/* Copy up to max records, oldest first, and consume them. At most 127 records are pending (one
 * of the 128 slots is the writer's in-flight slot); older ones are lost. Returns the number copied. */
uint16_t mt_log_read(mt_stack *s, mt_logrec *out, uint16_t max);

/* ICMP echo. The callback fires for each echo reply addressed to us. */
typedef void (*mt_echo_cb)(void *user, mt_ip4 from, uint16_t id, uint16_t seq,
                           const uint8_t *data, uint16_t len, uint32_t now);
void      mt_set_echo_cb(mt_stack *s, mt_echo_cb cb, void *user);
/* Send an echo request (len <= 1472 so it fits one frame; MT_ERR_INVAL otherwise).
 * Returns what mt_ip_output returns. */
int       mt_icmp_echo(mt_stack *s, mt_ip4 dst, uint16_t id, uint16_t seq, const uint8_t *data,
                       uint16_t len, uint32_t now);

/* ---- UDP ---- */
typedef struct mt_udp mt_udp;
typedef enum { MT_UDP_EV_DATA = 1, MT_UDP_EV_ICMP = 2 } mt_udp_event;  /* ICMP arg = (type<<8)|code */
typedef void (*mt_udp_cb)(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user);
typedef struct { mt_ip4 src; uint16_t sport; const uint8_t *data; uint16_t len; } mt_udp_dgram;
/* buf (owned by the caller, any alignment, must stay valid until release) receives queued
 * datagrams. NULL if no free slot, port in use, or buflen < 1024. lport 0 = ephemeral
 * (49152..65535). DATA fires once per datagram, before mt_input returns; no callback fires
 * after mt_udp_release. */
mt_udp  *mt_udp_create(mt_stack *s, uint16_t lport, uint8_t *buf, uint32_t buflen, mt_udp_cb cb,
                       void *user);
uint16_t mt_udp_port(const mt_udp *u);
/* len <= 65507 else MT_ERR_INVAL; checksum 0 sends field 0. Returns what mt_ip_output returns. */
int      mt_udp_send(mt_udp *u, mt_ip4 dst, uint16_t dport, const uint8_t *p, uint16_t len,
                     int checksum, uint32_t now);
/* 1 and *out filled (data points into the receive buffer) if a datagram is queued. */
int      mt_udp_peek(mt_udp *u, mt_udp_dgram *out);
void     mt_udp_done(mt_udp *u);                       /* drop the peeked (oldest) datagram */
uint32_t mt_udp_queued(const mt_udp *u);               /* number of queued datagrams */
void     mt_udp_release(mt_udp *u);

/* ---- DNS resolver (mt_dns.c) ---- */
#define MT_DNS_MAXADDR 4          /* matches MacTCP hostInfo addr[4] */
#define MT_DNS_NAMELEN 255
typedef enum { MT_DNS_OK = 0, MT_DNS_NOT_FOUND, MT_DNS_NO_ANSWER, MT_DNS_TIMEOUT,
               MT_DNS_NO_SERVER, MT_DNS_BUSY, MT_DNS_BAD_NAME,
               MT_DNS_PENDING   /* out->status while a query is in progress */
} mt_dns_status;
typedef enum { MT_DNS_Q_A = 1, MT_DNS_Q_PTR = 12, MT_DNS_Q_HINFO = 13, MT_DNS_Q_MX = 15 } mt_dns_qtype;
typedef struct { mt_dns_status status; char cname[MT_DNS_NAMELEN + 1];
  mt_ip4 addr[MT_DNS_MAXADDR]; uint8_t naddr;                 /* A */
  char cpu[64], os[64];                                        /* HINFO */
  uint16_t mx_pref; char mx_name[MT_DNS_NAMELEN + 1];          /* MX (lowest pref) */
} mt_dns_result;                                               /* PTR answer -> cname */
typedef void (*mt_dns_cb)(void *user, const mt_dns_result *r);
#define MT_DNS_IN_PROGRESS 1
/* Returns 0 (= MT_DNS_OK; *out filled, cb NOT called) for dotted-decimal names and cache hits,
 * MT_DNS_IN_PROGRESS (1; cb(user, out) fires later from mt_input / mt_tick; `out` must stay
 * valid until then; `name` is copied), or the NEGATED mt_dns_status (e.g. -MT_DNS_BUSY) on an
 * immediate error. (Errors are negative because positive status values would collide with the
 * "in progress" value 1.) The first network query lazily creates one ephemeral UDP endpoint
 * (counts against max_udp; kept for the stack's lifetime); if no slot is free the query fails
 * with -MT_DNS_BUSY. */
int  mt_dns_query(mt_stack *s, const char *name, mt_dns_qtype q, mt_dns_result *out,
                  mt_dns_cb cb, void *user, uint32_t now);
int  mt_dns_ptr_name(mt_ip4 a, char out[32]);    /* "d.c.b.a.in-addr.arpa"; returns its length */
typedef void (*mt_dns_enum_cb)(void *user, const char *name, const mt_ip4 *addr, uint8_t naddr,
                               uint32_t ttl_left_s);
void mt_dns_enum_cache(mt_stack *s, mt_dns_enum_cb cb, void *user, uint32_t now);

/* ---- TCP ---- */
typedef struct mt_tcp mt_tcp;
typedef enum {
  MT_TCP_EV_ESTABLISHED = 1, MT_TCP_EV_DATA, MT_TCP_EV_URGENT, MT_TCP_EV_CLOSING,
  MT_TCP_EV_ULP_TIMEOUT, MT_TCP_EV_TERMINATE, MT_TCP_EV_ICMP
} mt_tcp_event;
typedef enum {   /* mirrors MacTCP TCPTermReason; Plan 2 maps to numeric values */
  MT_TERM_REMOTE_ABORT = 1, MT_TERM_NETWORK_FAILURE, MT_TERM_SEC_PREC_MISMATCH,
  MT_TERM_ULP_TIMEOUT, MT_TERM_ULP_ABORT, MT_TERM_ULP_CLOSE, MT_TERM_SERVICE_ERROR,
  MT_TERM_OPEN_FAILED  /* RST or timeout during SYN-SENT */
} mt_term_reason;
typedef enum { MT_TCP_CLOSED, MT_TCP_LISTEN, MT_TCP_SYN_RCVD, MT_TCP_SYN_SENT, MT_TCP_ESTABLISHED,
  MT_TCP_FIN_WAIT_1, MT_TCP_FIN_WAIT_2, MT_TCP_CLOSE_WAIT, MT_TCP_CLOSING, MT_TCP_LAST_ACK,
  MT_TCP_TIME_WAIT } mt_tcp_state;   /* order matches MacTCP connectionState / 2 */
/* Callbacks run synchronously inside mt_input / mt_tick / the API call that caused them.
 * The ICMP arg is (type<<8)|code; DATA's arg is the number of bytes just queued. */
typedef void (*mt_tcp_cb)(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user);
/* buf is caller-owned (any alignment, valid until release) and is entirely the receive ring
 * (the advertised window is its free space, at most 65535). NULL if no free TCB or
 * buflen < 512 (MacTCP's TCPCreate minimum of 4096 is the glue's to enforce). Send data is not
 * buffered here: see mt_tcp_send_segs. The TCB starts CLOSED. */
mt_tcp *mt_tcp_create(mt_stack *s, uint8_t *buf, uint32_t buflen, mt_tcp_cb cb, void *user);
/* Active open: sends the SYN before returning. lport 0 = ephemeral. MT_ERR_STATE unless CLOSED,
 * MT_ERR_INUSE if the 4-tuple is taken, MT_ERR_NOROUTE / MT_ERR_UNCONFIGURED / MT_ERR_INVAL. */
int  mt_tcp_open(mt_tcp *t, mt_ip4 rip, uint16_t rport, uint16_t lport, uint32_t now);
/* Passive open (0 = any for every argument). The TCB itself becomes the one connection. */
int  mt_tcp_listen(mt_tcp *t, mt_ip4 rip_filter, uint16_t rport_filter, uint16_t lport);
int  mt_tcp_close(mt_tcp *t, uint32_t now);     /* FIN after queued data; MT_ERR_STATE if not open */
void mt_tcp_abort(mt_tcp *t, uint32_t now);     /* RST if synchronized; TERMINATE(ULP_ABORT) */
void mt_tcp_release(mt_tcp *t);                 /* aborts if needed; frees the TCB; no callbacks */
void mt_tcp_set_ulp_timeout(mt_tcp *t, uint16_t seconds, int abort);  /* default 120 s, abort */
typedef struct { mt_tcp_state state; mt_ip4 lip, rip; uint16_t lport, rport;
  uint32_t unacked, send_queued, rcv_avail, snd_wnd, rcv_wnd, srtt_ms, rto_ms; } mt_tcp_info;
void mt_tcp_get_info(const mt_tcp *t, mt_tcp_info *info);

/* Data transfer. Latency rules: a send with nothing unacked leaves before mt_tcp_send_segs
 * returns; in-order data is readable (and DATA fired) before the mt_input that delivered it
 * returns. */
#define MT_SEGS_MAX 8     /* segments per send request (MacTCP WDS entries) */
#define MT_SENDQ_MAX 2    /* send requests queued per connection (FIFO) */
typedef struct { const uint8_t *p; uint16_t len; } mt_seg;   /* any alignment; len 0 = skipped */
/* status: MT_OK once every byte of the request is acknowledged by the peer, MT_ERR_ABORTED if
 * the connection terminated first (the TERMINATE event follows the done callbacks). */
typedef void (*mt_send_done_cb)(void *ud, int status);
/* Queue a send request: the bytes of segs[0..nsegs) in order, sent from caller memory (see the
 * embedder contract). Requests continue one byte stream in FIFO order and complete in order;
 * `done` (may be NULL) fires once per request. `push` lets a sub-MSS segment that reaches the
 * end of this request go out while data is in flight (Nagle otherwise holds it); `urgent` marks
 * everything up to the end of this request as urgent. Transmits what the policy allows before
 * returning. MT_OK, MT_ERR_INVAL (segs NULL, nsegs 0 or > MT_SEGS_MAX, a non-empty segment with a
 * NULL pointer, or 0 bytes in total), MT_ERR_STATE unless ESTABLISHED / CLOSE_WAIT, MT_ERR_BUSY
 * when MT_SENDQ_MAX requests are already queued. */
int      mt_tcp_send_segs(mt_tcp *t, const mt_seg *segs, uint8_t nsegs, int push, int urgent,
                          mt_send_done_cb done, void *ud, uint32_t now);
uint32_t mt_tcp_recv(mt_tcp *t, uint8_t *dst, uint32_t max, uint32_t now);   /* copy out + consume */
/* Contiguous run of received data starting `off` bytes in (0 if none); *p points into the ring. */
uint32_t mt_tcp_peek(mt_tcp *t, uint32_t off, const uint8_t **p);
/* Drop n received bytes; sends a window update when the window reopens meaningfully. */
void     mt_tcp_consume(mt_tcp *t, uint32_t n, uint32_t now);
/* 1 if unread data precedes the urgent mark; *bytes_to_mark = bytes still to read up to it. */
int      mt_tcp_urgent(const mt_tcp *t, uint32_t *bytes_to_mark);

void      mt_input(mt_stack *s, const uint8_t *frame, uint16_t len, uint32_t now);
void      mt_tick(mt_stack *s, uint32_t now);
uint32_t  mt_next_deadline(const mt_stack *s, uint32_t now);   /* ms from now, or MT_NO_DEADLINE */

#endif
