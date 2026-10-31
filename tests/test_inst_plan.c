/* test_inst_plan.c - Plan 3 Task 9: the installer planner (mac/installer/inst_plan.[ch]; Review
 * Focus 1): cases, manifest codec, and properties over a simulated System Folder (install then
 * remove restores the original, for every crash prefix of both op lists). */
#include "t.h"
#include "inst_plan.h"

#define CDEV INST_FCC('c', 'd', 'e', 'v')
#define ZTCP INST_FCC('z', 't', 'c', 'p')
#define MTCP INST_FCC('m', 't', 'c', 'p')
#define MTPP INST_FCC('m', 't', 'p', 'p')
#define PREF INST_FCC('p', 'r', 'e', 'f')
#define TEXT INST_FCC('T', 'E', 'X', 'T')

/* ---- simulated volume: files tagged with their folder ---- */
typedef struct { char folder[32]; InstEntry e; char content[256]; int clen; } SimF;
typedef struct { SimF f[48]; int n; } Sim;

static void add(Sim *s, const char *folder, const char *name, uint32_t type, uint32_t cr, int is_dir) {
    SimF *f = &s->f[s->n++];
    memset(f, 0, sizeof *f);
    strcpy(f->folder, folder); strcpy(f->e.name, name);
    f->e.type = type; f->e.creator = cr; f->e.is_dir = (uint8_t)is_dir;
}
static void addsys(Sim *s, const char *name, uint32_t type, uint32_t cr) { add(s, "System Folder", name, type, cr, 0); }
static void adddis(Sim *s, const char *name, uint32_t type, uint32_t cr) { add(s, INST_DISABLED, name, type, cr, 0); }
static int sfind(const Sim *s, const char *folder, const char *name) {
    int i;
    for (i = 0; i < s->n; i++) if (!strcmp(s->f[i].folder, folder) && !strcmp(s->f[i].e.name, name)) return i;
    return -1;
}
static void setlock(Sim *s, const char *folder, const char *name) {
    int i = sfind(s, folder, name);
    T_CHECK(i >= 0);
    if (i >= 0) s->f[i].e.locked = 1;
}
static void snap(const Sim *s, InstEntry *sys, int *nsys, InstEntry *dis, int *ndis) {
    int i;
    *nsys = 0; *ndis = (sfind(s, "System Folder", INST_DISABLED) >= 0 && s->f[sfind(s, "System Folder", INST_DISABLED)].e.is_dir) ? 0 : -1;
    for (i = 0; i < s->n; i++) {
        if (!strcmp(s->f[i].folder, "System Folder")) sys[(*nsys)++] = s->f[i].e;
        else if (!strcmp(s->f[i].folder, INST_DISABLED)) dis[(*ndis)++] = s->f[i].e;
    }
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
        s->f[i] = s->f[--s->n];
        return 0;
    case INST_WRITE_MANIFEST:
    case INST_WRITE_MOOF:
        nb = parse_path(o->to, b);
        if (nb < 2) return 1;
        i = sfind(s, b[nb - 2], b[nb - 1]);
        if (i >= 0 && (s->f[i].e.locked || s->f[i].e.is_dir)) return 1;
        if (i < 0) { add(s, b[nb - 2], b[nb - 1], 0, 0, 0); i = s->n - 1; }
        if (o->kind == INST_WRITE_MANIFEST) {
            s->f[i].e.type = TEXT; s->f[i].e.creator = ZTCP;
            s->f[i].clen = inst_manifest_encode(man, s->f[i].content, sizeof s->f[i].content);
            if (s->f[i].clen < 0) return 1;
        } else {
            s->f[i].e.type = CDEV; s->f[i].e.creator = ZTCP; s->f[i].e.vers = 2;
        }
        return 0;
    case INST_DELETE_MOOF:
    case INST_DELETE_MANIFEST:
        na = parse_path(o->from, a);
        i = sfind(s, a[na - 2], a[na - 1]);
        if (i < 0 || s->f[i].e.locked) return 1;
        s->f[i] = s->f[--s->n];
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

static int do_install(const Sim *s, uint32_t vers, InstOp *ops, InstMsg *msg, InstManifest *man) {
    InstEntry sys[48], dis[48];
    InstManifest prev;
    int ns, nd, i = sfind(s, INST_DISABLED, INST_MANIFEST_NAME);
    snap(s, sys, &ns, dis, &nd);
    if (i >= 0 && inst_manifest_decode(s->f[i].content, (size_t)s->f[i].clen, &prev) >= 0)
        return inst_plan_install(sys, ns, dis, nd, vers, &prev, ops, INST_OPS_MAX, msg, man);
    return inst_plan_install(sys, ns, dis, nd, vers, NULL, ops, INST_OPS_MAX, msg, man);
}
/* remove: reads the manifest file out of the simulation like the executor would */
static int do_remove(const Sim *s, InstOp *ops, InstMsg *msg) {
    InstEntry sys[48], dis[48];
    InstManifest m;
    int ns, nd, i = sfind(s, INST_DISABLED, INST_MANIFEST_NAME);
    snap(s, sys, &ns, dis, &nd);
    if (i >= 0 && inst_manifest_decode(s->f[i].content, (size_t)s->f[i].clen, &m) >= 0)
        return inst_plan_remove(sys, ns, dis, nd, &m, ops, INST_OPS_MAX, msg);
    return inst_plan_remove(sys, ns, dis, nd, NULL, ops, INST_OPS_MAX, msg);
}

/* canonical multiset: "folder|name|type|creator|dir|locked" sorted; the MoofTCP file and an empty
 * 'MacTCP (Disabled)' are left out (Rulings: Remove deletes MoofTCP; a crash right after MKDIR
 * leaves an empty folder) */
static int canon(const Sim *s, char out[48][160]) {
    int i, n = 0, j, k;
    for (i = 0; i < s->n; i++) {
        const SimF *f = &s->f[i];
        if (!f->e.is_dir && !strcmp(f->e.name, INST_MOOF_NAME) && f->e.type == CDEV && f->e.creator == ZTCP) continue;
        if (f->e.is_dir && !strcmp(f->e.name, INST_DISABLED)) {
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
static int same(const Sim *a, const Sim *b) {
    static char ca[48][160], cb[48][160];
    int na = canon(a, ca), nb = canon(b, cb), i;
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
static void base(Sim *s) {              /* a plausible System Folder */
    memset(s, 0, sizeof *s);
    add(s, "System Folder", "System", INST_FCC('z', 's', 'y', 's'), INST_FCC('M', 'A', 'C', 'S'), 0);
    add(s, "System Folder", "Finder", INST_FCC('F', 'N', 'D', 'R'), INST_FCC('M', 'A', 'C', 'S'), 0);
}
static void trio3(Sim *s) {
    addsys(s, "MacTCP", CDEV, ZTCP); addsys(s, "MacTCP DNR", CDEV, MTCP); addsys(s, "MacTCP Prep", MTPP, MTCP);
}
static void addman(Sim *s, const char *name) {          /* manifest file listing one parked file */
    InstManifest m;
    memset(&m, 0, sizeof m);
    strcpy(m.e[0].name, name); strcpy(m.e[0].folder, "System Folder"); m.n = 1;
    adddis(s, INST_MANIFEST_NAME, TEXT, ZTCP);
    s->f[s->n - 1].clen = inst_manifest_encode(&m, s->f[s->n - 1].content, sizeof s->f[s->n - 1].content);
}
static void oldmoof(Sim *s, uint32_t vers) {
    addsys(s, "MoofTCP", CDEV, ZTCP);
    s->f[s->n - 1].e.vers = vers;
}

/* ---- cases ---- */
static void install_trio(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s); trio3(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(n, 6);                                  /* mkdir, manifest, 3 moves, write */
    T_EQ(ops[0].kind, INST_MKDIR);
    T_CHECK(!strcmp(ops[0].to, ":System Folder:MacTCP (Disabled)"));
    T_EQ(ops[1].kind, INST_WRITE_MANIFEST);      /* before the first move */
    T_EQ(ops[2].kind, INST_MOVE); T_EQ(ops[3].kind, INST_MOVE); T_EQ(ops[4].kind, INST_MOVE);
    T_EQ(ops[5].kind, INST_WRITE_MOOF);          /* last */
    T_CHECK(!strcmp(ops[2].from, ":System Folder:MacTCP"));
    T_CHECK(!strcmp(ops[2].to, ":System Folder:MacTCP (Disabled):MacTCP"));
    T_EQ(man.n, 3);
    for (i = 0; i < n; i++) T_CHECK(ops[i].kind != INST_DELETE_MOOF && ops[i].kind != INST_DELETE_MANIFEST);
    T_EQ(apply(&s, ops, n, &man), 0);
    T_EQ(sfind(&s, "System Folder", "MacTCP"), -1);
    T_CHECK(sfind(&s, INST_DISABLED, "MacTCP Prep") >= 0);
    T_CHECK(sfind(&s, "System Folder", "MoofTCP") >= 0);
}
static void install_mactcp_only(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); addsys(&s, "MacTCP", CDEV, ZTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP); T_EQ(n, 4); T_EQ(man.n, 1);
}
static void install_none(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_NO_MACTCP); T_EQ(n, 1); T_EQ(ops[0].kind, INST_WRITE_MOOF);
    T_CHECK(!strcmp(ops[0].to, ":System Folder:MoofTCP"));
    T_EQ(man.n, 0);
}
static void install_twice(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(n, 0); T_EQ(m, INST_ALREADY_INSTALLED);
}
static void install_moof_current_mactcp_back(void) {   /* both installed: park MacTCP, keep MoofTCP */
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); oldmoof(&s, 2); addsys(&s, "MacTCP", CDEV, ZTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP); T_EQ(n, 3);   /* mkdir, manifest, move: no WRITE_MOOF */
    T_EQ(ops[2].kind, INST_MOVE);
}
static void install_newer_moof_left_alone(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); oldmoof(&s, 3);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(n, 0); T_EQ(m, INST_ALREADY_INSTALLED);
}
static void upgrade_keeps_prefs(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s); oldmoof(&s, 1); addsys(&s, INST_PREFS_NAME, PREF, ZTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_UPGRADED); T_EQ(n, 1); T_EQ(ops[0].kind, INST_WRITE_MOOF);
    for (i = 0; i < n; i++) T_CHECK(!strstr(ops[i].from, "Prefs") && !strstr(ops[i].to, "Prefs"));
    T_EQ(apply(&s, ops, n, &man), 0);
    T_CHECK(sfind(&s, "System Folder", INST_PREFS_NAME) >= 0);
    T_EQ(s.f[sfind(&s, "System Folder", "MoofTCP")].e.vers, 2);
}
static void upgrade_with_mactcp(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); oldmoof(&s, 0); addsys(&s, "MacTCP Prep", MTPP, MTCP);
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_UPGRADED); T_EQ(n, 4);
}
static void disabled_exists_unrelated(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, k;
    base(&s); trio3(&s);
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "Old Notes", TEXT, INST_FCC('t', 't', 'x', 't'));
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(m, INST_INSTALLED_PARKED_MACTCP);
    T_EQ(n, 5);                                      /* no MKDIR */
    for (k = 0; k < n; k++) T_CHECK(ops[k].kind != INST_MKDIR);
    {
        Sim orig = s;
        T_EQ(apply(&s, ops, n, &man), 0);
        T_CHECK(sfind(&s, INST_DISABLED, "Old Notes") >= 0);
        n = do_remove(&s, ops, &m);
        T_EQ(m, INST_REMOVED_RESTORED);
        T_EQ(apply(&s, ops, n, NULL), 0);
        T_CHECK(same(&orig, &s));
        T_CHECK(sfind(&s, INST_DISABLED, "Old Notes") >= 0);   /* unrelated file stays, so does the folder */
        T_CHECK(sfind(&s, "System Folder", INST_DISABLED) >= 0);
    }
}
static void collision_install(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); trio3(&s);
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "MacTCP DNR", CDEV, MTCP);            /* someone else's copy */
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
    /* same name, other type, still in the way */
    adddis(&s, "MacTCP", TEXT, TEXT);
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
}
static void collision_file_named_like_folder(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); trio3(&s); addsys(&s, INST_DISABLED, TEXT, TEXT);
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
}
static void collision_moof_not_ours(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); addsys(&s, "MoofTCP", TEXT, TEXT);
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
    base(&s); add(&s, "System Folder", "MoofTCP", 0, 0, 1);
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_COLLISION);
}
static void locked_install(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); trio3(&s); setlock(&s, "System Folder", "MacTCP DNR");
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_LOCKED);
}
static void locked_old_moof(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    base(&s); oldmoof(&s, 1); setlock(&s, "System Folder", "MoofTCP");
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_BLOCKED_LOCKED);
    /* a locked MoofTCP that is current is not touched, so nothing blocks */
    base(&s); oldmoof(&s, 2); setlock(&s, "System Folder", "MoofTCP");
    T_EQ(do_install(&s, 2, ops, &m, &man), 0);
    T_EQ(m, INST_ALREADY_INSTALLED);
}
static void not_cdev_ztcp_untouched(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n, i;
    base(&s);
    addsys(&s, "MacTCP", TEXT, TEXT);                /* wrong type/creator */
    addsys(&s, "MacTCP DNR", CDEV, ZTCP);            /* creator of the wrong half */
    addsys(&s, "MacTCP Prep", MTCP, MTPP);           /* swapped */
    add(&s, "System Folder", "MacTCP", 0, 0, 1);     /* a folder (duplicate name is fine in the sim) */
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

/* ---- remove ---- */
static void remove_with_manifest(void) {
    Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
    base(&s); trio3(&s); orig = s;
    n = do_install(&s, 2, ops, &m, &man);
    T_EQ(apply(&s, ops, n, &man), 0);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED); T_EQ(n, 6);      /* 3 moves, delete moof, delete manifest, rmdir */
    T_EQ(ops[0].kind, INST_MOVE);
    T_CHECK(!strcmp(ops[0].to, ":System Folder:MacTCP"));
    T_EQ(ops[3].kind, INST_DELETE_MOOF); T_EQ(ops[4].kind, INST_DELETE_MANIFEST); T_EQ(ops[5].kind, INST_RMDIR);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(same(&orig, &s));
}
static void remove_fallback_without_manifest(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; int n;
    base(&s); oldmoof(&s, 2);
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "MacTCP", CDEV, ZTCP); adddis(&s, "MacTCP Prep", MTPP, MTCP);
    adddis(&s, "MacTCP DNR", TEXT, TEXT);            /* name matches, type does not: left alone */
    n = do_remove(&s, ops, &m);                      /* no manifest file: nothing of ours is parked */
    T_EQ(m, INST_REMOVED_NO_MACTCP); T_EQ(n, 1);
    adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);      /* manifest file present but empty/corrupt */
    s.f[s.n - 1].clen = 3; memcpy(s.f[s.n - 1].content, "bad", 3);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_RESTORED);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, "System Folder", "MacTCP") >= 0);
    T_CHECK(sfind(&s, "System Folder", "MacTCP Prep") >= 0);
    T_CHECK(sfind(&s, INST_DISABLED, "MacTCP DNR") >= 0);
    T_EQ(sfind(&s, INST_DISABLED, INST_MANIFEST_NAME), -1);
    T_EQ(sfind(&s, "System Folder", "MoofTCP"), -1);
    T_CHECK(sfind(&s, "System Folder", INST_DISABLED) >= 0);   /* not empty: no RMDIR */
}
static void remove_nothing(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m;
    base(&s); trio3(&s);
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_NOTHING_TO_REMOVE);
    /* parked files without MoofTCP or manifest are somebody else's business */
    base(&s); add(&s, "System Folder", INST_DISABLED, 0, 0, 1); adddis(&s, "MacTCP", CDEV, ZTCP);
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_NOTHING_TO_REMOVE);
}
static void remove_no_mactcp_and_prefs_kept(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; int n, i;
    base(&s); oldmoof(&s, 2); addsys(&s, INST_PREFS_NAME, PREF, ZTCP);
    n = do_remove(&s, ops, &m);
    T_EQ(m, INST_REMOVED_NO_MACTCP); T_EQ(n, 1); T_EQ(ops[0].kind, INST_DELETE_MOOF);
    T_EQ(apply(&s, ops, n, NULL), 0);
    T_CHECK(sfind(&s, "System Folder", INST_PREFS_NAME) >= 0);
    for (i = 0; i < n; i++) T_CHECK(!strstr(ops[i].from, "Prefs"));
}
static void remove_other_folder_from_manifest(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; InstEntry sys[8], dis[8]; int n;
    memset(&man, 0, sizeof man);
    strcpy(man.e[0].name, "MacTCP"); strcpy(man.e[0].folder, "Control Panels"); man.n = 1;
    base(&s); oldmoof(&s, 2);
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "MacTCP", CDEV, ZTCP);
    adddis(&s, INST_MANIFEST_NAME, TEXT, ZTCP);
    {
        int ns, nd;
        snap(&s, sys, &ns, dis, &nd);
        n = inst_plan_remove(sys, ns, dis, nd, &man, ops, INST_OPS_MAX, &m);
    }
    T_EQ(m, INST_REMOVED_RESTORED); T_EQ(n, 4);      /* move, moof, manifest, rmdir */
    T_CHECK(!strcmp(ops[0].to, ":Control Panels:MacTCP"));
}
static void collision_remove(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m;
    base(&s); oldmoof(&s, 2);
    addsys(&s, "MacTCP", CDEV, ZTCP);                /* the user put a new one back */
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "MacTCP", CDEV, ZTCP); addman(&s, "MacTCP");
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_COLLISION);
}
static void locked_remove(void) {
    Sim s; InstOp ops[INST_OPS_MAX]; InstMsg m;
    base(&s); oldmoof(&s, 2);
    add(&s, "System Folder", INST_DISABLED, 0, 0, 1);
    adddis(&s, "MacTCP", CDEV, ZTCP); addman(&s, "MacTCP"); setlock(&s, INST_DISABLED, "MacTCP");
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_LOCKED);
    s.n = 0; base(&s); oldmoof(&s, 2); setlock(&s, "System Folder", "MoofTCP");
    T_EQ(do_remove(&s, ops, &m), 0); T_EQ(m, INST_BLOCKED_LOCKED);
}

