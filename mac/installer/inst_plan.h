/* inst_plan.h - the MoofTCP installer planner (Plan 3 Task 9; portable: no Toolbox, no malloc, no
 * file system access). Task 10 writes the Toolbox executor and the UI on top of this contract.
 *
 * The planner is a pure function from a SNAPSHOT to an ordered OP LIST. The executor lists the
 * System Folder and the 'MacTCP (Disabled)' folder into InstEntry arrays, calls inst_plan_install
 * or inst_plan_remove, shows the InstMsg, and (when ops > 0) executes the ops in order, stopping at
 * the first failure. System 6.0.8 has no FSSpec and no FindFolder, so everything is names and
 * HFS-style paths; resolving a path to a volume/dirID is the executor's job.
 *
 * SNAPSHOT RULES (the executor guarantees them; the planner re-checks the NUL and returns
 * INST_E_ARG for an entry whose name has no NUL within its 32 bytes). `name` is a NUL-terminated
 * HFS name of 1..31 bytes (copy a Pascal string with its length byte, then add the NUL).
 *
 * Paths (InstOp.from/.to): ":System Folder:name" and ":System Folder:MacTCP (Disabled):name".
 * The longest possible is 64 characters; INST_PATH_MAX is 72 (compile-time checked in inst_plan.c),
 * and the planner returns INST_E_PATH instead of ever truncating or overflowing.
 *
 * Names: compared case-insensitively with ASCII folding only (HFS folds MacRoman too; none of the
 * names involved has such characters, and an entry that differs only by a MacRoman case pair is
 * not matched, which errs on the side of not touching it).
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
 * OP LIST ORDER. Install: MKDIR (if the folder is missing), WRITE_MANIFEST, the MOVEs, WRITE_MOOF
 * last (so MacTCP and MoofTCP are never live together because of us). Remove: DELETE_MOOF first
 * (MoofTCP gone before MacTCP returns: never two .IPP providers after a power loss), DELETE_TEMP of
 * a leftover System Folder MoofTCP.tmp (also before), the restoring MOVEs with the control panel
 * "MacTCP" LAST (Task 10, observed in Snow: the real MacTCP booted without "MacTCP DNR" creates a
 * fresh one, which would block the restore of the parked file after a crash), DELETE_TEMP of the
 * manifest temp, DELETE_MANIFEST, and RMDIR last. The manifest exists
 * from before the first move until after the last restore, so after a crash at ANY point (a
 * crash is a prefix of the op list, plus a half-done last op as described below) a fresh
 * inst_plan_remove returns the volume to its original state. Re-running install after a crash is
 * also safe. Property-tested in tests/test_inst_plan.c.
 *
 * ATOMICITY OF WRITE_MANIFEST / WRITE_MOOF (HFS on System 6.0.8 has no ExchangeFiles). Temp names:
 * INST_MANIFEST_TMP ("MoofTCP Manifest.tmp", in 'MacTCP (Disabled)') and INST_MOOF_TMP
 * ("MoofTCP.tmp", in the System Folder). THE TEMP FILE MUST NOT BE LOADABLE: System 6 runs the INIT
 * of every cdev in the System Folder whatever its name, so a complete cdev/ztcp temp would be a
 * second MoofTCP (or a MoofTCP next to MacTCP) at boot. Temps and a not-yet-final X therefore carry
 * type INST_TMP_TYPE ('MTMP') and creator INST_TMP_CREATOR ('mtin'); the type becomes cdev/ztcp
 * (TEXT/ztcp for the manifest) only as the LAST step. The executor writes X (the target) as:
 *   1. (manifest only) if X does not exist but X.tmp does and decodes, rename X.tmp to X (promote);
 *   2. delete X.tmp if it exists; PBHCreate X.tmp and IMMEDIATELY PBHSetFInfo it to type MTMP,
 *      creator mtin (and the Finder flags as shipped) BEFORE any fork is written (PBHCreate takes no
 *      type/creator: the file has type 0 / creator 0 until then); write all forks, flush to disk;
 *   3. delete X if it exists;
 *   4. rename X.tmp to X;
 *   5. set X's type/creator to cdev/ztcp (manifest: TEXT/ztcp). Only now is the file "complete".
 * What a crash leaves: before 3: X old (loadable) plus a partial or complete non-loadable X.tmp
 * (harmless); between 3 and 4: X absent, X.tmp complete; between 4 and 5: X with type MTMP/mtin
 * ("incomplete": the planner treats a file named MoofTCP of that type as ours, Install rewrites it,
 * Remove deletes it); after 5: the new X. A crash between PBHCreate and PBHSetFInfo leaves a
 * zero-info X.tmp (type 0, creator 0): that is also OURS (non-loadable, deleted or rewritten, never
 * blocking). For the manifest "exists" means: X or X.tmp is in the
 * Disabled folder (a FOLDER of that name is foreign and never read); the executor decodes X, else
 * X.tmp, and passes that as `prev` / `man` (NULL when neither decodes; the planner then uses the
 * known names). Temp files of ours (type MTMP/mtin, or type 0 / creator 0) are deleted by Remove (DELETE_TEMP, and for the System
 * Folder one BEFORE the restoring MOVEs) and replaced by step 2. A temp name that is a folder or has
 * any other type (a LOADABLE cdev/ztcp MoofTCP.tmp included) is foreign: BLOCKED_COLLISION naming it,
 * for Install (even when MoofTCP is otherwise up to date, so it never ends with two providers) and
 * Remove; nothing is deleted. Install checks this, and the lock bit, BEFORE it parks any MacTCP
 * file, so step 2 cannot fail after MacTCP is parked.
 * Step 3's PBHDelete of the old X returns fBsyErr (-47) if X is open (the Control Panel showing it,
 * or the INIT's resource file): the executor stops with a message to close the Control Panel and
 * quit other programs; the state left (old X plus an MTMP temp) is harmless and re-runnable.
 * WRITE_MOOF's data: the executor (the installer application) holds the MoofTCP file as its own
 * resources plus data fork; it writes a file with the same resources and the Finder flags of the
 * shipped file (hasBundle 0x2000), the 'BNDL'/'FREF'/icon resources included, so Finder shows the
 * icon, and retypes it cdev/ztcp in step 5. The installer's own version resource gives `moof_vers`.
 *
 * Op semantics:
 *   INST_MKDIR           create folder `to`.
 *   INST_WRITE_MANIFEST  write file `to` (TEXT/ztcp, data fork) with inst_manifest_encode(*man_out),
 *                        using the 5 steps above.
 *   INST_MOVE            move file `from` to `to` (same volume; forks, type/creator, Finder info and
 *                        dates preserved: a catalog move, never copy+delete).
 *   INST_WRITE_MOOF      create or replace the file `to` with the bundled MoofTCP, using the 5 steps.
 *   INST_DELETE_MOOF     delete file `from` (the MoofTCP file, complete or incomplete).
 *   INST_DELETE_MANIFEST delete file `from`.
 *   INST_DELETE_TEMP     delete the leftover temp file `from` (never anything but our temp types).
 *   INST_RMDIR           delete the empty folder `from` with PBHDelete, which refuses a non-empty folder
 *                        (fBsyErr, -47): that refusal is the safety net; NEVER delete recursively. Planned only for a folder this installer
 *                        created (manifest `created` flag) and only when it ends up empty.
 *
 * Return values: >= 0 = number of ops (0 together with BLOCKED_*, ALREADY_INSTALLED or
 * NOTHING_TO_REMOVE); negative = INST_E_*. A BLOCKED_* result has zero ops (nothing is moved) and
 * names the file in the way in `blocker`.
 *
 * LOCKS. The planner is conservative: BLOCKED_LOCKED is returned when ANY file the plan would move,
 * replace or delete is locked: a MacTCP file to park, a parked file to restore, MoofTCP (when it
 * is to be upgraded or removed), the manifest or a temp file. (Whether CatMove works on a locked
 * file was not verified on System 6; blocking is the safe side.) The executor shows the name and
 * asks the user to unlock it in Get Info.
 * Task 10 message hints: BLOCKED_LOCKED: "The file \"<blocker>\" is locked. Unlock it in the Finder
 * (File > Get Info) and run the installer again."; BLOCKED_COLLISION (install): "A file named
 * \"<blocker>\" is already in the folder MacTCP (Disabled) (or is in the way). Remove or rename it,
 * then run the installer again."; BLOCKED_COLLISION (remove): "There is a new \"<blocker>\" in the
 * System Folder. Remove it first so the original can be put back." (This is also what happens when
 * the user re-adds MacTCP after an install: nothing is overwritten.) */
