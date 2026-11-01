/* CRC-16 (reflected, poly 0xA001) used by Classic SIT. The bodies of
 * SitStillClassicSITCRC16Update and SitStillClassicSITCRC16 are copied from SitStill
 * src/core/classic_sit_codecs.c (MIT, see LICENSE); the rest of that file (the decoders)
 * is not needed to write archives. */
#include "sitstill/classic_sit.h"
#include "classic_sit_internal.h"

unsigned short SitStillClassicSITCRC16Update(unsigned short crc,
                                              const unsigned char *bytes,
                                              unsigned long length)
{
    unsigned long index;

    for (index = 0UL; index < length; ++index) {
        unsigned short bit;

        crc = (unsigned short)(crc ^ (unsigned short)bytes[index]);
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U)
                crc = (unsigned short)((crc >> 1) ^ 0xa001U);
            else
                crc = (unsigned short)(crc >> 1);
        }
    }
    return crc;
}

unsigned short SitStillClassicSITCRC16(const unsigned char *bytes,
                                       unsigned long length)
{
    return SitStillClassicSITCRC16Update(0U, bytes, length);
}
