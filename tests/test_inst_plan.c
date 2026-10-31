/* test_inst_plan.c - Plan 3 Task 9: the installer planner (mac/installer/inst_plan.[ch]; Review
 * Focus 1): cases, manifest codec, path bounds, and properties over a simulated System Folder
 * (install then remove restores the original, for every crash prefix of both op lists and for
 * half-done WRITE_MANIFEST / WRITE_MOOF ops). */
#include "t.h"
#include "inst_plan.h"

#define CDEV INST_FCC('c', 'd', 'e', 'v')
#define ZTCP INST_FCC('z', 't', 'c', 'p')
#define MTCP INST_FCC('m', 't', 'c', 'p')
#define MTPP INST_FCC('m', 't', 'p', 'p')
#define PREF INST_FCC('p', 'r', 'e', 'f')
#define TEXT INST_FCC('T', 'E', 'X', 'T')
#define SF "System Folder"

/* ---- simulated volume: files tagged with their folder ---- */
typedef struct { char folder[32]; InstEntry e; char content[256]; int clen; } SimF;
typedef struct { SimF f[48]; int n; } Sim;

static void add(Sim *s, const char *folder, const char *name, uint32_t type, uint32_t cr, int is_dir) {
    SimF *f = &s->f[s->n++];
    memset(f, 0, sizeof *f);
    strcpy(f->folder, folder); strcpy(f->e.name, name);
    f->e.type = type; f->e.creator = cr; f->e.is_dir = (uint8_t)is_dir;
}
static void addsys(Sim *s, const char *name, uint32_t type, uint32_t cr) { add(s, SF, name, type, cr, 0); }
static void adddis(Sim *s, const char *name, uint32_t type, uint32_t cr) { add(s, INST_DISABLED, name, type, cr, 0); }
static int sfind(const Sim *s, const char *folder, const char *name) {
    int i;
    for (i = 0; i < s->n; i++) if (!strcmp(s->f[i].folder, folder) && !strcmp(s->f[i].e.name, name)) return i;
    return -1;
}
static void sdel(Sim *s, int i) { s->f[i] = s->f[--s->n]; }
static void setlock(Sim *s, const char *folder, const char *name) {
    int i = sfind(s, folder, name);
    T_CHECK(i >= 0);
    if (i >= 0) s->f[i].e.locked = 1;
}
static int has_dis(const Sim *s) {
    int i = sfind(s, SF, INST_DISABLED);
    return i >= 0 && s->f[i].e.is_dir;
}
static void snap(const Sim *s, InstEntry *sys, int *nsys, InstEntry *dis, int *ndis) {
    int i;
    *nsys = 0; *ndis = has_dis(s) ? 0 : -1;
    for (i = 0; i < s->n; i++) {
        if (!strcmp(s->f[i].folder, SF)) sys[(*nsys)++] = s->f[i].e;
        else if (!strcmp(s->f[i].folder, INST_DISABLED)) dis[(*ndis)++] = s->f[i].e;
    }
}
/* manifest as the executor reads it: X, else X.tmp. -1 = no manifest file, 0 = unreadable, 1 = ok */
static int read_man(const Sim *s, InstManifest *m) {
    int i = sfind(s, INST_DISABLED, INST_MANIFEST_NAME);
    if (i < 0) i = sfind(s, INST_DISABLED, INST_MANIFEST_TMP);
    if (i < 0) return -1;
    return inst_manifest_decode(s->f[i].content, (size_t)s->f[i].clen, m) >= 0 ? 1 : 0;
}
static int parse_path(const char *p, char c[3][64]) {   /* ":a:b[:c]" -> components */
    int n = 0;
    const char *q;
    T_CHECK(p[0] == ':');
    p++;
    while (*p && n < 3) {
        q = strchr(p, ':');
        if (!q) q = p + strlen(p);
        memcpy(c[n], p, (size_t)(q - p)); c[n][q - p] = 0; n++;
        p = *q ? q + 1 : q;
    }
    return n;
}
static void put_manifest(SimF *f, const InstManifest *man, const char *name) {
    strcpy(f->e.name, name); f->e.type = TEXT; f->e.creator = ZTCP;
    f->clen = inst_manifest_encode(man, f->content, sizeof f->content);
}
/* applies one op; returns 0 on success, nonzero when the op is not applicable (a planner bug) */
static int apply1(Sim *s, const InstOp *o, const InstManifest *man) {
    char a[3][64], b[3][64];
    int na, nb, i;
    switch (o->kind) {
    case INST_MKDIR:
        nb = parse_path(o->to, b);
        if (nb != 2 || sfind(s, b[0], b[1]) >= 0) return 1;
        add(s, b[0], b[1], 0, 0, 1);
        return 0;
    case INST_RMDIR:
        na = parse_path(o->from, a);
        if (na != 2) return 1;
        for (i = 0; i < s->n; i++) if (!strcmp(s->f[i].folder, a[1])) return 1;   /* not empty */
        i = sfind(s, a[0], a[1]);
        if (i < 0 || !s->f[i].e.is_dir) return 1;
        sdel(s, i);
        return 0;
    case INST_WRITE_MANIFEST:
        nb = parse_path(o->to, b);
        if (nb != 3 || strcmp(b[2], INST_MANIFEST_NAME)) return 1;
        i = sfind(s, b[1], INST_MANIFEST_TMP);
        if (i >= 0) sdel(s, i);
        i = sfind(s, b[1], b[2]);
        if (i >= 0 && (s->f[i].e.locked || s->f[i].e.is_dir)) return 1;
        if (i < 0) { add(s, b[1], b[2], 0, 0, 0); i = s->n - 1; }
        put_manifest(&s->f[i], man, b[2]);
        return s->f[i].clen < 0;
    case INST_WRITE_MOOF:
        nb = parse_path(o->to, b);
        if (nb != 2) return 1;
        i = sfind(s, b[0], INST_MOOF_TMP);
        if (i >= 0) sdel(s, i);
        i = sfind(s, b[0], b[1]);
        if (i >= 0 && (s->f[i].e.locked || s->f[i].e.is_dir)) return 1;
        if (i < 0) { add(s, b[0], b[1], 0, 0, 0); i = s->n - 1; }
        s->f[i].e.type = CDEV; s->f[i].e.creator = ZTCP; s->f[i].e.vers = 2;
        return 0;
    case INST_DELETE_MOOF:
    case INST_DELETE_MANIFEST:
    case INST_DELETE_TEMP:
        na = parse_path(o->from, a);
        i = sfind(s, a[na - 2], a[na - 1]);
        if (i < 0 || s->f[i].e.locked) return 1;
        sdel(s, i);
        return 0;
    case INST_MOVE:
        na = parse_path(o->from, a); nb = parse_path(o->to, b);
        if (na < 2 || nb < 2 || strcmp(a[na - 1], b[nb - 1])) return 1;
        i = sfind(s, a[na - 2], a[na - 1]);
        if (i < 0 || s->f[i].e.locked || sfind(s, b[nb - 2], b[nb - 1]) >= 0) return 1;
        strcpy(s->f[i].folder, b[nb - 2]);
        return 0;
    }
    return 1;
}
static int apply(Sim *s, const InstOp *ops, int n, const InstManifest *man) {
    int i, bad = 0;
    for (i = 0; i < n; i++) bad |= apply1(s, &ops[i], man);
    return bad;
}
/* a half-done WRITE_* op (HFS has no ExchangeFiles; see the header for the 4 steps) */
static void partial(Sim *s, const InstOp *o, const InstManifest *man, int variant) {
    char b[3][64];
    int nb = parse_path(o->to, b), i;
    if (o->kind == INST_WRITE_MANIFEST) {
        char buf[256];
        int len = inst_manifest_encode(man, buf, sizeof buf);
        int xi = sfind(s, b[1], b[2]);
        T_CHECK(len > 0);
        if (variant == 0 || variant == 1) {              /* tmp (partial / complete) beside the old X */
            i = sfind(s, b[1], INST_MANIFEST_TMP);
            if (i < 0) { add(s, b[1], INST_MANIFEST_TMP, TEXT, ZTCP, 0); i = s->n - 1; }
            memcpy(s->f[i].content, buf, (size_t)len);
            s->f[i].clen = variant == 0 ? len / 2 : len;
            if (variant == 1 && xi >= 0) sdel(s, sfind(s, b[1], b[2]));   /* between steps 3 and 4 */
        } else {                                         /* X itself truncated (outside the protocol) */
            if (xi < 0) { add(s, b[1], b[2], TEXT, ZTCP, 0); xi = s->n - 1; }
            memcpy(s->f[xi].content, buf, (size_t)len);
            s->f[xi].clen = len / 2;
        }
    } else {
        T_EQ(nb, 2);
        if (sfind(s, b[0], INST_MOOF_TMP) < 0) addsys(s, INST_MOOF_TMP, CDEV, ZTCP);
        if (variant == 1 && sfind(s, b[0], b[1]) >= 0) sdel(s, sfind(s, b[0], b[1]));
    }
}

