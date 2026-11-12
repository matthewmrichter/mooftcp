/* mt_trace.h - the echo-path trace (PERF BUILDS ONLY: compile option MT_TRACE, allowed only together with MT_TUNE;
 * task 18). Records, with a sub-millisecond clock, where a request/reply round trip spends its time inside MoofTCP:
 * frames from the protocol handler, payload delivered to a connection, every .IPP Control call in and out, PB
 * completions, every EWrite and its completion, timer ticks and gate deferrals (events and fields: mt_trace_abi.h).
 *
 * Release and plain MT_TUNE builds compile every hook away (they are `#ifdef MT_TRACE` blocks; mt_trace.c and
 * mt_trace_mac.c compile to nothing), so their objects are unchanged.
 *
 * The ring, MtTrace, lives at the end of the resident block (MtRes.trace, locked in the system heap); the driver
 * registers it at boot (mt_trace_set). mt_trace_ev appends one record under a short interrupt mask (mt_os_mask:
 * the clock read and the store are one step, so any context may record, the protocol handler, the Time Manager
 * task, a completion routine and the application alike); no Memory Manager call, no core call. The newest
 * MT_TRACE_RECS records are kept. While frozen (Control MT_TRACE_OP_FREEZE) nothing is recorded and no clock is read.
 *
 * Clock: mt_os_trace_us (mt_os.h): monotonic microseconds, wrapping at 2^32. The Mac (mt_trace_mac.c) uses the
 * revised Time Manager as a stopwatch (RmvTime reads the time left of a task primed for -1 s, InsTime + PrimeTime
 * start it again; the task adds the second itself when no read comes for that long). System 6.0.8 on the Classic has
 * no Microseconds trap (docs/dev/MACTCP-INTERFACE.md: A193 unimplemented), and the VIA timers are not safe to read
 * (the low byte of a counter clears its interrupt flag). The host fake (tests/pbfake.c) is a settable counter. */
#ifndef MT_TRACE_H
#define MT_TRACE_H

#ifdef MT_TRACE
#ifndef MT_TUNE
#error "MT_TRACE is a perf-build option: it needs MT_TUNE"
#endif

#include <stdint.h>
#include "mt_trace_abi.h"
#include "mt_timer.h"
#include "mt_gate.h"
#include "mt_enet.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MT_TRACE_RECS
#define MT_TRACE_RECS 768u           /* 12 KB: about 58 rounds of a 1-byte echo (13 records each); 1024 left only
                                      * 270 B under mem-check's rule-1 cap (Snow, pump on), 768 leaves about 4.4 KB */
#endif

typedef struct MtTrace {
    MtTimer           clk;           /* Mac: the stopwatch's Time Manager task (unused on the host) */
    volatile uint32_t acc;           /* Mac: the stopwatch's microseconds up to its last start */
    uint32_t          lap_us;        /* cost of one clock read (boot) */
    uint16_t          flags;         /* MT_TRACE_F_TMCLK when the Mac clock runs */
    volatile uint8_t  frozen;
    uint8_t           pad;
    volatile uint32_t total;         /* records written since the last clear */
    volatile uint16_t next;          /* the slot the next record goes to */
    uint16_t          pad2;
    MtTraceRec        rec[MT_TRACE_RECS];
} MtTrace;

/* Register the ring (NULL: none; every hook then returns at once). The caller zeroed it. */
void mt_trace_set(MtTrace *t);
MtTrace *mt_trace_get(void);

/* Append one record (any context). */
void mt_trace_ev(uint8_t ev, uint8_t a, uint16_t b, uint32_t c, uint32_t d);
/* Frame fields (mt_trace_abi.h "Frame events") of an Ethernet frame of len bytes. */
void mt_trace_frame_fields(const uint8_t *f, uint16_t len, uint8_t *a, uint16_t *b, uint32_t *c, uint32_t *d);
/* Append a frame event; mark adds MT_TRACE_A_MARK. */
void mt_trace_frame(uint8_t ev, int mark, const uint8_t *f, uint16_t len);

/* Gate (mt_gate.c): the protocol handler committed a frame (the one before head) and pokes; deferred = the core is
 * owned. Records MT_TRACE_RX and remembers in the slot whether it waits (MtRxSlot.pad), for MT_TRACE_RXRUN. */
void mt_trace_gate_rx(MtGate *g, int deferred);
/* The gate owner takes slot s from the ring: MT_TRACE_RXRUN if it had waited. */
void mt_trace_gate_run(MtRxSlot *s);

/* Ethernet (mt_enet.c, mt_enet_pump.c): an EWrite of the slot's frame (n bytes) is about to be issued. Sets the PB's
 * completion routine to the trace's (mt_os_trace_ew_compl) and records MT_TRACE_EWRITE (pumped: the pump's task). */
void mt_trace_ewrite(MtEnetTx *s, uint16_t n, int pumped);
/* The trace's EWrite completion routine (the Mac's stub calls it with the PB, ioResult already set). */
void mt_trace_ew_done(MtEnetPB *pb);

/* Control MT_CS_TRACE (mt_pb.c): blk is the caller's MtTraceBlk; buffers through mt_os_ptr. 0 or MT_TRACE_PARAMERR. */
int16_t mt_trace_control(MtTraceBlk *blk);

/* Mac only (mt_trace_mac.c): start the stopwatch and measure lap_us; the driver calls it at boot. */
void mt_trace_mac_start(MtTrace *t);

#ifdef __cplusplus
}
#endif

#endif /* MT_TRACE */
#endif /* MT_TRACE_H */
