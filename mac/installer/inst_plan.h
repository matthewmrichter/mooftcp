/* inst_plan.h - the MoofTCP installer planner (Plan 3 Task 9; portable: no Toolbox, no malloc, no
 * file system access). Task 10 writes the Toolbox executor and the UI on top of this contract.
 *
 * The planner is a pure function from a SNAPSHOT to an ordered OP LIST. The executor lists the
 * System Folder and the 'MacTCP (Disabled)' folder into InstEntry arrays, calls inst_plan_install
 * or inst_plan_remove, shows the InstMsg, and (when ops > 0) executes the ops in order, stopping at
 * the first failure. System 6.0.8 has no FSSpec and no FindFolder, so everything is names and
 * HFS-style paths; resolving a path to a volume/dirID is the executor's job.
 *
 * Paths (InstOp.from/.to): ":System Folder:name" and ":System Folder:MacTCP (Disabled):name"; a
 * restore target is ":<folder>:name" with <folder> from the manifest. Always <= 63 chars.
 *
 * Names: HFS names, 1..31 bytes, no ':'. Compared case-insensitively with ASCII folding only
 * (HFS folds MacRoman too; none of the names involved has such characters, and an entry that
 * differs only by a MacRoman case pair is not matched, which errs on the side of not touching it).
 *
 * The MacTCP set (matched by name AND type/creator, never by name alone; a file named "MacTCP"
 * whose type/creator differ is NOT touched, and neither is a folder of that name):
 *     MacTCP        cdev/ztcp      MacTCP DNR   cdev/mtcp      MacTCP Prep   mtpp/mtcp
 * The planner NEVER deletes or edits a MacTCP file: it only MOVES it into 'MacTCP (Disabled)' (a
 * folder inside the System Folder, created if missing) and back. NOTE: MoofTCP's own interim dev
 * file was also called "MacTCP" (cdev/ztcp); it is indistinguishable from the real one, so the
 * planner treats any cdev/ztcp named MacTCP as MacTCP. Dev builds must go through
 * scripts/install-moof.sh --cdev, not the installer.
 * MoofTCP's own files: "MoofTCP" (cdev/ztcp, found by name and type/creator) and "MoofTCP Prefs"
 * (pref/ztcp). The planner never creates, replaces or deletes "MoofTCP Prefs" (the user's
 * configuration): it survives an upgrade and a Remove. If it is missing the executor writes none
 * (MoofTCP then starts unconfigured and the panel creates it).
 *
 * Op list order (interruption safety). Install: MKDIR (if the folder is missing), WRITE_MANIFEST,
 * the MOVEs, WRITE_MOOF last. Remove: the restoring MOVEs, DELETE_MOOF, DELETE_MANIFEST, RMDIR
 * (only if the folder is empty afterwards). The manifest is written before the first move and
 * deleted after the last restore, so after a crash at ANY point (a crash is a prefix of the op
 * list, each op atomic) a fresh inst_plan_remove, with the manifest when the file exists and the
 * known names otherwise, returns the volume to its original state. Re-running install after a
 * crash is also safe (it rewrites the manifest including the files already parked).
 * Exception: an empty 'MacTCP (Disabled)' left by a crash right after MKDIR is harmless and is
 * not removed (no MoofTCP and no manifest = NOTHING_TO_REMOVE).
 *
 * Op semantics:
 *   INST_MKDIR           create folder `to`.
 *   INST_WRITE_MANIFEST  create or overwrite file `to` (type 'TEXT', creator 'ztcp') with
 *                        inst_manifest_encode(*man_out) (data fork only).
 *   INST_MOVE            move file `from` to `to` (same volume; forks, type/creator, Finder info and
 *                        dates preserved: a catalog move, never copy+delete).
 *   INST_WRITE_MOOF      create or replace the file `to` with the bundled MoofTCP (cdev/ztcp).
 *   INST_DELETE_MOOF     delete file `from` (the MoofTCP file).
 *   INST_DELETE_MANIFEST delete file `from`.
 *   INST_RMDIR           delete the empty folder `from`.
 *
 * Return values: >= 0 = number of ops (0 together with BLOCKED_* or ALREADY_INSTALLED /
 * NOTHING_TO_REMOVE); negative = INST_E_*. A BLOCKED_* result has zero ops (nothing is moved). */
#ifndef INST_PLAN_H
#define INST_PLAN_H

#include <stddef.h>
#include <stdint.h>

#define INST_NAME_MAX 31
#define INST_PATH_MAX 64
#define INST_MAN_MAX 8            /* manifest entries (the MacTCP set has 3) */
#define INST_OPS_MAX 8            /* a cap of this size always suffices */

#define INST_SYSFOLDER   "System Folder"
#define INST_DISABLED    "MacTCP (Disabled)"
#define INST_MOOF_NAME   "MoofTCP"
#define INST_PREFS_NAME  "MoofTCP Prefs"
#define INST_MANIFEST_NAME "MoofTCP Manifest"   /* inside 'MacTCP (Disabled)', TEXT/ztcp */

#define INST_FCC(a, b, c, d) (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
                              ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* One directory entry of a snapshot. `vers` is the packed version of the file's 'vers' 1 resource
 * (0 = unknown or none); it is only read for the MoofTCP file. */
