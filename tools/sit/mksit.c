/* mksit: write a StuffIt 1.5.1-compatible (Classic SIT, stored, no compression) archive from
 * MacBinary files. Every input becomes one entry with its name, type, creator, Finder flags,
 * dates and both forks.
 *
 *   usage: mksit out.sit file1.bin file2.bin ...
 *
 * The archive writer is SitStill's (MIT, see LICENSE and NOTICE in this directory); this driver
 * is MoofTCP's. Exit 0 only when every input parsed, the archive validated by the writer's own
 * parameter checks and the output file was written completely. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sitstill/classic_sit.h"
#include "sitstill/stream.h"

#define MAX_FILES 64

typedef struct {
    unsigned char *buf;
    unsigned long len;
    SitStillMemoryStream dataMem, rsrcMem;
    SitStillInputStream dataIn, rsrcIn;
} Input;

static unsigned long be32(const unsigned char *b)
{
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
           ((unsigned long)b[2] << 8) | (unsigned long)b[3];
}

static int load(const char *path, Input *in, SitStillClassicSITStoredFileInput *f)
{
    FILE *fp = fopen(path, "rb");
    long n;
    unsigned long dl, rl, roff;
    const unsigned char *h;

    if (fp == 0 || fseek(fp, 0L, SEEK_END) != 0 || (n = ftell(fp)) < 128L ||
        fseek(fp, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "mksit: %s: cannot read\n", path);
        if (fp) fclose(fp);
        return 0;
    }
    in->buf = (unsigned char *)malloc((size_t)n);
    if (in->buf == 0 || fread(in->buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "mksit: %s: cannot read\n", path);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    in->len = (unsigned long)n;
    h = in->buf;
    dl = be32(h + 83);
    rl = be32(h + 87);
    roff = 128UL + ((dl + 127UL) / 128UL) * 128UL;
    if (h[0] != 0 || h[74] != 0 || h[1] == 0 || h[1] > 31 ||
        128UL + dl > in->len || roff + rl > in->len) {
        fprintf(stderr, "mksit: %s: not a MacBinary file (or name over 31 bytes)\n", path);
        return 0;
    }
    SitStillMemoryStreamInit(&in->dataMem, &in->dataIn, h + 128, dl);
    SitStillMemoryStreamInit(&in->rsrcMem, &in->rsrcIn, h + roff, rl);
    f->name = h + 2;
    f->nameLength = h[1];
    f->type = be32(h + 65);
    f->creator = be32(h + 69);
    f->finderFlags = (unsigned short)(((unsigned short)h[73] << 8) | h[101]);
    f->creationDate = be32(h + 91);
    f->modificationDate = be32(h + 95);
    f->dataInput = &in->dataIn;
    f->resourceInput = &in->rsrcIn;
    return 1;
}

int main(int argc, char **argv)
{
    static Input in[MAX_FILES];
    static SitStillClassicSITStoredFileInput files[MAX_FILES];
    int nfiles = argc - 2, i;
    unsigned long total = 0, outLen = 0, cap;
    unsigned char *out, work[4096];
    SitStillMemoryOutputStream mem;
    SitStillOutputStream os;
    SitStillClassicSITResult r;
    FILE *fp;

    if (argc < 3 || nfiles > MAX_FILES) {
        fprintf(stderr, "usage: %s out.sit file1.bin [file2.bin ...] (at most %d)\n", argv[0], MAX_FILES);
        return 2;
    }
    for (i = 0; i < nfiles; i++) {
        if (!load(argv[i + 2], &in[i], &files[i])) return 1;
        total += in[i].len;
    }
    cap = 2UL * total + 4096UL * (unsigned long)(nfiles + 1);
    out = (unsigned char *)malloc((size_t)cap);
    if (out == 0) return 1;
    SitStillMemoryOutputStreamInit(&mem, &os, out, cap);
    r = SitStillClassicSITCreateStoredArchiveStream(files, (unsigned short)nfiles, &os, work,
                                                    (unsigned long)sizeof work, &outLen);
    if (r != kSitStillClassicSITOK) {
        fprintf(stderr, "mksit: archive writer failed (%d)\n", (int)r);
        return 1;
    }
    fp = fopen(argv[1], "wb");
    if (fp == 0 || fwrite(out, 1, (size_t)outLen, fp) != (size_t)outLen || fclose(fp) != 0) {
        fprintf(stderr, "mksit: %s: cannot write\n", argv[1]);
        return 1;
    }
    for (i = 0; i < nfiles; i++) free(in[i].buf);
    free(out);
    return 0;
}
