/* cdev_main.c - the MoofTCP control panel (Plan 3 Task 8; the 'cdev' code resource of the file
 * "MoofTCP", mac/moof/moof.r + mac/cdev/moofcdev.r).
 *
 * The Control Panel desk accessory calls cdev_start.s with one message at a time; cdev_main gets
 * the arguments as a struct. What it does:
 *   initDev    allocate the state handle (the only allocation that outlives a message), read
 *              ":System Folder:MoofTCP Prefs" (MCFG 128) through cfe_decode into the five edit
 *              fields (empty when there is no file or no usable resource), show the status line.
 *   hitDev     Revert: put the values read at initDev back.
 *   keyEvtDev  Return/Enter are swallowed (a field would get a CR); every key re-checks the fields.
 *   nulDev     caret blink, and the check line (validation feedback) after a change.
 *   cut/copy/paste/clearDev   TextEdit on the current field, with the desk scrap.
 *   closeDev   if the fields differ from what was read: ask Save / Don't Save. Save validates
 *              (cfe_validate: an invalid field gets an alert naming it, nothing is written; the
 *              off-subnet router warning asks again) and writes the prefs (write_prefs), passing the
 *              existing MCFG bytes to cfe_encode so the mode word, flags byte and unknown tail
 *              survive (ledger R3). Then the state handle is freed.
 *   macDev     1 (runs on every machine; the 'mach' resource asks).
 * The status line looks at the unit table only (mt_units.h; ruling P3: the panel never opens
 * .IPP or .ENET0). Settings take effect at the next restart (the driver reads them at boot).
 *
 * Files: System 6 has no FindFolder/FSSpec. The prefs live in the blessed System Folder, whose
 * working-directory refnum is BootDrive (0x210): the same reference the driver's loader uses
 * (mac/glue/mt_cfg.c), so the panel and the driver can never look at different files.
 *
 * Code rules: position independent (-mpcrel, no globals, no tables of pointers, no libc), see
 * cdev_start.s. Memory Manager: the state handle is allocated in initDev and disposed of in closeDev;
 * handles the Resource Manager returns are released before the message returns. */
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
 * numItems items. */
enum { IT_REVERT = 1, IT_IP, IT_MASK, IT_GW, IT_DNS, IT_DOMAIN,
       IT_L_IP, IT_L_MASK, IT_L_GW, IT_L_DNS, IT_L_DOMAIN, IT_STATUS, IT_CHECK, IT_NOTE, IT_VERSION };

/* Alerts (moofcdev.r). Fixed IDs in the cdev's own file: CPanelID is the Control Panel desk
 * accessory's own resource base (observed -15904 = 0xC1E0 in Snow, DRVR 15's owned range), not the
 * cdev's -4064, so it cannot be used to find our resources. */
enum { AL_STOP = -4048, AL_ASK = -4047 };

/* The Pascal arguments as they lie on the stack (cdev_start.s) */
typedef struct {
    DialogPtr dlg;
    long storage;
    EventRecord *ev;
    short panel_id;
    short num_items;
    short item;
    short message;
} CdevArgs;

typedef struct {
    CfeFields loaded;     /* what initDev read (and what Revert puts back) */
    short check_pending;  /* a key or edit happened: re-check in the next nulDev */
    short shown_code;     /* the check line shows the result for (shown_field, shown_code) */
    short shown_field;
} CdevState;

#define PREFS_NAME ((ConstStringPtr)"\pMoofTCP Prefs")

/* --- small string helpers (no libc in this code resource) --- */

static void zero(void *p, long n) {
    char *c = (char *)p;
    while (n-- > 0) *c++ = 0;
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pcat(unsigned char *ps, const char *c) {   /* append a C string to a Pascal string */
    while (*c && ps[0] < 255) ps[++ps[0]] = (unsigned char)*c++;
}

/* --- dialog items --- */

static Handle item_h(DialogPtr d, short base, short it) {
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, (short)(base + it), &type, &h, &r);
    return h;
}

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
    pcat(s, c);
    set_ptext(d, base, it, s);
}

/* A field's text as a C string in out[cap]. Text that does not fit is replaced by "!" so the
 * validator reports the field (nothing longer than the buffer can be valid anyway). */
