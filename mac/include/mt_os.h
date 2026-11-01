/* mt_os.h - every Toolbox / OS service the MacTCP glue (mac/glue/mt_pb*.c) uses.
 *
 * The glue never calls the Toolbox directly; it calls these. The Mac build (Task 10) implements
 * them in the driver (register-style completion call, Pascal ASR call, SR masking, Ticks-based
 * clock); the host tests implement them in tests/pbfake.c, which records every call in order so
 * a test can assert the ordering of completions and ASRs.
 *
 * Pointers: every pointer-valued PB field is a MacPtr32 (mt_mactcp.h). mt_os_ptr turns one
 * into a C pointer and mt_os_addr does the reverse. On the 68k both are plain casts (define
 * MT_OS_MAC to get the inline versions); on a 64-bit host pbfake maps a fake 32-bit address
 * space onto host memory. Any memory whose address the glue hands back to an application
 * (stream handles, TCPGlobalInfo tables, RDS pointers into a receive buffer) must therefore lie in
 * that address space on the host (tests allocate the MtDrv there).
 *
 * Calling contexts (obs, docs/dev/MACTCP-REFERENCE-MATRIX.md "Entry context"): completion
 * routines and ASRs run at interrupt level 1 with the application's A5; a completion routine is
 * entered with A0 = the PB and the same pointer at 4(SP) and returns with a plain RTS; a TCP ASR
 * is a Pascal routine (StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
 * unsigned short terminReason, ICMPReport *icmpMsg); a UDP ASR has the same arguments without
 * terminReason. Both may call the driver again (re-entrancy is handled by the glue). */
#ifndef MT_OS_H
#define MT_OS_H

#include <stdint.h>
#include "mt_mactcp.h"
#include "mt_dnr_abi.h"
#include "mt_stackmark.h"      /* mt_os_stack_mark: interrupt-time stack depth, no-op on the host */

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 2)
/* The header every .IPP parameter block starts with (bytes 0..27). */
typedef struct MtPbHdr {
    int8_t   fill12[12];            /* queue link, type, trap, command address: Device Manager's */
    MacPtr32 ioCompletion;
    int16_t  ioResult;
    MacPtr32 ioNamePtr;
    int16_t  ioVRefNum;
    int16_t  ioCRefNum;
    int16_t  csCode;
} MtPbHdr;
#pragma pack(pop)

/* Any .IPP parameter block. Only the member matching csCode is meaningful. */
typedef union MtAnyPb {
    MtPbHdr           hdr;
    TCPiopb           tcp;
    UDPiopb           udp;
    GetAddrParamBlock addr;
    ICMPParamBlock    icmp;
    IPParamBlock      ip;
    MtDnrPb           dnr;          /* private DNR call (MT_CS_DNR), mt_pb_dns.c */
} MtAnyPb;

/* What an ASR call needs: the notify routine, the stream handle and the userDataPtr given at
 * create time. kind selects the argument list (TCP: 5 arguments, UDP: 4, no terminReason). */
enum { MT_ASR_TCP = 0, MT_ASR_UDP = 1 };
typedef struct MtStreamAsr {
    MacPtr32  proc;                 /* 0 = no ASR installed: mt_os_asr is never called with 0 */
    StreamPtr stream;
    MacPtr32  user;
    MacPtr32  a5;                   /* the creator's A5 (mt_os_a5() at TCPCreate / UDPCreate) */
    uint8_t   kind;
} MtStreamAsr;

/* Monotonic milliseconds (wraps at 2^32); the one clock every glue entry uses. */
uint32_t mt_os_ms(void);

/* The Toolbox tick count (low-memory Ticks, 60 Hz): for values an app compares with TickCount(),
 * e.g. ICMPEcho's echoRequestOut/echoReplyIn. */
uint32_t mt_os_ticks(void);

/* The A5 world of the application making the current call (the Mac reads CurrentA5, low memory
 * 0x904). The glue records it when a stream is created and mt_os_asr runs the stream's ASR with
 * that A5 (an ASR can run from the timer or the network input path, where A5 is whatever was
 * interrupted). Completion routines get the A5 the driver recorded when the PB was issued. */
MacPtr32 mt_os_a5(void);

/* Finish a PB that mt_pb_control earlier returned inProgress for: store err in ioResult, then
 * run ioCompletion (if nonzero) with the register convention above. Called at most once per
 * pending PB. The routine may call the driver again before this returns. */
void mt_os_complete(MtAnyPb *pb, int16_t err);

/* Call a stream's ASR (a->proc != 0). icmp is the address of an ICMPReport valid only during
 * the call (0 for events without one). The ASR may call the driver again. */
void mt_os_asr(const MtStreamAsr *a, uint16_t event, uint16_t reason, MacPtr32 icmp);

