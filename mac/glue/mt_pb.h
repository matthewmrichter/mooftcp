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
#include "mt_view.h"

#define MT_PB_MAX_TCP     16     /* stream table size; mt_limits.max_tcp may be smaller */
#define MT_PB_SENDQ       8      /* TCPSend PBs pending per stream (queued FIFO, like MacTCP) */
#define MT_PB_RCVQ        4      /* TCPRcv / TCPNoCopyRcv PBs pending per stream (FIFO) */
#define MT_PB_LOANS       4      /* NoCopyRcv buffers lent out per stream */
#define MT_PB_MIN_RCVBUF  4096u  /* TCPCreate minimum (matrix 200) */
#define MT_PB_WDS_MAX     64     /* WDS entries scanned before the WDS counts as unterminated */
#define MT_PB_MAX_UDP     8      /* UDP stream table size; mt_limits.max_udp may be smaller */
#define MT_PB_UDPLOANS    8      /* datagrams lent to UDPRead per stream until UDPBfrReturn (matrix 904-904d: 4+) */
#define MT_PB_UDPRQ       4      /* UDPRead PBs pending per stream (FIFO; matrix 906 shows two) */
#define MT_PB_MIN_UDPBUF  2048u  /* UDPCreate minimum (matrix 900) */
#define MT_PB_UDP_GATHER  1472u  /* largest multi-entry UDPWrite (one frame); a one-entry WDS is unlimited */
#define MT_PB_ECHOQ       4      /* ICMPEcho requests waiting for a reply or timeout */
#define MT_PB_ECHO_DATA   64     /* echoed bytes kept for the notify block */
#define MT_PB_ECHO_TMO_S  3      /* ICMPEcho timeout when the PB gives none (matrix A00..A03 used 3) */
#define MT_PB_UDP_MTU_OFFNET 548 /* UDPMaxMTUSize towards a host off our subnet (matrix 910) */
#define MT_PB_DNS         4      /* DNR lookups in flight (private csCode, mt_pb_dns.c); the core has 4 query slots */
#define MT_PB_OWNERS      8      /* MultiFinder applications tracked for the per-application sweep (Task 12) */
#define MT_PB_SWEEP_MS    250    /* periodic sweep while owners are recorded (mt_pb_sweep) */
#define MT_PB_SWEEP_RETRY_MS 20  /* a timer sweep that found a glue frame active retries after this */
#define MT_PB_CO_CTX      3      /* processes a call-out chain may span (yields inside routines; round 4) */
#define MT_PB_ECHO_NONOTIFY_S 10 /* ICMPEcho without icmpCompletion: its slot is held at most this long */

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
    MacPtr32  a5;               /* mt_os_a5() at TCPCreate: the A5 the ASR runs with */
    uint32_t  rlen;
    uint8_t   used;             /* 0 free, 1 live, 2 released (core release deferred) */
    uint8_t   gen;              /* bumped on every TCPCreate of this slot */
    uint16_t  conn;             /* bumped on every open: PBs and requests of older connections are stale */
    uint16_t  born;             /* MtDrv.own_ser at TCPCreate (mt_pb_in_gone) */
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

/* A UDP stream (UDPCreate .. UDPRelease). UDPRead hands out a pointer to the next datagram
 * inside the app's receive buffer (matrix 903); it stays queued in the core until UDPBfrReturn.
 * Up to MT_PB_UDPLOANS datagrams are lent at once (matrix 904-904d), returned in any order:
 * loan[0..loan_n-1] are their addresses in lending order, which is also their order in the
 * core's queue (loan[i] is the core's datagram i), so the next one to lend is datagram loan_n.
 * The loans belong to the stream's owner: release and the goodbye kiss forget them (Task 12). */
