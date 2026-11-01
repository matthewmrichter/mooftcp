#ifndef SITSTILL_STREAM_H
#define SITSTILL_STREAM_H

typedef enum SitStillStreamResult {
    kSitStillStreamOK = 0,
    kSitStillStreamEnd = 1,
    kSitStillStreamBadArgument = -1,
    kSitStillStreamNotSeekable = -2,
    kSitStillStreamOutOfRange = -3,
    kSitStillStreamCancelled = -4,
    /* Underlying device/File Manager I/O failed (distinct from a bounds error). */
    kSitStillStreamIOError = -5
} SitStillStreamResult;

typedef SitStillStreamResult (*SitStillStreamReadProc)(
    void *refCon, unsigned char *bytes, unsigned long request,
    unsigned long *actual);
typedef SitStillStreamResult (*SitStillStreamSeekProc)(
    void *refCon, unsigned long offset);
typedef SitStillStreamResult (*SitStillStreamTellProc)(
    void *refCon, unsigned long *offset);
typedef SitStillStreamResult (*SitStillStreamLengthProc)(
    void *refCon, unsigned long *length);
typedef SitStillStreamResult (*SitStillStreamWriteProc)(
    void *refCon, const unsigned char *bytes, unsigned long request,
    unsigned long *actual);

typedef struct SitStillInputStream {
    SitStillStreamReadProc read;
    SitStillStreamSeekProc seek;
    SitStillStreamTellProc tell;
    SitStillStreamLengthProc length;
    void *refCon;
} SitStillInputStream;

/* Output is deliberately separate from input: archive readers need only read,
 * while extractors can stream bounded chunks directly to an HFS fork. */
typedef struct SitStillOutputStream {
    SitStillStreamWriteProc write;
    void *refCon;
} SitStillOutputStream;

typedef struct SitStillMemoryStream {
    const unsigned char *bytes;
    unsigned long length;
    unsigned long position;
} SitStillMemoryStream;

typedef struct SitStillMemoryOutputStream {
    unsigned char *bytes;
    unsigned long capacity;
    unsigned long position;
} SitStillMemoryOutputStream;

typedef struct SitStillDiscardOutputStream {
    unsigned long length;
} SitStillDiscardOutputStream;

typedef struct SitStillBoundedOutputStream {
    SitStillOutputStream *parent;
    unsigned long *sharedRemaining;
    unsigned long limit;
    unsigned long written;
    unsigned short limitExceeded;
} SitStillBoundedOutputStream;

typedef struct SitStillLimitedStream {
    SitStillInputStream *parent;
    unsigned long start;
    unsigned long length;
    unsigned long position;
} SitStillLimitedStream;

void SitStillMemoryStreamInit(SitStillMemoryStream *storage,
                              SitStillInputStream *stream,
                              const unsigned char *bytes,
                              unsigned long length);

void SitStillMemoryOutputStreamInit(SitStillMemoryOutputStream *storage,
                                    SitStillOutputStream *stream,
                                    unsigned char *bytes,
                                    unsigned long capacity);

void SitStillDiscardOutputStreamInit(SitStillDiscardOutputStream *storage,
                                     SitStillOutputStream *stream);

SitStillStreamResult SitStillBoundedOutputStreamInit(
    SitStillBoundedOutputStream *storage, SitStillOutputStream *stream,
    SitStillOutputStream *parent, unsigned long limit);

SitStillStreamResult SitStillLimitedStreamInit(
    SitStillLimitedStream *storage, SitStillInputStream *stream,
    SitStillInputStream *parent, unsigned long start, unsigned long length);

SitStillStreamResult SitStillStreamReadExact(SitStillInputStream *stream,
                                              unsigned char *bytes,
                                              unsigned long request);

SitStillStreamResult SitStillStreamWriteExact(SitStillOutputStream *stream,
                                               const unsigned char *bytes,
                                               unsigned long request);

#endif