static void get_text(DialogPtr d, short base, short it, char *out, int cap) {
    Str255 s;
    int i, n;
    GetDialogItemText(item_h(d, base, it), s);
    n = s[0];
    if (n >= cap) { out[0] = '!'; out[1] = 0; return; }
    for (i = 0; i < n; i++) out[i] = (char)s[i + 1];
    out[n] = 0;
}

static void read_fields(DialogPtr d, short base, CfeFields *f) {
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
}

static int fields_equal(const CfeFields *a, const CfeFields *b) {
    return streq(a->ip, b->ip) && streq(a->mask, b->mask) && streq(a->gw, b->gw) &&
           streq(a->dns, b->dns) && streq(a->domain, b->domain);
}

/* --- messages --- */

static const char *field_name(int field) {
    switch (field) {
    case CFE_F_IP: return "IP address";
    case CFE_F_MASK: return "subnet mask";
    case CFE_F_GW: return "router address";
    case CFE_F_DNS: return "DNS server address";
    default: return "local domain";
    }
}

/* The sentence for a validation result (rc from cfe_validate, e from it). */
static void problem_text(const CfeFields *f, int rc, const CfeError *e, unsigned char *ps) {
    ps[0] = 0;
    switch (rc) {
    case CFE_OK: break;
    case CFE_E_IP:
        if (!f->ip[0]) pcat(ps, "IP address required: the other fields are set.");
        else pcat(ps, "The IP address is invalid. Type four numbers 0-255, like 192.168.1.20.");
        break;
    case CFE_E_MASK: pcat(ps, "The subnet mask is invalid (for example 255.255.255.0)."); break;
    case CFE_E_GW: pcat(ps, "The router address is invalid."); break;
    case CFE_E_DNS: pcat(ps, "The DNS server address is invalid."); break;
    case CFE_E_DOMAIN:
        pcat(ps, "The local domain is invalid: letters, digits, - _ and inner dots, up to 63.");
        break;
    case CFE_E_ADDR_RESERVED:
        pcat(ps, "The ");
        pcat(ps, field_name(e->field));
        pcat(ps, " is a reserved address.");
        break;
    case CFE_E_HOSTBITS:
        pcat(ps, "The IP address is the network or broadcast address of its subnet.");
        break;
    case CFE_E_GW_SELF: pcat(ps, "The router address is the IP address itself."); break;
    case CFE_E_GW_OFFNET: pcat(ps, "Note: the router is not on the IP address's subnet."); break;
    default: pcat(ps, "The settings are invalid."); break;
    }
}

