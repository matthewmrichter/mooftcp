/* test_inst_exec.c - Plan 3 Task 10: the installer executor (mac/installer/inst_exec.[ch]) over a
 * fake HFS volume (Review Focus 1). The fake keeps everything a MacBinary copy would show (forks,
 * type, creator, Finder flags, location, folder field, dates, lock) so "byte-identical after
 * Install then Remove" is checked on all of it; catalog moves keep the record, as HFS does.
 * Failures are injected at every file system change (and as partial writes), the debug hook stops
 * after every change count, and from every resulting state a fresh Remove (and a re-run Install
 * then Remove) must restore the original. The fake fails the test if a MacTCP file's fork is
 * ever opened. */
#include "t.h"
#include "inst_exec.h"

#define CDEV INST_FCC('c', 'd', 'e', 'v')
#define ZTCP INST_FCC('z', 't', 'c', 'p')
#define MTCP INST_FCC('m', 't', 'c', 'p')
#define MTPP INST_FCC('m', 't', 'p', 'p')
#define PREF INST_FCC('p', 'r', 'e', 'f')
#define TEXT INST_FCC('T', 'E', 'X', 'T')
#define MTMP INST_TMP_TYPE
#define MTIN INST_TMP_CREATOR

/* ---- the fake volume ---- */
enum { NN = 48, DCAP = 1100, RCAP = 5200, ROOT = 2, SYSD = 16 };

typedef struct {
    int used, is_dir, locked, busy;
    long parent, id;
    char name[32];
    uint32_t type, creator;
    uint16_t flags, fldr;
    int16_t lv, lh;
    uint32_t crdat, mddat;
    long dl, rl;
    uint8_t d[DCAP], r[RCAP];
} Node;

