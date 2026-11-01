#ifndef SITSTILL_CLASSIC_SIT_H
#define SITSTILL_CLASSIC_SIT_H

#include "sitstill/stream.h"

typedef enum SitStillClassicSITResult {
    kSitStillClassicSITOK = 0,
    kSitStillClassicSITBadHeader = -1,
    kSitStillClassicSITTruncated = -2,
    kSitStillClassicSITBadEntryCRC = -3,
    kSitStillClassicSITBadName = -4,
    kSitStillClassicSITLengthOverflow = -5,
    kSitStillClassicSITUnsupportedMethod = -6,
    kSitStillClassicSITBadForkCRC = -7,
    kSitStillClassicSITOutputTooSmall = -8,
    kSitStillClassicSITInvalidRLE = -9,
    kSitStillClassicSITOutputLengthMismatch = -10,
    kSitStillClassicSITInvalidInput = -11,
    kSitStillClassicSITInvalidLZW = -12,
    kSitStillClassicSITTruncatedLZW = -13,
    kSitStillClassicSITInvalidHuffman = -14,
    kSitStillClassicSITTruncatedHuffman = -15,
    kSitStillClassicSITPasswordRequired = -16,
    kSitStillClassicSITInvalidLZAH = -17,
    kSitStillClassicSITTruncatedLZAH = -18,
    kSitStillClassicSITInvalidMW = -19,
    kSitStillClassicSITTruncatedMW = -20,
    kSitStillClassicSITInvalidArsenic = -21,
    kSitStillClassicSITTruncatedArsenic = -22,
    /* Method 15 blocks exceeding the bounded Arsenic workspace are refused. */
    kSitStillClassicSITArsenicWorkspaceLimit = -23,
    kSitStillClassicSITWrongPassword = -24,
    kSitStillClassicSITUnsupportedEncryption = -25,
    kSitStillClassicSITCancelled = -26,
    kSitStillClassicSITWorkspaceUnavailable = -27
} SitStillClassicSITResult;

typedef struct SitStillClassicSITArchive {
    unsigned short entryCount; /* root count declared in the archive header */
    unsigned short recordCount; /* depth-first catalog records after validation */
    unsigned long firstEntryOffset;
    unsigned long declaredLength;
    unsigned char version;
} SitStillClassicSITArchive;

typedef struct SitStillClassicSITEntry {
    unsigned char resourceMethod;
    unsigned char dataMethod;
    unsigned char resourceEncrypted;
    unsigned char dataEncrypted;
    unsigned char nameLength;
    unsigned char name[64];
    unsigned long type;
    unsigned long creator;
    unsigned short finderFlags;
    unsigned long creationDate;
    unsigned long modificationDate;
    unsigned long resourceLength;
    unsigned long dataLength;
    unsigned long compressedResourceLength;
    unsigned long compressedDataLength;
    unsigned short resourceCRC;
    unsigned short dataCRC;
    unsigned char resourcePadding;
    unsigned char dataPadding;
    unsigned short childCount;
    unsigned long previousEntryOffset;
    unsigned long nextEntryOffset;
    unsigned long parentEntryOffset;
    unsigned long firstChildOffset;
    unsigned long headerOffset;
    unsigned long resourceOffset;
    unsigned long dataOffset;
    unsigned long nextOffset;
} SitStillClassicSITEntry;

/* Returns the shared, aligned decoder-workspace arena when it can hold at
 * least requiredBytes. Core decoders are synchronous and must not be nested. */
void *SitStillClassicSITSharedWorkspace(unsigned long requiredBytes);

/* Result of a read-only, forensic catalog scan.  Recovered entries are
 * independently header-CRC-valid and have fork bounds inside the archive. */
typedef struct SitStillClassicSITRecovery {
    unsigned char archiveVersion;
    unsigned char archiveHeaderDamaged;
    unsigned short declaredEntryCount;
    unsigned short recoveredEntryCount;
    unsigned long firstEntryOffset;
    unsigned long declaredLength;
    unsigned long firstRecoveredOffset;
    unsigned long firstFailureOffset;
    unsigned long lastRecoveredOffset;
    SitStillClassicSITResult firstError;
} SitStillClassicSITRecovery;

