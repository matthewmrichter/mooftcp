#ifndef PBFAKE_H
#define PBFAKE_H

/* Host implementation of mac/include/mt_os.h for the PB dispatcher tests.
 *
 *  - A fake 32-bit address space: PBF_SIZE bytes of host memory mapped at PBF_BASE. Everything
 *    whose Mac address the glue sees (PBs, WDS/RDS arrays, buffers, the MtDrv itself) is
 *    allocated there with pbf_alloc. mt_os_ptr / mt_os_addr translate; anything outside the
 *    space (other than 0) counts as a misuse in pbf_bad.
 *  - Routine addresses: pbf_compl / pbf_asr register a host function and return a fake code
 *    address for ioCompletion / notifyProc.
 *  - Every mt_os_complete and mt_os_asr call is appended to pbf_log in order (also those
 *    the Device Manager model pbt_call makes for immediate results), so tests can assert the
 *    order of completions and ASRs.
 *  - The clock is pbf_now (ms); interrupts are a nesting counter. */

#include <stdint.h>
#include "mt_os.h"
#include "mt_pb.h"
#include "net.h"

#define PBF_BASE 0x00400000u
#define PBF_SIZE (4u << 20)

typedef void (*pbf_compl_fn)(MtAnyPb *pb);
typedef void (*pbf_asr_fn)(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp);

typedef struct {
    char kind;                    /* 'C' completion, 'A' ASR */
    MacPtr32 pb;                  /* C: the PB */
    int16_t res, cs;              /* C: result, csCode */
    StreamPtr stream;             /* A */
    uint16_t ev, reason;          /* A */
    MacPtr32 user, icmp;          /* A */
    MacPtr32 a5;                  /* A: the A5 the ASR would run with */
} pbf_ev;
#define PBF_LOG 2048

extern uint32_t pbf_now;
extern pbf_ev pbf_log[PBF_LOG];
extern int pbf_nlog;
extern int pbf_bad;               /* misuse seen by the fake (bad pointer, unknown routine, ...) */
extern int pbf_irq, pbf_irq_max, pbf_irq_calls;
extern int pbf_in_int;            /* what mt_os_in_interrupt reports */
extern int pbf_mask, pbf_mask_max;      /* mt_os_mask depth (0 outside) and its maximum */
extern MacPtr32 pbf_app_lo, pbf_app_hi; /* mt_os_app_mem range, [lo, hi) (default: all; classic mode) */
extern void (*pbf_leave_hook)(void);   /* run at every outermost irq restore (gate leave) */
extern struct MtGate *pbf_gate;           /* set: irq_save/restore move its depth (the Mac gate) */
extern MacPtr32 pbf_a5;           /* what mt_os_a5 reports (the "current application's" A5) */

/* MultiFinder model (Plan 3 Task 12). pbf_app_part != 0: mt_os_app_mem reports the partition
 * [pbf_app_lo, pbf_app_hi) with header word pbf_app_h0 = 0x80000000 | (size + 8) (return 1); mt_os_app_alive(m)
 * is true while a live pbf_parts entry has m's lo and h0. pbf_mf_launch allocates a partition in fake memory and
 * makes it current; pbf_mf_launch_at reuses memory (a new application in a quit one's partition);
 * pbf_mf_switch makes another one current (ApplZone, A5, header: what MultiFinder swaps); pbf_mf_quit
 * disposes of it WITHOUT a goodbye (what MultiFinder does). pbf_mf_alloc / pbf_mf_compl / pbf_mf_asr give
 * memory and routines inside a partition. pbf_dead_calls counts completions and ASRs the glue ran for a
 * PB or routine that lies in a quit partition (a crash on the Mac). */
#define PBF_PARTS 32
typedef struct { MacPtr32 lo, hi; uint32_t h0, name; MacPtr32 a5; uint32_t used; int live; } pbf_part;
extern int pbf_app_part;
extern uint32_t pbf_app_h0, pbf_app_name;   /* name: what mt_os_app_mem reports as the CurApName hash */
extern pbf_part pbf_parts[PBF_PARTS];
extern int pbf_dead_calls;
int      pbf_mf_launch(uint32_t size);
int      pbf_mf_launch_at(MacPtr32 lo, uint32_t size, uint32_t name);   /* name 0: a new application */
uint32_t pbf_mf_left(int i);     /* bytes pbf_mf_alloc can still give in partition i */
void     pbf_mf_switch(int i);
void     pbf_mf_quit(int i);
void     pbf_mf_forget(int i);    /* drop a quit partition from the table (its memory is reused) */
void    *pbf_mf_alloc(int i, uint32_t n);
MacPtr32 pbf_mf_compl(int i, pbf_compl_fn f);
MacPtr32 pbf_mf_asr(int i, pbf_asr_fn f);

