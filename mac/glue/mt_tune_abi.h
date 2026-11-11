/* mt_tune_abi.h - the private Control call that reads and sets MoofTCP's run-time timing levers.
 * PERF BUILDS ONLY (compile option MT_TUNE, scripts/build-mac.sh with MT_EXTRA_CFLAGS=-DMT_TUNE=1): a release
 * driver does not implement it and answers Control csCode 0x4D55 like any code it does not know (-4).
 *
 * PBControl on .IPP, csCode MT_CS_TUNE ('MU'), csParam (PB offset 28) = pointer to a 16-byte block of 16-bit
 * words in the CPU's own order (big-endian on the Mac; the host tests use native order):
 *     0  version   (MT_TUNE_ABI_VERSION, 1)
 *     2  op        MT_TUNE_OP_GET: the call fills the fields below with the current values
 *                  MT_TUNE_OP_SET: the call applies the fields (mt_tune_set bounds); nothing changes
 *                                  on an error. Applies to timers armed from now until the next restart.
 *     4  rto_min   ms (100..5000)         6  rto_init  ms (rto_min..10000)
 *     8  delack    ms (0..500, 0 = ACK at once)        10 stall ms (1..100, link-busy retry)
 *    12  quickack  0..255 (the first n data segments of a connection are ACKed at once; 255 = all)
 *    14  reserved (0)
 * Result 0, or -50 (paramErr) for a bad version, op or out-of-range value. App time (not interrupt time). */
#ifndef MT_TUNE_ABI_H
#define MT_TUNE_ABI_H

#include <stdint.h>

#define MT_CS_TUNE          0x4D55
#define MT_TUNE_ABI_VERSION 1
#define MT_TUNE_OP_GET      0
#define MT_TUNE_OP_SET      1
#define MT_TUNE_BLOCK_BYTES 16
#define MT_TUNE_PARAMERR    (-50)
/* word offsets (in uint16_t units) */
enum { MT_TW_VERSION, MT_TW_OP, MT_TW_RTO_MIN, MT_TW_RTO_INIT, MT_TW_DELACK, MT_TW_STALL, MT_TW_QUICKACK, MT_TW_RSVD, MT_TW_N };

#endif
