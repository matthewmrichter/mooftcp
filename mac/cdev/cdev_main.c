/* cdev_main.c - the MoofTCP control panel (Plan 3 Task 8; the 'cdev' code resource of the file
 * "MoofTCP", mac/moof/moof.r + mac/cdev/moofcdev.r). The decisions live in cdev_logic.c
 * (host-tested); this file is the Toolbox shell.
 *
 * The Control Panel desk accessory calls cdev_start.s with one message at a time; cdev_main gets
 * the arguments as a struct (CdevArgs, cdev_logic.h). What it does:
 *   initDev    allocate the state handle, read ":System Folder:MoofTCP Prefs": MCFG 128 through
 *              cfe_decode (the saved settings; empty fields when there is no file or no usable
 *              resource) and the draft MCFD 128 (edits that could not be saved last time; shown
 *              instead of the saved values when present), show the status line.
 *   hitDev     "Keep a diagnostic log" checkbox (prefs flags bit 0; on by default for a new prefs file,
 *              Task 11b) and "Get the address by DHCP" (MCFG mode word 1, Plan 4): toggle, count
 *              as edits like the text fields. MoofTCP Lite (MT_NO_DHCP): item IT_DHCP is a static
 *              text saying there is no DHCP (moofcdev.r); f->dhcp stays 0 and nothing saves mode 1.
 *              Save: validate (an invalid field gets an alert naming it, the field is selected and
 *              the text stays; the off-subnet router warning asks first), then write the prefs.
 *              Revert: put the saved values back.
 *   keyEvtDev  Return/Enter are swallowed (a field would get a CR); a character that would make a
 *              field longer than its cap (15 / 63) is refused with a beep; every key re-checks.
 *   nulDev     caret blink, the check line (validation feedback) after a change, and the status
 *              line once a second (it follows the DHCP client: cdl_status_text).
 *   cut/copy/paste/clearDev   TextEdit on the current field, with the desk scrap; paste respects
 *              the cap (checked before the scrap is copied).
 *   closeDev   cdl_close_action: valid unsaved edits ask Save / Don't Save (closeDev cannot stop
 *              the close, so there is no Cancel); invalid unsaved edits are kept as the draft with an
 *              alert saying so; a draft the user reverted is deleted. Then the state handle is freed.
 *   macDev     1 (runs on every machine; the 'mach' resource asks).
 * Every write passes the existing MCFG bytes to cfe_encode so the mode word, flags byte and unknown
 * tail survive (ledger R3). The status line looks at the unit table only (mt_units.h; ruling P3:
 * the panel never opens .IPP or .ENET0); MoofTCP's status view (mt_view.h) is reached through the
 * unit's DCE (dCtlStorage). Settings take effect at the next restart.
 *
 * Files: System 6 has no FindFolder/FSSpec. The prefs live in the blessed System Folder, whose
 * working-directory refnum is BootDrive (0x210): the same reference the driver's loader uses
 * (mac/glue/mt_cfg.c), so the panel and the driver can never look at different files.
 *
 * Code rules: position independent (-mpcrel, no globals, no tables of pointers; check_pcrel.sh
 * fails the build on any absolute relocation reachable from _start, memcpy/memset included).
 * Memory: the state handle is allocated in initDev and disposed of in closeDev; everything else
 * (Resource Manager handles, the encode buffer) is released before the message returns. */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Resources.h>
#include <Files.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>

#include <stdint.h>
#include "cfg_edit.h"
#include "cdev_logic.h"
#include "mt_units.h"

#pragma GCC diagnostic ignored "-Warray-bounds"
#define LM_BOOTDRIVE   (*(volatile int16_t *)0x210)    /* WD refnum of the System Folder */
#define LM_TESCRPLEN   (*(volatile int16_t *)0x0AB0)
#define LM_TESCRPH     (*(Handle volatile *)0x0AB4)

/* Control Panel messages and results (Inside Macintosh V, "The Control Panel") */
enum { initDev = 0, hitDev, closeDev, nulDev, updateDev, activDev, deactivDev, keyEvtDev, macDev,
       undoDev, cutDev, copyDev, pasteDev, clearDev };
enum { cdevGenErr = -1, cdevMemErr = 0, cdevResErr = 1, cdevUnset = 3 };

