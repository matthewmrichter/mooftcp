#include "sitstill/stream.h"

static SitStillStreamResult MemoryRead(void *refCon, unsigned char *bytes,
                                       unsigned long request,
                                       unsigned long *actual)
{
    SitStillMemoryStream *stream;
    unsigned long available;
    unsigned long index;

    if (refCon == 0 || bytes == 0 || actual == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillMemoryStream *)refCon;
    available = stream->length - stream->position;
    if (request > available)
        request = available;
    for (index = 0UL; index < request; ++index)
        bytes[index] = stream->bytes[stream->position + index];
    stream->position += request;
    *actual = request;
    return request == 0UL ? kSitStillStreamEnd : kSitStillStreamOK;
}

static SitStillStreamResult MemorySeek(void *refCon, unsigned long offset)
{
    SitStillMemoryStream *stream;

    if (refCon == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillMemoryStream *)refCon;
    if (offset > stream->length)
        return kSitStillStreamOutOfRange;
    stream->position = offset;
    return kSitStillStreamOK;
}

static SitStillStreamResult MemoryTell(void *refCon, unsigned long *offset)
{
    SitStillMemoryStream *stream;

    if (refCon == 0 || offset == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillMemoryStream *)refCon;
    *offset = stream->position;
    return kSitStillStreamOK;
}

static SitStillStreamResult MemoryLength(void *refCon, unsigned long *length)
{
    SitStillMemoryStream *stream;

    if (refCon == 0 || length == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillMemoryStream *)refCon;
    *length = stream->length;
    return kSitStillStreamOK;
}

void SitStillMemoryStreamInit(SitStillMemoryStream *storage,
                              SitStillInputStream *stream,
                              const unsigned char *bytes,
                              unsigned long length)
{
    if (storage == 0 || stream == 0)
        return;
    storage->bytes = bytes;
    storage->length = length;
    storage->position = 0UL;
    stream->read = MemoryRead;
    stream->seek = MemorySeek;
    stream->tell = MemoryTell;
    stream->length = MemoryLength;
    stream->refCon = storage;
}

static SitStillStreamResult MemoryOutputWrite(void *refCon,
                                              const unsigned char *bytes,
                                              unsigned long request,
                                              unsigned long *actual)
{
    SitStillMemoryOutputStream *stream;
    unsigned long index;

    if (refCon == 0 || actual == 0 || (bytes == 0 && request != 0UL))
        return kSitStillStreamBadArgument;
    stream = (SitStillMemoryOutputStream *)refCon;
    if (request > stream->capacity - stream->position) {
        *actual = 0UL;
        return kSitStillStreamOutOfRange;
    }
    for (index = 0UL; index < request; ++index)
        stream->bytes[stream->position + index] = bytes[index];
    stream->position += request;
    *actual = request;
    return kSitStillStreamOK;
}

void SitStillMemoryOutputStreamInit(SitStillMemoryOutputStream *storage,
                                    SitStillOutputStream *stream,
                                    unsigned char *bytes,
                                    unsigned long capacity)
{
    if (storage == 0 || stream == 0 || (bytes == 0 && capacity != 0UL))
        return;
    storage->bytes = bytes;
    storage->capacity = capacity;
    storage->position = 0UL;
    stream->write = MemoryOutputWrite;
    stream->refCon = storage;
}

static SitStillStreamResult DiscardOutputWrite(void *refCon,
                                               const unsigned char *bytes,
                                               unsigned long request,
                                               unsigned long *actual)
{
    SitStillDiscardOutputStream *stream;

    if (refCon == 0 || actual == 0 || (bytes == 0 && request != 0UL))
        return kSitStillStreamBadArgument;
    stream = (SitStillDiscardOutputStream *)refCon;
    if (request > 0xffffffffUL - stream->length)
        return kSitStillStreamOutOfRange;
    stream->length += request;
    *actual = request;
    return kSitStillStreamOK;
}

void SitStillDiscardOutputStreamInit(SitStillDiscardOutputStream *storage,
                                     SitStillOutputStream *stream)
{
    if (storage == 0 || stream == 0)
        return;
    storage->length = 0UL;
    stream->write = DiscardOutputWrite;
    stream->refCon = storage;
}

static SitStillStreamResult BoundedOutputWrite(
    void *refCon, const unsigned char *bytes, unsigned long request,
    unsigned long *actual)
{
    SitStillBoundedOutputStream *bounded;
    SitStillStreamResult result;
    unsigned long written;

    if (refCon == 0 || actual == 0 || (bytes == 0 && request != 0UL))
        return kSitStillStreamBadArgument;
    bounded = (SitStillBoundedOutputStream *)refCon;
    *actual = 0UL;
    if (request > (bounded->sharedRemaining != 0 ?
                   *bounded->sharedRemaining :
                   bounded->limit - bounded->written)) {
        bounded->limitExceeded = 1U;
        return kSitStillStreamOutOfRange;
    }
    written = 0UL;
    result = bounded->parent->write(bounded->parent->refCon, bytes, request,
                                    &written);
    if (written > request)
        return kSitStillStreamOutOfRange;
    bounded->written += written;
    if (bounded->sharedRemaining != 0)
        *bounded->sharedRemaining -= written;
    *actual = written;
    return result;
}