static char blk[32];
static int do_install(const Sim *s, uint32_t vers, InstOp *ops, InstMsg *msg, InstManifest *man) {
    InstEntry sys[48], dis[48];
    InstManifest prev;
    int ns, nd, st = read_man(s, &prev);
    snap(s, sys, &ns, dis, &nd);
    return inst_plan_install(sys, ns, dis, nd, vers, st == 1 ? &prev : NULL, ops, INST_OPS_MAX, msg, blk, man);
}
/* remove: reads the manifest like the executor; an untrusted manifest is retried as unreadable */
static int do_remove(const Sim *s, InstOp *ops, InstMsg *msg) {
    InstEntry sys[48], dis[48];
    InstManifest m;
    int ns, nd, st = read_man(s, &m), rc;
    snap(s, sys, &ns, dis, &nd);
    if (st == 1) {
        rc = inst_plan_remove(sys, ns, dis, nd, &m, ops, INST_OPS_MAX, msg, blk);
        if (rc != INST_E_MANIFEST) return rc;
    }
    return inst_plan_remove(sys, ns, dis, nd, NULL, ops, INST_OPS_MAX, msg, blk);
}

/* canonical multiset "folder|name|type|creator|dir|locked", sorted. The MoofTCP file is left out
 * (Remove deletes it). skip_empty_dis: also leave out an empty 'MacTCP (Disabled)' (only allowed
 * when the manifest, and with it the 'created' flag, did not survive: see the header). */
static int canon(const Sim *s, char out[48][160], int skip_empty_dis) {
    int i, n = 0, j, k;
    for (i = 0; i < s->n; i++) {
        const SimF *f = &s->f[i];
        if (!f->e.is_dir && !strcmp(f->e.name, INST_MOOF_NAME) && f->e.type == CDEV && f->e.creator == ZTCP) continue;
        if (skip_empty_dis && f->e.is_dir && !strcmp(f->e.name, INST_DISABLED)) {
            int used = 0;
            for (j = 0; j < s->n; j++) if (!strcmp(s->f[j].folder, INST_DISABLED)) used = 1;
            if (!used) continue;
        }
        sprintf(out[n++], "%s|%s|%08x|%08x|%d|%d", f->folder, f->e.name, (unsigned)f->e.type,
                (unsigned)f->e.creator, f->e.is_dir, f->e.locked);
    }
    for (j = 1; j < n; j++) {         /* insertion sort */
        char t[160];
        strcpy(t, out[j]);
        for (k = j - 1; k >= 0 && strcmp(out[k], t) > 0; k--) strcpy(out[k + 1], out[k]);
        strcpy(out[k + 1], t);
    }
    return n;
}
static int same(const Sim *a, const Sim *b, int tol) {
    static char ca[48][160], cb[48][160];
    int na = canon(a, ca, 0), nb = canon(b, cb, tol && !has_dis(a)), i;
    if (tol && !has_dis(a)) na = canon(a, ca, 1);
    if (na != nb) return 0;
    for (i = 0; i < na; i++) if (strcmp(ca[i], cb[i])) return 0;
    return 1;
}
static void dump(const Sim *s) {
    int i;
    for (i = 0; i < s->n; i++) fprintf(stderr, "  [%s] %s %08x/%08x d%d l%d\n", s->f[i].folder, s->f[i].e.name,
                                       (unsigned)s->f[i].e.type, (unsigned)s->f[i].e.creator, s->f[i].e.is_dir, s->f[i].e.locked);
    fprintf(stderr, "--\n");
}
static int bothlive(const Sim *s) {      /* a cdev/ztcp MacTCP and MoofTCP together in the System Folder */
    int a = sfind(s, SF, "MacTCP"), b = sfind(s, SF, INST_MOOF_NAME);
    return a >= 0 && b >= 0 && s->f[a].e.type == CDEV && s->f[a].e.creator == ZTCP &&
           s->f[b].e.type == CDEV && s->f[b].e.creator == ZTCP;
}