#ifndef INST_PLAN_H
#define INST_PLAN_H

#include <stddef.h>
#include <stdint.h>

#define INST_NAME_MAX 31
#define INST_PATH_MAX 72
#define INST_MAN_MAX 8            /* manifest entries (the MacTCP set has 3; deduplicated) */
#define INST_OPS_MAX 12           /* a cap of this size always suffices */

#define INST_SYSFOLDER   "System Folder"
#define INST_DISABLED    "MacTCP (Disabled)"
#define INST_MOOF_NAME   "MoofTCP"
#define INST_MOOF_TMP    "MoofTCP.tmp"
#define INST_PREFS_NAME  "MoofTCP Prefs"
#define INST_MANIFEST_NAME "MoofTCP Manifest"       /* inside 'MacTCP (Disabled)', TEXT/ztcp */
#define INST_MANIFEST_TMP  "MoofTCP Manifest.tmp"
#define INST_TMP_TYPE      INST_FCC('M', 'T', 'M', 'P')   /* non-loadable type of temps and of an incomplete MoofTCP */
#define INST_TMP_CREATOR   INST_FCC('m', 't', 'i', 'n')

#define INST_FCC(a, b, c, d) (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
                              ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* Variants of the MoofTCP file (docs/dev/RELEASE.md "Variants"): the full build (DHCP client) and
 * MoofTCP Lite (compile option MT_NO_DHCP). One installer carries both and installs the one chosen.
 * The variant of a file is read from the long version string of its 'vers' 1: "MoofTCP Lite ..." =
 * Lite, anything else (every MoofTCP before Lite included) = full. */
