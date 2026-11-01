#include "sitstill/classic_sit.h"
#include "classic_sit_internal.h"

#define kSitClassicSITMaxWriteRecords 128U
#define kSitClassicSITMaxWriteFiles 64U

typedef struct SitStillClassicSITWritePlan {
    unsigned long resourceRLELength;
    unsigned long dataRLELength;
    unsigned short resourceCRC;
    unsigned short dataCRC;
} SitStillClassicSITWritePlan;

static void WriteBE16(unsigned char *bytes, unsigned short value)
{
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)value;
}

static void WriteBE32(unsigned char *bytes, unsigned long value)
{
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
}

SitStillClassicSITResult SitStillClassicSITCreateStoredFile(
    const unsigned char *name, unsigned char nameLength,
    unsigned long type, unsigned long creator, unsigned short finderFlags,
    unsigned long creationDate, unsigned long modificationDate,
    const unsigned char *resourceBytes, unsigned long resourceLength,
    const unsigned char *dataBytes, unsigned long dataLength,
    unsigned char *output, unsigned long outputCapacity,
    unsigned long *outputLength)
{
    unsigned long archiveLength;
    unsigned long position;
    unsigned long index;

    if (name == 0 || output == 0 || outputLength == 0 || nameLength == 0U ||
        nameLength > 63U || (resourceLength != 0UL && resourceBytes == 0) ||
        (dataLength != 0UL && dataBytes == 0))
        return kSitStillClassicSITInvalidInput;
    archiveLength = kSitArchiveHeaderSize + kSitEntryHeaderSize;
    if (resourceLength > 0xffffffffUL - archiveLength)
        return kSitStillClassicSITLengthOverflow;
    archiveLength += resourceLength;
    if (dataLength > 0xffffffffUL - archiveLength)
        return kSitStillClassicSITLengthOverflow;
    archiveLength += dataLength;
    if (archiveLength > outputCapacity)
        return kSitStillClassicSITOutputTooSmall;

    for (index = 0UL; index < archiveLength; ++index)
        output[index] = 0U;
    output[0] = 'S';
    output[1] = 'I';
    output[2] = 'T';
    output[3] = '!';
    WriteBE16(output + 4UL, 1U);
    WriteBE32(output + 6UL, archiveLength);
    output[10] = 'r';
    output[11] = 'L';
    output[12] = 'a';
    output[13] = 'u';
    output[14] = 1U;

    position = kSitArchiveHeaderSize;
    output[position + 2UL] = nameLength;
    for (index = 0UL; index < (unsigned long)nameLength; ++index)
        output[position + 3UL + index] = name[index];
    WriteBE32(output + position + 66UL, type);
    WriteBE32(output + position + 70UL, creator);
    WriteBE16(output + position + 74UL, finderFlags);
    WriteBE32(output + position + 76UL, creationDate);
    WriteBE32(output + position + 80UL, modificationDate);
    WriteBE32(output + position + 84UL, resourceLength);
    WriteBE32(output + position + 88UL, dataLength);
    WriteBE32(output + position + 92UL, resourceLength);
    WriteBE32(output + position + 96UL, dataLength);
    WriteBE16(output + position + 100UL,
              SitStillClassicSITCRC16(resourceBytes, resourceLength));
    WriteBE16(output + position + 102UL,
              SitStillClassicSITCRC16(dataBytes, dataLength));
    WriteBE16(output + position + 110UL,
              SitStillClassicSITCRC16(output + position, 110UL));
    position += kSitEntryHeaderSize;
    for (index = 0UL; index < resourceLength; ++index)
        output[position + index] = resourceBytes[index];
    position += resourceLength;
    for (index = 0UL; index < dataLength; ++index)
        output[position + index] = dataBytes[index];
    *outputLength = archiveLength;
    return kSitStillClassicSITOK;
}

static void AccumulateRLERun(unsigned long *encodedLength,
                             unsigned char value, unsigned long runLength)
{
    unsigned long repeats;
    unsigned long chunks;
    unsigned long cost;

    if (encodedLength == 0 || runLength == 0UL ||
        *encodedLength == 0xffffffffUL)
        return;
    repeats = runLength - 1UL;
    chunks = repeats / 254UL + (repeats % 254UL != 0UL ? 1UL : 0UL);
    cost = value == 0x90U ? 2UL : 1UL;
    if (chunks > (0xffffffffUL - cost) / 2UL ||
        *encodedLength > 0xffffffffUL - cost - chunks * 2UL)
        *encodedLength = 0xffffffffUL;
    else
        *encodedLength += cost + chunks * 2UL;
}