typedef struct {
    Node n[NN];
    long next_id, free_bytes;
    int vol_locked;
    long mut, flushes;
    long fail_at;            /* the fail_at-th change returns fail_err (0 = never) */
    int fail_err, fail_partial;
    int trio_opened;         /* a MacTCP file's fork was opened: always a failure */
    uint32_t clock;
    int list_err;
} Fake;

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ieq(const char *a, const char *b) {
    while (*a && *b) { if (fold((unsigned char)*a) != fold((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}
static int fk_find(Fake *f, long dir, const char *name) {
    int i;
    for (i = 0; i < NN; i++) if (f->n[i].used && f->n[i].parent == dir && ieq(f->n[i].name, name)) return i;
    return -1;
}
static int fk_byid(Fake *f, long id) {
    int i;
    for (i = 0; i < NN; i++) if (f->n[i].used && f->n[i].is_dir && f->n[i].id == id) return i;
    return -1;
}
static int dir_ok(Fake *f, long dir) { return dir == ROOT || fk_byid(f, dir) >= 0; }
static int fk_alloc(Fake *f) {
    int i;
    for (i = 0; i < NN; i++) if (!f->n[i].used) { memset(&f->n[i], 0, sizeof f->n[i]); f->n[i].used = 1; return i; }
    T_CHECK(0);
    return -1;
}
static int is_trio_name(const char *n) { return ieq(n, "MacTCP") || ieq(n, "MacTCP DNR") || ieq(n, "MacTCP Prep"); }
static int chg(Fake *f) {          /* a change: 0 = go, else the injected error */
    f->mut++;
    if (f->fail_at && f->mut == f->fail_at) return f->fail_err;
    return 0;
}

static int f_list(void *c, long dir, int index, InstEntry *e) {
    Fake *f = c; int i, k = 0;
    if (f->list_err) return f->list_err;
    if (!dir_ok(f, dir)) return INST_OS_DIRNF;
    for (i = 0; i < NN; i++) {
        Node *n = &f->n[i];
        if (!n->used || n->parent != dir) continue;
        if (++k == index) {
            strcpy(e->name, n->name); e->type = n->type; e->creator = n->creator;
            e->locked = (uint8_t)n->locked; e->is_dir = (uint8_t)n->is_dir; e->vers = 0;
            return 0;
        }
    }
    return INST_OS_FNF;
}
static int f_info(void *c, long dir, const char *name, InstInfo *in) {
    Fake *f = c; int i;
    if (!dir_ok(f, dir)) return INST_OS_DIRNF;
    i = fk_find(f, dir, name);
    if (i < 0) return INST_OS_FNF;
    memset(in, 0, sizeof *in);
    in->type = f->n[i].type; in->creator = f->n[i].creator; in->flags = f->n[i].flags;
    in->locked = (uint8_t)f->n[i].locked; in->is_dir = (uint8_t)f->n[i].is_dir; in->dirid = f->n[i].is_dir ? f->n[i].id : 0;
    return 0;
}
static int f_mkdir(void *c, long parent, const char *name) {
    Fake *f = c; int i, e;
    if (!dir_ok(f, parent)) return INST_OS_DIRNF;
    if (fk_find(f, parent, name) >= 0) return INST_OS_DUPFN;
    if ((e = chg(f)) != 0) return e;
    i = fk_alloc(f);
    strcpy(f->n[i].name, name); f->n[i].is_dir = 1; f->n[i].parent = parent; f->n[i].id = f->next_id++;
    f->n[i].crdat = f->n[i].mddat = ++f->clock;
    return 0;
}
static int f_create(void *c, long dir, const char *name) {
    Fake *f = c; int i, e;
    if (!dir_ok(f, dir)) return INST_OS_DIRNF;
    if (fk_find(f, dir, name) >= 0) return INST_OS_DUPFN;
    if ((e = chg(f)) != 0) return e;
    i = fk_alloc(f);
    strcpy(f->n[i].name, name); f->n[i].parent = dir;
    f->n[i].crdat = f->n[i].mddat = ++f->clock;
    return 0;
}
static int f_setinfo(void *c, long dir, const char *name, uint32_t type, uint32_t cr, uint16_t flags) {
    Fake *f = c; int i, e;
    i = fk_find(f, dir, name);
    if (i < 0) return INST_OS_FNF;
    if (f->n[i].is_dir) return INST_OS_FNF;
    if (is_trio_name(name) && (f->n[i].type == CDEV || f->n[i].type == MTPP)) f->trio_opened++;   /* never ours to touch */
    if ((e = chg(f)) != 0) return e;
    f->n[i].type = type; f->n[i].creator = cr; f->n[i].flags = flags;
    return 0;
}
static int f_write(void *c, long dir, const char *name, int rsrc, const uint8_t *p, long len) {
    Fake *f = c; int i, e; Node *n;
    i = fk_find(f, dir, name);
    if (i < 0) return INST_OS_FNF;
    n = &f->n[i];
    if (is_trio_name(name)) f->trio_opened++;
    if (n->is_dir) return INST_OS_FNF;
    if (n->locked) return INST_OS_PERM;
    if (n->busy) return INST_OS_OPWR;
    T_CHECK(len >= 0 && len <= (rsrc ? RCAP : DCAP));
    if (len < 0 || len > (rsrc ? RCAP : DCAP)) return INST_OS_DSKFUL;
    if ((e = chg(f)) != 0) {
        if (f->fail_partial) {                       /* half the bytes reached the disk */
            if (rsrc) { memcpy(n->r, p, (size_t)(len / 2)); n->rl = len / 2; }
            else { memcpy(n->d, p, (size_t)(len / 2)); n->dl = len / 2; }
        }
        return e;
    }
    if (rsrc) { if (len) memcpy(n->r, p, (size_t)len); n->rl = len; }
    else { if (len) memcpy(n->d, p, (size_t)len); n->dl = len; }
    n->mddat = ++f->clock;
    return 0;
}
static int f_read(void *c, long dir, const char *name, int rsrc, long off, uint8_t *p, long len, long *got) {
    Fake *f = c; int i; Node *n; long have;
    *got = 0;
    i = fk_find(f, dir, name);
    if (i < 0) return INST_OS_FNF;
    n = &f->n[i];
    if (is_trio_name(name)) f->trio_opened++;
    if (n->is_dir) return INST_OS_FNF;
    have = rsrc ? n->rl : n->dl;
    if (off < 0) return INST_OS_IO;
    if (off >= have) return 0;
    if (len > have - off) len = have - off;
    memcpy(p, (rsrc ? n->r : n->d) + off, (size_t)len);
    *got = len;
    return 0;
}
static int f_del(void *c, long dir, const char *name) {
    Fake *f = c; int i, k, e;
    i = fk_find(f, dir, name);
    if (i < 0) return INST_OS_FNF;
    if (f->n[i].is_dir) {
        for (k = 0; k < NN; k++) if (f->n[k].used && f->n[k].parent == f->n[i].id) return INST_OS_FBSY;
    } else {
        if (f->n[i].busy) return INST_OS_FBSY;
        if (f->n[i].locked) return INST_OS_FLCKD;
    }
    if ((e = chg(f)) != 0) return e;
    f->n[i].used = 0;
    return 0;
}
static int f_rename(void *c, long dir, const char *from, const char *to) {
    Fake *f = c; int i, e;
    i = fk_find(f, dir, from);
    if (i < 0) return INST_OS_FNF;
    if (fk_find(f, dir, to) >= 0) return INST_OS_DUPFN;
    if (f->n[i].locked) return INST_OS_FLCKD;
    if ((e = chg(f)) != 0) return e;
    strcpy(f->n[i].name, to);
    return 0;
}
static int f_move(void *c, long fromdir, const char *name, long todir) {
    Fake *f = c; int i, e;
    i = fk_find(f, fromdir, name);
    if (i < 0) return INST_OS_FNF;
    if (!dir_ok(f, todir)) return INST_OS_DIRNF;
    if (fk_find(f, todir, name) >= 0) return INST_OS_DUPFN;
    if ((e = chg(f)) != 0) return e;
    f->n[i].parent = todir;                          /* the catalog record moves, nothing else changes */
    return 0;
}
static int f_flush(void *c) { ((Fake *)c)->flushes++; return 0; }
static int f_volinfo(void *c, long *fb, int *locked) { Fake *f = c; *fb = f->free_bytes; *locked = f->vol_locked; return 0; }

static InstFs mkfs(Fake *f) {
    InstFs fs;
    fs.ctx = f; fs.sysdir = SYSD;
    fs.list = f_list; fs.info = f_info; fs.mkdir = f_mkdir; fs.create = f_create; fs.setinfo = f_setinfo;
    fs.write = f_write; fs.read = f_read; fs.del = f_del; fs.rename = f_rename; fs.move = f_move;
    fs.flush = f_flush; fs.volinfo = f_volinfo;
    return fs;
}

/* ---- building states ---- */

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
/* a resource fork with one 'vers' 1 resource whose first 4 bytes are v, plus `pad` filler bytes */
static long mkfork(uint8_t *out, uint32_t v, int pad) {
    long doff = 256, dlen = 4 + 8 + pad, moff = doff + dlen, mlen = 28 + 2 + 8 + 12, i;
    uint8_t *m;
    memset(out, 0, (size_t)(moff + mlen));
    wr32(out, (uint32_t)doff); wr32(out + 4, (uint32_t)moff); wr32(out + 8, (uint32_t)dlen); wr32(out + 12, (uint32_t)mlen);
    wr32(out + doff, (uint32_t)(8 + pad));                          /* resource length */
    wr32(out + doff + 4, v);
    for (i = 0; i < 4 + pad; i++) out[doff + 8 + i] = (uint8_t)(0xA5 ^ i);
    m = out + moff;
    m[25] = 28;                                                     /* type list at 28 */
    m[27] = (uint8_t)mlen;                                          /* name list at the end */
    m[28] = 0; m[29] = 0;                                           /* one type */
    memcpy(m + 30, "vers", 4); m[34] = 0; m[35] = 0; m[36] = 0; m[37] = 10;   /* one resource, refs at 10 */
    m[38] = 0; m[39] = 1; m[40] = 0xFF; m[41] = 0xFF;               /* id 1, no name, attr 0, offset 0 */
    return moff + mlen;
}

static Fake F0, F, G;                 /* original, working copy, scratch */
static uint8_t payload_fork[RCAP];
static InstPayload PL;
#define VNEW 0x01006001u              /* 1.0rc1 (beta 1) */

static int addn(Fake *f, long dir, const char *name, uint32_t type, uint32_t cr, int is_dir) {
    int i = fk_alloc(f);
    Node *n = &f->n[i];
    strcpy(n->name, name); n->parent = dir; n->type = type; n->creator = cr; n->is_dir = is_dir;
    if (is_dir) n->id = f->next_id++;
    n->crdat = 1000 + (uint32_t)i; n->mddat = 2000 + (uint32_t)i;
    n->lv = (int16_t)(10 * i); n->lh = (int16_t)(20 + i); n->fldr = (uint16_t)i;
    return i;
}
static void fillforks(Node *n, int seed, long dl, long rl) {
    long k;
    for (k = 0; k < dl; k++) n->d[k] = (uint8_t)(seed * 7 + k);
    for (k = 0; k < rl; k++) n->r[k] = (uint8_t)(seed * 13 + k * 3);
    n->dl = dl; n->rl = rl;
}
static void base(Fake *f) {
    int i;
    memset(f, 0, sizeof *f);
    f->next_id = 100; f->free_bytes = 4000000L; f->clock = 5000;
    i = addn(f, ROOT, "System Folder", 0, 0, 1);
    f->n[i].id = SYSD;
    addn(f, SYSD, "System", INST_FCC('z', 's', 'y', 's'), INST_FCC('M', 'A', 'C', 'S'), 0);
    addn(f, SYSD, "Finder", INST_FCC('F', 'N', 'D', 'R'), INST_FCC('M', 'A', 'C', 'S'), 0);
    addn(f, ROOT, "Read Me", TEXT, INST_FCC('t', 't', 'x', 't'), 0);
}
static void trio3(Fake *f) {
    int i;
    i = addn(f, SYSD, "MacTCP", CDEV, ZTCP, 0); f->n[i].flags = 0x2100; fillforks(&f->n[i], 1, 0, 900);
    i = addn(f, SYSD, "MacTCP DNR", CDEV, MTCP, 0); f->n[i].flags = 0x0100; fillforks(&f->n[i], 2, 10, 700);
    i = addn(f, SYSD, "MacTCP Prep", MTPP, MTCP, 0); f->n[i].flags = 0x0000; fillforks(&f->n[i], 3, 64, 0);
}
static void oldmoof(Fake *f, uint32_t v) {
    int i = addn(f, SYSD, "MoofTCP", CDEV, ZTCP, 0);
    f->n[i].flags = 0x2100;
    f->n[i].rl = mkfork(f->n[i].r, v, 5);
}
static void payload_init(void) {
    memset(&PL, 0, sizeof PL);
    PL.rlen = mkfork(payload_fork, VNEW, 300);
    PL.rsrc = payload_fork; PL.type = CDEV; PL.creator = ZTCP; PL.flags = 0x2000;
}

/* ---- comparing states: every node by folder name and name, all attributes and both forks ---- */
static const char *pname(const Fake *f, long parent) {
    int i;
    if (parent == ROOT) return "<root>";
    for (i = 0; i < NN; i++) if (f->n[i].used && f->n[i].is_dir && f->n[i].id == parent) return f->n[i].name;
    return "<gone>";
}
static int node_eq(const Fake *a, const Node *x, const Fake *b, const Node *y) {
    return x->is_dir == y->is_dir && x->locked == y->locked && x->type == y->type && x->creator == y->creator &&
           x->flags == y->flags && x->fldr == y->fldr && x->lv == y->lv && x->lh == y->lh && x->crdat == y->crdat &&
           x->mddat == y->mddat && x->dl == y->dl && x->rl == y->rl && !memcmp(x->d, y->d, (size_t)x->dl) &&
           !memcmp(x->r, y->r, (size_t)x->rl) && !strcmp(pname(a, x->parent), pname(b, y->parent)) &&
           !strcmp(x->name, y->name);
}
static int empty_dis(const Fake *f, int i) {
    int k;
    if (!f->n[i].is_dir || strcmp(f->n[i].name, INST_DISABLED)) return 0;
    for (k = 0; k < NN; k++) if (f->n[k].used && f->n[k].parent == f->n[i].id) return 0;
    return 1;
}
/* b equals a; tol: an empty 'MacTCP (Disabled)' in b that a does not have may stay (inst_plan.h:
 * possible only once a crash cost the manifest its 'created' flag) */
static int same(const Fake *a, const Fake *b, int tol) {
    int i, k, used[NN], na = 0, nb = 0;
    memset(used, 0, sizeof used);
    for (i = 0; i < NN; i++) if (a->n[i].used) na++;
    for (k = 0; k < NN; k++) if (b->n[k].used && !(tol && empty_dis(b, k) && fk_find((Fake *)a, SYSD, INST_DISABLED) < 0)) nb++;
    if (na != nb) return 0;
    for (i = 0; i < NN; i++) {
        if (!a->n[i].used) continue;
        for (k = 0; k < NN; k++) if (!used[k] && b->n[k].used && node_eq(a, &a->n[i], b, &b->n[k])) break;
        if (k == NN) return 0;
        used[k] = 1;
    }
    return 1;
}
static void dump(const Fake *f) {
    int i;
    for (i = 0; i < NN; i++) {
        const Node *n = &f->n[i];
        if (n->used) fprintf(stderr, "  [%s] %s %08x/%08x fl %04x d%d l%d dl %ld rl %ld\n", pname(f, n->parent), n->name,
                             (unsigned)n->type, (unsigned)n->creator, n->flags, n->is_dir, n->locked, n->dl, n->rl);
    }
    fprintf(stderr, "--\n");
}
static int providers(Fake *f) {     /* loadable cdev/ztcp in the System Folder (System 6 runs every cdev's INIT) */
    int i, n = 0;
    for (i = 0; i < NN; i++) if (f->n[i].used && f->n[i].parent == SYSD && f->n[i].type == CDEV && f->n[i].creator == ZTCP) n++;
    return n;
}
static int moof_complete(Fake *f) {
    int i = fk_find(f, SYSD, "MoofTCP");
    return i >= 0 && f->n[i].type == CDEV && f->n[i].creator == ZTCP && f->n[i].flags == 0x2000 &&
           f->n[i].rl == PL.rlen && !memcmp(f->n[i].r, PL.rsrc, (size_t)PL.rlen) && f->n[i].dl == 0;
}
static long dis_id(Fake *f) {
    int i = fk_find(f, SYSD, INST_DISABLED);
    return i >= 0 && f->n[i].is_dir ? f->n[i].id : -1;
}

static InstResult R;
static char msg[256];
static int run_install(Fake *f, long stop) { InstFs fs = mkfs(f); return inst_do_install(&fs, &PL, stop, &R); }
static int run_remove(Fake *f, long stop) { InstFs fs = mkfs(f); return inst_do_remove(&fs, stop, &R); }
static int mesg(void) { int rs = inst_message(&R, "1.0rc1", msg, sizeof msg); T_CHECK(strlen(msg) > 0 && strlen(msg) < 256); return rs; }

/* ---- scenarios ---- */
static void install_remove_identical(void) {
    InstManifest m; long d;
    payload_init(); base(&F0); trio3(&F0); addn(&F0, SYSD, INST_PREFS_NAME, PREF, ZTCP, 0);
    F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_INSTALLED_PARKED_MACTCP); T_EQ(R.nops, 6); T_EQ(R.done, 6); T_EQ(R.moves, 3);
    T_EQ(mesg(), 1);
    T_CHECK(strstr(msg, "MacTCP was found and moved") != NULL);
    T_CHECK(strstr(msg, "Restart") != NULL);
    T_CHECK(moof_complete(&F)); T_EQ(providers(&F), 1);
    d = dis_id(&F);
    T_CHECK(d > 0);
    T_CHECK(fk_find(&F, d, "MacTCP") >= 0 && fk_find(&F, d, "MacTCP DNR") >= 0 && fk_find(&F, d, "MacTCP Prep") >= 0);
    T_EQ(fk_find(&F, SYSD, INST_MOOF_TMP), -1); T_EQ(fk_find(&F, d, INST_MANIFEST_TMP), -1);
    {
        int i = fk_find(&F, d, INST_MANIFEST_NAME);
        T_CHECK(i >= 0);
        if (i >= 0) {
            T_EQ(F.n[i].type, TEXT); T_EQ(F.n[i].creator, ZTCP);
            T_EQ(inst_manifest_decode((const char *)F.n[i].d, (size_t)F.n[i].dl, &m), 3); T_EQ(m.created, 1);
        }
    }
    T_EQ(R.mutations, F.mut);
    T_CHECK(F.flushes >= F.mut + R.nops);             /* every change flushed at once, and every op */
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_REMOVED_RESTORED); T_EQ(R.nops, 6); T_EQ(R.man_retried, 0);
    T_EQ(mesg(), 1);
    T_CHECK(strstr(msg, "put back") != NULL && strstr(msg, "MoofTCP Prefs") != NULL);
    T_CHECK(same(&F0, &F, 0));                       /* forks, type, creator, flags, location, dates, folder */
    T_EQ(F.trio_opened, 0);
}
static void absent_trio(void) {
    payload_init(); base(&F0); F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_INSTALLED_NO_MACTCP); T_EQ(R.nops, 1);
    T_EQ(mesg(), 1);
    T_CHECK(strstr(msg, "No MacTCP was found") != NULL);
    T_CHECK(strstr(msg, "Control Panel > MoofTCP") != NULL);       /* no prefs: say where to configure */
    T_CHECK(moof_complete(&F)); T_EQ(dis_id(&F), -1);
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_REMOVED_NO_MACTCP);
    T_EQ(mesg(), 1);
    T_CHECK(strstr(msg, "Prefs") == NULL);                          /* there is none to keep */
    T_CHECK(same(&F0, &F, 0));
}
static void twice(void) {
    payload_init(); base(&F0); trio3(&F0); F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_ALREADY_INSTALLED); T_EQ(R.nops, 0);
    T_EQ(mesg(), 0);
    T_CHECK(strstr(msg, "MoofTCP is already installed") != NULL);
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_NOTHING_TO_REMOVE);
    T_EQ(mesg(), 0);
    T_CHECK(!strcmp(msg, "MoofTCP is not installed, nothing to remove."));
    T_CHECK(same(&F0, &F, 0));
}