#define INST_VAR_FULL 0
#define INST_VAR_LITE 1

/* One directory entry of a snapshot. `vers` is the packed version of the file's 'vers' 1 resource
 * (0 = unknown or none) and `variant` its INST_VAR_*; both are only read for the MoofTCP file. */
typedef struct {
    char name[32];                /* NUL-terminated, <= 31 characters */
    uint32_t type, creator;
    uint8_t locked, is_dir;
    uint32_t vers;
    uint8_t variant;
} InstEntry;

typedef enum {
    INST_MOVE, INST_MKDIR, INST_WRITE_MOOF, INST_DELETE_MOOF,
    INST_WRITE_MANIFEST, INST_DELETE_MANIFEST, INST_RMDIR, INST_DELETE_TEMP
} InstOpKind;

typedef struct { InstOpKind kind; char from[INST_PATH_MAX], to[INST_PATH_MAX]; } InstOp;

typedef enum {
    INST_ALREADY_INSTALLED = 1,   /* install: MoofTCP up to date, no MacTCP to park: 0 ops */
    INST_NOTHING_TO_REMOVE,       /* remove: no MoofTCP, no manifest, no temp file: 0 ops */
    INST_INSTALLED_PARKED_MACTCP, /* install: at least one MacTCP file parked (also when MoofTCP was already current) */
    INST_INSTALLED_NO_MACTCP,     /* install: fresh MoofTCP, nothing to park */
    INST_UPGRADED,                /* install: an older MoofTCP was replaced (takes precedence over PARKED) */
    INST_REMOVED_RESTORED,        /* remove: MoofTCP gone, at least one parked file restored */
    INST_REMOVED_NO_MACTCP,       /* remove: MoofTCP gone, nothing to restore */
    INST_BLOCKED_LOCKED,          /* a file that must move, be replaced or deleted is locked: 0 ops */
    INST_BLOCKED_COLLISION,       /* a same-named file is in the way (see below): 0 ops */
    INST_SWITCHED                 /* install: the same version of the OTHER variant was replaced (after UPGRADED,
                                   * before PARKED in precedence) */
} InstMsg;
/* COLLISION, install: a trio name already exists inside 'MacTCP (Disabled)' (it could be anything;
 * with the file also in the System Folder it is not ours), 'MacTCP (Disabled)' needed but a plain
 * file of that name is in the System Folder, or "MoofTCP" exists as a folder or with another
 * type/creator. Remove: a file to restore would land on a same-named file of the System Folder. */

enum { INST_E_ARG = -1, INST_E_CAP = -2, INST_E_MANIFEST = -3, INST_E_PATH = -4 };

typedef struct {
    int n;
    int created;                  /* 1 = this installer made 'MacTCP (Disabled)' (RMDIR it when empty) */
    struct { char name[32], folder[32]; } e[INST_MAN_MAX];   /* file name, folder it came from */
} InstManifest;

