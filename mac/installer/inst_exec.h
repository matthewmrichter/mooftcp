/* inst_exec.h - the MoofTCP installer executor (Plan 3 Task 10; portable: no Toolbox calls).
 *
 * The executor turns the planner's op list (inst_plan.h, the contract this file implements) into
 * file system calls. Everything that is not a Toolbox call lives here and is host-tested against a
 * fake file system (tests/test_inst_exec.c); mac/installer/inst_exec_mac.c is the real InstFs
 * table (HFS parameter-block calls only: System 6.0.8 has no FSSpec, FindFolder or FSp* calls).
 *
 * Folders are HFS directory IDs (long) on one volume chosen by the table. `sysdir` is the blessed
 * System Folder; the planner's path component "System Folder" always means sysdir, whatever the
 * folder is called on disk. "MacTCP (Disabled)" is looked up by name inside sysdir when an op
 * needs it (MKDIR may just have made it).
 *
 * Errors are Mac OSErr values (negative), the same numbers in the fake: the table returns 0 or an
 * OSErr. Every table call is complete when it returns (write/read open, transfer and close the
 * fork themselves), so a stop between two calls never leaves a file open.
 *
 * Safety rules (Task 10 ledger):
 *  - the executor opens the forks of exactly two kinds of existing files: the MoofTCP file (its
 *    resource fork, read only, for its 'vers' 1) and the manifest / manifest temp (data fork, read
 *    only). It never opens a MacTCP file's forks; the MacTCP files are only ever catalog-moved.
 *  - every change (delete, create, setinfo, fork write, rename, move, mkdir) passes one safe-target
 *    check right before its call (guard() in inst_exec.c): the object must be ":System Folder:MoofTCP"
 *    (cdev/ztcp or MTMP/mtin), ":System Folder:MoofTCP.tmp" (our temp types), the manifest or its temp
 *    inside 'MacTCP (Disabled)', that folder itself (mkdir/rmdir), or a MacTCP set file by name AND
 *    type/creator, which may only be catalog-moved between the two folders to an absent name. A wrong
 *    name or folder gives INST_X_REFUSED, a wrong existence or type INST_X_CHANGED (a stale snapshot);
 *    either way nothing is touched and the run stops.
 *  - ops run in the planner's order and the first failure stops the run (no rollback: the planner's
 *    recovery model is that Install or Remove re-run from any prefix restores order).
 *  - after every change (and every op) the volume is flushed, so a power loss leaves exactly a prefix
 *    of the changes: the crash model of inst_plan.h. */
#ifndef INST_EXEC_H
#define INST_EXEC_H

#include "inst_plan.h"

/* OSErr values used by the executor and the fake (Inside Macintosh, Files). */
enum {
    INST_OS_DSKFUL = -34, INST_OS_IO = -36, INST_OS_FNF = -43, INST_OS_WPR = -44, INST_OS_FLCKD = -45,
    INST_OS_VLCKD = -46, INST_OS_FBSY = -47, INST_OS_DUPFN = -48, INST_OS_OPWR = -49, INST_OS_PERM = -54,
    INST_OS_DIRNF = -120
};

typedef struct {
    uint32_t type, creator;
    uint16_t flags;           /* Finder flags (FInfo.fdFlags) */
    uint8_t locked, is_dir;
    long dirid;               /* folders: the directory ID */
} InstInfo;

typedef struct InstFs {
    void *ctx;
    long sysdir;              /* directory ID of the blessed System Folder */
    /* entry `index` (1-based) of folder `dir`; INST_OS_FNF after the last one. Fills name (NUL-
     * terminated), type, creator, locked, is_dir; vers = 0. */
    int (*list)(void *ctx, long dir, int index, InstEntry *e);
    /* catalog info of `name` in `dir` (file or folder); INST_OS_FNF if absent */
    int (*info)(void *ctx, long dir, const char *name, InstInfo *i);
    int (*mkdir)(void *ctx, long parent, const char *name);
    int (*create)(void *ctx, long dir, const char *name);          /* empty file, type/creator 0 */
    /* sets type, creator and Finder flags; keeps the rest of the Finder info and the dates */
    int (*setinfo)(void *ctx, long dir, const char *name, uint32_t type, uint32_t creator, uint16_t flags);
    /* replaces the whole fork (rsrc = 1: resource fork) with len bytes: open, write, set EOF, close */
    int (*write)(void *ctx, long dir, const char *name, int rsrc, const uint8_t *p, long len);
    /* reads up to len bytes at offset off of a fork, read-only; *got = bytes read (EOF is not an error) */
    int (*read)(void *ctx, long dir, const char *name, int rsrc, long off, uint8_t *p, long len, long *got);
    int (*del)(void *ctx, long dir, const char *name);             /* a file, or an empty folder */
    int (*rename)(void *ctx, long dir, const char *from, const char *to);
    int (*move)(void *ctx, long fromdir, const char *name, long todir);   /* catalog move, same name */
    int (*flush)(void *ctx);
    /* free bytes on the volume and whether it is locked (hardware or software) */
    int (*volinfo)(void *ctx, long *free_bytes, int *locked);
} InstFs;

/* The bundled MoofTCP file (the installer's resources MTCP/MTCD/MTCI 128). */
typedef struct {
    const uint8_t *rsrc; long rlen;     /* resource fork */
    const uint8_t *data; long dlen;     /* data fork (the release has none: dlen 0) */
    uint32_t type, creator;             /* cdev / ztcp */
    uint16_t flags;                     /* Finder flags as shipped (hasBundle 0x2000) */
} InstPayload;