/* From state `p` (orig = o): Remove restores o; Install completes, then Remove restores o. */
static Fake E;
static void recover(const Fake *o0, const Fake *p, const char *tag, long k) {
    const Fake *o = &E;
    int mi;
    E = *o0;                                         /* Remove deletes MoofTCP, an older one included */
    mi = fk_find(&E, SYSD, "MoofTCP");
    if (mi >= 0) E.n[mi].used = 0;
    G = *p;
    G.fail_at = 0; G.mut = 0;
    if (run_remove(&G, 0) != INST_X_OK) { fprintf(stderr, "%s %ld: remove status %d op %d err %d\n", tag, k, R.status, R.op_index, R.oserr); dump(p); T_CHECK(0); return; }
    T_CHECK(R.msg != INST_BLOCKED_COLLISION && R.msg != INST_BLOCKED_LOCKED);
    if (!same(o, &G, 1)) { fprintf(stderr, "%s %ld: remove did not restore\n", tag, k); dump(o); dump(p); dump(&G); T_CHECK(0); return; }
    G = *p;
    G.fail_at = 0; G.mut = 0;
    if (run_install(&G, 0) != INST_X_OK) { fprintf(stderr, "%s %ld: install status %d\n", tag, k, R.status); dump(p); T_CHECK(0); return; }
    T_CHECK(moof_complete(&G)); T_EQ(providers(&G), 1);
    T_EQ(fk_find(&G, SYSD, INST_MOOF_TMP), -1);
    if (run_remove(&G, 0) != INST_X_OK || !same(o, &G, 1)) { fprintf(stderr, "%s %ld: reinstall+remove\n", tag, k); dump(o); dump(&G); T_CHECK(0); }
    T_EQ(G.trio_opened, 0);
}