static void base(Sim *s) {              /* a plausible System Folder */
    memset(s, 0, sizeof *s);
    add(s, SF, "System", INST_FCC('z', 's', 'y', 's'), INST_FCC('M', 'A', 'C', 'S'), 0);
    add(s, SF, "Finder", INST_FCC('F', 'N', 'D', 'R'), INST_FCC('M', 'A', 'C', 'S'), 0);
}
static void trio3(Sim *s) {
    addsys(s, "MacTCP", CDEV, ZTCP); addsys(s, "MacTCP DNR", CDEV, MTCP); addsys(s, "MacTCP Prep", MTPP, MTCP);
}
static void addman(Sim *s, const char *name, int created) {   /* manifest file listing one parked file */
    InstManifest m;
    memset(&m, 0, sizeof m);
    strcpy(m.e[0].name, name); strcpy(m.e[0].folder, SF); m.n = 1; m.created = created;
    adddis(s, INST_MANIFEST_NAME, TEXT, ZTCP);
    put_manifest(&s->f[s->n - 1], &m, INST_MANIFEST_NAME);
}
static void oldmoof(Sim *s, uint32_t vers) {
    addsys(s, "MoofTCP", CDEV, ZTCP);
    s->f[s->n - 1].e.vers = vers;
}
static void mkdis(Sim *s) { add(s, SF, INST_DISABLED, 0, 0, 1); }

/* ---- install cases ---- */
static void install_trio(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s); trio3(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(n, 6);                                  /* mkdir, manifest, 3 moves, write */
    T_EQ(ops[0].kind, INST_MKDIR);
    T_CHECK(!strcmp(ops[0].to, ":System Folder:MacTCP (Disabled)"));
    T_EQ(ops[1].kind, INST_WRITE_MANIFEST);      /* before the first move */
    T_CHECK(!strcmp(ops[1].to, ":System Folder:MacTCP (Disabled):MoofTCP Manifest"));
    T_EQ(ops[2].kind, INST_MOVE); T_EQ(ops[3].kind, INST_MOVE); T_EQ(ops[4].kind, INST_MOVE);
    T_EQ(ops[5].kind, INST_WRITE_MOOF);          /* last */
    T_CHECK(!strcmp(ops[5].to, ":System Folder:MoofTCP"));
    T_CHECK(!strcmp(ops[2].from, ":System Folder:MacTCP"));
    T_CHECK(!strcmp(ops[2].to, ":System Folder:MacTCP (Disabled):MacTCP"));
    T_EQ(man.n, 3); T_EQ(man.created, 1);
    for (i = 0; i < n; i++) T_CHECK(ops[i].kind != INST_DELETE_MOOF && ops[i].kind != INST_DELETE_MANIFEST);
    T_EQ(apply(&s, ops, n, &man), 0);
    T_EQ(sfind(&s, SF, "MacTCP"), -1);
    T_CHECK(sfind(&s, INST_DISABLED, "MacTCP Prep") >= 0);
    T_CHECK(sfind(&s, SF, "MoofTCP") >= 0);
}
static void install_mactcp_only(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); addsys(&s, "MacTCP", CDEV, ZTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP); T_EQ(n, 4); T_EQ(man.n, 1);
    base(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_NO_MACTCP); T_EQ(n, 1); T_EQ(ops[0].kind, INST_WRITE_MOOF);
    T_CHECK(!strcmp(ops[0].to, ":System Folder:MoofTCP"));
    T_EQ(man.n, 0);
}
static void install_none_twice(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(n, 0); T_EQ(m, INST_ALREADY_INSTALLED);
    base(&s); oldmoof(&s, 3);                         /* newer: no downgrade */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0); T_EQ(m, INST_ALREADY_INSTALLED);
    base(&s); oldmoof(&s, 2); addsys(&s, "MacTCP", CDEV, ZTCP);   /* both present: park MacTCP */
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP); T_EQ(n, 3); T_EQ(ops[2].kind, INST_MOVE);
}
static void upgrade_keeps_prefs(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s); oldmoof(&s, 1); addsys(&s, INST_PREFS_NAME, PREF, ZTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_UPGRADED); T_EQ(n, 1); T_EQ(ops[0].kind, INST_WRITE_MOOF);
    for (i = 0; i < n; i++) T_CHECK(!strstr(ops[i].from, "Prefs") && !strstr(ops[i].to, "Prefs"));
    T_EQ(apply(&s, ops, n, &man), 0);
    T_CHECK(sfind(&s, SF, INST_PREFS_NAME) >= 0);
    T_EQ(s.f[sfind(&s, SF, "MoofTCP")].e.vers, 2);
    base(&s); oldmoof(&s, 0); addsys(&s, "MacTCP Prep", MTPP, MTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_UPGRADED); T_EQ(n, 4);
}
static void disabled_exists_unrelated(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, k;
    base(&s); trio3(&s);
    mkdis(&s);
    adddis(&s, "Old Notes", TEXT, INST_FCC('t', 't', 'x', 't'));
    orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(n, 5);                                      /* no MKDIR */
    T_EQ(man.created, 0);
    for (k = 0; k < n; k++) T_CHECK(ops[k].kind != INST_MKDIR);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    for (k = 0; k < n; k++) T_CHECK(ops[k].kind != INST_RMDIR);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 0));
    T_CHECK(sfind(&s, INST_DISABLED, "Old Notes") >= 0);
}
static void collisions_block_install(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); trio3(&s); mkdis(&s);
    adddis(&s, "MacTCP DNR", CDEV, MTCP);            /* someone else's copy */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION); T_CHECK(!strcmp(blk, "MacTCP DNR"));
    adddis(&s, "MacTCP", TEXT, TEXT);                /* same name, other type, still in the way */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
    base(&s); trio3(&s); addsys(&s, INST_DISABLED, TEXT, TEXT);   /* a file named like the folder */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION); T_CHECK(!strcmp(blk, INST_DISABLED));
    base(&s); addsys(&s, "MoofTCP", TEXT, TEXT);     /* MoofTCP not ours */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION); T_CHECK(!strcmp(blk, "MoofTCP"));
    base(&s); add(&s, SF, "MoofTCP", 0, 0, 1);
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
}
static void locks_block(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); trio3(&s); setlock(&s, SF, "MacTCP DNR");
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_LOCKED); T_CHECK(!strcmp(blk, "MacTCP DNR"));
    base(&s); oldmoof(&s, 1); setlock(&s, SF, "MoofTCP");           /* upgrade needs to replace it */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0); T_EQ(m, INST_BLOCKED_LOCKED);
    base(&s); oldmoof(&s, 2); setlock(&s, SF, "MoofTCP");           /* current: not touched */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0); T_EQ(m, INST_ALREADY_INSTALLED);
    /* Remove: a locked parked file, a locked MoofTCP (documented, conservative) */
    base(&s); oldmoof(&s, 2); mkdis(&s);
    adddis(&s, "MacTCP", CDEV, ZTCP); addman(&s, "MacTCP", 1); setlock(&s, INST_DISABLED, "MacTCP");
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_LOCKED); T_CHECK(!strcmp(blk, "MacTCP"));
    base(&s); oldmoof(&s, 2); setlock(&s, SF, "MoofTCP");
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_LOCKED); T_CHECK(!strcmp(blk, "MoofTCP"));
}
static void not_cdev_ztcp_untouched(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s);
    addsys(&s, "MacTCP", TEXT, TEXT);                /* wrong type/creator */
    addsys(&s, "MacTCP DNR", CDEV, ZTCP);            /* creator of the wrong half */
    addsys(&s, "MacTCP Prep", MTCP, MTPP);           /* swapped */
    add(&s, SF, "MacTCP", 0, 0, 1);                  /* a folder */
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_NO_MACTCP); T_EQ(n, 1);
    for (i = 0; i < n; i++) T_EQ(ops[i].kind, INST_WRITE_MOOF);
}
static void case_insensitive(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); addsys(&s, "mactcp prep", MTPP, MTCP); oldmoof(&s, 2);
    s.f[s.n - 1].e.name[0] = 'm';                     /* "moofTCP" */
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP); T_EQ(n, 3);
    T_CHECK(!strcmp(ops[2].from, ":System Folder:mactcp prep"));   /* the actual name is used */
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
}