static SitStillClassicSITResult InspectInputFork(SitStillInputStream *input,
                                                  unsigned char *workBuffer,
                                                  unsigned long workBufferLength,
                                                  unsigned long *forkLength,
                                                  unsigned short *forkCRC,
                                                  unsigned long *rleLength)
{
    unsigned long left;
    unsigned short crc;
    unsigned long runLength;
    unsigned long encodedLength;
    unsigned char previous;
    int havePrevious;
    SitStillStreamResult streamResult;

    if (forkLength == 0 || forkCRC == 0)
        return kSitStillClassicSITInvalidInput;
    if (input == 0) {
        *forkLength = 0UL;
        *forkCRC = 0U;
        if (rleLength != 0)
            *rleLength = 0UL;
        return kSitStillClassicSITOK;
    }
    if (input->read == 0 || input->seek == 0 || input->length == 0)
        return kSitStillClassicSITInvalidInput;
    streamResult = input->length(input->refCon, forkLength);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITInvalidInput;
    streamResult = input->seek(input->refCon, 0UL);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITInvalidInput;
    left = *forkLength;
    crc = 0U;
    runLength = 0UL;
    encodedLength = 0UL;
    previous = 0U;
    havePrevious = 0;
    while (left > 0UL) {
        unsigned long request;
        unsigned long index;

        request = left < workBufferLength ? left : workBufferLength;
        streamResult = SitStillStreamReadExact(input, workBuffer, request);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITTruncated;
        crc = SitStillClassicSITCRC16Update(crc, workBuffer, request);
        if (rleLength != 0) {
            for (index = 0UL; index < request; ++index) {
                unsigned char value;

                value = workBuffer[index];
                if (!havePrevious) {
                    previous = value;
                    runLength = 1UL;
                    havePrevious = 1;
                } else if (value == previous) {
                    ++runLength;
                } else {
                    AccumulateRLERun(&encodedLength, previous, runLength);
                    previous = value;
                    runLength = 1UL;
                }
            }
        }
        left -= request;
    }
    if (rleLength != 0 && havePrevious)
        AccumulateRLERun(&encodedLength, previous, runLength);
    streamResult = input->seek(input->refCon, 0UL);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITInvalidInput;
    *forkCRC = crc;
    if (rleLength != 0)
        *rleLength = encodedLength;
    return kSitStillClassicSITOK;
}

typedef struct SitStillRLEOutputBuffer {
    SitStillOutputStream *output;
    unsigned char bytes[256];
    unsigned short used;
    unsigned long length;
} SitStillRLEOutputBuffer;