static short alert(short which, const unsigned char *msg) {
    ParamText(msg, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    if (which == AL_STOP) return StopAlert(AL_STOP, NULL);
    return CautionAlert(AL_ASK, NULL);
}

/* --- the status line: what the unit table says (never opens a driver) --- */

static void show_status(DialogPtr d, short base) {
    const uint8_t *drv = mt_units_find_ipp();
    set_text(d, base, IT_STATUS, !drv ? "Not loaded: restart after saving."
                                 : mt_units_is_moof(drv) ? "MoofTCP is loaded."
                                 : "MacTCP is loaded; MoofTCP is off.");
}

/* --- the check line: live validation feedback --- */

static void update_check(DialogPtr d, short base, CdevState *st) {
    CfeFields f;
    CfeError e;
    Str255 ps;
    int rc;
    read_fields(d, base, &f);
    rc = cfe_validate(&f, &e);
    if (rc == st->shown_code && (rc == CFE_OK || e.field == st->shown_field)) return;
    problem_text(&f, rc, &e, ps);
    set_ptext(d, base, IT_CHECK, ps);
    st->shown_code = (short)rc;
    st->shown_field = (short)(rc == CFE_OK ? 0 : e.field);
}

/* --- the prefs file --- */

static void load_prefs(CfeFields *f) {
    short saved = CurResFile(), ref;
    Handle h;
    zero(f, sizeof *f);
    ref = OpenRFPerm(PREFS_NAME, LM_BOOTDRIVE, fsRdPerm);
    if (ref != -1) {
        UseResFile(ref);
        h = Get1Resource('MCFG', 128);
        if (h) {
            HLock(h);
            (void)cfe_decode((const uint8_t *)*h, (uint32_t)GetHandleSize(h), f);   /* -1: f empty */
            HUnlock(h);
            ReleaseResource(h);
        }
        CloseResFile(ref);
    }
    UseResFile(saved);
}

/* Write f into MCFG 128 of the prefs file, creating the file or its resource fork if needed.
 * Returns NULL on success, else why not (a sentence fragment); on failure the previous file is
 * left as it was whenever the failure can be seen before writing (locked volume or file, too little
 * space, file in use, damaged file). */
static const char *write_prefs(const CfeFields *f) {
    short wd = LM_BOOTDRIVE, saved = CurResFile(), ref, vol;
    HParamBlockRec vpb;
    ParamBlockRec fpb;
    Handle h;
    Ptr out;
    long plen;
    uint32_t len;
    OSErr e;
    const char *why = NULL;
    int created = 0;

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
        e = Create(PREFS_NAME, wd, 'ztcp', 'pref');
        if (e == dskFulErr) return "the startup disk is full";
        if (e != noErr) return "the file MoofTCP Prefs could not be created";
        created = 1;
    } else if (e != noErr) {
        return "the file MoofTCP Prefs could not be read";
    } else if (fpb.fileParam.ioFlAttrib & 0x01) {
        return "the file MoofTCP Prefs is locked";
    } else if (fpb.fileParam.ioFlAttrib & 0x04) {
        return "the file MoofTCP Prefs is in use";        /* its resource fork is open */
    }

    ref = OpenRFPerm(PREFS_NAME, wd, fsRdWrPerm);
    if (ref == -1 && ResError() == eofErr) {               /* no resource fork yet: make one */
        GetVol(NULL, &vol);
        SetVol(NULL, wd);
        CreateResFile(PREFS_NAME);
        e = ResError();
        SetVol(NULL, vol);
        if (e == noErr) ref = OpenRFPerm(PREFS_NAME, wd, fsRdWrPerm);
        if (ref == -1) {                                    /* leave no empty file behind */
            if (created) (void)FSDelete(PREFS_NAME, wd);
            UseResFile(saved);
            return e == dskFulErr ? "the startup disk is full"
                                  : "the file MoofTCP Prefs could not be created";
        }
    }
    if (ref == -1) {
        e = ResError();
        UseResFile(saved);
        if (e == permErr || e == opWrErr) return "the file MoofTCP Prefs is in use";
        return "the file MoofTCP Prefs is damaged (move it to the Trash and try again)";
    }
    UseResFile(ref);
    h = Get1Resource('MCFG', 128);
    plen = h ? GetHandleSize(h) : 0;
    out = NewPtr(plen + 128);
    if (!out) {
        why = "there is not enough memory";
    } else {
        if (h) HLock(h);
        len = cfe_encode(f, h ? (const uint8_t *)*h : NULL, (uint32_t)plen, (uint8_t *)out,
                         (uint32_t)(plen + 128));
        if (h) HUnlock(h);
        if (!len) {
            why = "the settings are invalid";
        } else if (h) {
            SetHandleSize(h, (Size)len);
            if (MemError() != noErr) why = "there is not enough memory";
            else {
                BlockMove(out, *h, (Size)len);
                ChangedResource(h);
                if (ResError() != noErr) why = "the file MoofTCP Prefs is protected";
            }
        } else {
            h = NewHandle((Size)len);
            if (!h) {
                why = "there is not enough memory";
            } else {
                BlockMove(out, *h, (Size)len);
                AddResource(h, 'MCFG', 128, (ConstStringPtr)"\p");
                if (ResError() != noErr) { DisposeHandle(h); why = "the resource could not be added"; }
            }
        }
        if (!why) {
            UpdateResFile(ref);
            e = ResError();
            if (e == dskFulErr) why = "the startup disk is full";
            else if (e == wPrErr || e == vLckdErr) why = "the startup disk is locked";
            else if (e != noErr) why = "the file MoofTCP Prefs could not be written";
        }
        DisposePtr(out);
    }
    CloseResFile(ref);                                      /* releases h */
    UseResFile(saved);
    FlushVol(NULL, wd);
    return why;
}