/* ---- remove cases ---- */
static void remove_with_manifest(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s); oldmoof(&s, 1); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED); T_EQ(n, 6);   /* delete moof, 3 moves, delete manifest, rmdir */
    T_EQ(ops[0].kind, INST_DELETE_MOOF);          /* MoofTCP goes first */
    T_CHECK(!strcmp(ops[0].from, ":System Folder:MoofTCP"));
    T_EQ(ops[1].kind, INST_MOVE);
    T_CHECK(!strcmp(ops[1].from, ":System Folder:MacTCP (Disabled):MacTCP"));
    T_CHECK(!strcmp(ops[1].to, ":System Folder:MacTCP"));
    T_EQ(ops[4].kind, INST_DELETE_MANIFEST);
    T_CHECK(!strcmp(ops[4].from, ":System Folder:MacTCP (Disabled):MoofTCP Manifest"));
    T_EQ(ops[5].kind, INST_RMDIR);
    T_CHECK(!strcmp(ops[5].from, ":System Folder:MacTCP (Disabled)"));
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 0));
}
static void remove_fallback_unreadable(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; int n;
    base(&s); oldmoof(&s, 2); mkdis(&s);
    adddis(&s, "MacTCP", CDEV, ZTCP); adddis(&s, "MacTCP Prep", MTPP, MTCP);
    adddis(&s, "MacTCP DNR", TEXT, TEXT);            /* name matches, type does not: left alone */
    n = do_remove(&s, ops, &m);                      /* no manifest file: nothing of ours is parked */
    T_EQ(m, INST_REMOVED_NO_MACTCP); T_EQ(n, 1);
    adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);      /* manifest file present but corrupt */
    s.f[s.n - 1].clen = 3; memcpy(s.f[s.n - 1].content, "bad", 3);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, SF, "MacTCP") >= 0);
    T_CHECK(sfind(&s, SF, "MacTCP Prep") >= 0);
    T_CHECK(sfind(&s, INST_DISABLED, "MacTCP DNR") >= 0);
    T_EQ(sfind(&s, INST_DISABLED, INST_MANIFEST_NAME), -1);
    T_EQ(sfind(&s, SF, "MoofTCP"), -1);
    T_CHECK(has_dis(&s));                            /* unreadable manifest: never RMDIR */
}
static void remove_nothing_and_temps(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; int n;
    base(&s); trio3(&s);
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_NOTHING_TO_REMOVE);
    /* parked files without MoofTCP or manifest are somebody else's business */
    base(&s); mkdis(&s); adddis(&s, "MacTCP", CDEV, ZTCP);
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_NOTHING_TO_REMOVE);
    /* a leftover MoofTCP.tmp (crash in the first WRITE_MOOF) is ours: deleted */
    base(&s); addsys(&s, INST_MOOF_TMP, CDEV, ZTCP);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_NO_MACTCP); T_EQ(n, 1); T_EQ(ops[0].kind, INST_DELETE_TEMP);
    T_CHECK(!strcmp(ops[0].from, ":System Folder:MoofTCP.tmp"));
    /* manifest only as .tmp (crash between delete X and rename): used and deleted */
    base(&s); oldmoof(&s, 2); mkdis(&s); adddis(&s, "MacTCP", CDEV, ZTCP);
    {
        InstManifest mm; memset(&mm, 0, sizeof mm);
        strcpy(mm.e[0].name, "MacTCP"); strcpy(mm.e[0].folder, SF); mm.n = 1; mm.created = 1;
        adddis(&s, INST_MANIFEST_TMP, TEXT, ZTCP);
        put_manifest(&s.f[s.n - 1], &mm, INST_MANIFEST_TMP);
    }
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, SF, "MacTCP") >= 0);
    T_CHECK(!has_dis(&s));
    T_EQ(ops[n - 2].kind, INST_DELETE_TEMP);
}
static void remove_no_mactcp_and_prefs_kept(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; int n, i;
    base(&s); oldmoof(&s, 2); addsys(&s, INST_PREFS_NAME, PREF, ZTCP);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_NO_MACTCP); T_EQ(n, 1); T_EQ(ops[0].kind, INST_DELETE_MOOF);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, SF, INST_PREFS_NAME) >= 0);
    for (i = 0; i < n; i++) T_CHECK(!strstr(ops[i].from, "Prefs"));
}

