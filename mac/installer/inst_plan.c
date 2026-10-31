/* inst_plan.c - installer planner (Plan 3 Task 9). See inst_plan.h for the contract. */
#include <string.h>
#include "inst_plan.h"

#define T_CDEV INST_FCC('c', 'd', 'e', 'v')
#define T_ZTCP INST_FCC('z', 't', 'c', 'p')
#define T_MTCP INST_FCC('m', 't', 'c', 'p')
#define T_MTPP INST_FCC('m', 't', 'p', 'p')

static const struct { const char *name; uint32_t type, creator; } trio[3] = {
    { "MacTCP", T_CDEV, T_ZTCP }, { "MacTCP DNR", T_CDEV, T_MTCP }, { "MacTCP Prep", T_MTPP, T_MTCP }
};

/* The longest path is ":System Folder:MacTCP (Disabled):" + a 31-byte name + NUL. */
typedef char inst_path_fits[(INST_PATH_MAX >= 1 + (int)sizeof(INST_SYSFOLDER) + (int)sizeof(INST_DISABLED) + INST_NAME_MAX + 1) ? 1 : -1];

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (fold((unsigned char)*a) != fold((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int find(const InstEntry *e, int n, const char *name) {
    int i;
    for (i = 0; i < n; i++) if (ieq(e[i].name, name)) return i;
    return -1;
}

/* index into trio, or -1 (not a file of the set) */
static int trio_idx(const InstEntry *e) {
    int t;
    if (e->is_dir) return -1;
    for (t = 0; t < 3; t++)
        if (ieq(e->name, trio[t].name) && e->type == trio[t].type && e->creator == trio[t].creator) return t;
    return -1;
}

static int trio_by_name(const char *name) {
    int t;
    for (t = 0; t < 3; t++) if (ieq(name, trio[t].name)) return t;
    return -1;
}

#define T_TMPT INST_TMP_TYPE
#define T_TMPC INST_TMP_CREATOR
#define T_TEXT INST_FCC('T', 'E', 'X', 'T')

/* the complete, loadable MoofTCP file */
static int moof_ok(const InstEntry *e) { return !e->is_dir && e->type == T_CDEV && e->creator == T_ZTCP; }
/* a file named MoofTCP that a crash between rename and retype left behind: ours, incomplete */
static int moof_incomplete(const InstEntry *e) { return !e->is_dir && e->type == T_TMPT && e->creator == T_TMPC; }
/* a temp file of ours: the non-loadable type, or what earlier builds created (loadable cdev/ztcp, TEXT/ztcp) */
static int tmp_ours(const InstEntry *e, uint32_t old_type) {
    return !e->is_dir && ((e->type == T_TMPT && e->creator == T_TMPC) || (e->type == old_type && e->creator == T_ZTCP));
}

static int valid_field(const char *s) {
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c < 32 || c == 127 || c == ':' || n >= INST_NAME_MAX) return 0;
    }
    return n > 0;
}

/* ":a[:b[:c]]" into out[INST_PATH_MAX]; -1 (out emptied) if it does not fit */
static int mkpath(char *out, const char *a, const char *b, const char *c) {
    const char *part[3];
    size_t len = 0, l;
    int i;
    part[0] = a; part[1] = b; part[2] = c;
    out[0] = 0;
    for (i = 0; i < 3 && part[i]; i++) {
        l = strlen(part[i]);
        if (len + 1 + l + 1 > INST_PATH_MAX) { out[0] = 0; return -1; }
        out[len++] = ':';
        memcpy(out + len, part[i], l);
        len += l;
    }
    out[len] = 0;
    return 0;
}

/* appends an op; from/to given as up to three components each (NULL = empty path) */
static int emit(InstOp *ops, int *n, InstOpKind k, const char *f0, const char *f1, const char *f2,
                const char *t0, const char *t1, const char *t2) {
    InstOp *o = &ops[*n];
    o->kind = k;
    if (mkpath(o->from, f0, f1, f2) < 0 || mkpath(o->to, t0, t1, t2) < 0) return INST_E_PATH;
    (*n)++;
    return 0;
}

static int entries_ok(const InstEntry *e, int n) {
    int i;
    for (i = 0; i < n; i++) if (!memchr(e[i].name, 0, sizeof e[i].name)) return 0;
    return 1;
}

static void setblock(char *blocker, const char *name) {
    if (blocker) { strncpy(blocker, name, 31); blocker[31] = 0; }
}

/* adds a manifest line for `name` (System Folder) unless a line for that folded name exists */
static int man_add(InstManifest *m, const char *name) {
    int i;
    for (i = 0; i < m->n; i++) if (ieq(m->e[i].name, name)) return 0;
    if (m->n >= INST_MAN_MAX) return -1;
    strcpy(m->e[m->n].name, name);
    strcpy(m->e[m->n].folder, INST_SYSFOLDER);
    m->n++;
    return 0;
}

int inst_manifest_encode(const InstManifest *m, char *buf, size_t cap) {
    static const char hdr[] = "#created\r";
    size_t len = 0, nl, fl;
    int i;
    if (!m || (!buf && cap) || m->n < 0 || m->n > INST_MAN_MAX) return INST_E_MANIFEST;
    for (i = 0; i < m->n; i++)
        if (!valid_field(m->e[i].name) || !valid_field(m->e[i].folder)) return INST_E_MANIFEST;
    if (m->created) {
        if (cap < sizeof hdr - 1) return INST_E_CAP;
        memcpy(buf, hdr, sizeof hdr - 1);
        len = sizeof hdr - 1;
    }
    for (i = 0; i < m->n; i++) {
        nl = strlen(m->e[i].name); fl = strlen(m->e[i].folder);
        if (len + nl + fl + 2 > cap) return INST_E_CAP;
        memcpy(buf + len, m->e[i].name, nl); len += nl;
        buf[len++] = '\t';
        memcpy(buf + len, m->e[i].folder, fl); len += fl;
        buf[len++] = '\r';
    }
    return (int)len;
}

int inst_manifest_decode(const char *text, size_t len, InstManifest *m) {
    size_t i = 0, j, tab, nl, fl;
    if (!m || (!text && len)) return INST_E_MANIFEST;
    memset(m, 0, sizeof *m);
    if (len >= 9 && memcmp(text, "#created\r", 9) == 0) { m->created = 1; i = 9; }
    while (i < len) {
        tab = (size_t)-1;
        for (j = i; j < len && text[j] != '\r'; j++) {
            if (text[j] == '\t') {
                if (tab != (size_t)-1) goto bad;
                tab = j;
            }
        }
        if (j >= len || tab == (size_t)-1 || m->n >= INST_MAN_MAX) goto bad;   /* no CR: truncated */
        nl = tab - i; fl = j - tab - 1;
        if (nl == 0 || fl == 0 || nl > INST_NAME_MAX || fl > INST_NAME_MAX) goto bad;
        memcpy(m->e[m->n].name, text + i, nl); m->e[m->n].name[nl] = 0;
        memcpy(m->e[m->n].folder, text + tab + 1, fl); m->e[m->n].folder[fl] = 0;
        if (!valid_field(m->e[m->n].name) || !valid_field(m->e[m->n].folder)) goto bad;
        m->n++;
        i = j + 1;
    }
    return m->n;
bad:
    memset(m, 0, sizeof *m);
    return INST_E_MANIFEST;
}

int inst_plan_install(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                      uint32_t moof_vers, const InstManifest *prev, InstOp *ops, int cap, InstMsg *msg,
                      char *blocker, InstManifest *man_out) {
    int mv[3], nmv = 0, t, i, j, mi, mani_any, write = 0, upgrade = 0, n = 0, have_t[3] = { 0, 0, 0 };
    int need, folder_exists, rc;
    if (!msg || !ops || !man_out || nsys < 0 || (nsys && !sys) || (ndisabled > 0 && !disabled))
        return INST_E_ARG;
    if (blocker) blocker[0] = 0;
    memset(man_out, 0, sizeof *man_out);
    folder_exists = ndisabled >= 0;
    if (!folder_exists) { ndisabled = 0; disabled = NULL; }
    if (!entries_ok(sys, nsys) || !entries_ok(disabled, ndisabled)) return INST_E_ARG;

    for (i = 0; i < nsys; i++) {
        t = trio_idx(&sys[i]);
        if (t >= 0 && !have_t[t]) { have_t[t] = 1; mv[nmv++] = i; }
    }
    mi = find(sys, nsys, INST_MOOF_NAME);
    if (mi >= 0) {
        if (moof_incomplete(&sys[mi])) write = 1;          /* a crash between rename and retype: redo */
        else if (!moof_ok(&sys[mi])) { setblock(blocker, sys[mi].name); *msg = INST_BLOCKED_COLLISION; return 0; }
        else if (sys[mi].vers < moof_vers) { write = 1; upgrade = 1; }
    } else {
        write = 1;
    }
    mani_any = 0;
    for (t = 0; t < 2; t++) {                   /* a folder of the manifest's name is foreign, never read */
        j = find(disabled, ndisabled, t ? INST_MANIFEST_TMP : INST_MANIFEST_NAME);
        if (j >= 0 && !disabled[j].is_dir) mani_any = 1;
        if (j >= 0 && nmv > 0 && (disabled[j].is_dir || (t && !tmp_ours(&disabled[j], T_TEXT)))) {
            setblock(blocker, disabled[j].name); *msg = INST_BLOCKED_COLLISION; return 0;
        }
    }
    if (write) {                                /* step 1 of the write protocol must be able to run (before parking) */
        j = find(sys, nsys, INST_MOOF_TMP);
        if (j >= 0 && !tmp_ours(&sys[j], T_CDEV)) { setblock(blocker, sys[j].name); *msg = INST_BLOCKED_COLLISION; return 0; }
        if (j >= 0 && sys[j].locked) { setblock(blocker, sys[j].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    }

    if (nmv > 0) {
        if (!folder_exists) {
            i = find(sys, nsys, INST_DISABLED);
            if (i >= 0 && !sys[i].is_dir) { setblock(blocker, sys[i].name); *msg = INST_BLOCKED_COLLISION; return 0; }
        }
        for (i = 0; i < nmv; i++)
            if (find(disabled, ndisabled, sys[mv[i]].name) >= 0) {
                setblock(blocker, sys[mv[i]].name); *msg = INST_BLOCKED_COLLISION; return 0;
            }
    }
    for (i = 0; i < nmv; i++)
        if (sys[mv[i]].locked) { setblock(blocker, sys[mv[i]].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (write && mi >= 0 && sys[mi].locked) { setblock(blocker, sys[mi].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (nmv > 0) {
        for (t = 0; t < 2; t++) {
            j = find(disabled, ndisabled, t ? INST_MANIFEST_TMP : INST_MANIFEST_NAME);
            if (j >= 0 && disabled[j].locked) { setblock(blocker, disabled[j].name); *msg = INST_BLOCKED_LOCKED; return 0; }
        }
    }

    if (nmv == 0 && !write) { *msg = INST_ALREADY_INSTALLED; return 0; }

    need = nmv + write + (nmv > 0 ? 1 + (folder_exists ? 0 : 1) : 0);
    if (cap < need) return INST_E_CAP;

    if (nmv > 0) {
        man_out->created = folder_exists ? (prev ? prev->created : 0) : 1;
        if (prev) {                           /* an earlier run was interrupted: keep its parked files listed */
            for (i = 0; i < prev->n; i++) {
                j = find(disabled, ndisabled, prev->e[i].name);
                if (j >= 0 && trio_idx(&disabled[j]) >= 0 && man_add(man_out, disabled[j].name) < 0)
                    return INST_E_MANIFEST;
            }
        } else if (mani_any) {                /* manifest file unreadable: the set's files there are ours */
            for (i = 0; i < ndisabled; i++)
                if (trio_idx(&disabled[i]) >= 0 && man_add(man_out, disabled[i].name) < 0) return INST_E_MANIFEST;
        }
        for (i = 0; i < nmv; i++) if (man_add(man_out, sys[mv[i]].name) < 0) return INST_E_MANIFEST;
        if (!folder_exists) {
            rc = emit(ops, &n, INST_MKDIR, NULL, NULL, NULL, INST_SYSFOLDER, INST_DISABLED, NULL);
            if (rc < 0) return rc;
        }
        rc = emit(ops, &n, INST_WRITE_MANIFEST, NULL, NULL, NULL, INST_SYSFOLDER, INST_DISABLED, INST_MANIFEST_NAME);
        if (rc < 0) return rc;
        for (i = 0; i < nmv; i++) {
            rc = emit(ops, &n, INST_MOVE, INST_SYSFOLDER, sys[mv[i]].name, NULL,
                      INST_SYSFOLDER, INST_DISABLED, sys[mv[i]].name);
            if (rc < 0) return rc;
        }
    }
    if (write) {
        rc = emit(ops, &n, INST_WRITE_MOOF, NULL, NULL, NULL, INST_SYSFOLDER,
                  mi >= 0 ? sys[mi].name : INST_MOOF_NAME, NULL);
        if (rc < 0) return rc;
    }
    *msg = upgrade ? INST_UPGRADED : nmv > 0 ? INST_INSTALLED_PARKED_MACTCP : INST_INSTALLED_NO_MACTCP;
    return n;
}

int inst_plan_remove(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                     const InstManifest *man, InstOp *ops, int cap, InstMsg *msg, char *blocker) {
    int mi, mx = -1, mt = -1, tm, tm_bad, bad = -1, dbad = -1, mani_any, folder_exists, i, j, k, n = 0, nr = 0, need, rest, rc;
    int rs[INST_MAN_MAX];
    if (!msg || !ops || nsys < 0 || (nsys && !sys) || (ndisabled > 0 && !disabled)) return INST_E_ARG;
    if (blocker) blocker[0] = 0;
    folder_exists = ndisabled >= 0;
    if (!folder_exists) { ndisabled = 0; disabled = NULL; }
    if (!entries_ok(sys, nsys) || !entries_ok(disabled, ndisabled)) return INST_E_ARG;
    mi = find(sys, nsys, INST_MOOF_NAME);
    if (mi >= 0 && !moof_ok(&sys[mi]) && !moof_incomplete(&sys[mi])) mi = -1;
    tm = find(sys, nsys, INST_MOOF_TMP);
    tm_bad = tm >= 0 && !tmp_ours(&sys[tm], T_CDEV);      /* a folder or a foreign type of that name */
    if (tm_bad) { bad = tm; tm = -1; }
    if (folder_exists) {
        mx = find(disabled, ndisabled, INST_MANIFEST_NAME);
        mt = find(disabled, ndisabled, INST_MANIFEST_TMP);
        if (mx >= 0 && disabled[mx].is_dir) { dbad = mx; mx = -1; }
        if (mt >= 0 && !tmp_ours(&disabled[mt], T_TEXT)) { dbad = mt; mt = -1; }
    }
    mani_any = mx >= 0 || mt >= 0;
    if (mi < 0 && tm < 0 && !mani_any) { *msg = INST_NOTHING_TO_REMOVE; return 0; }
    if (tm_bad) { setblock(blocker, sys[bad].name); *msg = INST_BLOCKED_COLLISION; return 0; }
    if (dbad >= 0) { setblock(blocker, disabled[dbad].name); *msg = INST_BLOCKED_COLLISION; return 0; }

    if (mani_any) {
        if (man) {
            for (i = 0; i < man->n; i++)      /* never trust the manifest */
                if (trio_by_name(man->e[i].name) < 0 || !ieq(man->e[i].folder, INST_SYSFOLDER))
                    return INST_E_MANIFEST;
            for (i = 0; i < man->n; i++) {
                j = find(disabled, ndisabled, man->e[i].name);
                if (j < 0 || trio_idx(&disabled[j]) < 0) continue;
                for (k = 0; k < nr; k++) if (rs[k] == j) break;   /* duplicate manifest line */
                if (k < nr) continue;
                rs[nr++] = j;
            }
        } else {
            for (i = 0; i < ndisabled && nr < INST_MAN_MAX; i++)
                if (trio_idx(&disabled[i]) >= 0) rs[nr++] = i;
        }
    }
    for (i = 0; i < nr; i++)
        if (find(sys, nsys, disabled[rs[i]].name) >= 0) {
            setblock(blocker, disabled[rs[i]].name); *msg = INST_BLOCKED_COLLISION; return 0;
        }
    for (i = 0; i < nr; i++)
        if (disabled[rs[i]].locked) { setblock(blocker, disabled[rs[i]].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (mi >= 0 && sys[mi].locked) { setblock(blocker, sys[mi].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (tm >= 0 && sys[tm].locked) { setblock(blocker, sys[tm].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (mx >= 0 && disabled[mx].locked) { setblock(blocker, disabled[mx].name); *msg = INST_BLOCKED_LOCKED; return 0; }
    if (mt >= 0 && disabled[mt].locked) { setblock(blocker, disabled[mt].name); *msg = INST_BLOCKED_LOCKED; return 0; }

    rest = ndisabled - nr - (mx >= 0) - (mt >= 0);
    k = folder_exists && man && man->created && mani_any && rest == 0;      /* plan RMDIR */
    need = (mi >= 0) + nr + (tm >= 0) + (mt >= 0) + (mx >= 0) + k;
    if (cap < need) return INST_E_CAP;
    if (mi >= 0) {
        rc = emit(ops, &n, INST_DELETE_MOOF, INST_SYSFOLDER, sys[mi].name, NULL, NULL, NULL, NULL);
        if (rc < 0) return rc;
    }
    if (tm >= 0) {                              /* a loadable leftover must go before MacTCP returns */
        rc = emit(ops, &n, INST_DELETE_TEMP, INST_SYSFOLDER, sys[tm].name, NULL, NULL, NULL, NULL);
        if (rc < 0) return rc;
    }
    for (i = 0; i < nr; i++) {
        rc = emit(ops, &n, INST_MOVE, INST_SYSFOLDER, INST_DISABLED, disabled[rs[i]].name,
                  INST_SYSFOLDER, disabled[rs[i]].name, NULL);
        if (rc < 0) return rc;
    }
    if (mt >= 0) {
        rc = emit(ops, &n, INST_DELETE_TEMP, INST_SYSFOLDER, INST_DISABLED, disabled[mt].name, NULL, NULL, NULL);
        if (rc < 0) return rc;
    }
    if (mx >= 0) {
        rc = emit(ops, &n, INST_DELETE_MANIFEST, INST_SYSFOLDER, INST_DISABLED, disabled[mx].name, NULL, NULL, NULL);
        if (rc < 0) return rc;
    }
    if (k) {
        rc = emit(ops, &n, INST_RMDIR, INST_SYSFOLDER, INST_DISABLED, NULL, NULL, NULL, NULL);
        if (rc < 0) return rc;
    }
    *msg = nr > 0 ? INST_REMOVED_RESTORED : INST_REMOVED_NO_MACTCP;
    return n;
}