/* Items of DITL -4064 (moofcdev.r), 1-based; the Control Panel appends them after its own
 * numItems items. Field item = IT_IP + (CFE_F_* - 1). */
enum { IT_REVERT = 1, IT_IP, IT_MASK, IT_GW, IT_DNS, IT_DOMAIN,
       IT_L_IP, IT_L_MASK, IT_L_GW, IT_L_DNS, IT_L_DOMAIN, IT_STATUS, IT_CHECK, IT_NOTE, IT_VERSION,
       IT_SAVE, IT_LOG, IT_DHCP /* Lite: the static text "MoofTCP Lite does not support DHCP." */ };

/* Alerts (moofcdev.r). Fixed IDs in the cdev's own file: CPanelID is the Control Panel desk
 * accessory's own resource base (observed -15904 = 0xC1E0 in Snow, DRVR 15's owned range), not the
 * cdev's -4064, so it cannot be used to find our resources. */
enum { AL_STOP = -4048, AL_ASK = -4047 };

typedef struct {
    CfeFields saved;      /* what MCFG 128 holds (Revert puts it back; "unchanged" means equal) */
    CfeFields draft;      /* the draft read at initDev or written since (valid if has_draft) */
    CfeFields cur;        /* the fields as last read while the items existed: the Control Panel removes
                           * our items BEFORE it sends closeDev when another panel is picked (Snow:
                           * GetControlValue on item 19 of a 2-item list = address error), so close
                           * must never touch the dialog */
    short has_draft;
    short check_pending;  /* a key or edit happened: re-check in the next nulDev */
    short shown_key;      /* cdl_check_key of what the check line shows; -1 = force a redraw */
    long status_at;       /* TickCount of the last status refresh */
    unsigned char status[64];   /* what the status line shows (Pascal) */
} CdevState;

#define PREFS_NAME ((ConstStringPtr)"\pMoofTCP Prefs")
#define NOTE_NORMAL "Changes take effect after you restart."
#define NOTE_DRAFT  "Kept from last time, not saved yet."

static void zero(void *p, long n) {
    char *c = (char *)p;
    while (n-- > 0) *c++ = 0;
}

/* --- dialog items --- */

/* Set an item's text. SetDialogItemText's own drawing does not show inside the Control Panel
 * (observed in Snow: the change appeared only with the next full redraw), so the item's rectangle
 * is also invalidated: the Control Panel redraws it with its next update event. */
static void set_ptext(DialogPtr d, short base, short it, const unsigned char *s) {
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, (short)(base + it), &type, &h, &r);
    SetDialogItemText(h, s);
    InsetRect(&r, -4, -4);                                  /* editText frames lie 3 px outside */
    InvalRect(&r);
}

static void set_text(DialogPtr d, short base, short it, const char *c) {
    Str255 s;
    s[0] = 0;
    cdl_pcat(s, c);
    set_ptext(d, base, it, s);
}

/* A field's text as a C string in out[cap]. Text that does not fit is replaced by "!" so the
 * validator reports the field (nothing longer than the buffer can be valid anyway). */
static void get_text(DialogPtr d, short base, short it, char *out, int cap) {
    Str255 s;
    short type;
    Handle h;
    Rect r;
    int i, n;
    GetDialogItem(d, (short)(base + it), &type, &h, &r);
    GetDialogItemText(h, s);
    n = s[0];
    if (n >= cap) { out[0] = '!'; out[1] = 0; return; }
    for (i = 0; i < n; i++) out[i] = (char)s[i + 1];
    out[n] = 0;
}

/* The checkboxes: "Keep a diagnostic log" (Task 11b), "Get the address by DHCP" (Plan 4). */
static ControlHandle box(DialogPtr d, short base, short it) {
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, (short)(base + it), &type, &h, &r);
    return (ControlHandle)h;
}

/* The dialog still holds our items (count word of the item list = items - 1). */
static int items_present(DialogPtr d, short base) {
    Handle il = ((DialogPeek)d)->items;
    return il && *il && (**(short **)il) + 1 >= base + IT_DHCP;
}