static SitStillClassicSITResult FlushRLEOutput(
    SitStillRLEOutputBuffer *writer)
{
    SitStillStreamResult streamResult;

    if (writer->used == 0U)
        return kSitStillClassicSITOK;
    streamResult = SitStillStreamWriteExact(writer->output, writer->bytes,
                                            writer->used);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
    writer->used = 0U;
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult PutRLEByte(SitStillRLEOutputBuffer *writer,
                                           unsigned char value)
{
    SitStillClassicSITResult result;

    if (writer->used == sizeof(writer->bytes)) {
        result = FlushRLEOutput(writer);
        if (result != kSitStillClassicSITOK)
            return result;
    }
    writer->bytes[writer->used++] = value;
    ++writer->length;
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult WriteRLERun(SitStillRLEOutputBuffer *writer,
                                            unsigned char value,
                                            unsigned long runLength)
{
    SitStillClassicSITResult result;
    unsigned long repeats;

    if (runLength == 0UL)
        return kSitStillClassicSITOK;
    if (value == 0x90U) {
        result = PutRLEByte(writer, 0x90U);
        if (result == kSitStillClassicSITOK)
            result = PutRLEByte(writer, 0U);
    } else
        result = PutRLEByte(writer, value);
    if (result != kSitStillClassicSITOK)
        return result;
    repeats = runLength - 1UL;
    while (repeats > 0UL) {
        unsigned long chunk;

        chunk = repeats > 254UL ? 254UL : repeats;
        result = PutRLEByte(writer, 0x90U);
        if (result == kSitStillClassicSITOK)
            result = PutRLEByte(writer, (unsigned char)(chunk + 1UL));
        if (result != kSitStillClassicSITOK)
            return result;
        repeats -= chunk;
    }
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult WriteRLEFork(
    SitStillInputStream *input, unsigned long forkLength,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *encodedLength)
{
    SitStillRLEOutputBuffer writer;
    SitStillStreamResult streamResult;
    SitStillClassicSITResult result;
    unsigned long left;
    unsigned long runLength;
    unsigned char previous;
    int havePrevious;

    if (input == 0 || input->read == 0 || input->seek == 0 || output == 0 ||
        output->write == 0 || workBuffer == 0 || workBufferLength == 0UL ||
        encodedLength == 0)
        return kSitStillClassicSITInvalidInput;
    streamResult = input->seek(input->refCon, 0UL);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITInvalidInput;
    writer.output = output;
    writer.used = 0U;
    writer.length = 0UL;
    left = forkLength;
    runLength = 0UL;
    previous = 0U;
    havePrevious = 0;
    while (left > 0UL) {
        unsigned long request;
        unsigned long index;

        request = left < workBufferLength ? left : workBufferLength;
        streamResult = SitStillStreamReadExact(input, workBuffer, request);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITTruncated;
        for (index = 0UL; index < request; ++index) {
            unsigned char value;

            value = workBuffer[index];
            if (!havePrevious) {
                previous = value;
                runLength = 1UL;
                havePrevious = 1;
            } else if (value == previous)
                ++runLength;
            else {
                result = WriteRLERun(&writer, previous, runLength);
                if (result != kSitStillClassicSITOK)
                    return result;
                previous = value;
                runLength = 1UL;
            }
        }
        left -= request;
    }
    if (havePrevious) {
        result = WriteRLERun(&writer, previous, runLength);
        if (result != kSitStillClassicSITOK)
            return result;
    }
    result = FlushRLEOutput(&writer);
    if (result != kSitStillClassicSITOK)
        return result;
    *encodedLength = writer.length;
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult CopyInputFork(SitStillInputStream *input,
                                               unsigned long forkLength,
                                               SitStillOutputStream *output,
                                               unsigned char *workBuffer,
                                               unsigned long workBufferLength)
{
    unsigned long left;
    SitStillStreamResult streamResult;

    if (forkLength == 0UL)
        return kSitStillClassicSITOK;
    if (input == 0 || input->read == 0 || input->seek == 0 ||
        input->seek(input->refCon, 0UL) != kSitStillStreamOK)
        return kSitStillClassicSITInvalidInput;
    left = forkLength;
    while (left > 0UL) {
        unsigned long request;

        request = left < workBufferLength ? left : workBufferLength;
        streamResult = SitStillStreamReadExact(input, workBuffer, request);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITTruncated;
        streamResult = SitStillStreamWriteExact(output, workBuffer, request);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
        left -= request;
    }
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult GetInputForkLength(
    SitStillInputStream *input, unsigned long *forkLength)
{
    SitStillStreamResult streamResult;

    if (forkLength == 0)
        return kSitStillClassicSITInvalidInput;
    if (input == 0) {
        *forkLength = 0UL;
        return kSitStillClassicSITOK;
    }
    if (input->read == 0 || input->seek == 0 || input->length == 0)
        return kSitStillClassicSITInvalidInput;
    streamResult = input->length(input->refCon, forkLength);
    if (streamResult == kSitStillStreamCancelled)
        return kSitStillClassicSITCancelled;
    if (streamResult != kSitStillStreamOK)
        return kSitStillClassicSITInvalidInput;
    return kSitStillClassicSITOK;
}

static SitStillClassicSITResult CreateClassicArchiveStreamInternal(
    const SitStillClassicSITStoredFileInput *files, unsigned short fileCount,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength,
    int allowRLE)
{
    unsigned char archiveHeader[kSitArchiveHeaderSize];
    unsigned char entryHeader[kSitEntryHeaderSize];
    unsigned long archiveLength;
    unsigned long writtenLength;
    unsigned long index;
    unsigned short fileIndex;
    SitStillClassicSITResult result;
    SitStillStreamResult streamResult;

    if (files == 0 || fileCount == 0U || output == 0 || output->write == 0 ||
        workBuffer == 0 || workBufferLength == 0UL || outputLength == 0)
        return kSitStillClassicSITInvalidInput;
    archiveLength = kSitArchiveHeaderSize +
                    (unsigned long)fileCount * kSitEntryHeaderSize;
    for (fileIndex = 0U; fileIndex < fileCount; ++fileIndex) {
        unsigned long forkLength;
        unsigned long rleLength;
        unsigned short forkCRC;

        if (files[fileIndex].name == 0 || files[fileIndex].nameLength == 0U ||
            files[fileIndex].nameLength > 63U)
            return kSitStillClassicSITInvalidInput;
        if (allowRLE)
            result = InspectInputFork(files[fileIndex].resourceInput,
                workBuffer, workBufferLength, &forkLength, &forkCRC,
                &rleLength);
        else
            result = GetInputForkLength(files[fileIndex].resourceInput,
                                        &forkLength);
        if (result != kSitStillClassicSITOK)
            return result;
        if (allowRLE && rleLength < forkLength)
            forkLength = rleLength;
        if (forkLength > 0xffffffffUL - archiveLength)
            return kSitStillClassicSITLengthOverflow;
        archiveLength += forkLength;
        if (allowRLE)
            result = InspectInputFork(files[fileIndex].dataInput,
                workBuffer, workBufferLength, &forkLength, &forkCRC,
                &rleLength);
        else
            result = GetInputForkLength(files[fileIndex].dataInput,
                                        &forkLength);
        if (result != kSitStillClassicSITOK)
            return result;
        if (allowRLE && rleLength < forkLength)
            forkLength = rleLength;
        if (forkLength > 0xffffffffUL - archiveLength)
            return kSitStillClassicSITLengthOverflow;
        archiveLength += forkLength;
    }
    for (index = 0UL; index < kSitArchiveHeaderSize; ++index)
        archiveHeader[index] = 0U;
    archiveHeader[0] = 'S';
    archiveHeader[1] = 'I';
    archiveHeader[2] = 'T';
    archiveHeader[3] = '!';
    WriteBE16(archiveHeader + 4UL, fileCount);
    WriteBE32(archiveHeader + 6UL, archiveLength);
    archiveHeader[10] = 'r';
    archiveHeader[11] = 'L';
    archiveHeader[12] = 'a';
    archiveHeader[13] = 'u';
    archiveHeader[14] = 1U;
    streamResult = SitStillStreamWriteExact(output, archiveHeader,
                                            kSitArchiveHeaderSize);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
    writtenLength = kSitArchiveHeaderSize;
    for (fileIndex = 0U; fileIndex < fileCount; ++fileIndex) {
        unsigned long resourceLength;
        unsigned long dataLength;
        unsigned long resourceRLELength;
        unsigned long dataRLELength;
        unsigned long resourceStoredLength;
        unsigned long dataStoredLength;
        unsigned short resourceCRC;
        unsigned short dataCRC;

        result = InspectInputFork(files[fileIndex].resourceInput, workBuffer,
                                  workBufferLength, &resourceLength,
                                  &resourceCRC,
                                  allowRLE ? &resourceRLELength : 0);
        if (result != kSitStillClassicSITOK)
            return result;
        result = InspectInputFork(files[fileIndex].dataInput, workBuffer,
                                  workBufferLength, &dataLength, &dataCRC,
                                  allowRLE ? &dataRLELength : 0);
        if (result != kSitStillClassicSITOK)
            return result;
        for (index = 0UL; index < kSitEntryHeaderSize; ++index)
            entryHeader[index] = 0U;
        entryHeader[2] = files[fileIndex].nameLength;
        for (index = 0UL; index < (unsigned long)files[fileIndex].nameLength;
             ++index)
            entryHeader[3UL + index] = files[fileIndex].name[index];
        WriteBE32(entryHeader + 66UL, files[fileIndex].type);
        WriteBE32(entryHeader + 70UL, files[fileIndex].creator);
        WriteBE16(entryHeader + 74UL, files[fileIndex].finderFlags);
        WriteBE32(entryHeader + 76UL, files[fileIndex].creationDate);
        WriteBE32(entryHeader + 80UL, files[fileIndex].modificationDate);
        resourceStoredLength = allowRLE &&
            resourceRLELength < resourceLength ? resourceRLELength :
            resourceLength;
        dataStoredLength = allowRLE && dataRLELength < dataLength ?
            dataRLELength : dataLength;
        entryHeader[0] = resourceStoredLength < resourceLength ? 1U : 0U;
        entryHeader[1] = dataStoredLength < dataLength ? 1U : 0U;
        WriteBE32(entryHeader + 84UL, resourceLength);
        WriteBE32(entryHeader + 88UL, dataLength);
        WriteBE32(entryHeader + 92UL, resourceStoredLength);
        WriteBE32(entryHeader + 96UL, dataStoredLength);
        WriteBE16(entryHeader + 100UL, resourceCRC);
        WriteBE16(entryHeader + 102UL, dataCRC);
        WriteBE16(entryHeader + 110UL,
                  SitStillClassicSITCRC16(entryHeader, 110UL));
        streamResult = SitStillStreamWriteExact(output, entryHeader,
                                                kSitEntryHeaderSize);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
        if (entryHeader[0] == 1U) {
            unsigned long encodedLength;

            result = WriteRLEFork(files[fileIndex].resourceInput,
                resourceLength, output, workBuffer, workBufferLength,
                &encodedLength);
            if (result == kSitStillClassicSITOK &&
                encodedLength != resourceStoredLength)
                result = kSitStillClassicSITOutputLengthMismatch;
        } else
            result = CopyInputFork(files[fileIndex].resourceInput,
                resourceLength, output, workBuffer, workBufferLength);
        if (result != kSitStillClassicSITOK)
            return result;
        if (entryHeader[1] == 1U) {
            unsigned long encodedLength;

            result = WriteRLEFork(files[fileIndex].dataInput, dataLength,
                output, workBuffer, workBufferLength, &encodedLength);
            if (result == kSitStillClassicSITOK &&
                encodedLength != dataStoredLength)
                result = kSitStillClassicSITOutputLengthMismatch;
        } else
            result = CopyInputFork(files[fileIndex].dataInput, dataLength,
                output, workBuffer, workBufferLength);
        if (result != kSitStillClassicSITOK)
            return result;
        if (resourceStoredLength > 0xffffffffUL - writtenLength)
            return kSitStillClassicSITLengthOverflow;
        writtenLength += resourceStoredLength;
        if (dataStoredLength > 0xffffffffUL - writtenLength)
            return kSitStillClassicSITLengthOverflow;
        writtenLength += dataStoredLength;
        if (writtenLength > archiveLength ||
            kSitEntryHeaderSize > archiveLength - writtenLength)
            return kSitStillClassicSITOutputLengthMismatch;
        writtenLength += kSitEntryHeaderSize;
    }
    if (writtenLength != archiveLength)
        return kSitStillClassicSITOutputLengthMismatch;
    *outputLength = archiveLength;
    return kSitStillClassicSITOK;
}

SitStillClassicSITResult SitStillClassicSITCreateStoredArchiveStream(
    const SitStillClassicSITStoredFileInput *files, unsigned short fileCount,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength)
{
    return CreateClassicArchiveStreamInternal(files, fileCount, output,
        workBuffer, workBufferLength, outputLength, 0);
}

SitStillClassicSITResult SitStillClassicSITCreateClassicArchiveStream(
    const SitStillClassicSITStoredFileInput *files, unsigned short fileCount,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength)
{
    return CreateClassicArchiveStreamInternal(files, fileCount, output,
        workBuffer, workBufferLength, outputLength, 1);
}

static SitStillClassicSITResult CreateClassicRecordsStreamInternal(
    const SitStillClassicSITStoredArchiveRecord *records,
    unsigned short recordCount, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength, int allowRLE)
{
    unsigned char archiveHeader[kSitArchiveHeaderSize];
    unsigned char entryHeader[kSitEntryHeaderSize];
    SitStillClassicSITWritePlan writePlan[kSitClassicSITMaxWriteFiles];
    unsigned short folderDepth;
    unsigned short recordIndex;
    unsigned short filePlanIndex;
    unsigned long archiveLength;
    unsigned long writtenLength;
    unsigned long index;
    SitStillStreamResult streamResult;

    if (records == 0 || recordCount == 0U ||
        recordCount > kSitClassicSITMaxWriteRecords || output == 0 ||
        output->write == 0 || workBuffer == 0 || workBufferLength == 0UL ||
        outputLength == 0)
        return kSitStillClassicSITInvalidInput;
    archiveLength = kSitArchiveHeaderSize +
                    (unsigned long)recordCount * kSitEntryHeaderSize;
    folderDepth = 0U;
    filePlanIndex = 0U;
    for (recordIndex = 0U; recordIndex < recordCount; ++recordIndex) {
        const SitStillClassicSITStoredArchiveRecord *record;

        record = &records[recordIndex];
        if (record->kind == kSitStillClassicSITStoredFolderEndRecord) {
            if (folderDepth == 0U || record->file.nameLength != 0U)
                return kSitStillClassicSITInvalidInput;
            --folderDepth;
            continue;
        }
        if (record->kind != kSitStillClassicSITStoredFileRecord &&
            record->kind != kSitStillClassicSITStoredFolderStartRecord)
            return kSitStillClassicSITInvalidInput;
        if (record->file.name == 0 || record->file.nameLength == 0U ||
            record->file.nameLength > 63U)
            return kSitStillClassicSITInvalidInput;
        if (record->kind == kSitStillClassicSITStoredFolderStartRecord) {
            if (folderDepth == 32U)
                return kSitStillClassicSITInvalidInput;
            ++folderDepth;
            continue;
        }
        {
            unsigned long forkLength;
            unsigned long rleLength;
            unsigned short forkCRC;
            SitStillClassicSITResult result;

            if (filePlanIndex >= kSitClassicSITMaxWriteFiles)
                return kSitStillClassicSITInvalidInput;

            if (allowRLE)
                result = InspectInputFork(record->file.resourceInput,
                    workBuffer, workBufferLength, &forkLength, &forkCRC,
                    &rleLength);
            else
                result = GetInputForkLength(record->file.resourceInput,
                                            &forkLength);
            if (result != kSitStillClassicSITOK)
                return result;
            if (allowRLE) {
                writePlan[filePlanIndex].resourceRLELength = rleLength;
                writePlan[filePlanIndex].resourceCRC = forkCRC;
            }
            if (allowRLE && rleLength < forkLength)
                forkLength = rleLength;
            if (forkLength > 0xffffffffUL - archiveLength)
                return kSitStillClassicSITLengthOverflow;
            archiveLength += forkLength;
            if (allowRLE)
                result = InspectInputFork(record->file.dataInput,
                    workBuffer, workBufferLength, &forkLength, &forkCRC,
                    &rleLength);
            else
                result = GetInputForkLength(record->file.dataInput,
                                            &forkLength);
            if (result != kSitStillClassicSITOK)
                return result;
            if (allowRLE) {
                writePlan[filePlanIndex].dataRLELength = rleLength;
                writePlan[filePlanIndex].dataCRC = forkCRC;
            }
            if (allowRLE && rleLength < forkLength)
                forkLength = rleLength;
            if (forkLength > 0xffffffffUL - archiveLength)
                return kSitStillClassicSITLengthOverflow;
            archiveLength += forkLength;
            ++filePlanIndex;
        }
    }
    if (folderDepth != 0U)
        return kSitStillClassicSITInvalidInput;
    for (index = 0UL; index < kSitArchiveHeaderSize; ++index)
        archiveHeader[index] = 0U;
    archiveHeader[0] = 'S';
    archiveHeader[1] = 'I';
    archiveHeader[2] = 'T';
    archiveHeader[3] = '!';
    WriteBE16(archiveHeader + 4UL, recordCount);
    WriteBE32(archiveHeader + 6UL, archiveLength);
    archiveHeader[10] = 'r';
    archiveHeader[11] = 'L';
    archiveHeader[12] = 'a';
    archiveHeader[13] = 'u';
    archiveHeader[14] = 1U;
    streamResult = SitStillStreamWriteExact(output, archiveHeader,
                                            kSitArchiveHeaderSize);
    if (streamResult != kSitStillStreamOK)
        return streamResult == kSitStillStreamCancelled ?
            kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
    writtenLength = kSitArchiveHeaderSize;
    filePlanIndex = 0U;
    for (recordIndex = 0U; recordIndex < recordCount; ++recordIndex) {
        const SitStillClassicSITStoredArchiveRecord *record;
        unsigned long resourceLength;
        unsigned long dataLength;
        unsigned long resourceRLELength;
        unsigned long dataRLELength;
        unsigned long resourceStoredLength;
        unsigned long dataStoredLength;
        unsigned short resourceCRC;
        unsigned short dataCRC;
        unsigned char method;
        unsigned char resourceMethod;
        unsigned char dataMethod;
        SitStillClassicSITResult result;

        record = &records[recordIndex];
        resourceMethod = 0U;
        dataMethod = 0U;
        for (index = 0UL; index < kSitEntryHeaderSize; ++index)
            entryHeader[index] = 0U;
        if (record->kind == kSitStillClassicSITStoredFolderStartRecord ||
            record->kind == kSitStillClassicSITStoredFolderEndRecord) {
            method = record->kind ==
                kSitStillClassicSITStoredFolderStartRecord ? 0x30U : 0x31U;
            entryHeader[0] = method;
            entryHeader[1] = method;
            if (record->kind ==
                kSitStillClassicSITStoredFolderStartRecord) {
                entryHeader[2] = record->file.nameLength;
                for (index = 0UL;
                     index < (unsigned long)record->file.nameLength; ++index)
                    entryHeader[3UL + index] = record->file.name[index];
                WriteBE32(entryHeader + 66UL, record->file.type);
                WriteBE32(entryHeader + 70UL, record->file.creator);
                WriteBE16(entryHeader + 74UL, record->file.finderFlags);
                WriteBE32(entryHeader + 76UL,
                          record->file.creationDate);
                WriteBE32(entryHeader + 80UL,
                          record->file.modificationDate);
            }
            resourceLength = 0UL;
            dataLength = 0UL;
            resourceStoredLength = 0UL;
            dataStoredLength = 0UL;
        } else {
            if (allowRLE) {
                result = GetInputForkLength(record->file.resourceInput,
                                            &resourceLength);
                if (result != kSitStillClassicSITOK)
                    return result;
                result = GetInputForkLength(record->file.dataInput,
                                            &dataLength);
                if (result != kSitStillClassicSITOK)
                    return result;
                resourceRLELength =
                    writePlan[filePlanIndex].resourceRLELength;
                dataRLELength = writePlan[filePlanIndex].dataRLELength;
                resourceCRC = writePlan[filePlanIndex].resourceCRC;
                dataCRC = writePlan[filePlanIndex].dataCRC;
            } else {
                result = InspectInputFork(record->file.resourceInput,
                    workBuffer, workBufferLength, &resourceLength,
                    &resourceCRC, 0);
                if (result != kSitStillClassicSITOK)
                    return result;
                result = InspectInputFork(record->file.dataInput,
                    workBuffer, workBufferLength, &dataLength,
                    &dataCRC, 0);
                if (result != kSitStillClassicSITOK)
                    return result;
                resourceRLELength = resourceLength;
                dataRLELength = dataLength;
            }
            entryHeader[2] = record->file.nameLength;
            for (index = 0UL; index <
                 (unsigned long)record->file.nameLength; ++index)
                entryHeader[3UL + index] = record->file.name[index];
            WriteBE32(entryHeader + 66UL, record->file.type);
            WriteBE32(entryHeader + 70UL, record->file.creator);
            WriteBE16(entryHeader + 74UL, record->file.finderFlags);
            WriteBE32(entryHeader + 76UL, record->file.creationDate);
            WriteBE32(entryHeader + 80UL, record->file.modificationDate);
            resourceStoredLength = allowRLE &&
                resourceRLELength < resourceLength ? resourceRLELength :
                resourceLength;
            dataStoredLength = allowRLE && dataRLELength < dataLength ?
                dataRLELength : dataLength;
            resourceMethod = resourceStoredLength < resourceLength ? 1U : 0U;
            dataMethod = dataStoredLength < dataLength ? 1U : 0U;
            entryHeader[0] = resourceMethod;
            entryHeader[1] = dataMethod;
            WriteBE32(entryHeader + 84UL, resourceLength);
            WriteBE32(entryHeader + 88UL, dataLength);
            WriteBE32(entryHeader + 92UL, resourceStoredLength);
            WriteBE32(entryHeader + 96UL, dataStoredLength);
            WriteBE16(entryHeader + 100UL, resourceCRC);
            WriteBE16(entryHeader + 102UL, dataCRC);
        }
        WriteBE16(entryHeader + 110UL,
                  SitStillClassicSITCRC16(entryHeader, 110UL));
        streamResult = SitStillStreamWriteExact(output, entryHeader,
                                                kSitEntryHeaderSize);
        if (streamResult != kSitStillStreamOK)
            return streamResult == kSitStillStreamCancelled ?
                kSitStillClassicSITCancelled : kSitStillClassicSITOutputTooSmall;
        if (record->kind == kSitStillClassicSITStoredFileRecord) {
            if (resourceMethod == 1U) {
                unsigned long encodedLength;

                result = WriteRLEFork(record->file.resourceInput,
                    resourceLength, output, workBuffer, workBufferLength,
                    &encodedLength);
                if (result == kSitStillClassicSITOK &&
                    encodedLength != resourceStoredLength)
                    result = kSitStillClassicSITOutputLengthMismatch;
            } else
                result = CopyInputFork(record->file.resourceInput,
                    resourceLength, output, workBuffer, workBufferLength);
            if (result != kSitStillClassicSITOK)
                return result;
            if (dataMethod == 1U) {
                unsigned long encodedLength;

                result = WriteRLEFork(record->file.dataInput, dataLength,
                    output, workBuffer, workBufferLength, &encodedLength);
                if (result == kSitStillClassicSITOK &&
                    encodedLength != dataStoredLength)
                    result = kSitStillClassicSITOutputLengthMismatch;
            } else
                result = CopyInputFork(record->file.dataInput, dataLength,
                    output, workBuffer, workBufferLength);
            if (result != kSitStillClassicSITOK)
                return result;
            ++filePlanIndex;
        }
        if (resourceStoredLength > 0xffffffffUL - writtenLength)
            return kSitStillClassicSITLengthOverflow;
        writtenLength += resourceStoredLength;
        if (dataStoredLength > 0xffffffffUL - writtenLength)
            return kSitStillClassicSITLengthOverflow;
        writtenLength += dataStoredLength;
        if (writtenLength > archiveLength ||
            kSitEntryHeaderSize > archiveLength - writtenLength)
            return kSitStillClassicSITOutputLengthMismatch;
        writtenLength += kSitEntryHeaderSize;
    }
    if (writtenLength != archiveLength)
        return kSitStillClassicSITOutputLengthMismatch;
    *outputLength = archiveLength;
    return kSitStillClassicSITOK;
}

SitStillClassicSITResult SitStillClassicSITCreateStoredRecordsStream(
    const SitStillClassicSITStoredArchiveRecord *records,
    unsigned short recordCount, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength)
{
    return CreateClassicRecordsStreamInternal(records, recordCount, output,
        workBuffer, workBufferLength, outputLength, 0);
}

SitStillClassicSITResult SitStillClassicSITCreateClassicRecordsStream(
    const SitStillClassicSITStoredArchiveRecord *records,
    unsigned short recordCount, SitStillOutputStream *output,
    unsigned char *workBuffer, unsigned long workBufferLength,
    unsigned long *outputLength)
{
    return CreateClassicRecordsStreamInternal(records, recordCount, output,
        workBuffer, workBufferLength, outputLength, 1);
}

SitStillClassicSITResult SitStillClassicSITCreateStoredFileStream(
    const unsigned char *name, unsigned char nameLength,
    unsigned long type, unsigned long creator, unsigned short finderFlags,
    unsigned long creationDate, unsigned long modificationDate,
    SitStillInputStream *resourceInput, SitStillInputStream *dataInput,
    SitStillOutputStream *output, unsigned char *workBuffer,
    unsigned long workBufferLength, unsigned long *outputLength)
{
    SitStillClassicSITStoredFileInput file;

    file.name = name;
    file.nameLength = nameLength;
    file.type = type;
    file.creator = creator;
    file.finderFlags = finderFlags;
    file.creationDate = creationDate;
    file.modificationDate = modificationDate;
    file.resourceInput = resourceInput;
    file.dataInput = dataInput;
    return SitStillClassicSITCreateStoredArchiveStream(
        &file, 1U, output, workBuffer, workBufferLength, outputLength);
}

const char *SitStillClassicSITMethodName(unsigned char method)
{
    switch (method & 15U) {
    case 0U:
        return "Stored";
    case 1U:
        return "RLE90";
    case 2U:
        return "LZC";
    case 3U:
        return "Huffman";
    case 5U:
        return "LZAH";
    case 6U:
        return "Fixed Huffman";
    case 8U:
        return "MW";
    case 13U:
        return "LZ + Huffman";
    case 14U:
        return "Installer Maker";
    case 15U:
        return "Arsenic";
    default:
        return "Unknown";
    }
}
