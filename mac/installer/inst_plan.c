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

static int moof_ok(const InstEntry *e) { return !e->is_dir && e->type == T_CDEV && e->creator == T_ZTCP; }

static int valid_field(const char *s) {
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c < 32 || c == 127 || c == ':' || n >= INST_NAME_MAX) return 0;
    }
    return n > 0;
}

static void path2(char *out, const char *a, const char *b) {   /* ":a:b", fits INST_PATH_MAX */
    size_t la = strlen(a), lb = strlen(b);
    out[0] = ':'; memcpy(out + 1, a, la); out[1 + la] = ':'; memcpy(out + 2 + la, b, lb + 1);
}
static void path3(char *out, const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    out[0] = ':'; memcpy(out + 1, a, la); out[1 + la] = ':';
    memcpy(out + 2 + la, b, lb); out[2 + la + lb] = ':'; memcpy(out + 3 + la + lb, c, lc + 1);
}

static InstOp *addop(InstOp *ops, int *n, InstOpKind k) {   /* callers checked cap beforehand */
    InstOp *o = &ops[(*n)++];
    o->kind = k; o->from[0] = 0; o->to[0] = 0;
    return o;
}

int inst_manifest_encode(const InstManifest *m, char *buf, size_t cap) {
    size_t len = 0, nl, fl;
    int i;
    if (!m || (!buf && cap) || m->n < 0 || m->n > INST_MAN_MAX) return INST_E_MANIFEST;
    for (i = 0; i < m->n; i++)
        if (!valid_field(m->e[i].name) || !valid_field(m->e[i].folder)) return INST_E_MANIFEST;
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
                      InstManifest *man_out) {
    int mv[3], nmv = 0, t, i, mi, mani, write = 0, upgrade = 0, n = 0, have_t[3] = { 0, 0, 0 };
    int need, folder_exists;
    InstOp *o;
    if (!msg || !ops || !man_out || nsys < 0 || (nsys && !sys) || (ndisabled > 0 && !disabled))
        return INST_E_ARG;
    memset(man_out, 0, sizeof *man_out);
    folder_exists = ndisabled >= 0;
    if (!folder_exists) { ndisabled = 0; disabled = NULL; }

    for (i = 0; i < nsys; i++) {
        t = trio_idx(&sys[i]);
        if (t >= 0 && !have_t[t]) { have_t[t] = 1; mv[nmv++] = i; }
    }
    mi = find(sys, nsys, INST_MOOF_NAME);
    if (mi >= 0) {
        if (!moof_ok(&sys[mi])) { *msg = INST_BLOCKED_COLLISION; return 0; }
        if (sys[mi].vers < moof_vers) { write = 1; upgrade = 1; }
    } else {
        write = 1;
    }
    mani = folder_exists ? find(disabled, ndisabled, INST_MANIFEST_NAME) : -1;

    if (nmv > 0) {
        if (!folder_exists) {
            i = find(sys, nsys, INST_DISABLED);
            if (i >= 0 && !sys[i].is_dir) { *msg = INST_BLOCKED_COLLISION; return 0; }
        }
        for (i = 0; i < nmv; i++)
            if (find(disabled, ndisabled, sys[mv[i]].name) >= 0) { *msg = INST_BLOCKED_COLLISION; return 0; }
    }
    for (i = 0; i < nmv; i++) if (sys[mv[i]].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }
    if (upgrade && sys[mi].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }
    if (nmv > 0 && mani >= 0 && disabled[mani].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }

    if (nmv == 0 && !write) { *msg = INST_ALREADY_INSTALLED; return 0; }

    need = nmv + write + (nmv > 0 ? 1 + (folder_exists ? 0 : 1) : 0);
    if (cap < need) return INST_E_CAP;

    if (nmv > 0) {
        if (prev) {                           /* an earlier run was interrupted: keep its parked files listed */
            for (i = 0; i < prev->n && man_out->n < INST_MAN_MAX; i++) {
                int j = find(disabled, ndisabled, prev->e[i].name);
                if (j >= 0 && !disabled[j].is_dir) man_out->e[man_out->n++] = prev->e[i];
            }
        }
        for (i = 0; i < nmv && man_out->n < INST_MAN_MAX; i++) {
            strcpy(man_out->e[man_out->n].name, sys[mv[i]].name);
            strcpy(man_out->e[man_out->n].folder, INST_SYSFOLDER);
            man_out->n++;
        }
        if (!folder_exists) {
            o = addop(ops, &n, INST_MKDIR);
            path2(o->to, INST_SYSFOLDER, INST_DISABLED);
        }
        o = addop(ops, &n, INST_WRITE_MANIFEST);
        path3(o->to, INST_SYSFOLDER, INST_DISABLED, INST_MANIFEST_NAME);
        for (i = 0; i < nmv; i++) {
            o = addop(ops, &n, INST_MOVE);
            path2(o->from, INST_SYSFOLDER, sys[mv[i]].name);
            path3(o->to, INST_SYSFOLDER, INST_DISABLED, sys[mv[i]].name);
        }
    }
    if (write) {
        o = addop(ops, &n, INST_WRITE_MOOF);
        path2(o->to, INST_SYSFOLDER, mi >= 0 ? sys[mi].name : INST_MOOF_NAME);
    }
    *msg = upgrade ? INST_UPGRADED : nmv > 0 ? INST_INSTALLED_PARKED_MACTCP : INST_INSTALLED_NO_MACTCP;
    return n;
}