unsigned short SitStillClassicSITCRC16(const unsigned char *bytes,
                                       unsigned long length);

SitStillClassicSITResult SitStillClassicSITParseArchive(
    const unsigned char *bytes, unsigned long length,
    SitStillClassicSITArchive *archive);

SitStillClassicSITResult SitStillClassicSITParseEntry(
    const unsigned char *bytes, unsigned long length, unsigned long offset,
    SitStillClassicSITEntry *entry);

/* Parses an entry header already read from a stream. */
SitStillClassicSITResult SitStillClassicSITParseEntryHeader(
    const unsigned char *bytes, unsigned long length, unsigned long offset,
    SitStillClassicSITEntry *entry);

/* Parses an entry header using the archive-version filename limit (63 bytes
 * for v1, 31 bytes for v2). */
SitStillClassicSITResult SitStillClassicSITParseEntryHeaderForVersion(
    const unsigned char *bytes, unsigned long length, unsigned long offset,
    unsigned char archiveVersion, SitStillClassicSITEntry *entry);

/* Extract method 0 (stored) only. Callers choose the resource or data fork. */
SitStillClassicSITResult SitStillClassicSITCopyStored(
    const unsigned char *bytes, unsigned long length,
    const SitStillClassicSITEntry *entry, int resourceFork,
    unsigned char *output, unsigned long outputCapacity,
    unsigned long *outputLength);

/* Extracts a stored or method-1 RLE fork into caller-owned bounded storage. */
SitStillClassicSITResult SitStillClassicSITExtractFork(
    const unsigned char *bytes, unsigned long length,
    const SitStillClassicSITEntry *entry, int resourceFork,
    unsigned char *output, unsigned long outputCapacity,
    unsigned long *outputLength);

/* Streams methods 0, 1, 2, 3, 5, 6, 8, 13, and 15 using caller-owned bounded
 * output storage and fixed codec workspaces. An experimental, currently
 * unclaimed Classic SIT method-14 decoder uses a temporary 256 KiB LZ history
 * allocation. Method 15 (Arsenic, used by SIT5)
 * refuses blocks requiring more than 48,000 rows of transform workspace.
 * Output may already have been written when an error is returned; callers
 * extracting to a file must use a temporary file and discard it on any
 * non-OK result. */
SitStillClassicSITResult SitStillClassicSITExtractForkStream(
    SitStillInputStream *input, const SitStillClassicSITEntry *entry,
    int resourceFork, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength);

/* Reads the 16-byte key trailer for an encrypted v2 fork. The helper checks
 * that the stored cipher region is block-aligned, padding fits within it, and
 * both cipher data and key trailer lie inside the input stream. */
SitStillClassicSITResult SitStillClassicSITReadForkKeyStream(
    SitStillInputStream *input, const SitStillClassicSITEntry *entry,
    int resourceFork, unsigned char key[16]);

/* Decrypts a Classic SIT v2 encrypted fork as a bounded stream before normal
 * decompression and CRC validation. The archive MKey resource must be supplied
 * by the caller from the archive file's Macintosh resource fork. */
SitStillClassicSITResult SitStillClassicSITExtractForkStreamWithPassword(
    SitStillInputStream *input, const SitStillClassicSITEntry *entry,
    int resourceFork, const unsigned char mkey[8],
    const unsigned char *password, unsigned long passwordLength,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength);

/* Validates every declared entry without extracting or modifying anything. */
SitStillClassicSITResult SitStillClassicSITValidateArchive(
    const unsigned char *bytes, unsigned long length);

/* Validates a catalog with bounded header reads; compressed forks stay on disk. */
SitStillClassicSITResult SitStillClassicSITValidateArchiveStream(
    SitStillInputStream *stream, SitStillClassicSITArchive *archive);

/* Scans a damaged classic SIT catalog for independently valid entries.
 * A valid archive reports all entries and firstError == kSitStillClassicSITOK.
 * A damaged catalog still returns OK after a completed scan; inspect recovery.
 * The function never decompresses, extracts, or modifies archive data. */