/* item 2: the manifest is not trusted */
static int rm_man(const InstManifest *man, Sim *s, InstOp *ops, InstMsg *m) {
    InstEntry sys[48], dis[48]; int ns, nd;
    snap(s, sys, &ns, dis, &nd);
    return inst_plan_remove(sys, ns, dis, nd, man, ops, INST_OPS_MAX, m, blk);
}
static void remove_manifest_untrusted(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, k;
    static const struct { const char *name, *folder; } bad[] = {
        { "My Notes", "Desktop Folder" }, { "My Notes", SF }, { "../", SF }, { "a:b", SF }, { "", SF },
        { INST_MANIFEST_NAME, SF }, { "MacTCP", "Desktop Folder" }, { "MacTCP", "" }, { "MacTCP", SF ":x" },
        { "MacTCP DNR2", SF }, { "0123456789012345678901234567890", SF },
    };
    base(&s); oldmoof(&s, 2); mkdis(&s);
    adddis(&s, "My Notes", TEXT, TEXT); adddis(&s, "MacTCP", CDEV, ZTCP); adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);
    for (k = 0; k < (int)(sizeof bad / sizeof bad[0]); k++) {
        memset(&man, 0, sizeof man);
        strcpy(man.e[0].name, bad[k].name); strcpy(man.e[0].folder, bad[k].folder); man.n = 1;
        T_EQ(rm_man(&man, &s, ops, &m), INST_E_MANIFEST);
    }
    /* one bad line poisons a good one */
    memset(&man, 0, sizeof man);
    strcpy(man.e[0].name, "MacTCP"); strcpy(man.e[0].folder, SF);
    strcpy(man.e[1].name, "My Notes"); strcpy(man.e[1].folder, SF); man.n = 2;
    T_EQ(rm_man(&man, &s, ops, &m), INST_E_MANIFEST);
    /* through the executor path: retried as unreadable, My Notes stays */
    memset(&man, 0, sizeof man);
    strcpy(man.e[0].name, "My Notes"); strcpy(man.e[0].folder, "Desktop Folder"); man.n = 1;
    put_manifest(&s.f[sfind(&s, INST_DISABLED, INST_MANIFEST_NAME)], &man, INST_MANIFEST_NAME);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, INST_DISABLED, "My Notes") >= 0);
    T_CHECK(sfind(&s, SF, "MacTCP") >= 0);
    T_CHECK(has_dis(&s));
    /* valid cases: absent file skipped, duplicates folded, wrong type skipped, created + non-empty = no RMDIR */
    base(&s); oldmoof(&s, 2); mkdis(&s);
    adddis(&s, "My Notes", TEXT, TEXT); adddis(&s, "MacTCP", CDEV, ZTCP);
    adddis(&s, "MacTCP Prep", TEXT, TEXT); adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);
    memset(&man, 0, sizeof man); man.created = 1;
    strcpy(man.e[0].name, "MacTCP"); strcpy(man.e[1].name, "mactcp"); strcpy(man.e[2].name, "MacTCP DNR");
    strcpy(man.e[3].name, "MacTCP Prep");
    for (k = 0; k < 4; k++) strcpy(man.e[k].folder, SF);
    man.n = 4;
    n = rm_man(&man, &s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    T_EQ(n, 3);                                      /* delete moof, ONE move, delete manifest; no RMDIR */
    T_EQ(ops[1].kind, INST_MOVE); T_EQ(ops[2].kind, INST_DELETE_MANIFEST);
    T_EQ(apply(&s, ops, n, NULL), 0);
}
static void created_marker(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, k, rm;
    /* user's own pre-existing EMPTY folder survives install + remove */
    base(&s); trio3(&s); mkdis(&s); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(man.created, 0);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    for (k = 0, rm = 0; k < n; k++) rm |= ops[k].kind == INST_RMDIR;
    T_EQ(rm, 0);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 0));
    T_CHECK(has_dis(&s));
    /* a folder the installer made is removed again, empty folders are compared too */
    base(&s); trio3(&s); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(man.created, 1);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 0));
    T_CHECK(!has_dis(&s));
    /* re-run keeps created */
    base(&s); trio3(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, 3, &man), 0);                /* mkdir, manifest, first move */
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(man.created, 1); T_EQ(man.n, 3);
    (void)n;
}
static void collision_remove_and_readd(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, n, &man), 0);
    addsys(&s, "MacTCP", CDEV, ZTCP);                /* the user put a new one back */
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_COLLISION); T_CHECK(!strcmp(blk, "MacTCP"));
    T_EQ(do_install(&s, 2, ops, &m, &man), 0); T_EQ(m, INST_BLOCKED_COLLISION); T_CHECK(!strcmp(blk, "MacTCP"));
    sdel(&s, sfind(&s, SF, "MacTCP"));
    n = do_remove(&s, ops, &m);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 0));
}

/* ---- re-install with an unreadable manifest (item 3), duplicates (item 7) ---- */
static void reinstall_unreadable_manifest(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, 3, &man), 0);                /* mkdir, manifest, move MacTCP */
    s.f[sfind(&s, INST_DISABLED, INST_MANIFEST_NAME)].clen = 5;   /* truncated */
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(man.n, 3);                                  /* the already parked MacTCP is listed */
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s, 1));
}
static void manifest_dedupe_and_full(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man, prev; int n, i;
    base(&s); addsys(&s, "MacTCP", CDEV, ZTCP); mkdis(&s);
    adddis(&s, "MacTCP DNR", CDEV, MTCP); adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);
    memset(&prev, 0, sizeof prev);
    for (i = 0; i < 8; i++) { strcpy(prev.e[i].name, i & 1 ? "MacTCP DNR" : "mactcp dnr"); strcpy(prev.e[i].folder, SF); }
    prev.n = 8;                                      /* a full manifest of duplicates */
    {
        InstEntry sys[48], dis[48]; int ns, nd;
        snap(&s, sys, &ns, dis, &nd);
        n = inst_plan_install(sys, ns, dis, nd, 2, &prev, ops, INST_OPS_MAX, &m, blk, &man);
    }
    T_CHECK(n > 0);
    T_EQ(man.n, 2);                                  /* MacTCP DNR once + the new MacTCP */
}

