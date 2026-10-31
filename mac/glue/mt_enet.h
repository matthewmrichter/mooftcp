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
    int16_t   last_err;          /* last nonzero Device Manager result */
    uint32_t  tx_ok, tx_pad, tx_drop_busy, tx_drop_len, tx_drop_closed, tx_err;
    uint32_t  tx_stuck, tx_killed, reattach, reattach_fail;   /* watchdog (Plan 3 Task 5) */
    uint32_t  reattach_blocked, reattach_abandoned;   /* sync call skipped (our PB at the head) / gave up */
    uint32_t  reattach_at;       /* ms: earliest next attempt */
    uint32_t  wd_next;           /* ms: next scan */
    uint8_t   tx_started;        /* slots with started != 0 (0: the watchdog returns at once) */
    uint8_t   wd_fresh;          /* a write started: scan at the next call */
    uint8_t   kill_req;          /* the watchdog asked for a KillIO (performed at task time) */
    uint8_t   reattach_req;      /* the watchdog asked; mt_enet_service performs it at task time */
    uint8_t   reattach_tries;    /* failed attempts of the current request */
    MtEnetTx  tx[MT_ENET_TXSLOTS];
} MtEnet;

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
 * PBKillIO once (tx_killed; its OSErr goes to last_err), then the reattach (EDetachPH, EAttachPH
 * only: no CloseDriver/OpenDriver) with time backoff MT_ENET_REATTACH_BACKOFF_MS * 2^n for at most
 * MT_ENET_REATTACH_TRIES attempts, then it is abandoned (reattach_abandoned). Before any synchronous
 * call to .ENET0 mt_os_enet_blocked() is asked: if one of our transmit PBs is the head of the
 * driver's queue the call would wait behind it forever, so it is skipped (reattach_blocked) and
 * counted as a failed attempt. Idle cost: tx_started == 0 -> one compare and return. */
#define MT_ENET_TX_STUCK_MS 2000u
#define MT_ENET_WD_SCAN_MS  100u       /* scan period while a slot is started */
#define MT_ENET_REATTACH_BACKOFF_MS 1000u
#define MT_ENET_REATTACH_TRIES 4u
#define MT_ENET_E_BUSY (-2000)         /* mt_enet_open: our PB heads the driver's queue; try later */
void mt_enet_watchdog(MtEnet *e, uint32_t now_ms);
void mt_enet_service(MtEnet *e, uint32_t now_ms);
/* The Toolbox part of a reattach (mt_enet.c; faked on the host): detach both, attach both. 0 or error. */
int  mt_enet_reattach_io(MtEnet *e);
/* mt_enet_open's first step, plain C: forget the link state (refnum, mac, attached, opened) but keep
 * tx[], tx_started, the watchdog state and the counters. Zero the whole MtEnet ONCE with mt_enet_init. */
void mt_enet_init(MtEnet *e);
void mt_enet_link_reset(MtEnet *e);
/* Deadline (ms, MT_NO_DEADLINE allowed) to arm the timer with: capped to the scan period while a
 * slot is started so the watchdog keeps getting ticks. */
uint32_t mt_enet_wd_deadline(const MtEnet *e, uint32_t d);
/* Choose a free slot (reaps finished ones, counts their errors); NULL = all in flight/quarantined. */
MtEnetTx *mt_enet_tx_claim(MtEnet *e);
/* Slots whose PB is still in flight after a reattach was asked (quarantined). */
int  mt_enet_tx_quarantined(const MtEnet *e);
/* Nonzero when one of our transmit PBs is the active/head request of .ENET0 (a synchronous call
 * would hang behind it). Mac: reads the DCE (dCtlQHdr.qHead); host: faked. */
int  mt_os_enet_blocked(int16_t refnum, const void *lo, const void *hi);

/* Detach the handlers and close. Transmits in flight finish on their own; the MtEnet must stay
 * allocated until their ioResult is not 1 (mt_enet_tx_busy() == 0). */
void mt_enet_close(MtEnet *e);
int  mt_enet_tx_busy(const MtEnet *e);       /* transmit slots in flight */

#ifdef __cplusplus
}
#endif

#endif /* MT_ENET_H */