SitStillClassicSITResult SitStillClassicSITRecoverArchiveStream(
    SitStillInputStream *stream, SitStillClassicSITRecovery *recovery);

/* Finds the next independently valid entry header at or after startOffset.
 * scanEndOffset is the last candidate offset (inclusive); declaredLength
 * bounds all candidate fork extents. A successful search with no match sets
 * foundOffset to zero. */
SitStillClassicSITResult SitStillClassicSITFindNextRecoveredEntry(
    SitStillInputStream *stream, unsigned long startOffset,
    unsigned long scanEndOffset, unsigned long declaredLength,
    unsigned char archiveVersion,
    unsigned long *foundOffset, SitStillClassicSITEntry *entry);

/* Creates a one-file, stored-fork classic SIT archive in caller-owned storage. */
SitStillClassicSITResult SitStillClassicSITCreateStoredFile(
    const unsigned char *name, unsigned char nameLength,
    unsigned long type, unsigned long creator, unsigned short finderFlags,
    unsigned long creationDate, unsigned long modificationDate,
    const unsigned char *resourceBytes, unsigned long resourceLength,
    const unsigned char *dataBytes, unsigned long dataLength,
    unsigned char *output, unsigned long outputCapacity,
    unsigned long *outputLength);

typedef struct SitStillClassicSITStoredFileInput {
    const unsigned char *name;
    unsigned char nameLength;
    unsigned long type;
    unsigned long creator;
    unsigned short finderFlags;
    unsigned long creationDate;
    unsigned long modificationDate;
    SitStillInputStream *resourceInput;
    SitStillInputStream *dataInput;
} SitStillClassicSITStoredFileInput;

typedef enum SitStillClassicSITStoredRecordKind {
    kSitStillClassicSITStoredFileRecord = 0,
    kSitStillClassicSITStoredFolderStartRecord = 1,
    kSitStillClassicSITStoredFolderEndRecord = 2
} SitStillClassicSITStoredRecordKind;

typedef struct SitStillClassicSITStoredArchiveRecord {
    SitStillClassicSITStoredRecordKind kind;
    SitStillClassicSITStoredFileInput file;
} SitStillClassicSITStoredArchiveRecord;

/* Creates a legacy-compatible stored SIT archive containing multiple files.
 * Forks are streamed and need not fit in memory; all inputs must be seekable.
 * The caller owns the file descriptor array and must keep it valid for the
 * duration of the call. */
SitStillClassicSITResult SitStillClassicSITCreateStoredArchiveStream(
    const SitStillClassicSITStoredFileInput *files, unsigned short fileCount,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength);

/* Creates a legacy-compatible Classic SIT archive, selecting method-1 RLE
 * independently for each fork only when it is smaller than Store. */
SitStillClassicSITResult SitStillClassicSITCreateClassicArchiveStream(
    const SitStillClassicSITStoredFileInput *files, unsigned short fileCount,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength);

/* Creates a stored classic SIT archive from a depth-first sequence of at most
 * 128 records and 64 files. Folder names are path components (not paths);
 * folders and files preserve this sequence exactly. Marker records have no
 * streams and use their file.name/nameLength fields. */
SitStillClassicSITResult SitStillClassicSITCreateStoredRecordsStream(
    const SitStillClassicSITStoredArchiveRecord *records,
    unsigned short recordCount, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength);

/* Like the stored variant, with per-fork method-1 RLE when smaller. */
SitStillClassicSITResult SitStillClassicSITCreateClassicRecordsStream(
    const SitStillClassicSITStoredArchiveRecord *records,
    unsigned short recordCount, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength);

/* Creates a one-file legacy-compatible stored SIT archive without requiring
 * either input fork to fit in memory. Inputs must be seekable. */
SitStillClassicSITResult SitStillClassicSITCreateStoredFileStream(
    const unsigned char *name, unsigned char nameLength,
    unsigned long type, unsigned long creator, unsigned short finderFlags,
    unsigned long creationDate, unsigned long modificationDate,
    SitStillInputStream *resourceInput, SitStillInputStream *dataInput,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength);

const char *SitStillClassicSITMethodName(unsigned char method);

#endif