/* ---- manifest codec ---- */
static void manifest_codec(void) {
    static const char *names[] = { "MacTCP", "MacTCP DNR", "Odd (name) -1.0 \xC5\x8E", "a", "0123456789012345678901234567890" };
    static const char *bad[] = { "a\tb", "a\rb", "a\nb", "a:b", "", "x\x01y", "0123456789012345678901234567890X" };
    InstManifest m, d; char buf[600]; int n, i; size_t k;
    memset(&m, 0, sizeof m);
    for (i = 0; i < 5; i++) { strcpy(m.e[i].name, names[i]); strcpy(m.e[i].folder, i & 1 ? "Control Panels" : SF); }
    m.n = 5;
    n = inst_manifest_encode(&m, buf, sizeof buf);
    T_CHECK(n > 0); T_EQ(buf[n - 1], '\r');
    T_EQ(inst_manifest_decode(buf, (size_t)n, &d), 5);
    T_EQ(d.created, 0);                              /* older format = not created */
    for (i = 0; i < 5; i++) { T_CHECK(!strcmp(d.e[i].name, m.e[i].name)); T_CHECK(!strcmp(d.e[i].folder, m.e[i].folder)); }
    m.created = 1;
    n = inst_manifest_encode(&m, buf, sizeof buf);
    T_CHECK(!memcmp(buf, "#created\r", 9));
    T_EQ(inst_manifest_decode(buf, (size_t)n, &d), 5); T_EQ(d.created, 1);
    T_EQ(inst_manifest_decode("#created\r", 9, &d), 0); T_EQ(d.created, 1);
    T_EQ(inst_manifest_decode("#created", 8, &d), INST_E_MANIFEST);
    m.n = 0; m.created = 0;
    T_EQ(inst_manifest_encode(&m, buf, sizeof buf), 0);
    T_EQ(inst_manifest_decode("", 0, &d), 0);
    m.n = 1; strcpy(m.e[0].name, "ab"); strcpy(m.e[0].folder, "cd");
    T_EQ(inst_manifest_encode(&m, buf, 6), 6);
    T_EQ(inst_manifest_encode(&m, buf, 5), INST_E_CAP);
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        memset(&m, 0, sizeof m);
        memcpy(m.e[0].name, bad[k], k == 6 ? 32 : strlen(bad[k]) + 1);   /* 32 bytes: no NUL at all */
        strcpy(m.e[0].folder, SF); m.n = 1;
        T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
        memset(&m, 0, sizeof m);
        strcpy(m.e[0].name, "ok"); memcpy(m.e[0].folder, bad[k], k == 6 ? 32 : strlen(bad[k]) + 1); m.n = 1;
        T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
    }
    m.n = INST_MAN_MAX + 1;
    T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
}
static void manifest_decode_strict(void) {
    static const char *bad[] = {
        "MacTCP\tSystem Folder", "MacTCP\r", "MacTCP\tA\tB\r", "\tSystem Folder\r", "MacTCP\t\r",
        "MacTCP\tSystem Folder\r\r", "MacTCP\tSystem:Folder\r", "Mac\nTCP\tSystem Folder\r",
        "MacTCP\tSystem Folder\rMacTCP DNR\tSyst",
    };
    InstManifest m; size_t i; int k;
    char big[400];
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) T_EQ(inst_manifest_decode(bad[i], strlen(bad[i]), &m), INST_E_MANIFEST);
    big[0] = 0;
    for (k = 0; k < INST_MAN_MAX + 1; k++) strcat(big, "n\tf\r");
    T_EQ(inst_manifest_decode(big, strlen(big), &m), INST_E_MANIFEST);
    T_EQ(m.n, 0);
}

static void arg_and_cap_errors(void) {
    InstEntry e[2]; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    memset(e, 0, sizeof e);
    T_EQ(inst_plan_install(NULL, 1, NULL, -1, 2, NULL, ops, INST_OPS_MAX, &m, NULL, &man), INST_E_ARG);
    T_EQ(inst_plan_install(e, -1, NULL, -1, 2, NULL, ops, INST_OPS_MAX, &m, NULL, &man), INST_E_ARG);
    T_EQ(inst_plan_install(NULL, 0, NULL, 3, 2, NULL, ops, INST_OPS_MAX, &m, NULL, &man), INST_E_ARG);
    T_EQ(inst_plan_remove(NULL, 1, NULL, -1, NULL, ops, INST_OPS_MAX, &m, NULL), INST_E_ARG);
    T_EQ(inst_plan_install(NULL, 0, NULL, -1, 2, NULL, ops, 0, &m, NULL, &man), INST_E_CAP);   /* needs 1 op */
    memset(e[0].name, 'x', sizeof e[0].name);        /* no NUL: refused, never read past the array */
    T_EQ(inst_plan_install(e, 1, NULL, -1, 2, NULL, ops, INST_OPS_MAX, &m, NULL, &man), INST_E_ARG);
    T_EQ(inst_plan_remove(e, 1, NULL, -1, NULL, ops, INST_OPS_MAX, &m, NULL), INST_E_ARG);
    T_EQ(inst_plan_remove(NULL, 0, e, 1, NULL, ops, INST_OPS_MAX, &m, NULL), INST_E_ARG);
    {
        Sim s; InstEntry sys[48], dis[48]; int ns, nd;
        base(&s); trio3(&s);
        T_EQ(do_install(&s, 2, ops, &m, &man), 6);
        snap(&s, sys, &ns, dis, &nd);
        T_EQ(inst_plan_install(sys, ns, dis, nd, 2, NULL, ops, 5, &m, NULL, &man), INST_E_CAP);
        base(&s); oldmoof(&s, 2); mkdis(&s); adddis(&s, "MacTCP", CDEV, ZTCP); addman(&s, "MacTCP", 1);
        snap(&s, sys, &ns, dis, &nd);
        T_EQ(inst_plan_remove(sys, ns, dis, nd, NULL, ops, 2, &m, NULL), INST_E_CAP);
    }
}

