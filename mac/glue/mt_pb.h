/* mt_pb.h - the .IPP parameter-block dispatcher: MacTCP's application interface on top of the
 * portable core (core/mt.h).
 *
 * The driver entry (Task 10) hands every PBControl call to mt_pb_control, the Ethernet input
 * path hands frames to mt_pb_input, and a timer calls mt_pb_tick when mt_pb_next_deadline_ms
 * reaches 0. All three may run at interrupt time; each masks the stack's interrupts
 * (mt_os_irq_save) for its duration, so calls into one MtDrv are serialised. The expected
 * values of every result are those of the real MacTCP 2.1 (docs/dev/MACTCP-REFERENCE-MATRIX.md).
 *
 * RESULT CONTRACT (authoritative; the driver entry of Task 10 implements exactly this):
 *  1. At entry the driver takes the PB off the Device Manager's driver queue (and clears the
 *     driver's busy state) before calling mt_pb_control, so several PBs can be pending at once.
 *     It never calls jIODone for a PB the glue owns.
 *  2. r == inProgress (1): the glue owns the PB. The driver returns noErr without reading or
 *     writing the PB again: it may already be completed (and even reissued by the app from its
 *     completion routine) before mt_pb_control returns. The glue runs mt_os_complete for it
 *     exactly once (later, or already inside mt_pb_control when an ASR must follow the
 *     completion: a TCPRcv that leaves data, matrix 500, or drains it after the FIN, 601).
 *  3. r <= 0 and r != -28: the call finished inside mt_pb_control and ioResult already holds r.
 *     The driver runs ioCompletion (if nonzero) once, after mt_pb_control has returned (glue
 *     depth 0), with A0 = the PB and the same pointer at 4(SP), the app's A5, plain RTS
 *     (matrix 150), then returns r in D0.
 *  4. r == -28 (MT_PB_NOTOPEN, driver unconfigured): no completion routine runs (matrix
 *     "Unconfigured run", U01-U12: the poll sees -28, no C event); ioResult is -28.
 *  5. mt_os_complete and mt_os_asr are re-entrant (the routine may call the driver again,
 *     which calls them again) and must not use the Memory Manager (interrupt level).
 *
 * Memory: MtDrv holds everything (stream table, PB queues, the TCPGlobalInfo tables); the core
 * arena follows it in the same block (mt_pb_size). No allocation, no globals. The block must
 * be aligned to MT_ARENA_ALIGN (4 on the 68k, 8 on 64-bit hosts) and stay put (stream handles
 * and RDS pointers handed to applications are addresses inside it and inside their buffers). */
#ifndef MT_PB_H
#define MT_PB_H

#include <stdint.h>
#include "mt.h"
#include "mt_mactcp.h"
#include "mt_os.h"

#define MT_PB_MAX_TCP     16     /* stream table size; mt_limits.max_tcp may be smaller */
#define MT_PB_SENDQ       8      /* TCPSend PBs pending per stream (queued FIFO, like MacTCP) */
#define MT_PB_RCVQ        4      /* TCPRcv / TCPNoCopyRcv PBs pending per stream (FIFO) */
#define MT_PB_LOANS       4      /* NoCopyRcv buffers lent out per stream */
#define MT_PB_MIN_RCVBUF  4096u  /* TCPCreate minimum (matrix 200) */
#define MT_PB_WDS_MAX     64     /* WDS entries scanned before the WDS counts as unterminated */
#define MT_PB_MAX_UDP     8      /* UDP stream table size; mt_limits.max_udp may be smaller */
#define MT_PB_UDPRQ       4      /* UDPRead PBs pending per stream (FIFO; matrix 906 shows two) */
#define MT_PB_MIN_UDPBUF  2048u  /* UDPCreate minimum (matrix 900) */
#define MT_PB_UDP_GATHER  1472u  /* largest multi-entry UDPWrite (one frame); a one-entry WDS is unlimited */
#define MT_PB_ECHOQ       4      /* ICMPEcho requests waiting for a reply or timeout */
#define MT_PB_ECHO_DATA   64     /* echoed bytes kept for the notify block */
#define MT_PB_ECHO_TMO_S  3      /* ICMPEcho timeout when the PB gives none (matrix A00..A03 used 3) */
#define MT_PB_UDP_MTU_OFFNET 548 /* UDPMaxMTUSize towards a host off our subnet (matrix 910) */

/* Results that are not .IPP codes: notOpenErr (Device Manager: driver not open, matrix
 * "Unconfigured run") and the -4 MacTCP returns for csCodes it does not implement (matrix 120). */
#define MT_PB_NOTOPEN     (-28)
#define MT_PB_UNIMP       (-4)

typedef struct MtStream MtStream;
typedef struct MtDrv MtDrv;

/* One pending TCPSend PB. Stream offsets count bytes queued on this connection from 0. */
typedef struct MtSendRec {
    MtAnyPb *pb;
    uint32_t len;               /* bytes in its WDS (sendLength is ignored, matrix 404b) */
    uint32_t end;               /* stream offset just past its last byte */
    uint8_t  push, urg;
} MtSendRec;