typedef struct MtUdpStream {
    mt_udp   *u;
    MtDrv    *d;
    StreamPtr handle;           /* address of this entry + 2 * (gen & 7) */
    MacPtr32  asr, user, rbuf;
    MacPtr32  a5;               /* mt_os_a5() at UDPCreate */
    uint32_t  rlen;
    uint8_t   used;             /* 0 free, 1 live, 2 releasing (pending reads being failed) */
    uint8_t   gen;
    uint8_t   rq_n;
    uint8_t   loan_n;
    uint16_t  born;             /* MtDrv.own_ser at UDPCreate (mt_pb_in_gone) */
    MtUdpRead rq[MT_PB_UDPRQ];
    MacPtr32  loan[MT_PB_UDPLOANS];
} MtUdpStream;

/* One ICMPEcho in flight. blk is MacTCP's own copy of the PB that the icmpCompletion routine
 * gets (matrix "ICMP echo notify"); the app's PB itself completed 0 when the request was sent. */
typedef struct MtEcho {
    ICMPParamBlock blk;
    uint32_t dl, dest;          /* absolute ms deadline, target address */
    uint32_t tk0, user;         /* Ticks at the request; the request's IPEchoPB.userDataPtr */
    uint16_t id, seq;
    uint16_t born;              /* MtDrv.own_ser at the request (mt_pb_in_gone) */
    uint8_t  state;             /* 0 free, 1 waiting, 2 notifying (slot not reusable yet) */
} MtEcho;

/* One DNR lookup (mt_pb_dns.c). The result memory is here, in the resident block, never in the
 * application's heap: the core writes into `res`, and the driver copies it to the caller's hostInfo /
 * returnRec only while the PB is pending (the goodbye kiss cancels pending lookups first). */
typedef struct MtDnsSlot {
    MtAnyPb    *pb;             /* the pending lookup PB */
    MtDrv      *d;
    mt_dns_result res;          /* A / HINFO / MX answer; a PTR answer is res.cname */
    MacPtr32    out;            /* the caller's hostInfo / returnRec */
    uint32_t    addr;           /* PTR: the address */
    uint16_t    born;           /* MtDrv.own_ser at the request (mt_pb_in_gone) */
    uint8_t     state;          /* 0 free, 1 query in the core, 2 PTR waiting for the reverse-lookup scratch */
    uint8_t     op;             /* MT_DNR_A / PTR / HINFO / MX */
} MtDnsSlot;

