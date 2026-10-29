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
} pbf_ev;
#define PBF_LOG 2048

extern uint32_t pbf_now;
extern pbf_ev pbf_log[PBF_LOG];
extern int pbf_nlog;
extern int pbf_bad;               /* misuse seen by the fake (bad pointer, unknown routine, ...) */
extern int pbf_irq, pbf_irq_max, pbf_irq_calls;
extern int pbf_in_int;            /* what mt_os_in_interrupt reports */

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
/* Device Manager model: mt_pb_control; a result other than inProgress is stored and the
 * completion routine is run, as IODone would. Returns the result. */
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
/* Log helpers. */
int      pbf_count(char kind);
int      pbf_find(char kind, MacPtr32 pb_or_stream, int from);   /* index or -1 */

#endif