extern int pbf_kill_calls;        /* mt_os_enet_kill calls and the last arguments */
extern int16_t pbf_kill_ref;
extern void *pbf_kill_pb;
extern int16_t pbf_kill_result;          /* mt_os_enet_kill's return value and side effect */
extern void (*pbf_kill_hook)(void);
extern const void *pbf_dev_head;         /* head request of .ENET0's queue, for mt_os_enet_blocked */
extern int pbf_blocked_calls;
extern int pbf_reattach_calls;           /* the fake mt_enet_reattach_io */
extern int pbf_reattach_result;

void     pbf_reset(void);         /* frees the address space, routines, log, clock */
void    *pbf_alloc(uint32_t n);   /* zeroed, 8-aligned; NULL when full */
MacPtr32 pbf_compl(pbf_compl_fn f);
MacPtr32 pbf_asr(pbf_asr_fn f);
MacPtr32 pbf_a(const void *p);    /* = mt_os_addr */
void    *pbf_p(MacPtr32 a);       /* = mt_os_ptr */

/* ---- test harness on top: one driver over a scripted peer ---- */
typedef struct {
    MtDrv *d;
    fake_link fl;
} pbt;
/* Driver in the fake address space, default limits; configured = address T_ME (/24, gateway
 * T_GW, ARP for T_PEER pre-resolved), else address 0. */
void     pbt_init(pbt *x, int configured);
/* A zeroed TCPiopb (fake memory) with csCode, stream and a recording completion routine. */
TCPiopb *pbt_pb(int16_t cs, StreamPtr s);
/* Driver-entry model (mt_pb.h result contract): mt_pb_control; a result other than
 * inProgress and -28 is stored and the completion routine is run, as IODone would. */
int16_t  pbt_call(pbt *x, TCPiopb *pb);
/* Scripted peer: T_PEER port PBT_PEER_PORT, MSS 1460 on SYNs. */
#define PBT_PEER_PORT 23
typedef struct { uint16_t lport; uint32_t ps, ms; } pbt_conn;   /* our port, next peer / our seq */
void      pbt_peer(pbt *x, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                   const void *data, uint16_t len);
int       pbt_last_tcp(pbt *x, tcpv *v);     /* last TCP frame captured, 1 if any */
StreamPtr pbt_create(pbt *x, uint32_t buflen, MacPtr32 asr, MacPtr32 user);
TCPiopb  *pbt_open(pbt *x, StreamPtr s, uint16_t lport);   /* pending ActiveOpen to the peer */
int       pbt_connect(pbt *x, StreamPtr s, pbt_conn *c);   /* open + SYN-ACK; 0 = open completed */
StreamPtr pbt_est(pbt *x, MacPtr32 asr, MacPtr32 user, pbt_conn *c);   /* create (8192) + connect */
TCPiopb  *pbt_send(pbt *x, StreamPtr s, const void *data, uint16_t len, int push);  /* one entry */
TCPiopb  *pbt_rcv(pbt *x, StreamPtr s, uint16_t len, int8_t timeout_s);
int16_t   pbt_simple(pbt *x, int16_t cs, StreamPtr s);    /* zeroed PB with only the stream */
/* UDP: a stream (UDPCreate with an rcvBuffLen-byte buffer in fake memory; 0 on failure; the
 * assigned port is stored in *port_out if given), a datagram from T_PEER:sport to T_ME:dport
 * (checksummed), the last UDP datagram we sent (1 if any; *data points into the frame log). */
StreamPtr pbt_ucreate(pbt *x, uint16_t port, uint32_t buflen, MacPtr32 asr, MacPtr32 user,
                      uint16_t *port_out);
void      pbt_udp_in(pbt *x, uint16_t sport, uint16_t dport, const void *data, uint16_t len);
typedef struct { uint16_t sport, dport, len, cksum; const uint8_t *data; int cksum_ok; mt_ip4 dst; } pbt_udpv;
int       pbt_last_udp(pbt *x, pbt_udpv *v);
/* An ICMP echo reply from `from` to T_ME. */
void      pbt_echo_reply(pbt *x, mt_ip4 from, uint16_t id, uint16_t seq, const void *data,
                         uint16_t len);
/* Log helpers. */
int      pbf_count(char kind);
int      pbf_find(char kind, MacPtr32 pb_or_stream, int from);   /* index or -1 */

#endif
