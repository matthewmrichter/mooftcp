/* mt_dnr_abi.h - the private Control call between the resolver code resource `dnrp` (mac/dnr,
 * running in the application's heap) and the .IPP driver (mac/glue/mt_pb_dns.c).
 *
 * Plain C99 and fixed-width integers only: dnrp includes it next to the Toolbox headers, the glue
 * next to mt_mactcp.h. The PB is a normal Control PB (28-byte header: ioCompletion at 12, ioResult
 * at 16, ioCRefNum at 24, csCode at 26) with the private csCode MT_CS_DNR; the Device Manager
 * routes it to the driver like any other PBControl.
 *
 *   MT_DNR_A / MT_DNR_HINFO / MT_DNR_MX   name = C string (copied at once), out = the caller's
 *                                         hostInfo (A) or returnRec (HINFO, MX)
 *   MT_DNR_PTR                            arg = IP address, out = hostInfo
 *   MT_DNR_ENUM                           arg = entry index, out = MtDnrEnum; slot = 1 if entry
 *   MT_DNR_CANCEL                         name = the pending PB's address, arg = its gen (a PB still queued in
 *                                         the driver entry is refused later, without a completion call)
 *
 * A lookup is made with PBControlAsync and ioCompletion = dnrp's routine:
 *   - it finishes inside the call (cache hit, dotted quad, immediate error): ioResult is final
 *     (0 or a DNR error) and `deferred` stays 0; the completion routine runs too and must ignore
 *     the PB (deferred == 0);
 *   - it needs the network: the driver sets deferred = 1 and slot, returns inProgress, and
 *     completes the PB (ioResult 0) from the network input path once the answer is in; before that
 *     it has written the result into `out` (rtnCode, cname, addresses). The result memory of the
 *     query itself lives in the driver (resident), never in the application's heap, so a quit
 *     with a lookup pending cannot be written after the heap is gone (goodbye cancels it).
 *   - MT_DNR_CANCEL on a pending lookup completes that PB with abortErr (-27).
 * `out` is written only while the PB is pending or during the call. rtnCode is set to cacheFault
 * (-23042) as soon as the lookup is accepted, as the real DNR does. */
#ifndef MT_DNR_ABI_H
#define MT_DNR_ABI_H

#include <stdint.h>

#define MT_CS_DNR        0x4D44        /* 'MD': private csCode (MoofTCP DNR) */

enum { MT_DNR_A = 1, MT_DNR_PTR = 2, MT_DNR_HINFO = 3, MT_DNR_MX = 4,
       MT_DNR_ENUM = 5, MT_DNR_CANCEL = 6 };

#pragma pack(push, 2)
typedef struct MtDnrPb {
    int8_t   fill12[12];
    uint32_t ioCompletion;      /* 12 */
    int16_t  ioResult;          /* 16 */
    uint32_t ioNamePtr;         /* 18 */
    int16_t  ioVRefNum;         /* 22 */
    int16_t  ioCRefNum;         /* 24 */
    int16_t  csCode;            /* 26: MT_CS_DNR */
    int16_t  op;                /* 28: MT_DNR_* */
    int16_t  slot;              /* 30: out for a deferred lookup; in for MT_DNR_CANCEL; out 0/1 for ENUM */
    uint32_t name;              /* 32: C string (A, HINFO, MX) */
    uint32_t arg;               /* 36: address (PTR) / entry index (ENUM) */
    uint32_t out;               /* 40: hostInfo / returnRec / MtDnrEnum */
    int16_t  hasproc;           /* 44: the caller gave a result proc */
    int16_t  deferred;          /* 46: out: the lookup is pending, the PB completes later */
    int16_t  gen;               /* 48: lookup generation (dnrp counts them); MT_DNR_CANCEL passes it in arg */
} MtDnrPb;                      /* 50 bytes */

/* One EnumCache entry: the name server record (index 0 when a server is set; type 17, ttl 0xFFFFFFFF,
 * empty name, rdata = server address, as MacTCP's) and then the cached A answers, one per address. */
typedef struct MtDnrEnum {
    char     name[66];          /* with the trailing dot, NUL terminated */
    uint16_t type, cclass;
    uint32_t rdata;             /* IP address */
    uint32_t ttl;               /* seconds left; 0xFFFFFFFF = permanent */
} MtDnrEnum;
#pragma pack(pop)

#endif /* MT_DNR_ABI_H */