/* Executor status (InstResult.status) */
enum {
    INST_X_OK = 0,
    INST_X_PLAN,        /* the planner returned an error (plan_rc) */
    INST_X_SNAPSHOT,    /* listing the folders failed (oserr), or more matching entries than fit */
    INST_X_OSERR,       /* op op_index failed with oserr at write step `step` (0 = not a write op) */
    INST_X_CHANGED,     /* an op's target is no longer what the snapshot showed: nothing done to it */
    INST_X_PATH,        /* an op path the executor cannot resolve (a planner/executor mismatch) */
    INST_X_NOSPACE,     /* free space check before the first op (nothing done) */
    INST_X_VOLLOCKED,   /* the startup disk is locked (nothing done) */
    INST_X_PAYLOAD,     /* the bundled MoofTCP is missing or damaged (nothing done) */
    INST_X_STOPPED,     /* debug hook: stopped after stop_after file system changes */
    INST_X_REFUSED      /* the op names an object the safe-target check never lets the executor change */
};

/* Sub-steps of WRITE_MANIFEST / WRITE_MOOF (InstResult.step), the 5 steps of inst_plan.h. */
enum { INST_W_PROMOTE = 1, INST_W_TEMP, INST_W_DELETE, INST_W_RENAME, INST_W_RETYPE };

typedef struct {
    int remove;              /* 0 = Install, 1 = Remove */
    int status;              /* INST_X_* */
    int plan_rc;             /* planner return (ops count or INST_E_*) */
    InstMsg msg;             /* valid when plan_rc >= 0 */
    char blocker[32];
    int nops, done;          /* ops planned, ops completed */
    int op_index;            /* failing op (0-based), -1 = none */
    InstOpKind op_kind;
    char op_name[32];        /* leaf name of the failing op's object */
    int step;                /* INST_W_* for a failed write op */
    int oserr;
    int moves;               /* MOVE ops in the plan (MacTCP files parked / restored) */
    int prefs;               /* "MoofTCP Prefs" is in the System Folder */
    int man_retried;         /* Remove: the manifest was refused and the plan was made with man = NULL */
    long mutations;          /* file system changes attempted (create, setinfo, write, delete, rename, move, mkdir) */
    long committed;          /* ... of which the call succeeded (0 = the disk is unchanged) */
    /* variants (Install and inst_do_query): the bundle's, and what the snapshot found installed */
    int variant;             /* INST_VAR_* of the bundled file (Install) */
    uint32_t vers;           /* packed 'vers' 1 of the bundled file (Install) */
    int had;                 /* the System Folder held: 0 no MoofTCP, 1 a complete one, 2 an incomplete
                              * one (MTMP/mtin, an interrupted write) or only a leftover temp / manifest */
    uint32_t old_vers;       /* had == 1: its packed 'vers' 1 (0 = unreadable) */
    int old_variant;         /* had == 1: its INST_VAR_* */
    char old_vstr[16];       /* had == 1: its short version string ("" = unreadable) */
} InstResult;

/* What the 'vers' 1 of a MoofTCP file says. */
typedef struct {
    uint32_t packed;         /* first 4 bytes, big-endian; 0 = no 'vers' 1 or damaged */
    int variant;             /* INST_VAR_LITE when the long string starts with "MoofTCP Lite", else FULL */
    char vstr[16];           /* the short string, NUL-terminated (cut at 15 characters) */
} InstVers;

typedef struct {
    const InstFs *fs;
    long stop_after;         /* debug hook: 0 = off, else stop before the (stop_after+1)-th change */
    long mutations, committed;
    InstResult *res;
} InstCtx;

/* Run the op list in order (WRITE_MANIFEST encodes *man); stops at the first failure and fills
 * x->res (status, op_index, op_kind, op_name, step, oserr, done). Returns res->status. */
int inst_run(InstCtx *x, const InstOp *ops, int n, const InstManifest *man, const InstPayload *pl);

/* Snapshot, plan, (if ops) free-space check and run. stop_after as in InstCtx. Return res->status. */
int inst_do_install(const InstFs *fs, const InstPayload *pl, long stop_after, InstResult *res);
int inst_do_remove(const InstFs *fs, long stop_after, InstResult *res);

/* The packed 'vers' 1 (first 4 bytes, big-endian) of a resource fork in memory; 0 = none/damaged. */
uint32_t inst_fork_vers(const uint8_t *fork, long len);
/* The same with the variant and the short string; returns v->packed. A 'vers' 1 too short to hold the
 * strings still gives its packed version, variant FULL and an empty string. */
uint32_t inst_fork_vinfo(const uint8_t *fork, long len, InstVers *v);

/* Snapshot only (no change, no plan): fills res->had / old_* / prefs. 0, or INST_X_SNAPSHOT. */
int inst_do_query(const InstFs *fs, InstResult *res);
/* The line for the window after inst_do_query ("Installed now: MoofTCP Lite 1.0rc3." ...), <= cap-1. */
void inst_query_message(const InstResult *r, char *buf, int cap);

/* The user-visible text for a result (C string, at most cap-1 bytes, cap <= 256 for a Str255).
 * `vers` is the version string of the bundled MoofTCP (e.g. "1.0rc1"); the variant's name comes from
 * r->variant ("MoofTCP" / "MoofTCP Lite"). Returns 1 when the run
 * changed the disk and the user should restart (the Restart dialog follows), else 0. */
int inst_message(const InstResult *r, const char *vers, char *buf, int cap);

#endif
