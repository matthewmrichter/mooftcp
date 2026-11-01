/* Trimmed from SitStill src/core/classic_sit_internal.h (MIT, see LICENSE): only what
 * classic_sit_writer.c needs. The decoder declarations were removed. */
#ifndef SITSTILL_CLASSIC_SIT_INTERNAL_H
#define SITSTILL_CLASSIC_SIT_INTERNAL_H

#include "sitstill/classic_sit.h"

#define kSitArchiveHeaderSize 22UL
#define kSitEntryHeaderSize 112UL
#define kSitV2MaxCatalogEntries 4096UL

unsigned short SitStillClassicSITCRC16Update(unsigned short crc,
                                              const unsigned char *bytes,
                                              unsigned long length);

#endif