/* ---- manifest codec ---- */
static void manifest_roundtrip(void) {
    static const char *names[] = { "MacTCP", "MacTCP DNR", "Odd (name) -1.0 \xC5\x8E", "a", "0123456789012345678901234567890" };
    InstManifest m, d; char buf[600]; int n, i;
    memset(&m, 0, sizeof m);
    for (i = 0; i < 5; i++) { strcpy(m.e[i].name, names[i]); strcpy(m.e[i].folder, i & 1 ? "Control Panels" : "System Folder"); }
    m.n = 5;
    n = inst_manifest_encode(&m, buf, sizeof buf);
    T_CHECK(n > 0);
    T_EQ(buf[n - 1], '\r');
    T_EQ(inst_manifest_decode(buf, (size_t)n, &d), 5);
    for (i = 0; i < 5; i++) {
        T_CHECK(!strcmp(d.e[i].name, m.e[i].name));
        T_CHECK(!strcmp(d.e[i].folder, m.e[i].folder));
    }
    m.n = 0;
    T_EQ(inst_manifest_encode(&m, buf, sizeof buf), 0);
    T_EQ(inst_manifest_decode("", 0, &d), 0);
    T_EQ(inst_manifest_encode(&m, NULL, 0), 0);
    /* exact fit and one short */
    m.n = 1; strcpy(m.e[0].name, "ab"); strcpy(m.e[0].folder, "cd");
    T_EQ(inst_manifest_encode(&m, buf, 6), 6);
    T_EQ(inst_manifest_encode(&m, buf, 5), INST_E_CAP);
}
static void manifest_rejects_bad_names(void) {
    static const char *bad[] = { "a\tb", "a\rb", "a\nb", "a:b", "", "x\x01y", "0123456789012345678901234567890X" };
    InstManifest m; char buf[600]; size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&m, 0, sizeof m);
        strncpy(m.e[0].name, bad[i], 31); m.e[0].name[31] = 0;
        strcpy(m.e[0].folder, "System Folder"); m.n = 1;
        if (i == 6) memcpy(m.e[0].name, bad[i], 32);      /* 32 chars fill the buffer: no NUL, still refused */
        T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
        memset(&m, 0, sizeof m);                           /* and as the folder */
        strcpy(m.e[0].name, "ok"); memcpy(m.e[0].folder, bad[i], i == 6 ? 32 : strlen(bad[i]) + 1); m.n = 1;
        T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
    }
    m.n = INST_MAN_MAX + 1;
    T_EQ(inst_manifest_encode(&m, buf, sizeof buf), INST_E_MANIFEST);
}
static void manifest_decode_strict(void) {
    static const char *bad[] = {
        "MacTCP\tSystem Folder",          /* truncated: no CR */
        "MacTCP\r",                       /* no TAB */
        "MacTCP\tA\tB\r",                 /* two TABs */
        "\tSystem Folder\r", "MacTCP\t\r", "MacTCP\tSystem Folder\r\r",
        "MacTCP\tSystem:Folder\r", "Mac\nTCP\tSystem Folder\r",
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
    InstEntry e[1]; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man;
    memset(e, 0, sizeof e);
    T_EQ(inst_plan_install(NULL, 1, NULL, -1, 2, NULL, ops, INST_OPS_MAX, &m, &man), INST_E_ARG);
    T_EQ(inst_plan_install(e, -1, NULL, -1, 2, NULL, ops, INST_OPS_MAX, &m, &man), INST_E_ARG);
    T_EQ(inst_plan_install(NULL, 0, NULL, 3, 2, NULL, ops, INST_OPS_MAX, &m, &man), INST_E_ARG);
    T_EQ(inst_plan_remove(NULL, 1, NULL, -1, NULL, ops, INST_OPS_MAX, &m), INST_E_ARG);
    T_EQ(inst_plan_install(NULL, 0, NULL, -1, 2, NULL, ops, 0, &m, &man), INST_E_CAP);   /* needs 1 op */
    {
        Sim s; base(&s); trio3(&s);
        T_EQ(do_install(&s, 2, ops, &m, &man), 6);
        {
            InstEntry sys[48], dis[48]; int ns, nd;
            snap(&s, sys, &ns, dis, &nd);
            T_EQ(inst_plan_install(sys, ns, dis, nd, 2, NULL, ops, 5, &m, &man), INST_E_CAP);
        }
    }
}

/* ---- properties over random snapshots ---- */
static uint32_t rng_state;
static unsigned rnd(unsigned n) { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) % n; }