static void read_fields(DialogPtr d, short base, CfeFields *f) {
    f->log = (uint8_t)(GetControlValue(box(d, base, IT_LOG)) ? 1 : 0);
#if CFE_HAS_DHCP
    f->dhcp = (uint8_t)(GetControlValue(box(d, base, IT_DHCP)) ? 1 : 0);
#else
    f->dhcp = 0;                        /* Lite: item IT_DHCP is the "no DHCP" static text */
#endif
    get_text(d, base, IT_IP, f->ip, (int)sizeof f->ip);
    get_text(d, base, IT_MASK, f->mask, (int)sizeof f->mask);
    get_text(d, base, IT_GW, f->gw, (int)sizeof f->gw);
    get_text(d, base, IT_DNS, f->dns, (int)sizeof f->dns);
    get_text(d, base, IT_DOMAIN, f->domain, (int)sizeof f->domain);
}

static void show_fields(DialogPtr d, short base, const CfeFields *f) {
    set_text(d, base, IT_IP, f->ip);
    set_text(d, base, IT_MASK, f->mask);
    set_text(d, base, IT_GW, f->gw);
    set_text(d, base, IT_DNS, f->dns);
    set_text(d, base, IT_DOMAIN, f->domain);
    SetControlValue(box(d, base, IT_LOG), f->log ? 1 : 0);
#if CFE_HAS_DHCP
    SetControlValue(box(d, base, IT_DHCP), f->dhcp ? 1 : 0);
#endif
}