/* Take / release ownership of the core against the network input path and the timer (the Mac
 * build maps these to MT_GATE_ENTER / MT_GATE_LEAVE, mac/glue/mt_gate.h: no interrupt mask is
 * touched; frames and ticks that arrive meanwhile run at the outermost restore). Nests. */
int  mt_os_irq_save(void);
void mt_os_irq_restore(int saved);

/* Nonzero while running at interrupt time (completion routines, ASRs and input run there). The
 * Mac reads the processor's interrupt mask (SR bits 8-10, nonzero = interrupt time). */
int  mt_os_in_interrupt(void);

/* The core gate's ownership depth (mac/glue/mt_gate.h; 0 = free). mt_os_irq_save / restore
 * move it by one. The glue records it when it calls application code (MtDrv.co_gate): a driver
 * call that finds the gate at exactly that depth comes from the routine itself (a call-out);
 * deeper means the routine is itself inside a core call and the caller preempted it. */
int  mt_os_gate_depth(void);

/* A real interrupt mask (level 7) around a few instructions: the driver's PB rings, filled by
 * preempting callers (mt_pb_entry.c). Never around a core call (see mt_gate.h). Nests. */
int  mt_os_mask(void);
void mt_os_unmask(int saved);

/* The memory of the application making the current call, [lo, hi), and its identity.
 *
 * System 6 without MultiFinder: lo = ApplZone, hi = BufPtr (heap, stack and A5 world of the one
 * application; the Device Manager sends the goodbye kiss at every launch and quit), h0 = 0,
 * and mt_os_app_mem returns 0.
 *
 * MultiFinder (Plan 3 Task 12, docs/dev/EMULATOR.md "MultiFinder"): each application runs in a
 * partition that is one block of MultiFinder's heap; ApplZone is the start of the block's data and
 * the 24-bit block header sits in the 8 bytes before it. mt_os_app_mem then returns 1 with
 * lo = ApplZone, hi = the end of the block, h0 = the header's first word (tag + physical size): with
 * lo the partition's identity. The second header word (the relative handle) is NOT part of it: it
 * changed for a live partition when MultiFinder's heap moved its start as the system heap grew
 * (measured, Snow: 003a0d14 -> 0039e3b0 for MfEchoA while MfEchoB launched). BufPtr is global under MultiFinder, so ApplZone..BufPtr would
 * cover the partitions above the current one. MultiFinder sends no goodbye kiss when an application
 * quits (measured), so the glue records the partitions of its callers and sweeps the ones that are
 * gone (mt_os_app_alive). Call it at task time only: at interrupt time the low-memory globals belong
 * to whichever application was interrupted. */
typedef struct MtAppMem {
    MacPtr32 lo, hi;
    uint32_t h0;
    uint32_t name;                  /* hash of CurApName (0x910): a different application of the same size
                                     * in the same place is a different owner; a relaunch of the SAME
                                     * application there is not told apart (the sweep period bounds it) */
} MtAppMem;
int  mt_os_app_mem(MtAppMem *m);

/* Nonzero while the MultiFinder partition recorded in *m (mt_os_app_mem returned 1) still exists:
 * the block header at m->lo - 8 still holds h0. MultiFinder disposes of the partition when the
 * application quits; the block becomes free (tag 0) or part of another block. A new partition of
 * exactly the same size at exactly the same place looks alive (the documented limit: the glue sweeps
 * every MT_PB_SWEEP_MS, so the block must be freed and reallocated between two sweeps). Reads two
 * words, no Toolbox call: legal at interrupt time. */
#ifdef MT_OS_MAC
static inline int mt_os_app_alive(const MtAppMem *m) {   /* one load: per frame while owners exist */
    uint32_t lo = *(volatile const MacPtr32 *)&m->lo;    /* one snapshot: a slot cleared under us reads 0 */
    if (!lo) return 0;
    return *(volatile const uint32_t *)((lo & 0x00FFFFFFu) - 8u) == m->h0;
}
#else
int  mt_os_app_alive(const MtAppMem *m);
#endif

/* KillIO on the Ethernet driver (PBKillIO with ioRefNum = refnum) for a transmit PB that stayed
 * in flight; `pb` is that PB (the Mac ignores it, the host fake records it). Returns the OSErr.
 * May be called from the Time Manager tick / protocol handler (the watchdog runs in the gate). */
int16_t mt_os_enet_kill(int16_t refnum, void *pb);

#ifdef MT_OS_MAC
static inline void *mt_os_ptr(MacPtr32 a) { return (void *)a; }
static inline MacPtr32 mt_os_addr(const void *p) { return (MacPtr32)p; }
#else
void    *mt_os_ptr(MacPtr32 a);          /* NULL for 0 */
MacPtr32 mt_os_addr(const void *p);      /* 0 for NULL */
#endif

#ifdef __cplusplus
}
#endif

#endif /* MT_OS_H */