SitStillStreamResult SitStillBoundedOutputStreamInit(
    SitStillBoundedOutputStream *storage, SitStillOutputStream *stream,
    SitStillOutputStream *parent, unsigned long limit)
{
    if (storage == 0 || stream == 0 || parent == 0 || parent->write == 0)
        return kSitStillStreamBadArgument;
    storage->parent = parent;
    storage->sharedRemaining = 0;
    storage->limit = limit;
    storage->written = 0UL;
    storage->limitExceeded = 0U;
    stream->write = BoundedOutputWrite;
    stream->refCon = storage;
    return kSitStillStreamOK;
}

static SitStillStreamResult LimitedRead(void *refCon, unsigned char *bytes,
                                        unsigned long request,
                                        unsigned long *actual)
{
    SitStillLimitedStream *stream;
    unsigned long available;
    SitStillStreamResult result;

    if (refCon == 0 || bytes == 0 || actual == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillLimitedStream *)refCon;
    available = stream->length - stream->position;
    if (request > available)
        request = available;
    if (request == 0UL) {
        *actual = 0UL;
        return kSitStillStreamEnd;
    }
    result = stream->parent->seek(stream->parent->refCon,
                                  stream->start + stream->position);
    if (result != kSitStillStreamOK)
        return result;
    result = stream->parent->read(stream->parent->refCon, bytes, request, actual);
    stream->position += *actual;
    return result;
}

static SitStillStreamResult LimitedSeek(void *refCon, unsigned long offset)
{
    SitStillLimitedStream *stream;

    if (refCon == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillLimitedStream *)refCon;
    if (offset > stream->length)
        return kSitStillStreamOutOfRange;
    stream->position = offset;
    return kSitStillStreamOK;
}

static SitStillStreamResult LimitedTell(void *refCon, unsigned long *offset)
{
    SitStillLimitedStream *stream;

    if (refCon == 0 || offset == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillLimitedStream *)refCon;
    *offset = stream->position;
    return kSitStillStreamOK;
}

static SitStillStreamResult LimitedLength(void *refCon, unsigned long *length)
{
    SitStillLimitedStream *stream;

    if (refCon == 0 || length == 0)
        return kSitStillStreamBadArgument;
    stream = (SitStillLimitedStream *)refCon;
    *length = stream->length;
    return kSitStillStreamOK;
}

SitStillStreamResult SitStillLimitedStreamInit(
    SitStillLimitedStream *storage, SitStillInputStream *stream,
    SitStillInputStream *parent, unsigned long start, unsigned long length)
{
    unsigned long parentLength;
    SitStillStreamResult result;

    if (storage == 0 || stream == 0 || parent == 0 || parent->read == 0 ||
        parent->seek == 0 || parent->length == 0)
        return kSitStillStreamBadArgument;
    result = parent->length(parent->refCon, &parentLength);
    if (result != kSitStillStreamOK)
        return result;
    if (start > parentLength || length > parentLength - start)
        return kSitStillStreamOutOfRange;
    storage->parent = parent;
    storage->start = start;
    storage->length = length;
    storage->position = 0UL;
    stream->read = LimitedRead;
    stream->seek = LimitedSeek;
    stream->tell = LimitedTell;
    stream->length = LimitedLength;
    stream->refCon = storage;
    return kSitStillStreamOK;
}

SitStillStreamResult SitStillStreamReadExact(SitStillInputStream *stream,
                                              unsigned char *bytes,
                                              unsigned long request)
{
    unsigned long total;

    if (stream == 0 || stream->read == 0 || (bytes == 0 && request != 0UL))
        return kSitStillStreamBadArgument;
    total = 0UL;
    while (total < request) {
        unsigned long actual;
        SitStillStreamResult result;

        actual = 0UL;
        result = stream->read(stream->refCon, bytes + total, request - total,
                              &actual);
        if (actual > request - total)
            return kSitStillStreamOutOfRange;
        total += actual;
        if (result == kSitStillStreamCancelled)
            return result;
        if (result != kSitStillStreamOK && result != kSitStillStreamEnd)
            return result;
        if (total == request)
            return kSitStillStreamOK;
        if (result == kSitStillStreamEnd)
            return result;
        if (actual == 0UL)
            return kSitStillStreamEnd;
    }
    return kSitStillStreamOK;
}

SitStillStreamResult SitStillStreamWriteExact(SitStillOutputStream *stream,
                                               const unsigned char *bytes,
                                               unsigned long request)
{
    unsigned long total;

    if (stream == 0 || stream->write == 0 || (bytes == 0 && request != 0UL))
        return kSitStillStreamBadArgument;
    total = 0UL;
    while (total < request) {
        unsigned long actual;
        SitStillStreamResult result;

        actual = 0UL;
        result = stream->write(stream->refCon, bytes + total, request - total,
                               &actual);
        if (actual > request - total)
            return kSitStillStreamOutOfRange;
        total += actual;
        if (result != kSitStillStreamOK && result != kSitStillStreamEnd)
            return result;
        if (total == request)
            return kSitStillStreamOK;
        if (result != kSitStillStreamOK || actual == 0UL)
            return result == kSitStillStreamOK ? kSitStillStreamEnd : result;
    }
    return kSitStillStreamOK;
}