static void setup(int variant) {    /* starting states for the injection runs */
    payload_init(); base(&F0);
    if (variant & 1) trio3(&F0);
    if (variant & 2) oldmoof(&F0, 0x01000000u);
    if (variant & 4) { addn(&F0, SYSD, INST_DISABLED, 0, 0, 1); addn(&F0, dis_id(&F0), "Notes", TEXT, TEXT, 0); }
    if (variant & 8) addn(&F0, SYSD, INST_PREFS_NAME, PREF, ZTCP, 0);
}

static void fail_every_install_change(void) {
    static const int errs[4] = { INST_OS_IO, INST_OS_DSKFUL, INST_OS_FBSY, INST_OS_FLCKD };
    int v, e, part, runs = 0;
    long k, total;
    for (v = 0; v < 16; v++) {
        setup(v);
        F = F0;
        T_EQ(run_install(&F, 0), INST_X_OK);
        total = F.mut;
        T_CHECK(total > 0);
        for (k = 1; k <= total; k++)
            for (e = 0; e < 4; e++)
                for (part = 0; part < 2; part++) {
                    F = F0; F.fail_at = k; F.fail_err = errs[e]; F.fail_partial = part;
                    T_EQ(run_install(&F, 0), INST_X_OSERR);
                    T_EQ(R.oserr, errs[e]);
                    T_CHECK(R.op_index >= 0 && R.op_index < R.nops);
                    T_EQ(R.done, R.op_index);
                    mesg();
                    T_CHECK(strstr(msg, "Step ") == msg);
                    if (errs[e] == INST_OS_FBSY) T_CHECK(strstr(msg, "Close the Control Panel") != NULL);
                    if (errs[e] == INST_OS_FLCKD) T_CHECK(strstr(msg, "locked") != NULL);
                    T_CHECK(providers(&F) <= (providers(&F0) > 1 ? providers(&F0) : 1));   /* never MacTCP and MoofTCP together because of us */
                    recover(&F0, &F, "install-fail", k * 10 + e);
                    runs++;
                }
    }
    T_CHECK(runs > 400);
}
static void fail_every_remove_change(void) {
    int v, part, runs = 0;
    long k, total;
    Fake inst;
    for (v = 0; v < 16; v++) {
        setup(v);
        F = F0;
        T_EQ(run_install(&F, 0), INST_X_OK);
        inst = F; inst.mut = 0;
        F = inst;
        T_EQ(run_remove(&F, 0), INST_X_OK);
        E = F0;
        if (fk_find(&E, SYSD, "MoofTCP") >= 0) E.n[fk_find(&E, SYSD, "MoofTCP")].used = 0;
        T_CHECK(same(&E, &F, 0));
        total = F.mut;
        for (k = 1; k <= total; k++)
            for (part = 0; part < 2; part++) {
                F = inst; F.fail_at = k; F.fail_err = part ? INST_OS_IO : INST_OS_FBSY;
                T_EQ(run_remove(&F, 0), INST_X_OSERR);
                mesg();
                T_CHECK(providers(&F) <= 1);
                recover(&F0, &F, "remove-fail", k);
                runs++;
            }
    }
    T_CHECK(runs > 50);
}
/* the debug hook (the Snow interruption test drives the same code with a count from a file) */
static void stop_after_every_count(void) {
    int v, runs = 0;
    long k, total;
    Fake inst;
    for (v = 0; v < 16; v++) {
        setup(v);
        F = F0;
        T_EQ(run_install(&F, 0), INST_X_OK);
        total = R.mutations;
        inst = F; inst.mut = 0;
        for (k = 1; k < total; k++) {
            F = F0;
            T_EQ(run_install(&F, k), INST_X_STOPPED);
            T_EQ(R.mutations, k); T_EQ(F.mut, k);
            mesg();
            T_CHECK(strstr(msg, "debug stop") != NULL);
            recover(&F0, &F, "install-stop", k);
            runs++;
        }
        F = F0;
        T_EQ(run_install(&F, total), INST_X_OK);    /* exactly enough */
        F = inst;
        T_EQ(run_remove(&F, 0), INST_X_OK);
        total = R.mutations;
        for (k = 1; k < total; k++) {
            F = inst;
            T_EQ(run_remove(&F, k), INST_X_STOPPED);
            T_EQ(F.mut, k);
            recover(&F0, &F, "remove-stop", k);
            runs++;
        }
    }
    T_CHECK(runs > 100);
}