/* every op's from/to is NUL-terminated inside the array and the cells after the list are untouched */
static void path_canary(void) {
    Sim s[6]; InstOp ops[INST_OPS_MAX + 1]; InstMsg m; InstManifest man; int i, k, n, kinds = 0;
    for (i = 0; i < 6; i++) base(&s[i]);
    trio3(&s[0]);                                            /* install: mkdir, manifest, moves, write */
    oldmoof(&s[1], 1); trio3(&s[1]);
    mkdis(&s[2]); trio3(&s[2]);
    oldmoof(&s[3], 2); mkdis(&s[3]); adddis(&s[3], "MacTCP", CDEV, ZTCP); addman(&s[3], "MacTCP", 1);
    adddis(&s[3], INST_MANIFEST_TMP, TEXT, ZTCP); addsys(&s[3], INST_MOOF_TMP, CDEV, ZTCP);
    for (i = 0; i < 4; i++) {
        memset(ops, 0xAA, sizeof ops);
        n = i < 3 ? do_install(&s[i], 2, ops, &m, &man) : do_remove(&s[i], ops, &m);
        T_CHECK(n > 0);
        for (k = 0; k < n; k++) {
            T_CHECK(memchr(ops[k].from, 0, INST_PATH_MAX) != NULL);
            T_CHECK(memchr(ops[k].to, 0, INST_PATH_MAX) != NULL);
            T_CHECK(strlen(ops[k].from) < INST_PATH_MAX && strlen(ops[k].to) < INST_PATH_MAX);
            kinds |= 1 << ops[k].kind;
        }
        for (k = n; k <= INST_OPS_MAX; k++) {                /* untouched cells */
            unsigned char *p = (unsigned char *)&ops[k];
            size_t z;
            for (z = 0; z < sizeof ops[k]; z++) T_EQ(p[z], 0xAA);
        }
    }
    T_EQ(kinds, 0xFF);                                       /* all 8 op kinds were exercised */
    /* 31-byte names in the snapshot (unrelated files) change nothing and overflow nothing */
    base(&s[4]); addsys(&s[4], "0123456789012345678901234567890", TEXT, TEXT); trio3(&s[4]); mkdis(&s[4]);
    adddis(&s[4], "0123456789012345678901234567890", TEXT, TEXT);
    n = do_install(&s[4], 2, ops, &m, &man);
    T_EQ(n, 5);
    /* a hostile 31-byte manifest entry is refused (it once overflowed the path buffer) */
    base(&s[5]); oldmoof(&s[5], 2); mkdis(&s[5]);
    adddis(&s[5], "0123456789012345678901234567890", TEXT, TEXT); adddis(&s[5], INST_MANIFEST_NAME, TEXT, ZTCP);
    memset(&man, 0, sizeof man);
    strcpy(man.e[0].name, "0123456789012345678901234567890"); strcpy(man.e[0].folder, "0123456789012345678901234567890"); man.n = 1;
    T_EQ(rm_man(&man, &s[5], ops, &m), INST_E_MANIFEST);
}

/* ---- properties over random snapshots ---- */
static uint32_t rng_state;
static unsigned rnd(unsigned n) { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) % n; }

static int has_foreign(const Sim *s) {      /* a MacTCP-set file in the Disabled folder the installer did not park */
    int i;
    for (i = 0; i < s->n; i++) {
        const SimF *f = &s->f[i];
        if (strcmp(f->folder, INST_DISABLED)) continue;
        if (!strcmp(f->e.name, "MacTCP Prep") && f->e.type == MTPP) return 1;
    }
    return 0;
}
static void gen(Sim *s) {
    static const char *other[] = { "Finder", "System", "Clipboard", "Scrapbook File", "Mac TCP", "MacTCP2",
                                   "0123456789012345678901234567890" };
    int i;
    base(s);
    for (i = 0; i < 4; i++) if (rnd(2)) addsys(s, other[rnd(7)], TEXT, TEXT);
    if (rnd(5) < 4) addsys(s, "MacTCP", rnd(8) ? CDEV : TEXT, ZTCP);
    if (rnd(2)) addsys(s, "MacTCP DNR", CDEV, MTCP);
    if (rnd(2)) addsys(s, "MacTCP Prep", MTPP, MTCP);
    if (rnd(3) == 0) oldmoof(s, (uint32_t)rnd(3));              /* 0,1: older; 2: current */
    if (rnd(2)) addsys(s, INST_PREFS_NAME, PREF, ZTCP);
    if (rnd(2)) {
        mkdis(s);                                               /* often empty: a user's own empty folder */
        if (rnd(3) == 0) adddis(s, "Notes", TEXT, TEXT);
        if (rnd(3) == 0) adddis(s, other[rnd(7)], TEXT, TEXT);
        if (rnd(4) == 0) adddis(s, "MacTCP Prep", MTPP, MTCP);  /* may collide: skipped when blocked */
    }
    if (rnd(6) == 0 && s->n > 2) s->f[rnd((unsigned)s->n)].e.locked = 1;
}

/* from a (possibly half-done) state p: (a) a fresh Remove and (b) a re-run Install then Remove
 * must both restore `orig` (an empty 'MacTCP (Disabled)' that no manifest vouches for may stay) */
