/* mt_enet.h - the Ethernet side of the MoofTCP glue: opens the DaynaPORT driver (.ENET0),
 * attaches the 0x0800 and 0x0806 protocol handlers and sends frames (Task 9).
 *
 * The interface is documented, with the evidence, in docs/dev/MACTCP-INTERFACE.md "Ethernet
 * driver calls". The facts this file relies on:
 *  - csCodes EAttachPH 247, EDetachPH 248, EWrite 249, EGetInfo 252; csParam at offset 28.
 *  - The protocol handler runs at interrupt time (SR 0x2600) and may be called by the driver from
 *    inside EWrite. Neither a synchronous EWrite from there nor one under an interrupt mask
 *    returns, so every transmit here is ASYNCHRONOUS: a frame is copied into a transmit slot, the
 *    call returns at once and the slot is free again when its ioResult is no longer 1.
 *  - Received frames reach the core through the gate (mt_gate.h), never directly.
 *
 * Plain C header (no Toolbox types); mt_enet.c holds the Toolbox calls. The caller allocates
 * MtEnet and MtGate in non-relocatable memory (the driver keeps pointers into them). */
#ifndef MT_ENET_H
#define MT_ENET_H

#include <stdint.h>
#include "mt_gate.h"
#include "mt_enet_logic.h"
#ifdef MT_TUNE
#include "mt_timer.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Transmit slots (each about 1.6 KB; 12 since Task 13). A pass at interrupt level (handler, mask 6; timer, mask 1)
 * frees no slot (async EWrite completes after the mask drops), so one ACK that opens the window
 * for N segments needs N slots; the core ignores link.tx failures (Task 13: 8 slots dropped 2 frames per 40 KB send, 12 drop none; core fix is Plan 3). */
#define MT_ENET_TXSLOTS 12u

/* Errors (negative, Mac OSErr style); other values are the Device Manager's own. */
#define MT_ENET_E_NODRIVER (-43)   /* fnfErr from OpenDriver: no .ENET0 */
#define MT_ENET_E_INUSE    (-94)   /* LAPProtErr: another handler (MacTCP) holds the type */
#define MT_ENET_E_BADMAC   (-1)    /* the driver reported no usable Ethernet address */

/* The Ethernet driver's parameter block, csParam at offset 28 (obs). Packed to 2 bytes. */
#pragma pack(push, 2)
typedef struct MtEnetPB {
    uint32_t qLink;
    int16_t  qType;
    int16_t  ioTrap;
    uint32_t ioCmdAddr;
    uint32_t ioCompletion;
    volatile int16_t ioResult;
    uint32_t ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioRefNum;
    int16_t  csCode;
    int16_t  eProtType;
    uint32_t ePointer;           /* handler / buffer / WDS pointer */
    int16_t  eBuffSize;
    int16_t  eDataSize;
    int16_t  spare[10];
} MtEnetPB;                      /* 58 bytes */
typedef struct MtEnetWDS { uint16_t len; uint32_t ptr; } MtEnetWDS;   /* 6 bytes, 0 length ends */
#pragma pack(pop)

typedef struct MtEnetTx {
    MtEnetPB  pb;
    uint8_t   started;           /* pb was handed to the driver and its result not yet checked */
    uint8_t   wd_stage;          /* watchdog: 0 none, 1 KillIO sent, 2 quarantined (reattach asked) */
    uint8_t   wd_seen;           /* wd_t0 is valid */
    uint8_t   pad;
    uint32_t  wd_t0;             /* ms: first watchdog look in flight (stage 0), then KillIO time */
    MtEnetWDS wds[2];
    uint8_t   frame[MT_ENET_MAXFRAME + 2];
} MtEnetTx;