struct MtDrv {
    mt_stack *s;
    uint8_t   configured;       /* an address is set: otherwise every call gets notOpenErr */
    uint8_t   depth;            /* > 0 while the glue is calling out (app routines, core callbacks) */
    uint8_t   in_core;          /* inside mt_input / mt_tick (they must not be re-entered) */
    uint8_t   co_gate;          /* gate depth (mt_os_gate_depth) when the innermost call-out began */
    uint8_t   rcv_kick;         /* a TCPRcv was queued instead of served inline: serve at the next tick */
    uint8_t   ntcp, next_slot;
    uint8_t   nudp, next_uslot;
    uint16_t  echo_seq;
    uint16_t  rel_mask;         /* streams whose core release is deferred */
    uint32_t  now;
    uint32_t  reenter_drops;    /* input/tick calls refused because the core was already running */
    uint32_t  early_done;       /* TCPSend PBs completed before their merged core request's done */
    uint32_t  n_opened, n_closed, n_aborted;   /* TCPGlobalInfo TCPStats the core does not count: */
    uint32_t  oct_in, oct_out;  /* connections established / ended by close / other ends; data bytes
                                 * arrived and ACKed (MacTCP Watcher shows them, Task 12) */
    MtStream  tcp[MT_PB_MAX_TCP];
    TCPParam  gparam;           /* TCPGlobalInfo hands out pointers to these */
    TCPStats  gstats;
    StreamPtr cdb[MT_PB_MAX_TCP];
    TCPConnectionStats cstats;  /* TCPStatus.connStatPtr (not tracked: zeros) */
    ICMPReport icmp;            /* TCP/UDP ICMPReceived report, valid during the ASR */
    MtUdpStream udp[MT_PB_MAX_UDP];
    MtEcho    echo[MT_PB_ECHOQ];
    MtDnsSlot dns[MT_PB_DNS];
    uint32_t  dead_pb[4];       /* DNR lookups cancelled while queued in the entry (mt_pb_dns.c) */
    uint16_t  dead_gen[4];
    uint8_t   echo_data[MT_PB_ECHO_DATA];   /* reply payload lent to the notify block (even address:
                                             * apps read it by words, ApiProbe A00 nPtrRel, Task 12) */
    uint8_t   ubuf[MT_PB_UDP_GATHER];       /* gathers a multi-entry UDPWrite */
    uint8_t   lap_mac[8];       /* LAPStats.ifPhysicalAddress */
    uint8_t   lap_if[12];       /* LAPStats.ifString: Pascal string, NUL terminated (a guess) */
    arp_entry arp[ARP_TABLE_SIZE];   /* LAPStats.AddrXlation: all zero (not mirrored from the core) */
    /* MultiFinder owners (Plan 3 Task 12): the partitions of the applications that called the driver
     * at task time (mt_os_app_mem returned 1). own[i].lo == 0 = free. mt_pb_sweep forgets the
     * objects of an owner whose partition is gone (no goodbye kiss under MultiFinder). */
    MtAppMem  own[MT_PB_OWNERS];
    uint16_t  own_born[MT_PB_OWNERS];   /* own_ser when the slot was recorded (round 4, mt_pb_in_gone) */
    uint16_t  own_ser;          /* owner serial: bumped at every record, never 0 (0 = unknown age) */
    uint32_t  own_dl;           /* next periodic sweep (ms clock) while n_own > 0 */
    uint32_t  n_own_gone;       /* owners found gone and swept */
    uint32_t  n_own_full;       /* callers not recorded: the table was full (their objects are never swept) */
    uint32_t  n_dead_calls;     /* routines of a gone owner not called (the sweep had not run yet) */
    uint32_t  n_dead_drops;     /* frames dropped / ticks skipped: an owner was gone and the sweep could not run */
    uint32_t  n_gone_call, n_gone_timer, n_gone_rx;   /* owners swept from a Control entry / the timer / the input path */
    /* The processes whose stacks hold the active call-out chain (Task 12 round 4, mt_pb_unstick): co_ctx[0] is
     * the CURRENT application's partition when the outermost call-out began at task time (none when it began at
     * interrupt time, or without MultiFinder: co_n 0); a nested task-time call-out made while another process
     * is current (the chain yielded, or it is dead and not proven so yet) appends that process with the depth
     * below it, and is dropped again when depth returns there. If every listed process quit, the frames never
     * return. More than MT_PB_CO_CTX processes: co_over = that depth + 1 (nothing is abandoned until it clears). */
    MtAppMem  co_ctx[MT_PB_CO_CTX];
    uint8_t   co_depth[MT_PB_CO_CTX];
    uint8_t   co_n, co_over;
    uint32_t  n_unstuck;        /* glue (and gate) frames abandoned because the processes running them are gone */
    uint32_t  n_unstick_wait;   /* abandonment postponed: a live frame holds the gate above the routine, or co_over */
    uint8_t   n_own;            /* used entries of own[] */
    uint8_t   dead_n;           /* a lone byte before the buffers above made them odd (Task 12) */
    uint8_t   co_gen;           /* bumped by every abandonment: a routine that resumes after one skips its depth-- */
    uint8_t   pad_co;
#ifndef MT_NO_DHCP
    struct MtStatusView *view;  /* DHCP mode: the status mirror the event callback updates (NULL: none) */
#endif
};

/* Completion nesting limit for reads served inline: a TCPRcv / TCPNoCopyRcv issued with data
 * available from a completion routine nested this deep (MtDrv.depth) is queued and served from the
 * next tick instead, so a reader that re-issues a 1-byte read from its completion cannot grow the
 * stack without bound (final review, fix 2). */
#define MT_PB_INLINE_MAX 3

/* Bytes needed for the MtDrv plus the core arena for `lim`. */
uint32_t mt_pb_size(const mt_limits *lim);
/* NULL if len is short, lim->max_tcp > MT_PB_MAX_TCP or mem is misaligned. Starts unconfigured. */
MtDrv   *mt_pb_init(void *mem, uint32_t len, const mt_limits *lim, const mt_link *link,
                    uint32_t seed);
