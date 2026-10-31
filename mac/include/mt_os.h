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

/* A real interrupt mask (level 7) around a few instructions: the driver's PB rings, filled by
 * preempting callers (mt_pb_entry.c). Never around a core call (see mt_gate.h). Nests. */
int  mt_os_mask(void);
void mt_os_unmask(int saved);

/* Nonzero if the address lies in the current application's memory (ApplZone .. BufPtr): the
 * goodbye kiss forgets only what belongs to the departing application (streams by receive
 * buffer, echoes by notify routine, queued PBs by address). */
int  mt_os_app_mem(MacPtr32 p);

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