static int recover(const Sim *orig, const Sim *p, const char *tag, int it, int k) {
    InstManifest pm;
    int st = read_man(p, &pm), tol = st != 1, checked = 0;
    InstOp r[INST_OPS_MAX], i2[INST_OPS_MAX]; InstMsg m2; InstManifest man2; int rn, in;
    Sim q;
    if (has_foreign(orig) && st == 0) return 0;      /* ambiguous by design: foreign file + unreadable manifest */
    q = *p;
    rn = do_remove(&q, r, &m2);
    T_CHECK(rn >= 0);
    if (m2 != INST_BLOCKED_LOCKED && m2 != INST_BLOCKED_COLLISION) {
        T_EQ(apply(&q, r, rn, NULL), 0);
        if (!same(orig, &q, tol)) { fprintf(stderr, "%s remove it %d prefix %d\n", tag, it, k); dump(orig); dump(&q); T_CHECK(0); return checked; }
        checked++;
    }
    q = *p;
    in = do_install(&q, 2, i2, &m2, &man2);
    T_CHECK(in >= 0);
    if (m2 != INST_BLOCKED_COLLISION && m2 != INST_BLOCKED_LOCKED) {
        T_EQ(apply(&q, i2, in, &man2), 0);
        rn = do_remove(&q, r, &m2);
        if (m2 != INST_BLOCKED_LOCKED && m2 != INST_BLOCKED_COLLISION) {
            T_EQ(apply(&q, r, rn, NULL), 0);
            if (!same(orig, &q, tol)) { fprintf(stderr, "%s reinstall it %d prefix %d\n", tag, it, k); dump(orig); dump(&q); T_CHECK(0); return checked; }
            checked++;
        }
    }
    return checked;
}

static void prop_install_remove(void) {
    int it, ran = 0, blocked = 0, readd = 0;
    rng_state = 12345;
    for (it = 0; it < 3000; it++) {
        Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, k;
        gen(&s); orig = s;
        n = do_install(&s, 2, ops, &m, &man);
        T_CHECK(n >= 0);
        if (m == INST_BLOCKED_COLLISION || m == INST_BLOCKED_LOCKED) { T_EQ(n, 0); T_CHECK(same(&orig, &s, 0)); blocked++; continue; }
        T_EQ(apply(&s, ops, n, &man), 0);
        for (k = 0; k < n; k++) T_CHECK(ops[k].kind != INST_DELETE_MANIFEST && ops[k].kind != INST_DELETE_MOOF);
        if (sfind(&s, INST_DISABLED, "MacTCP") >= 0 && rnd(4) == 0) {   /* the user re-adds MacTCP between the steps */
            Sim t = s; InstOp o2[INST_OPS_MAX]; InstMsg m2; InstManifest mm; int n2;
            addsys(&t, "MacTCP", CDEV, ZTCP);
            n2 = do_remove(&t, o2, &m2);
            T_EQ(n2, 0); T_EQ(m2, INST_BLOCKED_COLLISION);
            n2 = do_install(&t, 2, o2, &m2, &mm);
            T_EQ(n2, 0); T_EQ(m2, INST_BLOCKED_COLLISION);
            sdel(&t, sfind(&t, SF, "MacTCP"));
            s = t; readd++;
        }
        n = do_remove(&s, ops, &m);
        T_CHECK(n >= 0);
        if (m == INST_BLOCKED_LOCKED) { blocked++; continue; }           /* a lock the install did not need to touch */
        T_EQ(apply(&s, ops, n, NULL), 0);
        if (!same(&orig, &s, 0)) { fprintf(stderr, "iteration %d\n", it); dump(&orig); dump(&s); T_CHECK(0); return; }
        ran++;
    }
    T_CHECK(ran > 1000);
    T_CHECK(blocked > 0);
    T_CHECK(readd > 20);
}

static void prop_install_prefixes(void) {
    int it, k, v, checked = 0, partials = 0;
    rng_state = 777;
    for (it = 0; it < 1500; it++) {
        Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
        gen(&s); orig = s;
        n = do_install(&s, 2, ops, &m, &man);
        if (n <= 0) continue;
        for (k = 0; k <= n; k++) {
            Sim p = orig;
            T_EQ(apply(&p, ops, k, &man), 0);
            checked += recover(&orig, &p, "prefix", it, k);
            if (k < n && (ops[k].kind == INST_WRITE_MANIFEST || ops[k].kind == INST_WRITE_MOOF)) {
                for (v = 0; v < (ops[k].kind == INST_WRITE_MANIFEST ? 3 : 2); v++) {
                    Sim h = p;
                    partial(&h, &ops[k], &man, v);
                    checked += recover(&orig, &h, "half-op", it, k * 10 + v);
                    partials++;
                }
            }
        }
    }
    T_CHECK(checked > 3000);
    T_CHECK(partials > 1000);
}

static void prop_remove_prefixes(void) {
    int it, k, checked = 0;
    rng_state = 4242;
    for (it = 0; it < 1500; it++) {
        Sim s, orig; InstOp ops[INST_OPS_MAX], r[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, rn;
        gen(&s); orig = s;
        n = do_install(&s, 2, ops, &m, &man);
        if (n <= 0 || m == INST_BLOCKED_COLLISION || m == INST_BLOCKED_LOCKED) continue;
        T_EQ(apply(&s, ops, n, &man), 0);
        rn = do_remove(&s, r, &m);
        if (rn <= 0) continue;
        for (k = 0; k <= rn; k++) {
            Sim p = s; InstOp r2[INST_OPS_MAX]; InstMsg m2; InstManifest pm; int n2;
            T_EQ(apply(&p, r, k, NULL), 0);
            T_CHECK(!bothlive(&p));                  /* never two .IPP providers because of Remove */
            n2 = do_remove(&p, r2, &m2);
            T_CHECK(n2 >= 0);
            if (m2 == INST_BLOCKED_LOCKED) continue;
            T_EQ(apply(&p, r2, n2, NULL), 0);
            if (!same(&orig, &p, read_man(&s, &pm) != 1 || k > 0)) {      /* after DELETE_MANIFEST the folder may stay */
                fprintf(stderr, "remove it %d prefix %d\n", it, k); dump(&orig); dump(&p); T_CHECK(0); return;
            }
            checked++;
        }
    }
    T_CHECK(checked > 1000);
}

T_MAIN(install_trio, install_mactcp_only, install_none_twice, upgrade_keeps_prefs,
       disabled_exists_unrelated, collisions_block_install, locks_block, not_cdev_ztcp_untouched,
       case_insensitive, remove_with_manifest, remove_fallback_unreadable, remove_nothing_and_temps,
       remove_no_mactcp_and_prefs_kept, remove_manifest_untrusted, created_marker,
       collision_remove_and_readd, reinstall_unreadable_manifest, manifest_dedupe_and_full,
       manifest_codec, manifest_decode_strict, arg_and_cap_errors, path_canary, prop_install_remove,
       prop_install_prefixes, prop_remove_prefixes)
