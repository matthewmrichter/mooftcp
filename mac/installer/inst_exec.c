/* inst_exec.c - the installer executor (Plan 3 Task 10). See inst_exec.h; the op contract is
 * inst_plan.h. Portable: every file system access goes through the InstFs table. */
#include <string.h>
#include "inst_exec.h"

#define T_CDEV INST_FCC('c', 'd', 'e', 'v')
#define T_ZTCP INST_FCC('z', 't', 'c', 'p')
#define T_MTCP INST_FCC('m', 't', 'c', 'p')
#define T_MTPP INST_FCC('m', 't', 'p', 'p')
#define T_TEXT INST_FCC('T', 'E', 'X', 'T')
#define T_VERS INST_FCC('v', 'e', 'r', 's')

#define MAN_CAP 1024            /* a manifest is at most 9 + 8 * 64 bytes; a longer file is not ours */
#define SNAP_CAP 16             /* matching entries per folder (HFS allows one per folded name) */
#define LIST_MAX 32767          /* entries listed per folder at most (bounded loop) */
#define STOP 1                  /* internal: the debug hook stopped the run (not an OSErr) */

static const struct { const char *name; uint32_t type, creator; } trio[3] = {
    { "MacTCP", T_CDEV, T_ZTCP }, { "MacTCP DNR", T_CDEV, T_MTCP }, { "MacTCP Prep", T_MTPP, T_MTCP }
};