typedef struct MtEnet {
    MtGate   *gate;              /* where the handler stub puts received frames */
    int16_t   refnum;            /* driver reference number (obs: -48) */
    uint8_t   mac[6];
    uint8_t   attached;          /* bit 0: 0x0806, bit 1: 0x0800 */
    uint8_t   opened;
    uint8_t   tx_next;           /* round-robin search start */
    uint8_t   tx_live;           /* 1.1d3 (was a pad byte): slots with started != 0, quarantined ones too (the
                                  * watchdog's scan stops after the last of them); mt_enet_tx_mark_started, reap */
    int16_t   last_err;          /* last nonzero Device Manager result */
    uint32_t  tx_ok, tx_pad, tx_drop_busy, tx_drop_len, tx_drop_closed, tx_err;
    uint32_t  tx_stuck, tx_killed, reattach, reattach_fail;   /* watchdog (Plan 3 Task 5) */
    uint32_t  reattach_blocked, reattach_abandoned;   /* sync call skipped (a request heads .ENET0's queue) / gave up */
    uint32_t  reattach_at;       /* ms: earliest next attempt */
    uint32_t  wd_next;           /* ms: next scan */
    uint8_t   tx_started;        /* slots with started != 0 (0: the watchdog returns at once) */
    uint8_t   wd_fresh;          /* a write started: scan at the next call */
    uint8_t   kill_req;          /* the watchdog asked for a KillIO (performed at task time) */
    uint8_t   reattach_req;      /* the watchdog asked; mt_enet_service performs it at task time */
    uint8_t   reattach_tries;    /* failed detach/attach attempts of the current request */
    uint8_t   blocked_tries;     /* attempts skipped because a request headed the driver's queue */
    MtEnetTx  tx[MT_ENET_TXSLOTS];
} MtEnet;

/* The PB for an async EWrite of slot s's frame, n bytes (mt_enet_write; the perf builds' pump): the whole PB cleared
 * with one memset (1.1d1: was a byte loop), then csCode, refnum and the two-entry WDS. */
#define MT_ENET_CS_WRITE 249
static inline void mt_enet_tx_prep(const MtEnet *e, MtEnetTx *s, uint16_t n)
{
    memset(&s->pb, 0, sizeof s->pb);
    s->wds[0].len = n;
    s->wds[0].ptr = (uint32_t)(uintptr_t)s->frame;
    s->wds[1].len = 0;
    s->wds[1].ptr = 0;
    s->pb.csCode = MT_ENET_CS_WRITE;
    s->pb.ioRefNum = e->refnum;
    s->pb.ePointer = (uint32_t)(uintptr_t)s->wds;
}

/* A slot handed to the driver (or queued by the pump): the watchdog times it from its next scan (the first start
 * after an idle spell asks for a fresh scan), the owner's reap counts its result. Owner context only. */
static inline void mt_enet_tx_mark_started(MtEnet *e, MtEnetTx *s)
{
    s->started = 1;
    e->tx_live++;
    if (!e->tx_started++) e->wd_fresh = 1;
}

/* Open the driver, read the MAC, attach both handlers (rx frames go to `gate`). Frames can reach
 * the gate's rx callback as soon as the handlers are attached: the stack the callback feeds must
 * be initialised BEFORE this call (Task 10: mt_pb_init first, then mt_enet_open). 0 or an error
 * above / a Device Manager error. On failure everything is undone. */
int  mt_enet_open(MtEnet *e, MtGate *gate);

/* The core's link.tx (void *ctx = the MtEnet): copies the frame into a free transmit slot
 * (padded to 60 bytes) and starts an async EWrite. Returns 0 when started, 1 when the frame was
 * dropped (all slots busy: tx_drop_busy; bad length: tx_drop_len; closed: tx_drop_closed; driver
 * refused or later reported an error in ioResult: tx_err). A slot whose ioResult stays 1 is the
 * driver's (it owns the PB, WDS and buffer): it is never reused before ioResult <= 0 (the
 * watchdog below KillIOs it and finally asks for a reattach; the slot stays quarantined). Never
 * waits, never blocks, callable from any context including the protocol handler. */
int  mt_enet_write(void *ctx, const uint8_t *frame, uint16_t len);

/* ---- stuck-write watchdog (mt_enet_wd.c, plain C, host-tested: tests/test_enet_watchdog.c) ----
 * A slot whose ioResult stays 1 belongs to the driver (it owns PB, WDS and buffer) and is NEVER
 * reused until ioResult <= 0, and tx[] is never zeroed while the MtEnet lives (a re-open keeps it).
 * mt_enet_watchdog(e, now) runs inside the core gate (the driver's `after` callback; interrupt time
 * is possible) and makes NO driver call: a slot first seen in flight is timed; after
 * MT_ENET_TX_STUCK_MS it is counted (tx_stuck) and kill_req is set; after another
 * MT_ENET_TX_STUCK_MS it is quarantined and reattach_req is set. mt_enet_service(e, now) performs
 * the requests and must be called at task time only, gate owned, never from the goodbye call:
 * PBKillIO once (tx_killed; its OSErr goes to last_err; it aborts EVERY pending request on .ENET0,
 * other clients' too), then the reattach MT_ENET_TX_STUCK_MS later (the kill needs time to work; if all
 * quarantined slots completed meanwhile and the link is intact the reattach is cancelled). The reattach
 * is EDetachPH, EAttachPH only (no CloseDriver/OpenDriver) with time backoff 1 s, 2 s, 4 s, 8 s (cap)
 * between failed attempts. A failed attach leaves opened = 0 and is retried forever at the cap rate from
 * Control entries (OpenDriver(".IPP") does not re-run: the driver stays open). Before any synchronous
 * call to .ENET0 mt_os_enet_blocked() is asked: if ANY request heads the driver's queue the call could
 * wait behind a wedged one, so it is skipped (reattach_blocked) and retried every
 * MT_ENET_BLOCKED_RETRY_MS; blocked skips do not count as detach attempts, and after
 * MT_ENET_BLOCKED_TRIES of them an intact link (opened) gives up (reattach_abandoned).  Idle cost: tx_started == 0 -> one compare and return. */