static void gen(Sim *s) {
    static const char *other[] = { "Finder", "System", "Clipboard", "Scrapbook File", "Mac TCP", "MacTCP2" };
    int i;
    base(s);
    for (i = 0; i < 4; i++) if (rnd(2)) addsys(s, other[rnd(6)], TEXT, TEXT);
    if (rnd(5) < 4) addsys(s, "MacTCP", rnd(8) ? CDEV : TEXT, ZTCP);
    if (rnd(2)) addsys(s, "MacTCP DNR", CDEV, MTCP);
    if (rnd(2)) addsys(s, "MacTCP Prep", MTPP, MTCP);
    if (rnd(3) == 0) { oldmoof(s, (uint32_t)rnd(3)); }          /* 0,1: older; 2: current */
    if (rnd(2)) addsys(s, INST_PREFS_NAME, PREF, ZTCP);
    if (rnd(2)) {
        add(s, "System Folder", INST_DISABLED, 0, 0, 1);
        if (rnd(2)) adddis(s, "Notes", TEXT, TEXT);
        if (rnd(3) == 0) adddis(s, "MacTCP Prep", MTPP, MTCP);   /* may collide: skipped when blocked */
    }
    if (rnd(6) == 0 && s->n > 2) {                                /* a locked file somewhere */
        s->f[rnd((unsigned)s->n)].e.locked = 1;
    }
}

