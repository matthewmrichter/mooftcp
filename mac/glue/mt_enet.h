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
    uint8_t   pad;
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
 * driver's (it owns the PB, WDS and buffer): it is never reclaimed here, only counted busy; a
 * watchdog (KillIO, or detach/close/reopen) is Plan 3's. Never
 * waits, never blocks, callable from any context including the protocol handler. */
int  mt_enet_write(void *ctx, const uint8_t *frame, uint16_t len);

/* Detach the handlers and close. Transmits in flight finish on their own; the MtEnet must stay
 * allocated until their ioResult is not 1 (mt_enet_tx_busy() == 0). */
void mt_enet_close(MtEnet *e);
int  mt_enet_tx_busy(const MtEnet *e);       /* transmit slots in flight */

#ifdef __cplusplus
}
#endif

#endif /* MT_ENET_H */