/* Close with unsaved edits: ask, validate, write. */
static void close_panel(const CdevArgs *a, CdevState *st) {
    CfeFields f;
    CfeError e;
    Str255 ps;
    const char *why;
    int rc;
    short base = a->num_items;
    read_fields(a->dlg, base, &f);
    if (fields_equal(&f, &st->loaded)) return;
    ps[0] = 0;
    pcat(ps, "Save the changes to the MoofTCP settings? They take effect after you restart.");
    if (alert(AL_ASK, ps) != 1) return;       /* item 1 Save, item 2 Don't Save */
    rc = cfe_validate(&f, &e);
    if (cfe_is_fatal(rc)) {
        problem_text(&f, rc, &e, ps);
        pcat(ps, " The settings were not saved.");
        (void)alert(AL_STOP, ps);
        return;
    }
    if (rc == CFE_E_GW_OFFNET) {
        ps[0] = 0;
        pcat(ps, "The router is not on the same subnet as the IP address. Save anyway?");
        if (alert(AL_ASK, ps) != 1) return;
    }
    why = write_prefs(&f);
    if (why) {
        ps[0] = 0;
        pcat(ps, "The settings were not saved: ");
        pcat(ps, why);
        pcat(ps, ". The previous settings are unchanged.");
        (void)alert(AL_STOP, ps);
    }
}

/* --- TextEdit on the current field --- */

static void edit_cmd(DialogPtr d, short message) {
    DialogPeek dp = (DialogPeek)d;
    TEHandle te = dp->textH;
    long off;
    if (!te || dp->editField < 0) return;
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
        if (LM_TESCRPH) {
            long n = GetScrap(LM_TESCRPH, 'TEXT', &off);
            if (n >= 0 && n < 256) { LM_TESCRPLEN = (int16_t)n; TEPaste(te); }
            else SysBeep(5);
        }
        break;
    case clearDev: TEDelete(te); break;
    default: SysBeep(5); break;                             /* undoDev: TextEdit has no undo */
    }
}

static long dispatch(const CdevArgs *a) {
    Handle sh;
    CdevState *st;
    short base = a->num_items;
    long r = a->storage;

    if (a->message == macDev) return 1;
    if (a->message == initDev) {
        sh = NewHandle(sizeof(CdevState));
        if (!sh) return cdevMemErr;
        HLock(sh);
        st = (CdevState *)*sh;
        zero(st, sizeof *st);
        st->shown_code = -1;
        load_prefs(&st->loaded);
        show_fields(a->dlg, base, &st->loaded);
        show_status(a->dlg, base);
        update_check(a->dlg, base, st);
        SelectDialogItemText(a->dlg, (short)(base + IT_IP), 0, 32767);
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
            show_fields(a->dlg, base, &st->loaded);
            SelectDialogItemText(a->dlg, (short)(base + IT_IP), 0, 32767);
            st->check_pending = 1;
        }
        break;
    case keyEvtDev:
        if (!(a->ev->modifiers & cmdKey)) {
            char c = (char)(a->ev->message & charCodeMask);
            if (c == '\r' || c == 3) a->ev->what = nullEvent;   /* Return/Enter: not field text */
            st->check_pending = 1;
        }
        break;
    case nulDev:
        if (((DialogPeek)a->dlg)->textH) TEIdle(((DialogPeek)a->dlg)->textH);
        if (st->check_pending) {
            st->check_pending = 0;
            update_check(a->dlg, base, st);
        }
        break;
    case undoDev:
    case cutDev:
    case copyDev:
    case pasteDev:
    case clearDev:
        edit_cmd(a->dlg, a->message);
        st->check_pending = 1;
        break;
    case closeDev:
        close_panel(a, st);
        HUnlock(sh);
        DisposeHandle(sh);
        return r;
    default:                                                /* updateDev, activDev, deactivDev */
        break;
    }
    HUnlock(sh);
    return r;
}

/* Every message draws into the Control Panel's dialog, but the current port at entry is whatever
 * the host left (the Finder's window during nulDev): set it, and put the caller's back. */
long cdev_main(const CdevArgs *a);
long cdev_main(const CdevArgs *a) {
    GrafPtr saved;
    long r;
    if (a->message == macDev || !a->dlg) return dispatch(a);
    GetPort(&saved);
    SetPort((GrafPtr)a->dlg);
    r = dispatch(a);
    SetPort(saved);
    return r;
}