static short alert(short which, const unsigned char *msg) {
    ParamText(msg, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    if (which == AL_STOP) return StopAlert(AL_STOP, NULL);
    return CautionAlert(AL_ASK, NULL);
}

static void alert_c(short which, const char *a, const char *b, const char *c) {
    Str255 ps;
    ps[0] = 0;
    cdl_pcat(ps, a);
    if (b) cdl_pcat(ps, b);
    if (c) cdl_pcat(ps, c);
    (void)alert(which, ps);
}

/* --- the status line: what the unit table says (never opens a driver) --- */

/* Redraw only when the text changed (force: always). */
static void show_status(DialogPtr d, short base, CdevState *st, int force) {
    const uint8_t *dce, *drv = mt_units_find_ipp_dce(&dce);
    const MtStatusView *v = NULL;
    int unit = !drv ? CDL_UNIT_NONE : mt_units_is_moof(drv) ? CDL_UNIT_MOOF : CDL_UNIT_MACTCP;
    Str255 ps;
    int i;
    if (unit == CDL_UNIT_MOOF) v = mt_units_view_in(dce, MT_UNITS_MASK24);
    cdl_status_text(unit, v, (uint32_t)TickCount(), ps);
    st->status_at = TickCount();
    if (ps[0] > 63) ps[0] = 63;
    if (!force && ps[0] == st->status[0]) {
        for (i = 1; i <= ps[0] && ps[i] == st->status[i]; i++) {}
        if (i > ps[0]) return;
    }
    for (i = 0; i <= ps[0]; i++) st->status[i] = ps[i];
    set_ptext(d, base, IT_STATUS, ps);
}

/* --- the check line: live validation feedback --- */

static void update_check(DialogPtr d, short base, CdevState *st) {
    CfeFields f;
    CfeError e;
    Str255 ps;
    int rc, key;
    read_fields(d, base, &f);
    rc = cfe_validate(&f, &e);
    key = cdl_check_key(&f, rc, &e);
    if (key == st->shown_key) return;
    cdl_problem_text(&f, rc, &e, ps);
    set_ptext(d, base, IT_CHECK, ps);
    st->shown_key = (short)key;
}

/* --- the prefs file --- */

/* Read MCFG 128 into *saved and MCFD 128 into *draft; returns 1 if a draft was found. */
static int load_prefs(CfeFields *saved, CfeFields *draft) {
    short cur = CurResFile(), ref;
    Handle h;
    int has = 0;
    zero(saved, sizeof *saved);
    zero(draft, sizeof *draft);
    cdl_load_saved(NULL, 0, saved);                         /* no file: empty fields, log on */
    ref = OpenRFPerm(PREFS_NAME, LM_BOOTDRIVE, fsRdPerm);
    if (ref != -1) {
        UseResFile(ref);
        h = Get1Resource('MCFG', 128);
        if (h) {
            HLock(h);
            cdl_load_saved((const uint8_t *)*h, (uint32_t)GetHandleSize(h), saved);
            HUnlock(h);
            ReleaseResource(h);
        }
        h = Get1Resource('MCFD', 128);
        if (h) {
            HLock(h);
            has = cdl_draft_decode((const uint8_t *)*h, (uint32_t)GetHandleSize(h), draft,
                                   saved->log) == 0;
            HUnlock(h);
            ReleaseResource(h);
        }
        CloseResFile(ref);
    }
    UseResFile(cur);
    return has;
}

/* Open the prefs file's resource fork for writing (current file afterwards), after the checks
 * that can be made before anything is written: volume locked, under 4 KB free, file locked, its
 * resource fork already open, damaged. may_create: create the file and/or its resource fork if
 * absent (*created is then 1 if THIS call created the file). Returns NULL and *ref, or why not;
 * on failure nothing is left behind (a file this call created is deleted). *ref = -1 and NULL
 * when the file does not exist and may_create is 0. */
static const char *prefs_open(int may_create, short *ref, int *created) {
    short wd = LM_BOOTDRIVE, vol;
    HParamBlockRec vpb;
    ParamBlockRec fpb;
    OSErr e;
    *ref = -1;
    *created = 0;
    zero(&vpb, sizeof vpb);
    vpb.volumeParam.ioVRefNum = wd;
    if (PBHGetVInfoSync(&vpb) != noErr) return "the startup disk could not be read";
    if ((uint16_t)vpb.volumeParam.ioVAtrb & 0x8080u) return "the startup disk is locked";
    if ((long)(uint16_t)vpb.volumeParam.ioVFrBlk * vpb.volumeParam.ioVAlBlkSiz < 4096L)
        return "the startup disk is full";

    zero(&fpb, sizeof fpb);
    fpb.fileParam.ioNamePtr = (StringPtr)PREFS_NAME;
    fpb.fileParam.ioVRefNum = wd;
    e = PBGetFInfoSync(&fpb);
    if (e == fnfErr) {
        if (!may_create) return NULL;
        e = Create(PREFS_NAME, wd, 'ztcp', 'pref');
        if (e == dskFulErr) return "the startup disk is full";
        if (e != noErr) return "the file MoofTCP Prefs could not be created";
        *created = 1;
    } else if (e != noErr) {
        return "the file MoofTCP Prefs could not be read";
    } else if (fpb.fileParam.ioFlAttrib & 0x01) {
        return "the file MoofTCP Prefs is locked";
    } else if (fpb.fileParam.ioFlAttrib & 0x04) {
        return "the file MoofTCP Prefs is in use";        /* its resource fork is open */
    }

    *ref = OpenRFPerm(PREFS_NAME, wd, fsRdWrPerm);
    e = *ref == -1 ? ResError() : noErr;
    if (*ref == -1 && e == eofErr && may_create) {          /* no resource fork yet: make one */
        GetVol(NULL, &vol);
        SetVol(NULL, wd);
        CreateResFile(PREFS_NAME);
        e = ResError();
        SetVol(NULL, vol);
        if (e == noErr) {
            *ref = OpenRFPerm(PREFS_NAME, wd, fsRdWrPerm);
            e = *ref == -1 ? ResError() : noErr;
        }
    }
    if (*ref == -1) {
        if (*created) (void)FSDelete(PREFS_NAME, wd);       /* leave no empty file behind */
        *created = 0;
        if (e == dskFulErr) return "the startup disk is full";
        if (e == permErr || e == opWrErr) return "the file MoofTCP Prefs is in use";
        if (e == eofErr) return NULL;                       /* no fork and !may_create: nothing to do */
        return "the file MoofTCP Prefs is damaged (move it to the Trash and try again)";
    }
    UseResFile(*ref);
    return NULL;
}

/* Close; if why is set and this call created the file, delete it again. Returns why, or the
 * write error if why was NULL and UpdateResFile failed. */
static const char *prefs_close(short ref, int created, const char *why, short cur) {
    OSErr e = noErr;
    if (!why) {
        UpdateResFile(ref);
        e = ResError();
        if (e == dskFulErr) why = "the startup disk is full";
        else if (e == wPrErr || e == vLckdErr) why = "the startup disk is locked";
        else if (e != noErr) why = "the file MoofTCP Prefs could not be written";
    }
    CloseResFile(ref);                                      /* releases its resource handles */
    UseResFile(cur);
    if (why && created) (void)FSDelete(PREFS_NAME, LM_BOOTDRIVE);
    FlushVol(NULL, LM_BOOTDRIVE);
    return why;
}

/* The resource (type, 128) of the current file, or NULL. "Not in the map" and "in the map but not
 * loadable" (no memory, read error) are told apart, because adding a resource in the second case
 * would make a second one with the same ID: the map is looked up with ResLoad off (the entry, no
 * data), then the data is loaded and checked. Observed in Snow (System 6.0.8): a plain
 * Get1Resource of a missing resource does not leave ResError() == resNotFound, so ResError cannot
 * make this distinction. */
static Handle get_res(ResType t, const char **why) {
    Handle h;
    SetResLoad(false);
    h = Get1Resource(t, 128);
    SetResLoad(true);
    if (!h) return NULL;                                    /* not in the map */
    LoadResource(h);
    if (ResError() != noErr) {
        *why = "the file MoofTCP Prefs could not be read";
        return NULL;
    }
    return h;
}

/* Replace or add (type, 128) with len bytes. */
static const char *put_res(ResType t, Handle h, const uint8_t *data, uint32_t len) {
    if (h) {
        SetHandleSize(h, (Size)len);
        if (MemError() != noErr) return "there is not enough memory";
        BlockMove(data, *h, (Size)len);
        ChangedResource(h);
        if (ResError() != noErr) return "the file MoofTCP Prefs is protected";
        return NULL;
    }
    h = NewHandle((Size)len);
    if (!h) return "there is not enough memory";
    BlockMove(data, *h, (Size)len);
    AddResource(h, t, 128, (ConstStringPtr)"\p");
    if (ResError() != noErr) { DisposeHandle(h); return "the resource could not be added"; }
    return NULL;
}

static const char *drop_res(ResType t) {
    const char *why = NULL;
    Handle h = get_res(t, &why);
    if (h) {
        RemoveResource(h);
        if (ResError() != noErr) return "the file MoofTCP Prefs is protected";
        DisposeHandle(h);
    }
    return why;
}

/* Write f as MCFG 128 (keeping the prior bytes' mode, flags and tail) and drop the draft. */
static const char *write_prefs(const CfeFields *f) {
    short cur = CurResFile(), ref;
    int created;
    Handle h;
    Ptr out;
    long plen;
    uint32_t len;
    const char *why = prefs_open(1, &ref, &created);
    if (why) return why;
    h = get_res('MCFG', &why);
    if (!why) {
        plen = h ? GetHandleSize(h) : 0;
        out = NewPtr(plen + 128);
        if (!out) {
            why = "there is not enough memory";
        } else {
            if (h) HLock(h);
            len = cfe_encode(f, h ? (const uint8_t *)*h : NULL, (uint32_t)plen, (uint8_t *)out,
                             (uint32_t)(plen + 128));
            if (h) HUnlock(h);
            why = len ? put_res('MCFG', h, (const uint8_t *)out, len) : "the settings are invalid";
            DisposePtr(out);
        }
    }
    if (!why) why = drop_res('MCFD');
    return prefs_close(ref, created, why, cur);
}

static const char *write_draft(const CfeFields *f) {
    short cur = CurResFile(), ref;
    int created;
    uint8_t buf[160];                                       /* draft v2: at most 1 + 4*16 + 64 + 1 */
    uint32_t len = cdl_draft_encode(f, buf, sizeof buf);
    const char *why = prefs_open(1, &ref, &created);
    Handle h;
    if (why) return why;
    h = get_res('MCFD', &why);
    if (!why) why = put_res('MCFD', h, buf, len);
    return prefs_close(ref, created, why, cur);
}

static void drop_draft(void) {
    short cur = CurResFile(), ref;
    int created;
    const char *why = prefs_open(0, &ref, &created);
    if (why || ref == -1) return;
    why = drop_res('MCFD');
    (void)prefs_close(ref, 0, why, cur);
}

/* --- Save and close --- */

static void select_field(DialogPtr d, short base, int field) {
    if (field >= CFE_F_IP && field <= CFE_F_DOMAIN)
        SelectDialogItemText(d, (short)(base + IT_IP + field - 1), 0, 32767);
}

/* The Save button. Returns 1 if the settings were written. */
static int save_now(const CdevArgs *a, CdevState *st) {
    DialogPtr d = (DialogPtr)a->dlg;
    short base = a->num_items;
    CfeFields f;
    CfeError e;
    Str255 ps;
    const char *why;
    int rc;
    read_fields(d, base, &f);
    rc = cfe_validate(&f, &e);
    switch (cdl_save_action(rc)) {
    case CDL_SAVE_REFUSE:
        cdl_problem_text(&f, rc, &e, ps);
        cdl_pcat(ps, " Nothing was saved.");
        (void)alert(AL_STOP, ps);
        select_field(d, base, e.field);
        return 0;
    case CDL_SAVE_ASK_WARN:
        ps[0] = 0;
        cdl_pcat(ps, "The router is not on the same subnet as the IP address. Save anyway?");
        if (alert(AL_ASK, ps) != 1) return 0;
        break;
    default: break;
    }
    why = write_prefs(&f);
    if (why) {
        alert_c(AL_STOP, "The settings were not saved: ", why, ". The previous settings are unchanged.");
        return 0;
    }
    st->saved = f;
    st->has_draft = 0;
    set_text(d, base, IT_NOTE, NOTE_NORMAL);
    set_text(d, base, IT_CHECK, "Saved. Restart to use the new settings.");
    st->shown_key = -1;
    return 1;
}

static void close_panel(CdevState *st) {
    CfeFields f;
    CfeError e;
    Str255 ps;
    const char *why;
    int rc;
    f = st->cur;                                            /* not read_fields: the items are gone */
    rc = cfe_validate(&f, &e);
    switch (cdl_close_action(&f, &st->saved, st->has_draft ? &st->draft : NULL, rc)) {
    case CDL_CLOSE_DROP_DRAFT:
        drop_draft();
        break;
    case CDL_CLOSE_ASK:
        ps[0] = 0;
        cdl_pcat(ps, "Save the changes to the MoofTCP settings? They take effect after you restart.");
        if (rc == CFE_E_GW_OFFNET) cdl_pcat(ps, " (The router is not on the IP address's subnet.)");
        if (alert(AL_ASK, ps) == 1) {                    /* item 1 Save, item 2 Don't Save */
            why = write_prefs(&f);
            if (why) alert_c(AL_STOP, "The settings were not saved: ", why,
                             ". The previous settings are unchanged.");
        } else if (st->has_draft) {
            drop_draft();
        }
        break;
    case CDL_CLOSE_KEEP_DRAFT:
        cdl_problem_text(&f, rc, &e, ps);
        why = write_draft(&f);
        if (!why) {
            cdl_pcat(ps, " The settings were not saved; your changes are kept until you open MoofTCP again.");
        } else {
            cdl_pcat(ps, " The settings were not saved and your changes could not be kept: ");
            cdl_pcat(ps, why);
            cdl_pcat(ps, ".");
        }
        (void)alert(AL_STOP, ps);
        break;
    default: break;
    }
}

/* --- TextEdit on the current field --- */

/* The cap of the field TextEdit is editing, 0 if it is none of ours. */
static int current_cap(const CdevArgs *a, TEHandle *te) {
    DialogPeek dp = (DialogPeek)a->dlg;
    int it = dp->editField + 1 - a->num_items;              /* editField is 0-based */
    *te = dp->textH;
    if (!*te || dp->editField < 0 || it < IT_IP || it > IT_DOMAIN) return 0;
    return cdl_field_cap(it - IT_IP + 1);
}

static int insert_ok(TEHandle te, int cap, long n) {
    TEPtr t = *te;
    return cdl_insert_ok(t->teLength, t->selEnd - t->selStart, (int)(n > 255 ? 256 : n), cap);
}

static void edit_cmd(const CdevArgs *a, short message) {
    TEHandle te;
    int cap = current_cap(a, &te);
    long off, n;
    if (!cap) return;
    switch (message) {
    case cutDev:
    case copyDev:
        if (message == cutDev) TECut(te); else TECopy(te);
        if (ZeroScrap() == noErr && LM_TESCRPH) {
            HLock(LM_TESCRPH);
            (void)PutScrap(LM_TESCRPLEN, 'TEXT', *LM_TESCRPH);
            HUnlock(LM_TESCRPH);
        }
        break;
    case pasteDev:
        n = GetScrap(NULL, 'TEXT', &off);                 /* the length only, nothing copied */
        if (n < 0 || !LM_TESCRPH || !insert_ok(te, cap, n)) { SysBeep(5); break; }
        n = GetScrap(LM_TESCRPH, 'TEXT', &off);
        if (n < 0) { SysBeep(5); break; }
        LM_TESCRPLEN = (int16_t)n;
        TEPaste(te);
        break;
    case clearDev: TEDelete(te); break;
    default: SysBeep(5); break;                             /* undoDev: TextEdit has no undo */
    }
}

static long dispatch(const CdevArgs *a) {
    Handle sh;
    CdevState *st;
    DialogPtr d = (DialogPtr)a->dlg;
    EventRecord *ev = (EventRecord *)a->ev;
    short base = a->num_items;
    long r = a->storage;

    if (a->message == initDev) {
        sh = NewHandle(sizeof(CdevState));
        if (!sh) return cdevMemErr;
        HLock(sh);
        st = (CdevState *)*sh;
        zero(st, sizeof *st);
        st->shown_key = -1;
        st->has_draft = (short)load_prefs(&st->saved, &st->draft);
        show_fields(d, base, st->has_draft ? &st->draft : &st->saved);
        if (st->has_draft) set_text(d, base, IT_NOTE, NOTE_DRAFT);
        show_status(d, base, st, 1);
        update_check(d, base, st);
        SelectDialogItemText(d, (short)(base + IT_IP), 0, 32767);
        read_fields(d, base, &st->cur);
        HUnlock(sh);
        return (long)sh;
    }
    sh = (Handle)a->storage;
    if (!sh || r == cdevUnset || !*sh) return r;
    HLock(sh);
    st = (CdevState *)*sh;
    switch (a->message) {
    case hitDev:
        if (a->item - base == IT_REVERT) {
            show_fields(d, base, &st->saved);
            SelectDialogItemText(d, (short)(base + IT_IP), 0, 32767);
            st->check_pending = 1;
        } else if (a->item - base == IT_SAVE) {
            (void)save_now(a, st);
        } else if (a->item - base == IT_LOG || (CFE_HAS_DHCP && a->item - base == IT_DHCP)) {
            ControlHandle c = box(d, base, (short)(a->item - base));
            SetControlValue(c, GetControlValue(c) ? 0 : 1);
            st->check_pending = 1;
        }
        break;
    case keyEvtDev:
        if (!(ev->modifiers & cmdKey)) {
            unsigned char c = (unsigned char)(ev->message & charCodeMask);
            TEHandle te;
            int cap = current_cap(a, &te);
            if (c == '\r' || c == 3) {
                ev->what = nullEvent;                       /* Return/Enter: not field text */
            } else if (cap && cdl_key_inserts(c) && !insert_ok(te, cap, 1)) {
                ev->what = nullEvent;                       /* the field is full */
                SysBeep(5);
            }
            st->check_pending = 1;
        }
        break;
    case nulDev:
        if (((DialogPeek)d)->textH) TEIdle(((DialogPeek)d)->textH);
        if (st->check_pending) {
            st->check_pending = 0;
            update_check(d, base, st);
        }
        if ((unsigned long)(TickCount() - st->status_at) >= 60ul) show_status(d, base, st, 0);
        break;
    case undoDev:
    case cutDev:
    case copyDev:
    case pasteDev:
    case clearDev:
        edit_cmd(a, a->message);
        st->check_pending = 1;
        break;
    case closeDev:
        close_panel(st);
        HUnlock(sh);
        DisposeHandle(sh);
        return r;
    default:                                                /* updateDev, activDev, deactivDev */
        break;
    }
    if (items_present(d, base)) read_fields(d, base, &st->cur);
    HUnlock(sh);
    return r;
}

/* Every message draws into the Control Panel's dialog, but the current port at entry is whatever
 * the host left (the Finder's window during nulDev): set it, and put the caller's back. */
long cdev_main(const CdevArgs *a);
long cdev_main(const CdevArgs *a) {
    GrafPtr saved;
    long r;
    if (a->message == macDev) return 1;
    if (!a->dlg) return a->storage;
    GetPort(&saved);
    SetPort((GrafPtr)a->dlg);
    r = dispatch(a);
    SetPort(saved);
    return r;
}
