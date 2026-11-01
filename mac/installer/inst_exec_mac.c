/* inst_exec_mac.c - the real InstFs table (Plan 3 Task 10): HFS parameter-block calls only, the set
 * System 6.0.8 has (no FSSpec, FindFolder or FSp* calls): PBGetCatInfo, PBDirCreate, PBHCreate,
 * PBHGetFInfo/PBHSetFInfo, PBHOpen/PBHOpenRF + PBRead/PBWrite/PBSetEOF/PBClose, PBHDelete, PBHRename,
 * PBCatMove, PBFlushVol, PBHGetVInfo. Every call names its folder by directory ID; the volume is
 * the startup disk.
 *
 * Finding the System Folder (ruling R2): the boot volume is the one BootDrive (low memory 0x210)
 * refers to: the working directory of the System Folder that the system itself, the MoofTCP driver's
 * prefs loader (mac/glue/mt_cfg.c) and the control panel use, so the installer writes where they
 * read. Its directory ID is taken from the volume's blessed folder (PBHGetVInfo ioVFndrInfo[0],
 * Pascal's ioVFndrInfo[1]): the folder the system booted from and whose cdevs INIT 31 runs.
 * Calls pass the returned volume reference and an explicit directory ID (a nonzero ioDirID wins
 * over the working directory, so either kind of reference is right). */
#include <Files.h>
#include <string.h>
#include "inst_exec.h"
#include "inst_exec_mac.h"

#pragma GCC diagnostic ignored "-Warray-bounds"
#define LM_BOOTDRIVE (*(volatile short *)0x210)

static short vref_of(void *c) { return ((InstMacCtx *)c)->vref; }