/* Set the interface configuration; an address of 0 makes the driver unconfigured (every
 * call returns notOpenErr -28, as an unopened .IPP does, matrix "Unconfigured run"). */
void     mt_pb_set_config(MtDrv *d, const mt_ifconfig *c);
#ifndef MT_NO_DHCP   /* MoofTCP Lite has no DHCP mode */
/* DHCP mode (Plan 4, docs/dev/DHCP-DESIGN.md section 4): apply c with the address forced to 0 (its
 * mask/gateway/DNS/domain are the fallbacks for options the server does not send), install the event
 * callback (mt_pb_dhcp_event, user = d) and start the core's client with `hint` as the requested
 * address. view (may be NULL) is the status mirror the callback keeps up to date. The driver stays
 * unconfigured (every call -28) until the first lease; from then on it stays configured for the rest
 * of the boot, even when the lease is lost (the core drops the address; calls on live streams keep
 * working, ruling). Returns what mt_dhcp_start returns. */
int      mt_pb_dhcp_start(MtDrv *d, const mt_ifconfig *c, mt_ip4 hint, struct MtStatusView *view);
/* The core's DHCP callback (exported for the host tests). */
void     mt_pb_dhcp_event(void *user, int ev, uint8_t state, mt_ip4 addr, uint32_t lease_s);
#endif

int16_t  mt_pb_control(MtDrv *d, MtAnyPb *pb);
void     mt_pb_input(MtDrv *d, const uint8_t *frame, uint16_t len);
uint32_t mt_pb_next_deadline_ms(MtDrv *d);     /* ms from now, or MT_NO_DEADLINE */
void     mt_pb_tick(MtDrv *d);
/* The Device Manager's goodbye kiss (csCode -1, the application heap is about to be reset):
 * forget every TCP and UDP stream whose receive buffer, ASR or any pending PB lies in the
 * application's memory (mt_os_app_mem: its MultiFinder partition, else ApplZone..BufPtr), every
 * ICMPEcho whose notify routine does and every DNR lookup whose PB does (system-heap clients and
 * other partitions keep theirs), WITHOUT calling any completion routine or ASR (the application's
 * code and buffers are going away). Core objects are released, so the core stops touching the app's
 * buffers (a live connection gets an RST). The UDP loans go with their stream. Call it at task time
 * with no glue frame active (depth 0, not inside the core): the driver does. */
void     mt_pb_goodbye(MtDrv *d);
/* The same for an explicit range (the goodbye's and the sweep's worker). */
void     mt_pb_forget(MtDrv *d, const MtAppMem *m);
/* Nonzero if p lies in [m->lo, m->hi) (24-bit compare when hi fits in 24 bits). */
int      mt_pb_in_app(const MtAppMem *m, MacPtr32 p);
/* mt_pb_in_app(m, p), minus the objects of a new tenant of the range (round 4; used by every forget): a
 * dead owner that could not be swept at once (a glue frame was active) may have had its block reused by
 * an application that already called and created objects there. Those are told apart by age: an object
 * born (MtDrv.own_ser at its creation) at or after the live overlapping owner's record (own_born) is the
 * tenant's; born 0 = unknown (the entry's queued PBs): the range alone decides. */
int      mt_pb_in_gone(const MtDrv *d, const MtAppMem *m, uint16_t m_born, MacPtr32 p, uint16_t born);
                                 /* m_born: the age of m's own record (a tenant is an owner recorded AFTER
                                  * it; an older overlapping record is a previous occupant's, alias or not) */

/* MultiFinder per-application cleanup (Plan 3 Task 12). note != 0: the call is made at task time by
 * an application, so record its partition (a recorded owner whose range the new partition overlaps
 * is gone: its block was reused). Then every owner whose partition no longer exists
 * (mt_os_app_alive) is forgotten: drop(ctx, range) first (the driver entry's queued PBs), then
 * mt_pb_forget. Does nothing while a glue frame is active or the core runs (retried at the next
 * call or tick). Returns the number of owners forgotten. The driver calls it at every Control entry
 * and from the timer (mt_pb_next_deadline_ms asks for a tick every MT_PB_SWEEP_MS while owners are
 * recorded). Without MultiFinder no owner is ever recorded and it returns at once. */