static void prop_install_remove(void) {
    int it, ran = 0, blocked = 0;
    rng_state = 12345;
    for (it = 0; it < 3000; it++) {
        Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
        gen(&s); orig = s;
        n = do_install(&s, 2, ops, &m, &man);
        T_CHECK(n >= 0);
        if (m == INST_BLOCKED_COLLISION || m == INST_BLOCKED_LOCKED) { T_EQ(n, 0); T_CHECK(same(&orig, &s)); blocked++; continue; }
        T_EQ(apply(&s, ops, n, &man), 0);
        { int k; for (k = 0; k < n; k++) T_CHECK(ops[k].kind != INST_DELETE_MANIFEST && ops[k].kind != INST_DELETE_MOOF);
          /* MacTCP files are only ever moved */ }
        n = do_remove(&s, ops, &m);
        T_CHECK(n >= 0);
        if (m == INST_BLOCKED_LOCKED) {                          /* a locked file the install did not need to touch */
            blocked++; continue;
        }
        T_EQ(apply(&s, ops, n, NULL), 0);
        if (!same(&orig, &s)) { fprintf(stderr, "iteration %d\n", it); dump(&orig); dump(&s); T_CHECK(0); return; }
        ran++;
    }
    T_CHECK(ran > 1000);
    T_CHECK(blocked > 0);
}

