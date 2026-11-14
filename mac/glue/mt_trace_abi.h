/* mt_trace_abi.h - the private Control call that reads MoofTCP's echo-path trace, and the record format.
 * PERF BUILDS ONLY (compile options MT_TUNE and MT_TRACE together: `MT_TUNE=1 MT_TRACE=1 scripts/build-mac.sh`, tree
 * build-mac-tune-trace/; task 18). A release or a plain MT_TUNE driver does not implement it and answers Control
 * csCode 0x4D56 like any code it does not know (-4).
 *
 * PBControl on .IPP, csCode MT_CS_TRACE ('MV'), csParam (PB offset 28) = pointer to a 32-byte MtTraceBlk in the CPU's
 * own byte order (big-endian on the Mac; the host tests use native order). Ops:
 *   MT_TRACE_OP_INFO    fills the out fields; nothing changes.
 *   MT_TRACE_OP_FREEZE  stops recording (the records stay) and fills the out fields. Read only a frozen ring.
 *   MT_TRACE_OP_READ    the ring must be frozen: copies up to `max` records, starting at the `first`-th kept record
 *                       (0 = the oldest kept), to `buf`; `got` = records copied (0 past the end).
 *   MT_TRACE_OP_CLEAR   forgets every record and resumes recording (also after a freeze).
 * Result 0, or -50 (paramErr) for a bad version or op, a READ of a ring that is not frozen, or a NULL buf. App time
 * (not interrupt time). The trace call itself, and MT_CS_TUNE, are never recorded.
 *
 * The ring (mt_trace.h) keeps the newest MT_TRACE_RECS records: older ones are overwritten (`total` keeps counting).
 * A record is 16 bytes, t first. t is the trace clock in microseconds (mt_os_trace_us: on the Mac the revised Time
 * Manager used as a stopwatch, mt_trace_mac.c; it wraps at 2^32 us, about 71.6 minutes: take differences modulo 2^32).
 * Every record costs one clock read (`lap_us`, measured at boot; about 0.2 ms on a Mac Classic), which lies inside the
 * interval between two consecutive records.
 *
 * Events (ev) and their fields:
 *   Frame events: a = TCP flags (low 6 bits: FIN 01 SYN 02 RST 04 PSH 08 ACK 10 URG 20) | MT_TRACE_A_MARK (meaning per
 *   event) | MT_TRACE_A_NOTTCP; b = TCP payload bytes; c = TCP sequence number; d = source port << 16 | dest port.
 *   Not TCP: b = frame bytes, c = ethertype << 16 | IP protocol (0 for ARP), d = UDP ports (0 otherwise).
 *     MT_TRACE_RX      a frame reached the gate from the protocol handler, before the core. MARK: the core was owned,
 *                      the frame waits in the ring (gate deferral).
 *     MT_TRACE_RXRUN   a deferred frame is run by the gate owner (the core takes it now).
 *     MT_TRACE_EWRITE  an EWrite is issued (PBControlAsync next). MARK: by the transmit pump's task (txpump).
 *     MT_TRACE_EWDONE  that EWrite's completion routine ran (the Ethernet driver's IODone). MARK: ioResult != 0.
 *   MT_TRACE_DATA   TCP payload delivered to a connection inside mt_input (the core's DATA event): b = stream index,
 *                   c = sequence number of the first new byte, d = bytes.
 *   MT_TRACE_PBIN   a .IPP Control call enters the dispatcher: a = MT_TRACE_A_ASYNC | MT_TRACE_A_IRQ, b = csCode,
 *                   c = PB address, d = 0.
 *   MT_TRACE_PBOUT  ... and returns to the Device Manager: a, b, c as PBIN; d = result << 16 | (uint16) ioResult
 *                   (ioResult 1 = still in progress).
 *   MT_TRACE_PBDONE a PB is completed (ioResult set; the completion routine, if any, is called next): a = 1 if it has
 *                   a completion routine, b = csCode, c = PB address, d = the result (sign-extended).
 *   MT_TRACE_TICK   the Time Manager tick reaches the glue (mt_pb_tick, which runs mt_tick when due): c = ms clock.
 *   MT_TRACE_TDEFER the timer fired while the core was owned: the tick waits for the owner (gate deferral). */
#ifndef MT_TRACE_ABI_H
#define MT_TRACE_ABI_H

#include <stdint.h>

#define MT_CS_TRACE          0x4D56
#define MT_TRACE_ABI_VERSION 1
#define MT_TRACE_OP_INFO     0
#define MT_TRACE_OP_FREEZE   1
#define MT_TRACE_OP_READ     2
#define MT_TRACE_OP_CLEAR    3
#define MT_TRACE_PARAMERR    (-50)

enum { MT_TRACE_RX = 1, MT_TRACE_RXRUN, MT_TRACE_DATA, MT_TRACE_PBIN, MT_TRACE_PBOUT, MT_TRACE_PBDONE,
       MT_TRACE_EWRITE, MT_TRACE_EWDONE, MT_TRACE_TICK, MT_TRACE_TDEFER, MT_TRACE_NEV };

#define MT_TRACE_A_FLAGS  0x3Fu
#define MT_TRACE_A_MARK   0x40u
#define MT_TRACE_A_NOTTCP 0x80u
#define MT_TRACE_A_ASYNC  0x01u
#define MT_TRACE_A_IRQ    0x02u

#pragma pack(push, 2)
typedef struct MtTraceRec {
    uint32_t t;                 /* us, trace clock */
    uint8_t  ev, a;
    uint16_t b;
    uint32_t c, d;
} MtTraceRec;                   /* 16 bytes */

typedef struct MtTraceBlk {
    uint16_t version;           /* in: MT_TRACE_ABI_VERSION */
    uint16_t op;                /* in: MT_TRACE_OP_* */
    uint16_t first;             /* in (READ): index of the first record to copy, 0 = oldest kept */
    uint16_t max;               /* in (READ): records `buf` holds */
    uint32_t buf;               /* in (READ): address of max * 16 bytes */
    uint16_t got;               /* out: READ: records copied; else: records kept */
    uint16_t cap;               /* out: MT_TRACE_RECS */
    uint32_t total;             /* out: records written since the last clear (total - kept were overwritten) */
    uint32_t lap_us;            /* out: cost of one clock read, measured at boot */
    uint32_t now_us;            /* out: the trace clock now */
    uint16_t rec_bytes;         /* out: sizeof(MtTraceRec) */
    uint16_t flags;             /* out: MT_TRACE_F_* */
} MtTraceBlk;                   /* 32 bytes */
#pragma pack(pop)

#define MT_TRACE_F_FROZEN 0x0001u
#define MT_TRACE_F_TMCLK  0x0002u   /* the clock is the Time Manager stopwatch (the Mac) */

#endif