int      mt_pb_sweep(MtDrv *d, int note, void (*drop)(void *ctx, const MtAppMem *m), void *ctx);
#define MT_PB_SWEEP_NOTE_ONLY 2  /* note value: a task-time call while the gate is held (abandoned frames whose
                                  * release is postponed, or a nested call-out): record the caller, forget nothing */
/* A routine (completion / ASR) run inline by the glue whose PROCESS quits from inside it (ExitToShell,
 * a MultiFinder force quit) never returns: depth (and co_gate, in_core) stay set and every sweep would
 * bail. When the outermost call-out starts at task time, the glue records the current application's
 * partition (co_ctx: the process whose stack holds the frames; under MultiFinder the glue runs app A's
 * routine inline while app B is current, so this is NOT the routine's owner, and ExitToShell from inside
 * it ends B). A call-out that begins at interrupt time records nothing (no process can quit there, and
 * the interrupted process may be mid-quit). If depth != 0 and co_ctx is provably dead (its partition
 * header changed, or the task-time caller's partition overlaps it with another identity: the block was
 * reused), and the gate holds exactly the levels the abandoned chain entered (gate_below == co_gate:
 * no live frame above it), mt_pb_unstick clears depth, co_gate and in_core, bumps co_gen and returns 1;
 * the caller then releases the gate levels the chain held (co_gate, read before the call). A routine
 * of a live process (merely slow, hung, or yielding) is never reset; a different application calling
 * proves nothing (it may be running the routine's drain). */
int      mt_pb_unstick(MtDrv *d, const MtAppMem *cur, unsigned gate_below);   /* cur: the task-time
                                 * caller's partition (NULL at interrupt time / classic); gate_below: the
                                 * gate depth without the detector's own holding */

/* 1 while every recorded owner's partition header is unchanged (the input path's per-frame check,
 * no gate needed: reads only). */
int      mt_pb_owners_alive(const MtDrv *d);

/* Owner table slots are written so that a reader without the gate (quiet() at task time, the input
 * path) sees either a whole owner or an empty slot: a new owner's lo is stored LAST, a dropped owner's
 * lo is cleared FIRST (compiler barriers between), readers snapshot lo once and treat 0 as empty, and
 * every reader loop is bounded by MT_PB_OWNERS (n_own is only a hint). */

/* The one table each for core -> MacTCP codes (exported so every row is tested). */
uint16_t mt_pb_event_code(int core_ev);        /* mt_tcp_event -> TCP ASR eventCode, 0 = none */
uint16_t mt_pb_term_reason(int core_reason);   /* mt_term_reason -> terminReason, 0 = no ASR */
int16_t  mt_pb_icmp_type(uint32_t core_arg);   /* (type << 8) | code -> ICMPReport.reportType */

/* ---- glue-internal (mt_pb.c <-> mt_pb_tcp.c) ---- */
void     mt_pb_call_complete(MtDrv *d, MtAnyPb *pb, int16_t err);
void     mt_pb_flush(MtDrv *d);                /* deferred core releases, if no glue frame is active */
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
int16_t  mt_pb_dns_control(MtDrv *d, MtAnyPb *pb);   /* csCode MT_CS_DNR (mt_pb_dns.c) */
void     mt_pb_dns_goodbye(MtDrv *d, const MtAppMem *m, uint16_t m_born);   /* cancel the lookups of the
                                                 * application in m (m_born as mt_pb_in_gone) */
/* Nonzero if p lies in a recorded MultiFinder owner whose partition is gone (counts n_dead_calls): the
 * glue then neither calls that owner's routines nor writes its result records (Task 12). */
int      mt_pb_gone(MtDrv *d, MacPtr32 p);
void     mt_pb_ip_init(MtDrv *d);              /* echo callback, constant strings */
void     mt_pb_ip_timers(MtDrv *d);            /* expire ICMPEcho timeouts */
uint32_t mt_pb_ip_deadline(MtDrv *d);

#endif /* MT_PB_H */