/* One core send request (mt_tcp_send_segs): consecutive bytes of one or more PBs. */
typedef struct MtCoreReq {
    mt_seg    seg[MT_SEGS_MAX]; /* referenced by the core until its done fires */
    MtStream *s;
    uint32_t  len, end;         /* bytes; stream offset just past its last byte */
    uint16_t  conn;             /* s->conn when submitted: stale dones are ignored */
    uint8_t   busy;
} MtCoreReq;

typedef struct MtRcvRec {
    MtAnyPb *pb;
    uint32_t dl;                /* absolute ms deadline (commandTimeoutValue) if timed */
    uint8_t  timed;
} MtRcvRec;

/* Receive-ring bytes handed out by TCPNoCopyRcv (key = the first RDS pointer) or copied out
 * by a TCPRcv while older loans were outstanding (returned = 1 at once). The ring is consumed
 * in order, so bytes are given back to the core only when every older loan is returned. */
typedef struct MtLoan {
    MacPtr32 key;
    uint32_t len;
    uint8_t  returned;
} MtLoan;

struct MtStream {
    mt_tcp   *t;
    MtDrv    *d;
    StreamPtr handle;           /* address of this entry + 2 * (gen & 7) */
    MacPtr32  asr, user, rbuf;
    uint32_t  rlen;
    uint8_t   used;             /* 0 free, 1 live, 2 released (core release deferred) */
    uint8_t   gen;              /* bumped on every TCPCreate of this slot */
    uint16_t  conn;             /* bumped on every open: PBs and requests of older connections are stale */
    uint8_t   fin;              /* the peer's FIN has arrived */
    uint8_t   closing_sent;     /* TCPClosing ASR delivered */
    uint8_t   close_deferred;   /* TCPClose waits for queued PB bytes to reach the core */
    uint8_t   in_data_asr;      /* a TCPDataArrival ASR of this stream is running */
    uint8_t   data_due;         /* ...and another one is due when it returns (no recursion) */
    uint8_t   ulp_s, ulp_abort, tos, prec;
    /* open / close */
    MtAnyPb  *open_pb;
    uint32_t  open_dl;
    uint8_t   open_timed;
    int16_t   open_err;         /* result for open_pb if the connection ends (0 = default) */
    MtAnyPb  *close_pb;
    /* send: sq[0..sq_n) FIFO; sq[0..sub_i) are wholly handed to the core */
    MtSendRec sq[MT_PB_SENDQ];
    uint8_t   sq_n, sub_i, sub_e;    /* sub_e: next WDS entry of sq[sub_i] */
    uint32_t  sub_left;              /* bytes of sq[sub_i] not yet handed to the core */
    uint32_t  q_total, sub_total, acked;   /* stream offsets: queued, submitted, ACKed (whole requests) */
    MtCoreReq req[MT_SENDQ_MAX];
    uint8_t   req_h, req_n;
    /* receive */
    MtRcvRec  rq[MT_PB_RCVQ];
    uint8_t   rq_n, loan_n;
    MtLoan    loan[MT_PB_LOANS];
    uint32_t  lent;
};

/* UDPWrite / ICMPEcho destinations the glue refuses with ipBadAddr: 0.0.0.0 and class D / E
 * other than the limited broadcast (the core does not route them; matrix: not probed). */
static inline int mt_pb_dest_ok(mt_ip4 a) {
    return a != 0 && (a < 0xE0000000u || a == 0xFFFFFFFFu);
}

/* One pending UDPRead. */
typedef struct MtUdpRead {
    MtAnyPb *pb;
    uint32_t dl;                /* absolute ms deadline (timeOut seconds) if timed */
    uint8_t  timed;
} MtUdpRead;

/* A UDP stream (UDPCreate .. UDPRelease). UDPRead hands out a pointer to the oldest datagram
 * inside the app's receive buffer (matrix 903); it stays queued in the core until
 * UDPBfrReturn, so one datagram is lent at a time (loan = its address, 0 = none). */
typedef struct MtUdpStream {
    mt_udp   *u;
    MtDrv    *d;
    StreamPtr handle;           /* address of this entry + 2 * (gen & 7) */
    MacPtr32  asr, user, rbuf, loan;
    uint32_t  rlen;
    uint8_t   used;             /* 0 free, 1 live, 2 releasing (pending reads being failed) */
    uint8_t   gen;
    uint8_t   rq_n;
    MtUdpRead rq[MT_PB_UDPRQ];
} MtUdpStream;

/* One ICMPEcho in flight. blk is MacTCP's own copy of the PB that the icmpCompletion routine
 * gets (matrix "ICMP echo notify"); the app's PB itself completed 0 when the request was sent. */
typedef struct MtEcho {
    ICMPParamBlock blk;
    uint32_t dl, t0, dest;      /* absolute ms deadline, ms at the request, target address */
    uint16_t id, seq;
    uint8_t  state;             /* 0 free, 1 waiting, 2 notifying (slot not reusable yet) */
} MtEcho;