static void open_file_47(void) {
    int i, j;
    payload_init(); base(&F0); trio3(&F0); oldmoof(&F0, 0x01000000u); addn(&F0, SYSD, INST_PREFS_NAME, PREF, ZTCP, 0);
    F = F0;
    i = fk_find(&F, SYSD, "MoofTCP");
    F.n[i].busy = 1;                                  /* the Control Panel shows it */
    T_EQ(run_install(&F, 0), INST_X_OSERR);
    T_EQ(R.oserr, INST_OS_FBSY); T_EQ(R.op_kind, INST_WRITE_MOOF); T_EQ(R.step, INST_W_DELETE);
    T_EQ(R.msg, INST_UPGRADED);
    mesg();
    T_CHECK(strstr(msg, "the file is in use. Close the Control Panel and quit other programs") != NULL);
    T_CHECK(strstr(msg, "\"MoofTCP\"") != NULL);
    j = fk_find(&F, SYSD, INST_MOOF_TMP);             /* harmless: old X plus a non-loadable temp */
    T_CHECK(j >= 0 && F.n[j].type == MTMP && F.n[j].creator == MTIN);
    T_CHECK(F.n[i].type == CDEV && F.n[i].rl == F0.n[i].rl);
    T_EQ(providers(&F), 1);
    F.n[i].busy = 0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_UPGRADED);
    T_CHECK(moof_complete(&F)); T_EQ(fk_find(&F, SYSD, INST_MOOF_TMP), -1);
    T_CHECK(fk_find(&F, SYSD, INST_PREFS_NAME) >= 0);
    /* Remove while it is open: the first op fails, nothing changed */
    i = fk_find(&F, SYSD, "MoofTCP");
    F.n[i].busy = 1;
    G = F;
    T_EQ(run_remove(&F, 0), INST_X_OSERR);
    T_EQ(R.op_index, 0); T_EQ(R.op_kind, INST_DELETE_MOOF); T_EQ(R.oserr, INST_OS_FBSY);
    mesg();
    T_CHECK(strstr(msg, "Close the Control Panel") != NULL && strstr(msg, "Nothing was changed.") != NULL);
    T_CHECK(same(&G, &F, 0));
    F.n[i].busy = 0;
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_REMOVED_RESTORED);
    {   /* F0 had the old MoofTCP: compare without it */
        Fake o = F0;
        o.n[fk_find(&o, SYSD, "MoofTCP")].used = 0;
        T_CHECK(same(&o, &F, 0));
    }
}
static void locked_paths(void) {
    int i;
    payload_init(); base(&F0); trio3(&F0);
    F = F0;
    i = fk_find(&F, SYSD, "MacTCP DNR"); F.n[i].locked = 1;
    G = F;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_BLOCKED_LOCKED); T_EQ(R.nops, 0); T_CHECK(!strcmp(R.blocker, "MacTCP DNR"));
    T_EQ(mesg(), 0);
    T_CHECK(!strcmp(msg, "The file \"MacTCP DNR\" is locked. Unlock it in the Finder (File > Get Info) and run the installer again. Nothing was changed."));
    T_CHECK(same(&G, &F, 0)); T_EQ(F.mut, 0);
    /* Remove: a parked file locked after the install */
    F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    i = fk_find(&F, dis_id(&F), "MacTCP"); F.n[i].locked = 1;
    G = F;
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_BLOCKED_LOCKED); T_CHECK(!strcmp(R.blocker, "MacTCP"));
    T_CHECK(same(&G, &F, 0));
    F.n[i].locked = 0;
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_CHECK(same(&F0, &F, 0));
    /* the OS says locked in the middle (the snapshot did not show it): reported, re-runnable */
    F = F0; F.fail_at = 4; F.fail_err = INST_OS_FLCKD;
    T_EQ(run_install(&F, 0), INST_X_OSERR);
    mesg();
    T_CHECK(strstr(msg, "the file is locked. Unlock it in the Finder") != NULL);
    T_CHECK(strstr(msg, "Run Install or Remove again") != NULL);
    recover(&F0, &F, "locked", 0);
    /* a locked disk: nothing is tried */
    F = F0; F.vol_locked = 1;
    T_EQ(run_install(&F, 0), INST_X_VOLLOCKED);
    mesg(); T_CHECK(strstr(msg, "startup disk is locked") != NULL);
    T_EQ(F.mut, 0);
}
static void retry_with_null(void) {
    static const char bad[] = "#created\rMy Notes\tSystem Folder\r";   /* a decodable but refused manifest */
    int i; long d;
    payload_init(); base(&F0); trio3(&F0); F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    d = dis_id(&F);
    addn(&F, d, "My Notes", TEXT, TEXT, 0);
    i = fk_find(&F, d, INST_MANIFEST_NAME);
    memcpy(F.n[i].d, bad, sizeof bad - 1); F.n[i].dl = (long)sizeof bad - 1;
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.man_retried, 1); T_EQ(R.msg, INST_REMOVED_RESTORED);
    T_CHECK(fk_find(&F, SYSD, "MacTCP") >= 0 && fk_find(&F, SYSD, "MacTCP DNR") >= 0 && fk_find(&F, SYSD, "MacTCP Prep") >= 0);
    T_CHECK(fk_find(&F, d, "My Notes") >= 0);         /* untouched, and so the folder stays */
    T_EQ(fk_find(&F, d, INST_MANIFEST_NAME), -1);
    T_EQ(F.trio_opened, 0);
    /* an undecodable manifest: also planned with NULL (not "retried": nothing was refused) */
    F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    d = dis_id(&F);
    i = fk_find(&F, d, INST_MANIFEST_NAME);
    F.n[i].dl = 3;
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_EQ(R.man_retried, 0); T_EQ(R.msg, INST_REMOVED_RESTORED);
    T_CHECK(same(&F0, &F, 1));
}
static void upgrade_and_versions(void) {
    payload_init(); base(&F0); oldmoof(&F0, 0x01005001u); addn(&F0, SYSD, INST_PREFS_NAME, PREF, ZTCP, 0);
    F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_UPGRADED);
    T_EQ(mesg(), 1);
    T_CHECK(strstr(msg, "updated to 1.0rc1; your settings were kept") != NULL);
    T_CHECK(moof_complete(&F)); T_CHECK(fk_find(&F, SYSD, INST_PREFS_NAME) >= 0);
    F = F0; F.n[fk_find(&F, SYSD, INST_PREFS_NAME)].used = 0;           /* no prefs: nothing to keep */
    T_EQ(run_install(&F, 0), INST_X_OK); T_EQ(R.msg, INST_UPGRADED);
    mesg(); T_CHECK(strstr(msg, "settings") == NULL); T_CHECK(strstr(msg, "Control Panel > MoofTCP") != NULL);
    T_CHECK(moof_complete(&F)); T_EQ(fk_find(&F, SYSD, INST_PREFS_NAME), -1);   /* none is ever written */
    base(&F); oldmoof(&F, 0x01008000u);                /* 1.0 final: newer, no downgrade */
    T_EQ(run_install(&F, 0), INST_X_OK); T_EQ(R.msg, INST_ALREADY_INSTALLED);
    base(&F); oldmoof(&F, VNEW);
    T_EQ(run_install(&F, 0), INST_X_OK); T_EQ(R.msg, INST_ALREADY_INSTALLED);
    base(&F); oldmoof(&F, VNEW); F.n[fk_find(&F, SYSD, "MoofTCP")].rl = 10;   /* unreadable vers: replaced */
    T_EQ(run_install(&F, 0), INST_X_OK); T_EQ(R.msg, INST_UPGRADED); T_CHECK(moof_complete(&F));
    base(&F); addn(&F, SYSD, "MoofTCP", MTMP, MTIN, 0);   /* incomplete: rewritten */
    T_EQ(run_install(&F, 0), INST_X_OK); T_CHECK(moof_complete(&F));
}
static void stale_snapshot_changed(void) {
    long d; int i;
    /* a MOVE target that is not a MacTCP file is never moved, a DELETE_TEMP target must still be ours */
    InstOp op; InstCtx x; InstFs fs;
    payload_init(); base(&F); addn(&F, SYSD, "MacTCP", TEXT, TEXT, 0); addn(&F, SYSD, INST_DISABLED, 0, 0, 1);
    d = dis_id(&F);
    fs = mkfs(&F);
    memset(&R, 0, sizeof R);
    x.fs = &fs; x.stop_after = 0; x.mutations = 0; x.committed = 0; x.res = &R;
    memset(&op, 0, sizeof op); op.kind = INST_MOVE;
    strcpy(op.from, ":System Folder:MacTCP"); strcpy(op.to, ":System Folder:MacTCP (Disabled):MacTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_CHANGED);
    T_EQ(F.mut, 0); T_EQ(fk_find(&F, d, "MacTCP"), -1);
    mesg(); T_CHECK(strstr(msg, "changed while the installer was working") != NULL);
    T_CHECK(strstr(msg, "Nothing was changed.") != NULL);
    i = addn(&F, SYSD, INST_MOOF_TMP, CDEV, ZTCP, 0);  /* a loadable temp-named file is not ours */
    memset(&op, 0, sizeof op); op.kind = INST_DELETE_TEMP; strcpy(op.from, ":System Folder:MoofTCP.tmp");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_CHANGED);
    T_CHECK(F.n[i].used);
    memset(&op, 0, sizeof op); op.kind = INST_DELETE_MOOF; strcpy(op.from, ":System Folder:MacTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_REFUSED);
    memset(&op, 0, sizeof op); op.kind = INST_RMDIR; strcpy(op.from, ":System Folder:MacTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_REFUSED);
    /* paths outside the contract */
    memset(&op, 0, sizeof op); op.kind = INST_DELETE_MOOF; strcpy(op.from, ":Desktop Folder:MoofTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_PATH);
    strcpy(op.from, ":System Folder:Other:MoofTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_PATH);
    strcpy(op.from, "System Folder:MoofTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_PATH);
    strcpy(op.from, ":System Folder::MoofTCP");
    T_EQ(inst_run(&x, &op, 1, NULL, NULL), INST_X_PATH);
    memset(&op, 0, sizeof op); op.kind = INST_WRITE_MOOF; strcpy(op.to, ":System Folder:Finder");
    T_EQ(inst_run(&x, &op, 1, NULL, &PL), INST_X_REFUSED);
    T_EQ(F.mut, 0);
    T_EQ(F.trio_opened, 0);
}

/* Review fix 1: a hostile op list (or a planner bug) cannot make the executor change anything but
 * MoofTCP's own objects; the MacTCP set can only be moved. Every op is refused, nothing changes. */
static void hostile_ops_refused(void) {
    static const struct { InstOpKind k; const char *from, *to; int want; } bad[] = {
        { INST_DELETE_MOOF, ":System Folder:MacTCP", "", INST_X_REFUSED },                 /* the REAL cdev/ztcp */
        { INST_DELETE_MOOF, ":System Folder:MacTCP (Disabled):MacTCP", "", INST_X_REFUSED },
        { INST_DELETE_MOOF, ":System Folder:Finder", "", INST_X_REFUSED },
        { INST_DELETE_MANIFEST, ":System Folder:MacTCP (Disabled):MacTCP DNR", "", INST_X_REFUSED },   /* parked */
        { INST_DELETE_MANIFEST, ":System Folder:MacTCP Prep", "", INST_X_REFUSED },
        { INST_DELETE_MANIFEST, ":System Folder:MoofTCP Manifest", "", INST_X_REFUSED },  /* wrong folder */
        { INST_DELETE_TEMP, ":System Folder:MacTCP", "", INST_X_REFUSED },
        { INST_DELETE_TEMP, ":System Folder:MacTCP (Disabled):Notes", "", INST_X_REFUSED },
        { INST_RMDIR, ":System Folder:Other Folder", "", INST_X_REFUSED },
        { INST_MKDIR, "", ":System Folder:Other Folder", INST_X_REFUSED },
        { INST_MOVE, ":System Folder:Finder", ":System Folder:MacTCP (Disabled):Finder", INST_X_REFUSED },
        { INST_MOVE, ":System Folder:MacTCP (Disabled):Notes", ":System Folder:Notes", INST_X_REFUSED },
        { INST_MOVE, ":System Folder:MacTCP (Disabled):MacTCP DNR", ":System Folder:MacTCP (Disabled):MacTCP DNR", INST_X_REFUSED },
        { INST_MOVE, ":System Folder:MacTCP DNR", ":System Folder:MacTCP (Disabled):MacTCP DNR", INST_X_CHANGED }, /* target exists */
        /* writes whose rename / delete would land on a set file or elsewhere */
        { INST_WRITE_MOOF, "", ":System Folder:MacTCP", INST_X_REFUSED },
        { INST_WRITE_MOOF, "", ":System Folder:MacTCP (Disabled):MoofTCP", INST_X_REFUSED },
        { INST_WRITE_MANIFEST, "", ":System Folder:MacTCP (Disabled):MacTCP", INST_X_REFUSED },
        { INST_WRITE_MANIFEST, "", ":System Folder:MoofTCP Manifest", INST_X_REFUSED },
        /* paths with odd ':' components */
        { INST_DELETE_MOOF, ":System Folder::MacTCP", "", INST_X_PATH },
        { INST_DELETE_MOOF, ":System Folder:MacTCP (Disabled):x:MacTCP", "", INST_X_PATH },
        { INST_DELETE_MANIFEST, "::System Folder:MacTCP", "", INST_X_PATH },
    };
    InstManifest man; InstOp op; InstCtx x; InstFs fs; long d; size_t k;
    payload_init(); base(&F0); trio3(&F0);
    addn(&F0, SYSD, INST_DISABLED, 0, 0, 1);
    d = dis_id(&F0);
    F0.n[addn(&F0, d, "MacTCP DNR", CDEV, MTCP, 0)].rl = 3;   /* a parked copy too */
    addn(&F0, d, "Notes", TEXT, TEXT, 0);
    addn(&F0, SYSD, "Other Folder", 0, 0, 1);
    memset(&man, 0, sizeof man); strcpy(man.e[0].name, "MacTCP"); strcpy(man.e[0].folder, "System Folder"); man.n = 1;
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        F = F0;
        fs = mkfs(&F);
        memset(&R, 0, sizeof R);
        x.fs = &fs; x.stop_after = 0; x.mutations = 0; x.committed = 0; x.res = &R;
        memset(&op, 0, sizeof op); op.kind = bad[k].k; strcpy(op.from, bad[k].from); strcpy(op.to, bad[k].to);
        if (inst_run(&x, &op, 1, &man, &PL) != bad[k].want) {
            fprintf(stderr, "hostile op %d: status %d, want %d\n", (int)k, R.status, bad[k].want); T_CHECK(0);
        }
        T_EQ(R.op_index, 0); T_EQ(R.done, 0); T_EQ(F.mut, 0); T_EQ(F.trio_opened, 0);
        T_CHECK(same(&F0, &F, 0));
        mesg(); T_CHECK(strstr(msg, "Nothing was changed.") != NULL);
        if (bad[k].want == INST_X_REFUSED) T_CHECK(strstr(msg, "refused") != NULL);
    }
}

/* Review fix 2: a resource map larger than 4 KB still gives the version (it once read as 0 = always upgrade) */
static void vers_big_map(void) {
    static uint8_t f[RCAP];
    long n = 400, doff = 256, dlen = 12, moff = doff + dlen, mlen = 28 + 2 + 16 + 12 * (n + 1), i;
    uint8_t *m;
    T_CHECK(moff + mlen <= RCAP);
    memset(f, 0, sizeof f);
    wr32(f, (uint32_t)doff); wr32(f + 4, (uint32_t)moff); wr32(f + 8, (uint32_t)dlen); wr32(f + 12, (uint32_t)mlen);
    wr32(f + doff, 8); wr32(f + doff + 4, 0x01020304u);
    m = f + moff;
    m[24] = 0; m[25] = 28; m[26] = (uint8_t)(mlen >> 8); m[27] = (uint8_t)mlen;
    m[28] = 0; m[29] = 1;                                    /* two types */
    memcpy(m + 30, "pad ", 4); m[34] = (uint8_t)((n - 1) >> 8); m[35] = (uint8_t)(n - 1); m[36] = 0; m[37] = 18;
    memcpy(m + 38, "vers", 4); m[42] = 0; m[43] = 0; m[44] = (uint8_t)((18 + 12 * n) >> 8); m[45] = (uint8_t)(18 + 12 * n);
    for (i = 0; i < n; i++) { uint8_t *r = m + 28 + 18 + 12 * i; r[0] = (uint8_t)(i >> 8); r[1] = (uint8_t)i; r[2] = r[3] = 0xFF; }
    { uint8_t *r = m + 28 + 18 + 12 * n; r[1] = 1; r[2] = r[3] = 0xFF; }
    T_CHECK(mlen > 4096);
    T_EQ(inst_fork_vers(f, moff + mlen), 0x01020304u);
    /* the installed file with that map: same version through the table, so Install says "already installed" */
    payload_init(); base(&F);
    i = addn(&F, SYSD, "MoofTCP", CDEV, ZTCP, 0);
    memcpy(F.n[i].r, f, (size_t)(moff + mlen)); F.n[i].rl = moff + mlen;
    wr32(F.n[i].r + doff + 4, 0x01008000u);                  /* 1.0 final: newer than the bundle */
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_ALREADY_INSTALLED);
}
static void collision_messages(void) {
    payload_init();
    base(&F); trio3(&F); addn(&F, SYSD, INST_DISABLED, 0, 0, 1); addn(&F, dis_id(&F), "MacTCP DNR", CDEV, MTCP, 0);
    T_EQ(run_install(&F, 0), INST_X_OK); T_EQ(R.msg, INST_BLOCKED_COLLISION);
    mesg();
    T_CHECK(!strcmp(msg, "A file named \"MacTCP DNR\" is already in the folder MacTCP (Disabled). Remove or rename it, then run the installer again. Nothing was changed."));
    base(&F0); trio3(&F0); F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    addn(&F, SYSD, "MacTCP", CDEV, ZTCP, 0);          /* the user put a new one back */
    T_EQ(run_remove(&F, 0), INST_X_OK); T_EQ(R.msg, INST_BLOCKED_COLLISION);
    mesg();
    T_CHECK(!strcmp(msg, "There is a new \"MacTCP\" in the System Folder. Remove it first so the original can be put back. Nothing was changed."));
    base(&F); oldmoof(&F, VNEW); addn(&F, SYSD, INST_MOOF_TMP, CDEV, ZTCP, 0);
    T_EQ(run_remove(&F, 0), INST_X_OK); T_EQ(R.msg, INST_BLOCKED_COLLISION);
    mesg();
    T_CHECK(strstr(msg, "An item named \"MoofTCP.tmp\" is in the way") == msg);
    T_CHECK(strstr(msg, "Remove or rename it") != NULL);
    T_EQ(F.mut, 0);
}
static void prechecks(void) {
    payload_init(); base(&F0); trio3(&F0);
    F = F0; F.free_bytes = PL.rlen;                   /* no room for the temp copy */
    T_EQ(run_install(&F, 0), INST_X_NOSPACE);
    mesg(); T_CHECK(strstr(msg, "not enough free space") != NULL);
    T_EQ(F.mut, 0); T_CHECK(same(&F0, &F, 0));
    F = F0;
    PL.rlen = 0;
    T_EQ(run_install(&F, 0), INST_X_PAYLOAD);
    payload_init();
    PL.rlen = 20;                                     /* no vers in it */
    T_EQ(run_install(&F, 0), INST_X_PAYLOAD);
    mesg(); T_CHECK(strstr(msg, "damaged") != NULL);
    payload_init();
    F.list_err = INST_OS_IO;
    T_EQ(run_install(&F, 0), INST_X_SNAPSHOT); T_EQ(R.oserr, INST_OS_IO);
    mesg(); T_CHECK(strstr(msg, "could not be read (error -36)") != NULL);
    T_EQ(run_remove(&F, 0), INST_X_SNAPSHOT);
    T_EQ(F.mut, 0);
}
static void fork_vers_parser(void) {
    uint8_t f[600], g[600];
    long n = mkfork(f, 0x01020304u, 7), i, k;
    uint32_t seed = 99;
    T_EQ(inst_fork_vers(f, n), 0x01020304u);
    T_EQ(inst_fork_vers(f, n - 1), 0u);               /* map cut short */
    T_EQ(inst_fork_vers(f, 15), 0u);
    T_EQ(inst_fork_vers(NULL, n), 0u);
    memcpy(g, f, (size_t)n); g[n - 12 + 1] = 2;          /* the only resource is vers 2 */
    T_EQ(inst_fork_vers(g, n), 0u);
    for (i = 0; i < 20000; i++) {                     /* random corruption never reads out of bounds (ASan) */
        memcpy(g, f, (size_t)n);
        for (k = 0; k < 4; k++) { seed = seed * 1103515245u + 12345u; g[(seed >> 8) % (uint32_t)n] = (uint8_t)(seed >> 20); }
        (void)inst_fork_vers(g, n);
    }
}
static void big_folders(void) {
    int i; long d;
    char nm[32];
    payload_init(); base(&F0); trio3(&F0);
    for (i = 0; i < 20; i++) { sprintf(nm, "Extra %d", i); addn(&F0, SYSD, nm, TEXT, TEXT, 0); }
    addn(&F0, SYSD, INST_DISABLED, 0, 0, 1);
    d = dis_id(&F0);
    for (i = 0; i < 6; i++) { sprintf(nm, "Old %d", i); addn(&F0, d, nm, TEXT, TEXT, 0); }
    F = F0;
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_INSTALLED_PARKED_MACTCP); T_EQ(R.nops, 5);
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_CHECK(same(&F0, &F, 0));                       /* the folder had other files: no RMDIR */
    T_CHECK(dis_id(&F) > 0);
}
static void promote_manifest_tmp(void) {
    /* crash between steps 3 and 4 of an earlier WRITE_MANIFEST: X gone, X.tmp complete. Install
     * promotes it (step 1) before writing, so the parked file it lists is never forgotten. */
    long d; int i;
    static const char man[] = "#created\rMacTCP\tSystem Folder\r";
    payload_init(); base(&F0); trio3(&F0);
    F = F0;
    addn(&F, SYSD, INST_DISABLED, 0, 0, 1);
    d = dis_id(&F);
    F.n[fk_find(&F, SYSD, "MacTCP")].parent = d;       /* parked by the interrupted run */
    i = addn(&F, d, INST_MANIFEST_TMP, MTMP, MTIN, 0);
    memcpy(F.n[i].d, man, sizeof man - 1); F.n[i].dl = (long)sizeof man - 1;
    G = F;
    G.fail_at = 2; G.fail_err = INST_OS_IO;          /* stop right after the promote: X must be the old manifest */
    T_EQ(run_install(&G, 0), INST_X_OSERR);
    T_EQ(R.step, INST_W_TEMP);
    i = fk_find(&G, d, INST_MANIFEST_NAME);
    T_CHECK(i >= 0 && G.n[i].dl == (long)sizeof man - 1);
    T_EQ(run_install(&F, 0), INST_X_OK);
    T_EQ(R.msg, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(run_remove(&F, 0), INST_X_OK);
    T_CHECK(same(&F0, &F, 0));                       /* created survived: the folder went too */
}
static void message_lengths(void) {
    static const InstMsg msgs[] = { INST_ALREADY_INSTALLED, INST_NOTHING_TO_REMOVE, INST_INSTALLED_PARKED_MACTCP,
        INST_INSTALLED_NO_MACTCP, INST_UPGRADED, INST_REMOVED_RESTORED, INST_REMOVED_NO_MACTCP, INST_BLOCKED_LOCKED,
        INST_BLOCKED_COLLISION };
    static const int errs[] = { INST_OS_FBSY, INST_OS_FLCKD, INST_OS_DSKFUL, INST_OS_VLCKD, INST_OS_FNF, -32768, 0 };
    char small[16];
    int s, m, e, k, rem;
    for (s = INST_X_OK; s <= INST_X_REFUSED; s++)
        for (m = 0; m < 9; m++)
            for (e = 0; e < 7; e++)
                for (k = 0; k < 8; k++)
                    for (rem = 0; rem < 2; rem++) {
                        memset(&R, 0, sizeof R);
                        R.status = s; R.msg = msgs[m]; R.oserr = errs[e]; R.op_kind = (InstOpKind)k; R.remove = rem;
                        R.op_index = 11; R.nops = 12; R.done = 3; R.mutations = 2147483647L; R.moves = 3; R.plan_rc = -32768;
                        strcpy(R.blocker, "0123456789012345678901234567890");
                        strcpy(R.op_name, "0123456789012345678901234567890");
                        (void)inst_message(&R, "1.0rc1", msg, sizeof msg);
                        T_CHECK(strlen(msg) > 0 && strlen(msg) <= 255);
                        (void)inst_message(&R, "1.0rc1", small, sizeof small);
                        T_CHECK(strlen(small) <= 15);
                    }
}

T_MAIN(install_remove_identical, absent_trio, twice, fail_every_install_change, fail_every_remove_change,
       stop_after_every_count, open_file_47, locked_paths, retry_with_null, upgrade_and_versions,
       stale_snapshot_changed, collision_messages, prechecks, fork_vers_parser, big_folders,
       promote_manifest_tmp, message_lengths, hostile_ops_refused, vers_big_map)