#define MT_ENET_TX_STUCK_MS 2000u
#define MT_ENET_WD_SCAN_MS  100u       /* scan period while a slot is started */
#define MT_ENET_REATTACH_BACKOFF_MS 1000u
#define MT_ENET_REATTACH_BACKOFF_MAX_MS 8000u
#define MT_ENET_BLOCKED_RETRY_MS 500u
#define MT_ENET_BLOCKED_TRIES 16u   /* skipped attempts before an intact link (opened) gives up */
#define MT_ENET_E_BUSY (-2000)         /* mt_enet_open: our PB heads the driver's queue; try later */
void mt_enet_watchdog(MtEnet *e, uint32_t now_ms);
void mt_enet_service(MtEnet *e, uint32_t now_ms);
/* The Toolbox part of a reattach (mt_enet.c; faked on the host): detach both, attach both. 0 or error. */
int  mt_enet_reattach_io(MtEnet *e);
/* mt_enet_open's first step, plain C: forget the link state (refnum, mac, attached, opened) but keep
 * tx[], tx_started, the watchdog state and the counters. Zero the whole MtEnet ONCE with mt_enet_init. */
void mt_enet_init(MtEnet *e);
void mt_enet_link_reset(MtEnet *e);
/* Deadline (ms from now, MT_NO_DEADLINE allowed) to arm the timer with: while a slot is started, capped at the
 * next scan (wd_next - now, 0 when due or overdue) so the watchdog keeps getting ticks.
 * Absolute since 1.1d1: the timer is re-armed only for earlier deadlines, and a cap of "100 ms from every query"
 * would have been one. */
uint32_t mt_enet_wd_deadline(const MtEnet *e, uint32_t d, uint32_t now);
/* Choose a free slot (reaps finished ones, counts their errors); NULL = all in flight/quarantined. */
MtEnetTx *mt_enet_tx_claim(MtEnet *e);
/* Slots whose PB is still in flight after a reattach was asked (quarantined). */
int  mt_enet_tx_quarantined(const MtEnet *e);
/* Nonzero when ANY request (ours or another client's) is queued/active at .ENET0 (a synchronous
 * call would wait behind it; a healthy queue drains in milliseconds). Mac: reads the DCE (dCtlQHdr.qHead); host: faked. */
int  mt_os_enet_blocked(int16_t refnum);

/* Detach the handlers and close. Transmits in flight finish on their own; the MtEnet must stay
 * allocated until their ioResult is not 1 (mt_enet_tx_busy() == 0). */
void mt_enet_close(MtEnet *e);
int  mt_enet_tx_busy(const MtEnet *e);       /* transmit slots in flight */