struct MtDrv {
    mt_stack *s;
    uint8_t   configured;       /* an address is set: otherwise every call gets notOpenErr */
    uint8_t   depth;            /* > 0 while the glue is calling out (app routines, core callbacks) */
    uint8_t   in_core;          /* inside mt_input / mt_tick (they must not be re-entered) */
    uint8_t   ntcp, next_slot;
    uint8_t   nudp, next_uslot;
    uint16_t  echo_seq;
    uint16_t  rel_mask;         /* streams whose core release is deferred */
    uint32_t  now;
    uint32_t  reenter_drops;    /* input/tick calls refused because the core was already running */
    uint32_t  early_done;       /* TCPSend PBs completed before their merged core request's done */
    MtStream  tcp[MT_PB_MAX_TCP];
    TCPParam  gparam;           /* TCPGlobalInfo hands out pointers to these */
    TCPStats  gstats;
    StreamPtr cdb[MT_PB_MAX_TCP];
    TCPConnectionStats cstats;  /* TCPStatus.connStatPtr (not tracked: zeros) */
    ICMPReport icmp;            /* TCP/UDP ICMPReceived report, valid during the ASR */
    MtUdpStream udp[MT_PB_MAX_UDP];
    MtEcho    echo[MT_PB_ECHOQ];
    uint8_t   echo_data[MT_PB_ECHO_DATA];   /* reply payload lent to the notify block */
    uint8_t   ubuf[MT_PB_UDP_GATHER];       /* gathers a multi-entry UDPWrite */
    uint8_t   lap_mac[8];       /* LAPStats.ifPhysicalAddress */
    uint8_t   lap_if[12];       /* LAPStats.ifString: Pascal string, NUL terminated (a guess) */
    arp_entry arp[ARP_TABLE_SIZE];   /* LAPStats.AddrXlation: all zero (not mirrored from the core) */
};

/* Bytes needed for the MtDrv plus the core arena for `lim`. */
uint32_t mt_pb_size(const mt_limits *lim);
/* NULL if len is short, lim->max_tcp > MT_PB_MAX_TCP or mem is misaligned. Starts unconfigured. */
MtDrv   *mt_pb_init(void *mem, uint32_t len, const mt_limits *lim, const mt_link *link,
                    uint32_t seed);
/* Set the interface configuration; an address of 0 makes the driver unconfigured (every
 * call returns notOpenErr -28, as an unopened .IPP does, matrix "Unconfigured run"). */
void     mt_pb_set_config(MtDrv *d, const mt_ifconfig *c);

int16_t  mt_pb_control(MtDrv *d, MtAnyPb *pb);
void     mt_pb_input(MtDrv *d, const uint8_t *frame, uint16_t len);
uint32_t mt_pb_next_deadline_ms(MtDrv *d);     /* ms from now, or MT_NO_DEADLINE */
void     mt_pb_tick(MtDrv *d);

/* The one table each for core -> MacTCP codes (exported so every row is tested). */
uint16_t mt_pb_event_code(int core_ev);        /* mt_tcp_event -> TCP ASR eventCode, 0 = none */
uint16_t mt_pb_term_reason(int core_reason);   /* mt_term_reason -> terminReason, 0 = no ASR */
int16_t  mt_pb_icmp_type(uint32_t core_arg);   /* (type << 8) | code -> ICMPReport.reportType */

/* ---- glue-internal (mt_pb.c <-> mt_pb_tcp.c) ---- */
void     mt_pb_call_complete(MtDrv *d, MtAnyPb *pb, int16_t err);
void     mt_pb_call_asr(MtDrv *d, const MtStreamAsr *a, uint16_t ev, uint16_t reason, MacPtr32 icmp);
int16_t  mt_pb_tcp_control(MtDrv *d, MtAnyPb *pb);
void     mt_pb_tcp_after_core(MtDrv *d);       /* close / send progress after input and ticks */
void     mt_pb_tcp_timers(MtDrv *d);           /* expire command timeouts */
uint32_t mt_pb_tcp_deadline(MtDrv *d);         /* earliest command timeout, ms from now */
void     mt_pb_tcp_free(MtDrv *d, MtStream *s);/* the deferred mt_tcp_release */
int16_t  mt_pb_udp_control(MtDrv *d, MtAnyPb *pb);   /* csCodes 20..29 (mt_pb_udp.c) */
void     mt_pb_udp_timers(MtDrv *d);           /* expire UDPRead timeouts */
uint32_t mt_pb_udp_deadline(MtDrv *d);
int16_t  mt_pb_ip_control(MtDrv *d, MtAnyPb *pb);    /* csCodes 15, 17, 19 (mt_pb_ip.c) */
void     mt_pb_ip_init(MtDrv *d);              /* echo callback, constant strings */
void     mt_pb_ip_timers(MtDrv *d);            /* expire ICMPEcho timeouts */
uint32_t mt_pb_ip_deadline(MtDrv *d);

#endif /* MT_PB_H */