/* ---- names ---- */

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b) {     /* the planner's comparison: ASCII folding only */
    while (*a && *b) {
        if (fold((unsigned char)*a) != fold((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int is_trio_file(const InstInfo *i, const char *name) {
    int t;
    if (i->is_dir) return 0;
    for (t = 0; t < 3; t++)
        if (ieq(name, trio[t].name) && i->type == trio[t].type && i->creator == trio[t].creator) return 1;
    return 0;
}

static int tmp_ours(const InstInfo *i) {
    return !i->is_dir && ((i->type == INST_TMP_TYPE && i->creator == INST_TMP_CREATOR) || (i->type == 0 && i->creator == 0));
}

static int moof_ours(const InstInfo *i) {        /* complete or incomplete MoofTCP */
    return !i->is_dir && ((i->type == T_CDEV && i->creator == T_ZTCP) ||
                          (i->type == INST_TMP_TYPE && i->creator == INST_TMP_CREATOR));
}

static void copyname(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n > INST_NAME_MAX) n = INST_NAME_MAX;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- resource forks: the 'vers' 1 of the MoofTCP file and of the payload ---- */

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static unsigned rd16(const uint8_t *p) { return ((unsigned)p[0] << 8) | (unsigned)p[1]; }

/* Reads n bytes at offset off of a resource fork: 0 = all n bytes read. */
typedef int (*RdFn)(void *c, long off, uint8_t *p, long n);

/* Offset (inside the data area) of resource (type, id) in the map at moff..moff+mlen, or -1. Reads the
 * entries one at a time (no buffer for the whole map: any map size, 12 bytes of stack). Bounds-checked. */
static long map_find(RdFn rd, void *c, long moff, long mlen, uint32_t type, int id) {
    uint8_t b[12];
    long tl, nt, i, k, nr, rl, e, r;
    if (mlen < 30 || rd(c, moff + 24, b, 2)) return -1;
    tl = (long)rd16(b);
    if (tl + 2 > mlen || rd(c, moff + tl, b, 2)) return -1;
    nt = (long)((rd16(b) + 1) & 0xFFFF);
    for (i = 0; i < nt; i++) {
        e = tl + 2 + 8 * i;
        if (e + 8 > mlen || rd(c, moff + e, b, 8)) return -1;
        if (rd32(b) != type) continue;
        nr = (long)rd16(b + 4) + 1;
        rl = tl + (long)rd16(b + 6);
        for (k = 0; k < nr; k++) {
            r = rl + 12 * k;
            if (r + 12 > mlen || rd(c, moff + r, b, 12)) return -1;
            if ((int)(int16_t)rd16(b) == id) return ((long)b[5] << 16) | ((long)b[6] << 8) | (long)b[7];
        }
    }
    return -1;
}

/* the packed 'vers' 1 through a reader; 0 = none or damaged */
static uint32_t vers_of(RdFn rd, void *c) {
    uint8_t h[16], v[8];
    long doff, moff, mlen, r;
    if (rd(c, 0, h, 16)) return 0;
    doff = (long)rd32(h); moff = (long)rd32(h + 4); mlen = (long)rd32(h + 12);
    if (doff < 0 || moff < 0 || mlen < 30) return 0;
    r = map_find(rd, c, moff, mlen, T_VERS, 1);
    if (r < 0 || rd(c, doff + r, v, 8) || rd32(v) < 4) return 0;
    return rd32(v + 4);
}

typedef struct { const uint8_t *f; long len; } MemFork;
static int mem_rd(void *c, long off, uint8_t *p, long n) {
    const MemFork *m = (const MemFork *)c;
    if (off < 0 || n < 0 || off > m->len || n > m->len - off) return -1;
    memcpy(p, m->f + off, (size_t)n);
    return 0;
}

uint32_t inst_fork_vers(const uint8_t *f, long len) {
    MemFork m;
    if (!f || len < 16) return 0;
    m.f = f; m.len = len;
    return vers_of(mem_rd, &m);
}

/* the same, read through the table (only ever called for the MoofTCP file) */
typedef struct { const InstFs *fs; long dir; const char *name; } FsFork;
static int fs_rd(void *c, long off, uint8_t *p, long n) {
    const FsFork *f = (const FsFork *)c;
    long got = 0;
    if (off < 0 || n < 0) return -1;
    if (f->fs->read(f->fs->ctx, f->dir, f->name, 1, off, p, n, &got) || got != n) return -1;
    return 0;
}
static uint32_t file_vers(const InstFs *fs, long dir, const char *name) {
    FsFork f;
    f.fs = fs; f.dir = dir; f.name = name;
    return vers_of(fs_rd, &f);
}

/* ---- the snapshot ---- */

static const char *const sys_names[] = { "MacTCP", "MacTCP DNR", "MacTCP Prep", INST_MOOF_NAME, INST_MOOF_TMP,
                                         INST_PREFS_NAME, INST_DISABLED, NULL };
static const char *const dis_names[] = { "MacTCP", "MacTCP DNR", "MacTCP Prep", INST_MANIFEST_NAME,
                                         INST_MANIFEST_TMP, NULL };

static int listed(const char *const *names, const char *n) {
    int i;
    for (i = 0; names[i]; i++) if (ieq(names[i], n)) return 1;
    return 0;
}

/* Lists a folder into e[] keeping only what the planner can look at: the names in `names`, and
 * (keep_other) the first other entry, so the planner still sees "the folder is not empty". Every
 * other entry is irrelevant to it (it matches entries by these names only, and for the Disabled
 * folder counts the rest only against zero). Returns the count or an OSErr / INST_OS_IO on overflow. */
static int snap_dir(const InstFs *fs, long dir, const char *const *names, int keep_other, InstEntry *e) {
    InstEntry t;
    int i, n = 0, other = 0, rc;
    for (i = 1; i <= LIST_MAX; i++) {
        memset(&t, 0, sizeof t);
        rc = fs->list(fs->ctx, dir, i, &t);
        if (rc == INST_OS_FNF) return n;
        if (rc) return rc;
        t.name[31] = 0;
        if (!t.name[0]) continue;
        t.vers = 0;
        if (listed(names, t.name)) {
            if (n >= SNAP_CAP) return INST_OS_IO;
            e[n++] = t;
        } else if (keep_other && !other) {
            if (n >= SNAP_CAP) return INST_OS_IO;
            e[n++] = t; other = 1;
        }
    }
    return INST_OS_IO;
}

typedef struct {
    InstEntry sys[SNAP_CAP], dis[SNAP_CAP];
    int nsys, ndis;
    long disdir;
    InstManifest man;
    int man_ok;               /* 1 = a manifest (X, else X.tmp) decoded into man */
} Snap;

static int read_manifest(const InstFs *fs, long dir, const char *name, InstManifest *m) {
    static uint8_t buf[MAN_CAP + 1];            /* one run at a time: static is fine, keeps the stack small */
    InstInfo i;
    long got;
    if (fs->info(fs->ctx, dir, name, &i) || i.is_dir) return 0;
    if (fs->read(fs->ctx, dir, name, 0, 0, buf, MAN_CAP + 1, &got) || got < 0 || got > MAN_CAP) return 0;
    return inst_manifest_decode((const char *)buf, (size_t)got, m) >= 0;
}

static int take_snapshot(const InstFs *fs, Snap *s, InstResult *res) {
    InstInfo di;
    int i, rc;
    memset(s, 0, sizeof *s);
    rc = snap_dir(fs, fs->sysdir, sys_names, 0, s->sys);
    if (rc < 0) { res->oserr = rc; return -1; }
    s->nsys = rc;
    s->ndis = -1;
    for (i = 0; i < s->nsys; i++) {
        InstEntry *e = &s->sys[i];
        if (ieq(e->name, INST_PREFS_NAME) && !e->is_dir) res->prefs = 1;
        if (ieq(e->name, INST_MOOF_NAME) && !e->is_dir && e->type == T_CDEV && e->creator == T_ZTCP)
            e->vers = file_vers(fs, fs->sysdir, e->name);
        if (ieq(e->name, INST_DISABLED) && e->is_dir) {
            rc = fs->info(fs->ctx, fs->sysdir, e->name, &di);
            if (rc) { res->oserr = rc; return -1; }
            if (!di.is_dir) { res->oserr = INST_OS_IO; return -1; }
            s->disdir = di.dirid;
            rc = snap_dir(fs, di.dirid, dis_names, 1, s->dis);
            if (rc < 0) { res->oserr = rc; return -1; }
            s->ndis = rc;
        }
    }
    if (s->ndis >= 0) {
        s->man_ok = read_manifest(fs, s->disdir, INST_MANIFEST_NAME, &s->man);
        if (!s->man_ok) s->man_ok = read_manifest(fs, s->disdir, INST_MANIFEST_TMP, &s->man);
        if (!s->man_ok) memset(&s->man, 0, sizeof s->man);
    }
    return 0;
}

/* ---- running ops ---- */

/* the debug hook: 1 = stop now (do not make this change) */
static int mut(InstCtx *x) {
    if (x->stop_after > 0 && x->mutations >= x->stop_after) return STOP;
    x->mutations++;
    return 0;
}

/* Every change is flushed at once, so what is on the disk after a power loss is exactly a prefix of
 * the changes (the crash model of inst_plan.h). Observed in Snow before this: a kill after a debug stop
 * lost the unflushed catalog changes of the op in progress (recoverable, but not the state named). */
static int done_call(InstCtx *x, int rc) {
    if (rc == 0) {
        x->committed++;
        rc = x->fs->flush(x->fs->ctx);
    }
    return rc;
}

#define MUT(call) (mut(x) ? STOP : done_call(x, (call)))

/* ":System Folder:a" -> (sysdir, a); ":System Folder:MacTCP (Disabled):a" -> (Disabled's ID, a) */
static int resolve(InstCtx *x, const char *path, long *dir, char *name) {
    const InstFs *fs = x->fs;
    char c[3][32];
    int n = 0;
    size_t l;
    const char *p = path, *q;
    InstInfo i;
    if (*p != ':') return INST_X_PATH;
    p++;
    while (*p) {
        if (n >= 3) return INST_X_PATH;
        q = strchr(p, ':');
        l = q ? (size_t)(q - p) : strlen(p);
        if (l == 0 || l > INST_NAME_MAX) return INST_X_PATH;
        memcpy(c[n], p, l); c[n][l] = 0; n++;
        p += l;
        if (*p == ':') p++;
    }
    if (n < 2 || strcmp(c[0], INST_SYSFOLDER)) return INST_X_PATH;
    if (n == 2) {
        *dir = fs->sysdir;
    } else {
        if (!ieq(c[1], INST_DISABLED)) return INST_X_PATH;
        if (fs->info(fs->ctx, fs->sysdir, c[1], &i) || !i.is_dir) return INST_X_CHANGED;
        *dir = i.dirid;
    }
    strcpy(name, c[n - 1]);
    return 0;
}

/* 0, STOP, an OSErr, CHANGED (the target exists or not, or has a type, other than the snapshot said) or
 * REFUSED (the op names an object this installer may never change) */
#define CHANGED (-1000 - INST_X_CHANGED)
#define REFUSED (-1000 - INST_X_REFUSED)

static int exists(const InstFs *fs, long dir, const char *name, InstInfo *i) {
    int rc = fs->info(fs->ctx, dir, name, i);
    if (rc == INST_OS_FNF) return 0;
    if (rc) return rc;
    return 1;
}

/* ---- the safe-target check: EVERY change goes through it, right before its file system call ----
 * A role names what the object must be. Folder and name are checked first (REFUSED: never touched,
 * whatever a planner bug or a hostile op list says), then existence and type against the role
 * (CHANGED: a stale snapshot). So the executor can change only these objects:
 *   R_MOOF      ":System Folder:MoofTCP"                   cdev/ztcp, or our incomplete MTMP/mtin
 *   R_MOOF_TMP  ":System Folder:MoofTCP.tmp"               our temp types (MTMP/mtin or zero-info)
 *   R_MAN       ":System Folder:MacTCP (Disabled):MoofTCP Manifest"   a file
 *   R_MAN_TMP   ":System Folder:MacTCP (Disabled):MoofTCP Manifest.tmp"   our temp types
 *   R_TRIO      a MacTCP set file (name AND its type/creator) in the System Folder or the Disabled folder:
 *               only ever catalog-moved (g_move), never deleted, renamed, retyped or opened
 *   R_DIS       ":System Folder:MacTCP (Disabled)"         a folder (mkdir / rmdir only) */
enum { R_MOOF, R_MOOF_TMP, R_MAN, R_MAN_TMP, R_TRIO, R_DIS };
enum { M_ANY, M_EXISTS, M_ABSENT };

static int is_trio_name(const char *n) {
    int k;
    for (k = 0; k < 3; k++) if (ieq(n, trio[k].name)) return 1;
    return 0;
}

static int guard(InstCtx *x, int role, long dir, const char *name, int must) {
    const InstFs *fs = x->fs;
    InstInfo i, d;
    long disdir = -1;
    int ex, ok;
    if (fs->info(fs->ctx, fs->sysdir, INST_DISABLED, &d) == 0 && d.is_dir) disdir = d.dirid;
    switch (role) {
    case R_MOOF:     ok = dir == fs->sysdir && ieq(name, INST_MOOF_NAME); break;
    case R_MOOF_TMP: ok = dir == fs->sysdir && ieq(name, INST_MOOF_TMP); break;
    case R_MAN:      ok = disdir >= 0 && dir == disdir && ieq(name, INST_MANIFEST_NAME); break;
    case R_MAN_TMP:  ok = disdir >= 0 && dir == disdir && ieq(name, INST_MANIFEST_TMP); break;
    case R_TRIO:     ok = is_trio_name(name) && (dir == fs->sysdir || (disdir >= 0 && dir == disdir)); break;
    case R_DIS:      ok = dir == fs->sysdir && ieq(name, INST_DISABLED); break;
    default:         ok = 0; break;
    }
    if (!ok) return REFUSED;
    ex = exists(fs, dir, name, &i);
    if (ex < 0) return ex;
    if ((must == M_EXISTS && !ex) || (must == M_ABSENT && ex)) return CHANGED;
    if (!ex) return 0;
    switch (role) {
    case R_MOOF:     ok = moof_ours(&i); break;
    case R_MOOF_TMP:
    case R_MAN_TMP:  ok = tmp_ours(&i); break;
    case R_MAN:      ok = !i.is_dir; break;
    case R_TRIO:     ok = is_trio_file(&i, name); break;
    default:         ok = i.is_dir; break;          /* R_DIS */
    }
    return ok ? 0 : CHANGED;
}

/* the guarded changes: check, then (debug hook) the call */
static int g_del(InstCtx *x, int role, long dir, const char *name) {
    int rc = guard(x, role, dir, name, M_EXISTS);
    return rc ? rc : MUT(x->fs->del(x->fs->ctx, dir, name));
}
static int g_create(InstCtx *x, int role, long dir, const char *name) {
    int rc = guard(x, role, dir, name, M_ABSENT);
    return rc ? rc : MUT(x->fs->create(x->fs->ctx, dir, name));
}
static int g_setinfo(InstCtx *x, int role, long dir, const char *name, uint32_t t, uint32_t c, uint16_t fl) {
    int rc = guard(x, role, dir, name, M_EXISTS);
    return rc ? rc : MUT(x->fs->setinfo(x->fs->ctx, dir, name, t, c, fl));
}
static int g_write(InstCtx *x, int role, long dir, const char *name, int rsrc, const uint8_t *p, long n) {
    int rc = guard(x, role, dir, name, M_EXISTS);
    return rc ? rc : MUT(x->fs->write(x->fs->ctx, dir, name, rsrc, p, n));
}
static int g_rename(InstCtx *x, int rfrom, int rto, long dir, const char *from, const char *to) {
    int rc = guard(x, rfrom, dir, from, M_EXISTS);
    if (!rc) rc = guard(x, rto, dir, to, M_ABSENT);
    return rc ? rc : MUT(x->fs->rename(x->fs->ctx, dir, from, to));
}
static int g_move(InstCtx *x, long fromdir, const char *name, long todir, const char *toname) {
    int rc;
    if (strcmp(name, toname) || fromdir == todir) return REFUSED;
    rc = guard(x, R_TRIO, fromdir, name, M_EXISTS);
    if (!rc) rc = guard(x, R_TRIO, todir, toname, M_ABSENT);
    return rc ? rc : MUT(x->fs->move(x->fs->ctx, fromdir, name, todir));
}

typedef struct {
    const uint8_t *data, *rsrc;
    long dlen, rlen;
    uint32_t type, creator;
    uint16_t flags;
    int manifest;
} WSpec;

/* The 5 steps of inst_plan.h. x->res->step tracks the step. rx/rt: the roles of X and of its temp. */
static int write_file(InstCtx *x, long dir, const char *X, const char *T, const WSpec *w) {
    const InstFs *fs = x->fs;
    InstInfo i;
    InstManifest tm;
    int rc, ex, rx = w->manifest ? R_MAN : R_MOOF, rt = w->manifest ? R_MAN_TMP : R_MOOF_TMP;
    x->res->step = INST_W_PROMOTE;
    if (w->manifest) {                              /* 1. promote a decodable X.tmp when X is absent */
        ex = exists(fs, dir, X, &i);
        if (ex < 0) return ex;
        if (!ex && read_manifest(fs, dir, T, &tm))
            if ((rc = g_rename(x, rt, rx, dir, T, X)) != 0) return rc;
    }
    x->res->step = INST_W_TEMP;                     /* 2. a fresh, non-loadable temp */
    ex = exists(fs, dir, T, &i);
    if (ex < 0) return ex;
    if (ex && (rc = g_del(x, rt, dir, T)) != 0) return rc;
    if ((rc = g_create(x, rt, dir, T)) != 0) return rc;
    if ((rc = g_setinfo(x, rt, dir, T, INST_TMP_TYPE, INST_TMP_CREATOR, w->flags)) != 0) return rc;
    if (w->dlen > 0 || w->manifest)
        if ((rc = g_write(x, rt, dir, T, 0, w->data, w->dlen)) != 0) return rc;
    if (w->rlen > 0)
        if ((rc = g_write(x, rt, dir, T, 1, w->rsrc, w->rlen)) != 0) return rc;
    if ((rc = fs->flush(fs->ctx)) != 0) return rc;
    x->res->step = INST_W_DELETE;                   /* 3. the old X goes */
    ex = exists(fs, dir, X, &i);
    if (ex < 0) return ex;
    if (ex && (rc = g_del(x, rx, dir, X)) != 0) return rc;
    x->res->step = INST_W_RENAME;                   /* 4. */
    if ((rc = g_rename(x, rt, rx, dir, T, X)) != 0) return rc;
    x->res->step = INST_W_RETYPE;                   /* 5. only now loadable / final */
    if ((rc = g_setinfo(x, rx, dir, X, w->type, w->creator, w->flags)) != 0) return rc;
    return fs->flush(fs->ctx);
}

static int run1(InstCtx *x, const InstOp *o, const InstManifest *man, const InstPayload *pl) {
    const InstFs *fs = x->fs;
    char name[32], name2[32];
    long dir, dir2;
    WSpec w;
    int rc;
    static char mbuf[MAN_CAP];
    const char *path = (o->kind == INST_MKDIR || o->kind == INST_WRITE_MANIFEST || o->kind == INST_WRITE_MOOF) ? o->to : o->from;

    rc = resolve(x, path, &dir, name);
    if (rc) return -1000 - rc;
    copyname(x->res->op_name, name);
    switch (o->kind) {
    case INST_MKDIR:
        rc = guard(x, R_DIS, dir, name, M_ABSENT);
        return rc ? rc : MUT(fs->mkdir(fs->ctx, dir, name));
    case INST_RMDIR:
        return g_del(x, R_DIS, dir, name);              /* PBHDelete: refuses a folder that is not empty */
    case INST_MOVE:
        rc = resolve(x, o->to, &dir2, name2);
        if (rc) return -1000 - rc;
        return g_move(x, dir, name, dir2, name2);       /* the planner only ever moves the MacTCP set */
    case INST_DELETE_MOOF:
        return g_del(x, R_MOOF, dir, name);
    case INST_DELETE_MANIFEST:
        return g_del(x, R_MAN, dir, name);
    case INST_DELETE_TEMP:
        return g_del(x, ieq(name, INST_MOOF_TMP) ? R_MOOF_TMP : R_MAN_TMP, dir, name);
    case INST_WRITE_MANIFEST:
        if (!man) return -1000 - INST_X_PATH;
        rc = guard(x, R_MAN, dir, name, M_ANY);       /* the name and folder before anything else */
        if (rc) return rc;
        rc = inst_manifest_encode(man, mbuf, sizeof mbuf);
        if (rc <= 0) return -1000 - INST_X_PLAN;
        memset(&w, 0, sizeof w);
        w.data = (const uint8_t *)mbuf; w.dlen = rc; w.type = T_TEXT; w.creator = T_ZTCP; w.manifest = 1;
        return write_file(x, dir, name, INST_MANIFEST_TMP, &w);
    case INST_WRITE_MOOF:
        if (!pl) return -1000 - INST_X_PATH;
        rc = guard(x, R_MOOF, dir, name, M_ANY);
        if (rc) return rc;
        memset(&w, 0, sizeof w);
        w.data = pl->data; w.dlen = pl->dlen; w.rsrc = pl->rsrc; w.rlen = pl->rlen;
        w.type = pl->type; w.creator = pl->creator; w.flags = pl->flags;
        return write_file(x, dir, name, INST_MOOF_TMP, &w);
    }
    return -1000 - INST_X_PATH;
}

int inst_run(InstCtx *x, const InstOp *ops, int n, const InstManifest *man, const InstPayload *pl) {
    InstResult *r = x->res;
    int k, rc;
    r->nops = n; r->done = 0; r->op_index = -1; r->status = INST_X_OK;
    for (k = 0; k < n; k++) {
        r->step = 0; r->op_name[0] = 0;
        rc = run1(x, &ops[k], man, pl);
        if (rc == 0) rc = x->fs->flush(x->fs->ctx);
        r->mutations = x->mutations;
        r->committed = x->committed;
        if (rc == 0) { r->done = k + 1; continue; }
        r->op_index = k;
        r->op_kind = ops[k].kind;
        if (rc == STOP) r->status = INST_X_STOPPED;
        else if (rc <= -1000) r->status = -1000 - rc;
        else { r->status = INST_X_OSERR; r->oserr = rc; }
        return r->status;
    }
    return INST_X_OK;
}

static void res_init(InstResult *r, int remove) {
    memset(r, 0, sizeof *r);
    r->remove = remove;
    r->op_index = -1;
}

static int count_moves(const InstOp *ops, int n) {
    int k, m = 0;
    for (k = 0; k < n; k++) m += ops[k].kind == INST_MOVE;
    return m;
}

/* nothing is done on a locked volume, nor without room for a temp copy beside the old file */
static int pre_checks(const InstFs *fs, const InstOp *ops, int n, const InstPayload *pl, InstResult *r) {
    long freeb = 0, need = 0;
    int locked = 0, k, rc;
    rc = fs->volinfo(fs->ctx, &freeb, &locked);
    if (rc) { r->oserr = rc; return INST_X_SNAPSHOT; }
    if (locked) return INST_X_VOLLOCKED;
    for (k = 0; k < n; k++) {
        if (ops[k].kind == INST_WRITE_MOOF && pl) need += pl->rlen + pl->dlen + 16384L;
        if (ops[k].kind == INST_WRITE_MANIFEST || ops[k].kind == INST_MKDIR) need += 4096L;
    }
    if (need > freeb) return INST_X_NOSPACE;
    return INST_X_OK;
}

int inst_do_install(const InstFs *fs, const InstPayload *pl, long stop_after, InstResult *res) {
    static Snap s;
    static InstOp ops[INST_OPS_MAX];
    InstManifest man;
    InstCtx x;
    uint32_t v;
    int n;
    res_init(res, 0);
    v = pl ? inst_fork_vers(pl->rsrc, pl->rlen) : 0;
    if (!pl || !pl->rsrc || pl->rlen <= 0 || v == 0 || pl->dlen < 0 || (pl->dlen > 0 && !pl->data))
        return res->status = INST_X_PAYLOAD;
    if (take_snapshot(fs, &s, res) < 0) return res->status = INST_X_SNAPSHOT;
    n = inst_plan_install(s.sys, s.nsys, s.dis, s.ndis, v, s.man_ok ? &s.man : NULL, ops, INST_OPS_MAX,
                          &res->msg, res->blocker, &man);
    res->plan_rc = n;
    if (n < 0) return res->status = INST_X_PLAN;
    res->nops = n;
    res->moves = count_moves(ops, n);
    if (n == 0) return res->status = INST_X_OK;
    if ((res->status = pre_checks(fs, ops, n, pl, res)) != INST_X_OK) return res->status;
    x.fs = fs; x.stop_after = stop_after; x.mutations = 0; x.committed = 0; x.res = res;
    return inst_run(&x, ops, n, &man, pl);
}

int inst_do_remove(const InstFs *fs, long stop_after, InstResult *res) {
    static Snap s;
    static InstOp ops[INST_OPS_MAX];
    InstCtx x;
    int n = INST_E_MANIFEST;
    res_init(res, 1);
    if (take_snapshot(fs, &s, res) < 0) return res->status = INST_X_SNAPSHOT;
    if (s.man_ok)
        n = inst_plan_remove(s.sys, s.nsys, s.dis, s.ndis, &s.man, ops, INST_OPS_MAX, &res->msg, res->blocker);
    if (n == INST_E_MANIFEST) {                     /* absent, unreadable or refused: plan without it */
        res->man_retried = s.man_ok;
        n = inst_plan_remove(s.sys, s.nsys, s.dis, s.ndis, NULL, ops, INST_OPS_MAX, &res->msg, res->blocker);
    }
    res->plan_rc = n;
    if (n < 0) return res->status = INST_X_PLAN;
    res->nops = n;
    res->moves = count_moves(ops, n);
    if (n == 0) return res->status = INST_X_OK;
    if ((res->status = pre_checks(fs, ops, n, NULL, res)) != INST_X_OK) return res->status;
    x.fs = fs; x.stop_after = stop_after; x.mutations = 0; x.committed = 0; x.res = res;
    return inst_run(&x, ops, n, NULL, NULL);
}

/* ---- messages ---- */

typedef struct { char *b; int cap, len; } Txt;

static void put(Txt *t, const char *s) {
    while (*s && t->len < t->cap - 1) t->b[t->len++] = *s++;
    t->b[t->len] = 0;
}

static void putn(Txt *t, long v) {
    char d[12];
    int n = 0;
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
    if (v < 0) put(t, "-");
    do { d[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u && n < 11);
    while (n > 0) { char c[2]; c[0] = d[--n]; c[1] = 0; put(t, c); }
}

static void putq(Txt *t, const char *name) { put(t, "\""); put(t, name); put(t, "\""); }

static const char *what(InstOpKind k) {
    switch (k) {
    case INST_MKDIR: return "creating the folder";
    case INST_RMDIR: return "removing the folder";
    case INST_MOVE: return "moving";
    case INST_WRITE_MANIFEST: case INST_WRITE_MOOF: return "writing";
    default: return "deleting";
    }
}

int inst_message(const InstResult *r, const char *vers, char *buf, int cap) {
    Txt t;
    t.b = buf; t.cap = cap; t.len = 0;
    if (cap <= 0) return 0;
    buf[0] = 0;
    switch (r->status) {
    case INST_X_OK:
        break;
    case INST_X_PLAN:
        put(&t, "Internal error (plan "); putn(&t, r->plan_rc); put(&t, "). Nothing was changed.");
        return 0;
    case INST_X_SNAPSHOT:
        put(&t, "The System Folder could not be read (error "); putn(&t, r->oserr); put(&t, "). Nothing was changed.");
        return 0;
    case INST_X_NOSPACE:
        put(&t, "There is not enough free space on the startup disk (MoofTCP needs about 150K). Nothing was changed.");
        return 0;
    case INST_X_VOLLOCKED:
        put(&t, "The startup disk is locked. Nothing was changed.");
        return 0;
    case INST_X_PAYLOAD:
        put(&t, "This copy of the installer is damaged: the MoofTCP file inside it is missing. Nothing was changed.");
        return 0;
    default:                                         /* OSERR, CHANGED, PATH, STOPPED: op r->op_index */
        put(&t, "Step "); putn(&t, r->op_index + 1); put(&t, " of "); putn(&t, r->nops); put(&t, " (");
        put(&t, what(r->op_kind)); put(&t, " "); putq(&t, r->op_name); put(&t, ") stopped: ");
        if (r->status == INST_X_STOPPED) {
            put(&t, "debug stop after "); putn(&t, r->mutations); put(&t, " changes.");
        } else if (r->status == INST_X_CHANGED) {
            put(&t, "it changed while the installer was working.");
        } else if (r->status == INST_X_REFUSED) {
            put(&t, "refused: it is not a file this installer may change (internal error).");
        } else if (r->status == INST_X_PATH) {
            put(&t, "internal error (path).");
        } else if (r->oserr == INST_OS_FBSY && r->op_kind == INST_RMDIR) {
            put(&t, "the folder is not empty, so it was left in place. Everything else is done.");
            return 0;
        } else if (r->oserr == INST_OS_FBSY || r->oserr == INST_OS_OPWR || r->oserr == INST_OS_PERM) {
            put(&t, "the file is in use. Close the Control Panel and quit other programs, then run the installer again.");
        } else if (r->oserr == INST_OS_FLCKD) {
            put(&t, "the file is locked. Unlock it in the Finder (File > Get Info) and run the installer again.");
        } else if (r->oserr == INST_OS_VLCKD || r->oserr == INST_OS_WPR) {
            put(&t, "the disk is locked.");
        } else if (r->oserr == INST_OS_DSKFUL) {
            put(&t, "the disk is full.");
        } else {
            put(&t, "error "); putn(&t, r->oserr); put(&t, ".");
        }
        put(&t, r->committed == 0 ? " Nothing was changed."
                                                 : " Run Install or Remove again to finish or undo it.");
        return 0;
    }
    switch (r->msg) {
    case INST_ALREADY_INSTALLED:
        put(&t, "MoofTCP is already installed (version "); put(&t, vers); put(&t, " or newer). Nothing was changed.");
        return 0;
    case INST_NOTHING_TO_REMOVE:
        put(&t, "MoofTCP is not installed, nothing to remove.");
        return 0;
    case INST_BLOCKED_LOCKED:
        put(&t, "The file "); putq(&t, r->blocker);
        put(&t, " is locked. Unlock it in the Finder (File > Get Info) and run the installer again. Nothing was changed.");
        return 0;
    case INST_BLOCKED_COLLISION:
        if (!is_trio_name(r->blocker)) {
            put(&t, "An item named "); putq(&t, r->blocker);
            put(&t, " is in the way; it was not made by this installer. Remove or rename it, then run the installer again. Nothing was changed.");
        } else if (r->remove) {
            put(&t, "There is a new "); putq(&t, r->blocker);
            put(&t, " in the System Folder. Remove it first so the original can be put back. Nothing was changed.");
        } else {
            put(&t, "A file named "); putq(&t, r->blocker);
            put(&t, " is already in the folder MacTCP (Disabled). Remove or rename it, then run the installer again. Nothing was changed.");
        }
        return 0;
    case INST_INSTALLED_PARKED_MACTCP:
    case INST_INSTALLED_NO_MACTCP:
    case INST_UPGRADED:
        if (r->msg == INST_UPGRADED) {
            put(&t, "MoofTCP was updated to "); put(&t, vers);
            put(&t, r->prefs ? "; your settings were kept." : ".");
        } else {
            put(&t, "MoofTCP "); put(&t, vers); put(&t, " is installed.");
        }
        if (r->moves)
            put(&t, " MacTCP was found and moved into the folder \"MacTCP (Disabled)\" in the System Folder; Remove puts it back.");
        else if (r->msg == INST_INSTALLED_NO_MACTCP)
            put(&t, " No MacTCP was found, so nothing was moved.");
        put(&t, r->prefs ? " Restart to use MoofTCP."
                                                    : " After restarting, set the address in Control Panel > MoofTCP.");
        return 1;
    case INST_REMOVED_RESTORED:
        put(&t, "MoofTCP was removed and MacTCP was put back in the System Folder.");
        if (r->prefs) put(&t, " The file \"MoofTCP Prefs\" (your settings) was kept.");
        put(&t, " Restart to use MacTCP.");
        return 1;
    case INST_REMOVED_NO_MACTCP:
        put(&t, "MoofTCP was removed. No MacTCP parked by this installer was found, so none was put back.");
        if (r->prefs) put(&t, " The file \"MoofTCP Prefs\" (your settings) was kept.");
        put(&t, " Restart to finish.");
        return 1;
    }
    return 0;
}