/* Manifest text: an optional first line "#created<CR>", then one line per parked file,
 * "name<TAB>folder<CR>", every line CR-terminated. A manifest without the first line (older
 * format) means created = 0. Names and folders: 1..31 bytes, no ':' and no control characters (so
 * no TAB, CR, LF); spaces, parentheses and bytes >= 128 are fine. The encoder refuses
 * (INST_E_MANIFEST) on a bad name or n > INST_MAN_MAX; it returns the byte count (no NUL is
 * written) or INST_E_CAP when `cap` is too small. The decoder is strict: a missing TAB, extra TAB,
 * empty or bad field, a last line without CR (a truncated write), or more than INST_MAN_MAX lines
 * return INST_E_MANIFEST, and so does empty text (a file truncated to nothing; the planner never writes an empty manifest). Returns the entry count. The codec is generic; the PLANNER only accepts
 * entries naming one of the three MacTCP files with folder "System Folder". */
int inst_manifest_encode(const InstManifest *m, char *buf, size_t cap);
int inst_manifest_decode(const char *text, size_t len, InstManifest *m);

/* Plan an install.
 *   sys, nsys            entries of the System Folder (files and folders)
 *   disabled, ndisabled  entries of 'MacTCP (Disabled)'; ndisabled < 0 = the folder does not exist
 *   moof_vers            packed 'vers' of the MoofTCP file being installed; an installed MoofTCP
 *                        with a smaller vers (including 0 = unknown) is replaced, an equal or
 *                        newer one is left alone (no downgrade)
 *   moof_variant         INST_VAR_* of the file being installed: an installed MoofTCP of the SAME
 *                        vers but the other variant is replaced too (INST_SWITCHED; variant switch
 *                        without Remove). A newer one of either variant is never replaced. The write
 *                        is the ordinary WRITE_MOOF (the 5 steps), so a switch has exactly the crash
 *                        and recovery properties of an upgrade.
 *   prev                 the decoded manifest of an earlier (interrupted) install, or NULL when
 *                        there is none or it is unreadable. If the snapshot shows a manifest file
 *                        (or temp) but prev is NULL, the MacTCP-set files found in the Disabled
 *                        folder are treated as ours and listed in the new manifest (so a later
 *                        Remove cannot strand them; `created` is then unknown and stays 0).
 *   ops, cap             output op list (cap >= INST_OPS_MAX always suffices; else INST_E_CAP)
 *   msg                  result message (always set when the return is >= 0)
 *   blocker              32-byte buffer (or NULL): the name in the way for BLOCKED_*, else ""
 *   man_out              set to the manifest to write when the list holds INST_WRITE_MANIFEST,
 *                        else empty. Entries are deduplicated by folded name.
 * Deviation from the first sketch of the signature: moof_vers, prev, blocker and man_out are
 * added; the planner cannot decide "up to date", build the manifest file or name the culprit
 * without them. */
int inst_plan_install(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                      uint32_t moof_vers, int moof_variant, const InstManifest *prev, InstOp *ops, int cap, InstMsg *msg,
                      char *blocker, InstManifest *man_out);

/* Plan a Remove. Same snapshot arguments. `man` is the decoded manifest, or NULL when the manifest
 * is unreadable or absent. The manifest is NOT trusted: only entries naming one of the three
 * MacTCP files (case-folded) with folder "System Folder" are accepted; any other entry (a foreign
 * name, another folder, ':' or '..' tricks, the manifest's own name) makes the call return
 * INST_E_MANIFEST and the executor must call again with man = NULL. An accepted entry is restored
 * only if the file is in the Disabled folder with the set's type/creator; otherwise it is skipped
 * (already restored, or not ours). Duplicates are folded. With man == NULL but a manifest file (or
 * temp) in the snapshot, every MacTCP-set file in the Disabled folder goes back to the System
 * Folder. With no manifest file and no temp, nothing is restored (nothing of ours is parked).
 * MoofTCP is deleted, "MoofTCP Prefs" is kept. RMDIR is planned only if the manifest says this
 * installer created the folder and it ends empty (an unreadable manifest never RMDIRs). */
int inst_plan_remove(const InstEntry *sys, int nsys, const InstEntry *disabled, int ndisabled,
                     const InstManifest *man, InstOp *ops, int cap, InstMsg *msg, char *blocker);

#endif