typedef struct {
    char name[32];                /* NUL-terminated, <= 31 characters */
    uint32_t type, creator;
    uint8_t locked, is_dir;
    uint32_t vers;
} InstEntry;

typedef enum {
    INST_MOVE, INST_MKDIR, INST_WRITE_MOOF, INST_DELETE_MOOF,
    INST_WRITE_MANIFEST, INST_DELETE_MANIFEST, INST_RMDIR
} InstOpKind;

typedef struct { InstOpKind kind; char from[INST_PATH_MAX], to[INST_PATH_MAX]; } InstOp;

typedef enum {
    INST_ALREADY_INSTALLED = 1,   /* install: MoofTCP up to date, no MacTCP to park: 0 ops */
    INST_NOTHING_TO_REMOVE,       /* remove: no MoofTCP and no manifest: 0 ops */
    INST_INSTALLED_PARKED_MACTCP, /* install: at least one MacTCP file parked (also when MoofTCP was already current) */
    INST_INSTALLED_NO_MACTCP,     /* install: fresh MoofTCP, nothing to park */
    INST_UPGRADED,                /* install: an older MoofTCP was replaced (takes precedence over PARKED) */
    INST_REMOVED_RESTORED,        /* remove: MoofTCP gone, at least one parked file restored */
    INST_REMOVED_NO_MACTCP,       /* remove: MoofTCP gone, nothing to restore */
    INST_BLOCKED_LOCKED,          /* a file that must move or be replaced/deleted is locked: 0 ops */
    INST_BLOCKED_COLLISION        /* a same-named file is in the way (see below): 0 ops */
} InstMsg;
/* COLLISION, install: a trio name already exists inside 'MacTCP (Disabled)' (it could be anything;
 * with the file also in the System Folder it is not ours), 'MacTCP (Disabled)' needed but a plain
 * file of that name is in the System Folder, or "MoofTCP" exists as a folder or with another
 * type/creator. Remove: a file to restore would land on a same-named file of the System Folder. */

enum { INST_E_ARG = -1, INST_E_CAP = -2, INST_E_MANIFEST = -3 };

typedef struct {
    int n;
    struct { char name[32], folder[32]; } e[INST_MAN_MAX];   /* file name, folder it came from */
} InstManifest;

/* Manifest text: one line per parked file, "name<TAB>folder<CR>", every line CR-terminated.
 * Names and folders: 1..31 bytes, no ':' and no control characters (so no TAB, CR, LF); spaces,
 * parentheses and bytes >= 128 are fine. The encoder refuses (INST_E_MANIFEST) on a bad name or
 * n > INST_MAN_MAX; it returns the byte count (no NUL is written) or INST_E_CAP when `cap` is too
 * small. The decoder is strict: a missing TAB, extra TAB, empty or bad field, a last line without
 * CR (a truncated write), or more than INST_MAN_MAX lines return INST_E_MANIFEST; the caller then
 * passes NULL to inst_plan_remove (fallback to the known names). Returns the entry count. */
int inst_manifest_encode(const InstManifest *m, char *buf, size_t cap);
int inst_manifest_decode(const char *text, size_t len, InstManifest *m);

/* Plan an install.
 *   sys, nsys            entries of the System Folder (files and folders)
 *   disabled, ndisabled  entries of 'MacTCP (Disabled)'; ndisabled < 0 = the folder does not exist
 *   moof_vers            packed 'vers' of the MoofTCP file being installed; an installed MoofTCP
 *                        with a smaller vers (including 0 = unknown) is replaced, an equal or
 *                        newer one is left alone (no downgrade)
 *   prev                 the decoded manifest of an interrupted earlier install, or NULL (no manifest file,
 *                        or unreadable). Only files it lists AND that are in 'MacTCP (Disabled)' count as
 *                        already parked by us; any other MacTCP-named file there is not ours.
 *   ops, cap             output op list (cap >= INST_OPS_MAX always suffices; else INST_E_CAP)
 *   msg                  result message (always set when the return is >= 0)
 *   man_out              set to the manifest to write when the list holds INST_WRITE_MANIFEST (it
 *                        also lists the files of `prev` that are still parked), else empty
 * Deviation from the first sketch of the signature: moof_vers, prev and man_out are added; the planner
 * cannot decide "up to date" or build the manifest file without them. */
int inst_plan_install(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                      uint32_t moof_vers, const InstManifest *prev, InstOp *ops, int cap, InstMsg *msg,
                      InstManifest *man_out);

/* Plan a Remove. Same snapshot arguments. `man` is the decoded manifest, or NULL when the manifest
 * file is unreadable (inst_manifest_decode failed; the file is in the snapshot). If the manifest
 * file does not exist nothing is restored: the manifest is written before the first move, so
 * nothing of ours is parked, and a MacTCP file in the folder belongs to someone else. With a manifest, every listed name found
 * in 'MacTCP (Disabled)' is moved to the listed folder (restoring to a folder other than
 * "System Folder" cannot be collision-checked; the executor's move then fails cleanly). Without
 * a usable one (file present), every MacTCP-set file (name and type/creator) found in 'MacTCP (Disabled)' goes back to the
 * System Folder. MoofTCP is deleted, "MoofTCP Prefs" is kept. The manifest file and, if then
 * empty, the folder are deleted. */
int inst_plan_remove(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                     const InstManifest *man, InstOp *ops, int cap, InstMsg *msg);

#endif