#ifdef MT_TUNE
/* ---- transmit pump (PERF BUILDS ONLY: MT_TUNE lever txpump, task 17; mt_enet_pump.c, plain C, host-tested:
 * tests/test_enet_pump.c) ----
 * Why: mt_enet_write issues the async EWrite wherever the core emits a frame: inside the Ethernet driver's
 * receive delivery (handler -> gate -> core -> ACK) and from the application's call at mask 0. MacTCP issues
 * every EWrite from its own Time Manager task, after the driver's poll returned, and DaynaPORT and MoofPort
 * both handle the other contexts late (echo-latency analysis R1).
 * What: the driver's link.tx is mt_enet_tx_write (mt_drv.c). With the lever off and nothing queued it calls
 * mt_enet_write itself (the release path). With the lever on, or while frames are still
 * queued (so a switch-off never reorders), it claims and fills a slot the same way, marks it ours (ioResult 1:
 * mt_enet_tx_claim skips it), counts it started (tx_started, tx_ok: the watchdog times it from now, which also
 * catches a pump that never ran) and puts the slot on a FIFO; the first frame primes a dedicated Time Manager
 * task MT_ENET_PUMP_MS away (not MtTimer's deadline rounding to 17 ms). No free slot: 1 (busy), as today; the
 * core stalls and retries. The task (mt_enet_pump_run) issues the queued EWrites in order. It never enters the
 * core and touches only the queued slots' PBs and its own fields (the counters the gate owner keeps are not
 * written from it: a refused EWrite only stores its error in the slot's ioResult, and the owner's next reap
 * counts it in tx_err as any async error). A link that is not open fails the slot with -28 the same way.
 * Such a pumped frame is DROPPED (link.tx already returned 0, so TCP does not stall and retry it as it does for the
 * direct path's refusal; the RTO recovers it), and tx_ok counts frames when they are queued (also those dropped
 * this way): MtEnetPump.issued counts the EWrites the task issued.
 * Watchdog: enet_service (mt_drv.c) holds the pump (mt_enet_pump_hold) around mt_enet_service, so no pumped EWrite
 * lands between mt_os_enet_blocked's check and a synchronous KillIO / EDetach / EAttach.
 * Contexts: link.tx runs only in the gate owner (one producer: head); the task is the one consumer (tail). The
 * driver may deliver frames inside the task's EWrite: the handler then runs the core through the gate and its
 * frames join the FIFO behind; `running` tells such a nested link.tx that the task drains them (no prime); the
 * task re-checks after clearing it, so a frame queued by an interrupt in between is never stranded. */
#define MT_ENET_PUMP_Q  16u        /* FIFO entries: a power of two above MT_ENET_TXSLOTS (it can never fill) */
#define MT_ENET_PUMP_MS 1u         /* Time Manager delay of the pump task after the first queued frame */
typedef struct MtEnetPump {
    MtTimer  tm;                   /* the pump's own Time Manager task (Mac; unused on the host) */
    volatile uint8_t on;           /* lever txpump (Control 0x4D55); written by the gate owner only */
    volatile uint8_t head;         /* next FIFO entry link.tx fills (gate owner) */
    volatile uint8_t tail;         /* next FIFO entry the task issues (task) */
    volatile uint8_t armed;        /* primed, task not yet started (owner sets, task clears) */
    volatile uint8_t running;      /* the task is draining the FIFO */
    volatile uint8_t hold;         /* the gate owner is about to make synchronous .ENET0 calls (watchdog KillIO /
                                    * reattach, mt_drv.c enet_service): the task issues nothing (mt_enet_pump_hold) */
    uint8_t  max_q;                /* deepest FIFO seen (owner) */
    uint8_t  q[MT_ENET_PUMP_Q];    /* slot indexes into MtEnet.tx */
    uint32_t runs;                 /* task runs (task) */
    uint32_t issued;               /* EWrites the task issued (task) */
    uint32_t nested;               /* frames queued while the task was draining (owner): delivered inside its EWrite */
#ifdef MT_TEST_HOOKS
    void (*test_window)(struct MtEnetPump *p);   /* host tests: runs after the task's last look, before running = 0 */
#endif
} MtEnetPump;
/* The driver's link.tx in perf builds (see above). */
int  mt_enet_tx_write(MtEnet *e, MtEnetPump *p, const uint8_t *frame, uint16_t len);
/* The pump task's body (Time Manager time on the Mac; tests call it as the task). While `hold` is set it returns
 * at once (armed cleared, FIFO untouched). */
void mt_enet_pump_run(MtEnet *e, MtEnetPump *p);
/* Gate owner, task time only: on = 1 before synchronous .ENET0 calls (no pumped EWrite can then slip in between
 * mt_os_enet_blocked's check and a sync EDetach/EAttach and make it wait); on = 0 after them: frames still queued are
 * primed again. */
void mt_enet_pump_hold(MtEnetPump *p, int on);
/* Toolbox side (mt_enet.c; faked in tests/pbfake.c): PBControlAsync on the slot's prepared PB (its result), and
 * PrimeTime of the pump's task MT_ENET_PUMP_MS from now (legal from any context, the task itself included). */
int16_t mt_enet_io_write(MtEnet *e, MtEnetTx *s);
void    mt_enet_pump_prime(MtEnetPump *p);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MT_ENET_H */
