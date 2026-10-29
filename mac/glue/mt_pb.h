/* mt_pb.h - the .IPP parameter-block dispatcher: MacTCP's application interface on top of the
 * portable core (core/mt.h).
 *
 * The driver entry (Task 10) hands every PBControl call to mt_pb_control, the Ethernet input
 * path hands frames to mt_pb_input, and a timer calls mt_pb_tick when mt_pb_next_deadline_ms
 * reaches 0. All three may run at interrupt time; each masks the stack's interrupts
 * (mt_os_irq_save) for its duration, so calls into one MtDrv are serialised. The expected
 * values of every result are those of the real MacTCP 2.1 (docs/dev/MACTCP-REFERENCE-MATRIX.md).
 *
 * Result convention of mt_pb_control: a value <= 0 is the final result of a call that finished
 * inside mt_pb_control (also stored in ioResult); the caller (the driver entry, like IODone)
 * runs the completion routine for it. inProgress (1) means the glue owns the PB: ioResult is 1
 * (or already final) and the glue calls mt_os_complete exactly once, later or already before
 * returning (a TCPRcv that drains the last data completes itself first so the TCPClosing ASR can
 * follow its completion, as MacTCP orders them).
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

struct MtDrv {
    mt_stack *s;
    uint8_t   configured;       /* an address is set: otherwise every call gets notOpenErr */
    uint8_t   depth;            /* > 0 while the glue is calling out (app routines, core callbacks) */
    uint8_t   in_core;          /* inside mt_input / mt_tick (they must not be re-entered) */
    uint8_t   ntcp, next_slot;
    uint16_t  rel_mask;         /* streams whose core release is deferred */
    uint32_t  now;
    uint32_t  reenter_drops;    /* input/tick calls refused because the core was already running */
    MtStream  tcp[MT_PB_MAX_TCP];
    TCPParam  gparam;           /* TCPGlobalInfo hands out pointers to these */
    TCPStats  gstats;
    StreamPtr cdb[MT_PB_MAX_TCP];
    TCPConnectionStats cstats;  /* TCPStatus.connStatPtr (not tracked: zeros) */
    ICMPReport icmp;            /* TCPICMPReceived report, valid during the ASR */
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

#endif /* MT_PB_H */