static void prop_install_prefixes(void) {
    int it, k, checked = 0;
    rng_state = 777;
    for (it = 0; it < 1500; it++) {
        Sim s, orig; InstOp ops[INST_OPS_MAX]; InstMsg m; InstManifest man; int n;
        gen(&s); orig = s;
        n = do_install(&s, 2, ops, &m, &man);
        if (n <= 0) continue;
        for (k = 0; k <= n; k++) {
            Sim p = orig; InstOp r[INST_OPS_MAX]; InstMsg m2; int rn;
            T_EQ(apply(&p, ops, k, &man), 0);
            /* (a) a fresh Remove returns to the original */
            {
                Sim q = p;
                rn = do_remove(&q, r, &m2);
                T_CHECK(rn >= 0);
                if (m2 != INST_BLOCKED_LOCKED) {
                    T_EQ(apply(&q, r, rn, NULL), 0);
                    if (!same(&orig, &q)) { fprintf(stderr, "install it %d prefix %d\n", it, k); dump(&orig); dump(&q); T_CHECK(0); return; }
                    checked++;
                }
            }
            /* (b) re-running Install completes, then Remove returns to the original */
            {
                InstOp i2[INST_OPS_MAX]; InstManifest man2; int in;
                in = do_install(&p, 2, i2, &m2, &man2);
                T_CHECK(in >= 0);
                if (m2 != INST_BLOCKED_COLLISION && m2 != INST_BLOCKED_LOCKED) {
                    T_EQ(apply(&p, i2, in, &man2), 0);
                    rn = do_remove(&p, r, &m2);
                    if (m2 != INST_BLOCKED_LOCKED) {
                        T_EQ(apply(&p, r, rn, NULL), 0);
                        if (!same(&orig, &p)) { fprintf(stderr, "reinstall it %d prefix %d\n", it, k); T_CHECK(0); }
                    }
                }
            }
        }
    }
    T_CHECK(checked > 1000);
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
            Sim p = s; InstOp r2[INST_OPS_MAX]; InstMsg m2; int n2;
            T_EQ(apply(&p, r, k, NULL), 0);
            n2 = do_remove(&p, r2, &m2);
            T_CHECK(n2 >= 0);
            if (m2 == INST_BLOCKED_LOCKED) continue;
            T_EQ(apply(&p, r2, n2, NULL), 0);
            if (!same(&orig, &p)) { fprintf(stderr, "remove it %d prefix %d\n", it, k); T_CHECK(0); }
            checked++;
        }
    }
    T_CHECK(checked > 1000);
}

T_MAIN(install_trio, install_mactcp_only, install_none, install_twice,
       install_moof_current_mactcp_back, install_newer_moof_left_alone, upgrade_keeps_prefs,
       upgrade_with_mactcp, disabled_exists_unrelated, collision_install,
       collision_file_named_like_folder, collision_moof_not_ours, locked_install, locked_old_moof,
       not_cdev_ztcp_untouched, case_insensitive, remove_with_manifest,
       remove_fallback_without_manifest, remove_nothing, remove_no_mactcp_and_prefs_kept,
       remove_other_folder_from_manifest, collision_remove, locked_remove, manifest_roundtrip,
       manifest_rejects_bad_names, manifest_decode_strict, arg_and_cap_errors, prop_install_remove,
       prop_install_prefixes, prop_remove_prefixes)