int inst_plan_remove(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                     const InstManifest *man, InstOp *ops, int cap, InstMsg *msg) {
    int mi, mani = -1, folder_exists, i, j, k, n = 0, nr = 0, need, rest;
    int rs[INST_MAN_MAX];
    const char *rf[INST_MAN_MAX];
    InstOp *o;
    if (!msg || !ops || nsys < 0 || (nsys && !sys) || (ndisabled > 0 && !disabled)) return INST_E_ARG;
    folder_exists = ndisabled >= 0;
    if (!folder_exists) { ndisabled = 0; disabled = NULL; }
    mi = find(sys, nsys, INST_MOOF_NAME);
    if (mi >= 0 && !moof_ok(&sys[mi])) mi = -1;
    if (folder_exists) mani = find(disabled, ndisabled, INST_MANIFEST_NAME);
    if (mi < 0 && mani < 0) { *msg = INST_NOTHING_TO_REMOVE; return 0; }

    if (mani >= 0 || man) {            /* no manifest file = nothing of ours was ever parked */
        if (man) {
            for (i = 0; i < man->n && nr < INST_MAN_MAX; i++) {
                j = find(disabled, ndisabled, man->e[i].name);
                if (j < 0 || disabled[j].is_dir) continue;
                for (k = 0; k < nr; k++) if (rs[k] == j) break;   /* duplicate manifest line */
                if (k < nr) continue;
                rs[nr] = j; rf[nr++] = man->e[i].folder;
            }
        } else {
            for (i = 0; i < ndisabled && nr < INST_MAN_MAX; i++)
                if (trio_idx(&disabled[i]) >= 0) { rs[nr] = i; rf[nr++] = INST_SYSFOLDER; }
        }
    }
    for (i = 0; i < nr; i++)
        if (ieq(rf[i], INST_SYSFOLDER) && find(sys, nsys, disabled[rs[i]].name) >= 0) {
            *msg = INST_BLOCKED_COLLISION; return 0;
        }
    for (i = 0; i < nr; i++) if (disabled[rs[i]].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }
    if (mi >= 0 && sys[mi].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }
    if (mani >= 0 && disabled[mani].locked) { *msg = INST_BLOCKED_LOCKED; return 0; }

    rest = ndisabled - nr - (mani >= 0 ? 1 : 0);
    need = nr + (mi >= 0) + (mani >= 0) + (folder_exists && rest == 0);
    if (cap < need) return INST_E_CAP;
    for (i = 0; i < nr; i++) {
        o = addop(ops, &n, INST_MOVE);
        path3(o->from, INST_SYSFOLDER, INST_DISABLED, disabled[rs[i]].name);
        path2(o->to, rf[i], disabled[rs[i]].name);
    }
    if (mi >= 0) {
        o = addop(ops, &n, INST_DELETE_MOOF);
        path2(o->from, INST_SYSFOLDER, sys[mi].name);
    }
    if (mani >= 0) {
        o = addop(ops, &n, INST_DELETE_MANIFEST);
        path3(o->from, INST_SYSFOLDER, INST_DISABLED, disabled[mani].name);
    }
    if (folder_exists && rest == 0) {
        o = addop(ops, &n, INST_RMDIR);
        path2(o->from, INST_SYSFOLDER, INST_DISABLED);
    }
    *msg = nr > 0 ? INST_REMOVED_RESTORED : INST_REMOVED_NO_MACTCP;
    return n;
}