static void pstr(unsigned char *p, const char *c) {
    size_t n = strlen(c);
    if (n > 31) n = 31;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void zero(void *p, long n) {
    char *c = (char *)p;
    while (n-- > 0) *c++ = 0;
}

static int cat(void *c, long dir, const char *name, int index, unsigned char *nm, CInfoPBRec *pb) {
    zero(pb, sizeof *pb);
    if (name) pstr(nm, name); else nm[0] = 0;
    pb->hFileInfo.ioNamePtr = nm;
    pb->hFileInfo.ioVRefNum = vref_of(c);
    pb->hFileInfo.ioFDirIndex = (short)index;
    pb->hFileInfo.ioDirID = dir;
    return PBGetCatInfoSync(pb);
}

static int m_list(void *c, long dir, int index, InstEntry *e) {
    CInfoPBRec pb;
    unsigned char nm[64];
    int rc = cat(c, dir, NULL, index, nm, &pb), n;
    if (rc) return rc;
    n = nm[0] > 31 ? 31 : nm[0];
    memcpy(e->name, nm + 1, (size_t)n);
    e->name[n] = 0;
    e->is_dir = (pb.hFileInfo.ioFlAttrib & 0x10) != 0;
    e->locked = !e->is_dir && (pb.hFileInfo.ioFlAttrib & 0x01) != 0;
    e->type = e->is_dir ? 0 : (uint32_t)pb.hFileInfo.ioFlFndrInfo.fdType;
    e->creator = e->is_dir ? 0 : (uint32_t)pb.hFileInfo.ioFlFndrInfo.fdCreator;
    e->vers = 0;
    return 0;
}

static int m_info(void *c, long dir, const char *name, InstInfo *i) {
    CInfoPBRec pb;
    unsigned char nm[64];
    int rc = cat(c, dir, name, 0, nm, &pb);
    if (rc) return rc;
    zero(i, sizeof *i);
    i->is_dir = (pb.hFileInfo.ioFlAttrib & 0x10) != 0;
    if (i->is_dir) {
        i->dirid = pb.dirInfo.ioDrDirID;
    } else {
        i->locked = (pb.hFileInfo.ioFlAttrib & 0x01) != 0;
        i->type = (uint32_t)pb.hFileInfo.ioFlFndrInfo.fdType;
        i->creator = (uint32_t)pb.hFileInfo.ioFlFndrInfo.fdCreator;
        i->flags = pb.hFileInfo.ioFlFndrInfo.fdFlags;
    }
    return 0;
}

static void hfile(void *c, HParamBlockRec *pb, unsigned char *nm, long dir, const char *name) {
    zero(pb, sizeof *pb);
    pstr(nm, name);
    pb->fileParam.ioNamePtr = nm;
    pb->fileParam.ioVRefNum = vref_of(c);
    pb->fileParam.ioDirID = dir;
}

static int m_mkdir(void *c, long parent, const char *name) {
    HParamBlockRec pb;
    unsigned char nm[64];
    hfile(c, &pb, nm, parent, name);
    return PBDirCreateSync(&pb);
}

static int m_create(void *c, long dir, const char *name) {
    HParamBlockRec pb;
    unsigned char nm[64];
    hfile(c, &pb, nm, dir, name);
    return PBHCreateSync(&pb);
}

static int m_setinfo(void *c, long dir, const char *name, uint32_t type, uint32_t creator, uint16_t flags) {
    HParamBlockRec pb;
    unsigned char nm[64];
    int rc;
    hfile(c, &pb, nm, dir, name);
    rc = PBHGetFInfoSync(&pb);                  /* keep location, folder field and dates */
    if (rc) return rc;
    pb.fileParam.ioFlFndrInfo.fdType = (OSType)type;
    pb.fileParam.ioFlFndrInfo.fdCreator = (OSType)creator;
    pb.fileParam.ioFlFndrInfo.fdFlags = flags;
    pb.fileParam.ioDirID = dir;                 /* PBHGetFInfo returned the file number here */
    pb.fileParam.ioNamePtr = nm;
    pb.fileParam.ioFDirIndex = 0;
    return PBHSetFInfoSync(&pb);
}

static int m_open(void *c, long dir, const char *name, int rsrc, char perm, short *ref) {
    HParamBlockRec pb;
    unsigned char nm[64];
    int rc;
    hfile(c, &pb, nm, dir, name);
    pb.ioParam.ioPermssn = perm;
    pb.ioParam.ioMisc = NULL;
    rc = rsrc ? PBHOpenRFSync(&pb) : PBHOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return rc;
}

static int m_close(short ref) {
    ParamBlockRec pb;
    zero(&pb, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    return PBCloseSync(&pb);
}

static int m_write(void *c, long dir, const char *name, int rsrc, const uint8_t *p, long len) {
    ParamBlockRec pb;
    short ref;
    int rc, rc2;
    rc = m_open(c, dir, name, rsrc, fsRdWrPerm, &ref);
    if (rc) return rc;
    zero(&pb, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)p;
    pb.ioParam.ioReqCount = len;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = 0;
    rc = len > 0 ? PBWriteSync(&pb) : 0;
    if (!rc && len > 0 && pb.ioParam.ioActCount != len) rc = INST_OS_IO;
    if (!rc) {
        zero(&pb, sizeof pb);
        pb.ioParam.ioRefNum = ref;
        pb.ioParam.ioMisc = (Ptr)len;           /* the new logical EOF */
        rc = PBSetEOFSync(&pb);
    }
    rc2 = m_close(ref);
    return rc ? rc : rc2;
}

static int m_read(void *c, long dir, const char *name, int rsrc, long off, uint8_t *p, long len, long *got) {
    ParamBlockRec pb;
    short ref;
    int rc;
    *got = 0;
    rc = m_open(c, dir, name, rsrc, fsRdPerm, &ref);
    if (rc) return rc;
    zero(&pb, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)p;
    pb.ioParam.ioReqCount = len;
    pb.ioParam.ioPosMode = fsFromStart;
    pb.ioParam.ioPosOffset = off;
    rc = len > 0 ? PBReadSync(&pb) : 0;
    if (rc == eofErr) rc = 0;                   /* a short file: what was there */
    *got = pb.ioParam.ioActCount;
    (void)m_close(ref);
    return rc;
}

static int m_del(void *c, long dir, const char *name) {
    HParamBlockRec pb;
    unsigned char nm[64];
    hfile(c, &pb, nm, dir, name);
    return PBHDeleteSync(&pb);
}

static int m_rename(void *c, long dir, const char *from, const char *to) {
    HParamBlockRec pb;
    unsigned char nm[64], nn[64];
    hfile(c, &pb, nm, dir, from);
    pstr(nn, to);
    pb.ioParam.ioMisc = (Ptr)nn;
    pb.fileParam.ioDirID = dir;
    return PBHRenameSync(&pb);
}

static int m_move(void *c, long fromdir, const char *name, long todir) {
    CMovePBRec pb;
    unsigned char nm[64];
    zero(&pb, sizeof pb);
    pstr(nm, name);
    pb.ioNamePtr = nm;
    pb.ioVRefNum = vref_of(c);
    pb.ioDirID = fromdir;
    pb.ioNewName = NULL;                        /* the destination is ioNewDirID itself */
    pb.ioNewDirID = todir;
    return PBCatMoveSync(&pb);
}

static int m_flush(void *c) {
    ParamBlockRec pb;
    zero(&pb, sizeof pb);
    pb.ioParam.ioVRefNum = vref_of(c);
    return PBFlushVolSync(&pb);
}

static int vinfo(short vref, HParamBlockRec *pb) {
    zero(pb, sizeof *pb);
    pb->volumeParam.ioVRefNum = vref;
    pb->volumeParam.ioVolIndex = 0;
    return PBHGetVInfoSync(pb);
}

static int m_volinfo(void *c, long *free_bytes, int *locked) {
    HParamBlockRec pb;
    int rc = vinfo(vref_of(c), &pb);
    if (rc) return rc;
    *free_bytes = (long)(uint16_t)pb.volumeParam.ioVFrBlk * pb.volumeParam.ioVAlBlkSiz;
    *locked = ((uint16_t)pb.volumeParam.ioVAtrb & 0x8080u) != 0;
    return 0;
}

int inst_mac_fs(InstFs *fs, InstMacCtx *c) {
    HParamBlockRec pb;
    int rc;
    zero(fs, sizeof *fs);
    zero(c, sizeof *c);
    rc = vinfo(LM_BOOTDRIVE, &pb);
    if (rc) return rc;
    if ((uint16_t)pb.volumeParam.ioVSigWord != 0x4244u) return INST_MAC_NOT_HFS;     /* 'BD': HFS */
    if (pb.volumeParam.ioVFndrInfo[0] == 0) return INST_MAC_NO_SYSFOLDER;
    c->vref = pb.volumeParam.ioVRefNum;
    fs->ctx = c;
    fs->sysdir = pb.volumeParam.ioVFndrInfo[0];
    fs->list = m_list; fs->info = m_info; fs->mkdir = m_mkdir; fs->create = m_create;
    fs->setinfo = m_setinfo; fs->write = m_write; fs->read = m_read; fs->del = m_del;
    fs->rename = m_rename; fs->move = m_move; fs->flush = m_flush; fs->volinfo = m_volinfo;
    return 0;
}
