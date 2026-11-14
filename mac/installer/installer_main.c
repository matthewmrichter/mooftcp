/* installer_main.c - the application "MoofTCP Installer" (Plan 3 Task 10, spec 6.3): the Toolbox
 * shell around inst_exec.c (logic, host-tested) and inst_exec_mac.c (the file system table).
 *
 * One window, "MoofTCP Installer" (DLOG 128): Install, Remove, Quit, a message area and the checkbox
 * "Lite version (no DHCP)" with its note. At launch the message area says what is installed now
 * (inst_do_query, read only). Install and Remove take a snapshot of the System Folder, plan, and run;
 * the message area then says what happened (inst_message). After a run that changed the disk, an alert asks "Restart now?"
 * (Restart / Not now); Restart uses the Shutdown Manager (ShutDwnStart) when its trap is there,
 * else it tells the user to choose Restart from the Special menu.
 *
 * Both MoofTCP files are inside this application (docs/dev/DHCP-DESIGN.md section 10): resource fork
 * as 'MTCP', data fork as 'MTCD' (absent: the release files have none), type/creator/Finder flags as
 * 'MTCI', id 128 = MoofTCP (full, DHCP client), id 129 = MoofTCP Lite (scripts/mkembed.py). Install
 * loads only the one the checkbox picks (and checks that its 'vers' 1 names that variant), writes it
 * and releases it. The checkbox starts ticked only when MoofTCP Lite is what is installed now, so a
 * plain Install never swaps the variant by accident; otherwise it starts off (full is the default).
 *
 * Unattended (MoofAgent's runs; the run file "Agent Run" starts with "!!", runid_unattended): no window, no alert,
 * no restart question. The installer reads and deletes "MoofTCPInstall.cfg" beside itself (one line,
 * inst_auto.h: install | remove | reinstall, with lite and prefs), does what it says with the same
 * inst_do_install / inst_do_remove the buttons use, and writes "MoofTCPInstall-<id>.out": line 1 names the
 * installer and the variant it was asked for, then one line per step, last "RESULT <message>" (the
 * inst_message text). scripts/moofctl-plugins/mooftcp.py reads it. A run without a valid cfg changes nothing.
 * Without a "!" run file the installer behaves as before: runid_name would open a dialog (DLOG 200) that this
 * application has no resource for, so it is called only when the run file starts with '!'.
 *
 * Debug builds (MT_INST_DEBUG=1, target MoofInstallerDbg, never shipped) read "[I|R]N" (N decimal)
 * from the data fork of the file "MoofTCP Installer Debug" at the root of the startup disk and stop
 * every run (I: only Install, R: only Remove) after N file system changes (the interruption test,
 * scripts/installer-test.sh). The file is not beside the app, so the test folder holds one icon
 * and the Finder puts it where the harness clicks. */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Resources.h>
#include <Memory.h>
#include <Files.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <SegLoad.h>
#include <string.h>
#include "inst_exec.h"
#include "inst_exec_mac.h"
#include "inst_auto.h"
#include "runid.h"

#pragma GCC diagnostic ignored "-Warray-bounds"
#define LM_BOOTDRIVE (*(volatile short *)0x210)

#ifndef MT_INST_DEBUG
#define MT_INST_DEBUG 0
#endif

/* VERS_STR goes into the result messages ("MoofTCP Lite <VERS_STR> is installed."; the variant's name
 * comes from the executor). */
#define VERS_STR "1.1rc1"

enum { DLG_MAIN = 128, AL_RESTART = 129, AL_NOTE = 130 };
enum { IT_INSTALL = 1, IT_REMOVE, IT_QUIT, IT_MSG, IT_INTRO, IT_LITE, IT_NOTE };
enum { RID_FULL = 128, RID_LITE = 129 };
enum { MN_APPLE = 128, MN_FILE, MN_EDIT };

QDGlobals qd;
static DialogPtr g_dlg;
static int g_quit;
static long g_stop;                         /* debug: stop after this many changes (0 = off) */
static char g_stop_only;                    /* debug: 'I' = Install only, 'R' = Remove only, 0 = both */
static int g_lite;                          /* the checkbox: 1 = install MoofTCP Lite */

static void c2p(Str255 p, const char *c) {
    size_t n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void set_item_text(short it, const char *c) {
    short type;
    Handle h;
    Rect r;
    Str255 s;
    GrafPtr old;
    c2p(s, c);
    GetPort(&old);
    SetPort((GrafPtr)g_dlg);
    GetDialogItem(g_dlg, it, &type, &h, &r);
    SetDialogItemText(h, s);
    EraseRect(&r);
    InvalRect(&r);
    SetPort(old);
}

static void set_lite(int v) {
    short type;
    Handle h;
    Rect r;
    g_lite = v ? 1 : 0;
    GetDialogItem(g_dlg, IT_LITE, &type, &h, &r);
    SetControlValue((ControlHandle)h, (short)g_lite);
}

/* What is installed now (read only), into the message area; returns the installed variant (-1 none). */
static int show_installed(void) {
    InstFs fs;
    InstMacCtx mc;
    InstResult r;
    char buf[256];
    if (inst_mac_fs(&fs, &mc)) return -1;
    (void)inst_do_query(&fs, &r);
    inst_query_message(&r, buf, sizeof buf);
    set_item_text(IT_MSG, buf);
    return r.status == INST_X_OK && r.had == 1 ? r.old_variant : -1;
}

static void note(const char *c) {
    Str255 s;
    c2p(s, c);
    ParamText(s, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    (void)NoteAlert(AL_NOTE, NULL);
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* The trap is implemented if its address differs from _Unimplemented's (both Toolbox traps). */
static int shutdown_available(void) {
    return GetToolTrapAddress(_ShutDown) != GetToolTrapAddress(_Unimplemented);
}

static void ask_restart(void) {
    if (CautionAlert(AL_RESTART, NULL) != 1) return;           /* item 1 Restart, item 2 Not now */
    if (shutdown_available()) {
        ShutDwnStart();                                         /* does not return */
        return;
    }
    note("Choose Restart from the Special menu in the Finder to use the change.");
}

/* Remove on *fs: the message into buf (at most 255 characters), the result into *r; returns 1 when the
 * disk changed and a restart is due (inst_message). Shared by the window and the unattended run. */
static int remove_now(const InstFs *fs, char *buf, InstResult *r) {
    (void)inst_do_remove(fs, g_stop_only == 'I' ? 0 : g_stop, r);
    return inst_message(r, VERS_STR, buf, 256);
}

/* Install on *fs with the embedded MoofTCP of the variant `lite` (RID_FULL / RID_LITE): as remove_now. */
static int install_now(const InstFs *fs, int lite, char *buf, InstResult *r) {
    InstPayload pl;
    Handle hr = NULL, hd = NULL, hi = NULL;
    InstVers v;
    short id = lite ? RID_LITE : RID_FULL;
    int restart = 0;

    memset(&pl, 0, sizeof pl);
    memset(r, 0, sizeof *r);
    hr = GetResource('MTCP', id);
    hi = GetResource('MTCI', id);
    SetResLoad(false);
    hd = GetResource('MTCD', id);                           /* optional: look up without loading */
    SetResLoad(true);
    if (hd) LoadResource(hd);
    if (!hr || !hi || GetHandleSize(hi) < 10 || (hd && !*hd)) {
        strcpy(buf, "There is not enough memory to install MoofTCP (or this copy of the installer is damaged). Nothing was changed.");
    } else {
        HLock(hr); HLock(hi);
        if (hd) HLock(hd);
        if (inst_fork_vinfo((const uint8_t *)*hr, GetHandleSize(hr), &v) == 0 || v.variant != lite) {
            /* the file under this id must be the variant the box names (scripts/mkembed.py checks it at build time too) */
            strcpy(buf, "This copy of the installer is damaged: the MoofTCP file for this choice is not the right one. Nothing was changed.");
        } else {
            pl.rsrc = (const uint8_t *)*hr; pl.rlen = GetHandleSize(hr);
            pl.data = hd ? (const uint8_t *)*hd : NULL; pl.dlen = hd ? GetHandleSize(hd) : 0;
            pl.type = be32((const unsigned char *)*hi);
            pl.creator = be32((const unsigned char *)*hi + 4);
            pl.flags = (uint16_t)((((const unsigned char *)*hi)[8] << 8) | ((const unsigned char *)*hi)[9]);
            (void)inst_do_install(fs, &pl, g_stop_only == 'R' ? 0 : g_stop, r);
            restart = inst_message(r, VERS_STR, buf, 256);
        }
    }
    if (hr) { HUnlock(hr); ReleaseResource(hr); }
    if (hd) { HUnlock(hd); ReleaseResource(hd); }
    if (hi) { HUnlock(hi); ReleaseResource(hi); }
    return restart;
}

/* Is the embedded MoofTCP of this variant there and the right one? Looks at it and lets it go; changes nothing.
 * 0 = ok, else 1 with the install_now text in buf. The unattended run asks this before it touches the disk. */
static int payload_bad(int lite, char *buf) {
    Handle hr = GetResource('MTCP', lite ? RID_LITE : RID_FULL);
    Handle hi = GetResource('MTCI', lite ? RID_LITE : RID_FULL);
    InstVers v;
    int bad = 0;
    if (!hr || !hi || GetHandleSize(hi) < 10) {
        strcpy(buf, "There is not enough memory to install MoofTCP (or this copy of the installer is damaged). Nothing was changed.");
        bad = 1;
    } else {
        HLock(hr);
        if (inst_fork_vinfo((const uint8_t *)*hr, GetHandleSize(hr), &v) == 0 || v.variant != lite) {
            strcpy(buf, "This copy of the installer is damaged: the MoofTCP file for this choice is not the right one. Nothing was changed.");
            bad = 1;
        }
        HUnlock(hr);
    }
    if (hr) ReleaseResource(hr);
    if (hi) ReleaseResource(hi);
    return bad;
}

/* A failed step of an unattended run that says "Nothing was changed." although an earlier step of the same run did
 * change the disk (the prefs copy, a Remove): say so instead (in place, when it fits). */
static void say_earlier_changes(char *buf) {
    static const char old[] = "Nothing was changed.";
    static const char now[] = "Earlier steps of this run did change the disk.";
    char *p = strstr(buf, old);
    if (p && p[sizeof old - 1] == 0 && (p - buf) + (int)sizeof now <= 255) strcpy(p, now);
}

/* The text for a failed inst_mac_fs (rc). */
static const char *no_fs_text(int rc) {
    return rc == INST_MAC_NOT_HFS ? "The startup disk is not an HFS disk. Nothing was changed."
           : rc == INST_MAC_NO_SYSFOLDER ? "The startup disk has no System Folder. Nothing was changed."
           : "The startup disk could not be read. Nothing was changed.";
}

static void run(int remove) {
    InstFs fs;
    InstMacCtx mc;
    InstResult r;
    char buf[256];
    int rc, restart = 0;

    set_item_text(IT_MSG, remove ? "Removing..." : "Installing...");
    DrawDialog(g_dlg);                      /* now: the run is synchronous, no update event comes before it ends */
    SetCursor(*GetCursor(watchCursor));
    rc = inst_mac_fs(&fs, &mc);
    if (rc) strcpy(buf, no_fs_text(rc));
    else if (remove) restart = remove_now(&fs, buf, &r);
    else restart = install_now(&fs, g_lite, buf, &r);
    InitCursor();
    set_item_text(IT_MSG, buf);
    if (restart) {
        DrawDialog(g_dlg);                                      /* the result stays visible behind the alert */
        ask_restart();
    }
}

/* ---- unattended run ---- */

#define CFG_NAME "\pMoofTCPInstall.cfg"
#define PREFS_NAME "MoofTCP Prefs"

static short g_outref = -1;

static void wr(const char *s) {
    long n = (long)strlen(s);
    if (g_outref >= 0) (void)FSWrite(g_outref, &n, (Ptr)s);
}

static void wrn(long v) {
    char b[16];
    int i = 15;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (neg) b[--i] = '-';
    wr(b + i);
}

/* 1 if the run file ("Agent Run", else "MoofPort Run") exists and starts with '!' (see the top comment). */
static int has_run_file(void) {
    static const char *const names[2] = { RUNID_FILE, RUNID_FILE_OLD };
    int k;
    for (k = 0; k < 2; k++) {
        short r;
        long len = 1;
        char c = 0;
        if (FSOpen((ConstStr255Param)names[k], 0, &r) != noErr) continue;
        if (FSRead(r, &len, &c) != noErr && len <= 0) c = 0;
        FSClose(r);
        return c == '!';
    }
    return 0;
}

/* Read and delete MoofTCPInstall.cfg beside the installer. 0 = valid (*a filled), -1 = missing or bad. */
static int read_cfg(InstAuto *a) {
    short r;
    long len = 64;
    char buf[64];
    int rc;
    a->action = IA_NONE; a->lite = a->prefs = 0;
    if (FSOpen((ConstStr255Param)CFG_NAME, 0, &r) != noErr) return -1;
    if (FSRead(r, &len, (Ptr)buf) != noErr && len <= 0) len = 0;
    FSClose(r);
    (void)FSDelete((ConstStr255Param)CFG_NAME, 0);
    if (len >= (long)sizeof buf) return -1;          /* the buffer was filled: a longer file is not a cfg */
    rc = inst_auto_parse(buf, len, a);
    return rc;
}

/* One fork of the file "MoofTCP Prefs" beside the installer into a new block (*p, *len); 0 or an OSErr. */
static int read_own_fork(int rsrc, Ptr *p, long *len) {
    short r;
    int rc;
    *p = NULL; *len = 0;
    rc = rsrc ? OpenRF((ConstStr255Param)"\pMoofTCP Prefs", 0, &r) : FSOpen((ConstStr255Param)"\pMoofTCP Prefs", 0, &r);
    if (rc) return rc;
    rc = GetEOF(r, len);
    if (!rc && (*len < 0 || *len > 32000)) rc = -50;                /* far larger than any prefs file */
    if (!rc && *len > 0) {
        *p = NewPtr(*len);
        if (!*p) rc = -108;
        else {
            long n = *len;
            rc = FSRead(r, &n, *p);
            if (rc == eofErr && n == *len) rc = 0;
        }
    }
    FSClose(r);
    if (rc && *p) { DisposePtr(*p); *p = NULL; }
    return rc;
}

/* Copy "MoofTCP Prefs" (both forks, type and creator) from the installer's folder over the System Folder's.
 * Logs what it did; returns 0, or fills why (the RESULT text) and returns 1. */
static int copy_prefs(const InstFs *fs, char *why) {
    FInfo fi;
    InstInfo ii;
    Ptr d = NULL, r = NULL;
    long dl = 0, rl = 0;
    int rc, existed;
    if (GetFInfo((ConstStr255Param)"\pMoofTCP Prefs", 0, &fi) != noErr) {
        strcpy(why, "The file \"MoofTCP Prefs\" is not in the installer's folder. Nothing was changed.");
        return 1;
    }
    if ((rc = read_own_fork(0, &d, &dl)) != 0 || (rc = read_own_fork(1, &r, &rl)) != 0) {
        strcpy(why, "The file \"MoofTCP Prefs\" in the installer's folder could not be read. Nothing was changed.");
        wr("prefs: read error "); wrn(rc); wr("\r");
        if (d) DisposePtr(d);
        return 1;
    }
    existed = fs->info(fs->ctx, fs->sysdir, PREFS_NAME, &ii) == 0;
    rc = existed ? 0 : fs->create(fs->ctx, fs->sysdir, PREFS_NAME);
    if (!rc) rc = fs->write(fs->ctx, fs->sysdir, PREFS_NAME, 0, (const uint8_t *)d, dl);
    if (!rc) rc = fs->write(fs->ctx, fs->sysdir, PREFS_NAME, 1, (const uint8_t *)r, rl);
    if (!rc) rc = fs->setinfo(fs->ctx, fs->sysdir, PREFS_NAME, (uint32_t)fi.fdType, (uint32_t)fi.fdCreator, 0);
    if (!rc) rc = fs->flush(fs->ctx);
    if (d) DisposePtr(d);
    if (r) DisposePtr(r);
    if (rc) {
        strcpy(why, "The file \"MoofTCP Prefs\" could not be copied into the System Folder (error ");
        /* the number goes after the text */
        {
            char n[12];
            int i = 11;
            long v = rc < 0 ? -(long)rc : rc;
            n[i] = 0;
            do { n[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 1);
            if (rc < 0) n[--i] = '-';
            strcat(why, n + i);
        }
        strcat(why, "). The installer did not go on.");
        wr("prefs: copy failed, error "); wrn(rc); wr("\r");
        return 1;
    }
    wr("prefs: copied \"MoofTCP Prefs\" (data fork "); wrn(dl); wr(" bytes, resource fork "); wrn(rl);
    wr(" bytes, type/creator kept) into the System Folder, ");
    wr(existed ? "replacing the file there\r" : "no file was there before\r");
    return 0;
}

/* The status and message of a finished Remove that lets a reinstall go on to the Install. */
static int remove_cleared(const InstResult *r) {
    return r->status == INST_X_OK &&
           (r->msg == INST_REMOVED_RESTORED || r->msg == INST_REMOVED_NO_MACTCP || r->msg == INST_NOTHING_TO_REMOVE);
}

static void auto_run(const unsigned char *oname) {
    static const char *const verb[] = { "none", "install", "remove", "reinstall" };
    InstAuto a;
    InstFs fs;
    InstMacCtx mc;
    InstResult r;
    char buf[256];
    int rc;

    (void)FSDelete((ConstStringPtr)oname, 0);
    Create((ConstStringPtr)oname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)oname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    rc = read_cfg(&a);
    wr("MoofTCP Installer " VERS_STR " "); wr(a.lite ? "lite" : "full"); wr("\r");
    if (rc) {
        wr("cfg: MoofTCPInstall.cfg is missing or not valid (install|remove|reinstall [lite] [prefs])\r");
        strcpy(buf, "Unattended run without a valid MoofTCPInstall.cfg. Nothing was changed.");
    } else {
        wr("cfg: "); wr(verb[a.action]); if (a.lite) wr(" lite"); if (a.prefs) wr(" prefs"); wr(" (read and deleted)\r");
        rc = inst_mac_fs(&fs, &mc);
        if (rc) {
            strcpy(buf, no_fs_text(rc));
        } else if ((a.action == IA_INSTALL || a.action == IA_REINSTALL) && payload_bad(a.lite, buf)) {
            wr("install: "); wr(buf); wr("\r");                /* checked first: nothing has been touched yet */
        } else if (a.prefs && copy_prefs(&fs, buf)) {
            /* buf says why */
        } else {
            int changed = a.prefs;                              /* the prefs copy succeeded: the disk is different */
            if (a.action == IA_REMOVE || a.action == IA_REINSTALL) {
                int restart = remove_now(&fs, buf, &r);
                wr("remove: "); wr(buf); wr("\r");
                if (restart || r.committed > 0) changed = 1;
                if (a.action == IA_REINSTALL && !remove_cleared(&r)) a.action = IA_REMOVE;   /* stop: buf is the result */
            }
            if (a.action == IA_INSTALL || a.action == IA_REINSTALL) {
                if (!install_now(&fs, a.lite, buf, &r) && changed) say_earlier_changes(buf);
                wr("install: "); wr(buf); wr("\r");
            } else if (a.action == IA_REMOVE && changed && !remove_cleared(&r)) {
                say_earlier_changes(buf);
            }
        }
    }
    wr("RESULT "); wr(buf); wr("\r");
    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(NULL, 0);
}

#if MT_INST_DEBUG
/* ":MoofTCP Installer Debug" at the root (directory 2) of the startup disk (BootDrive's volume). */
static long read_debug_count(void) {
    HParamBlockRec pb;
    ParamBlockRec io;
    static unsigned char nm[] = "\pMoofTCP Installer Debug";
    char b[16];
    long n = 0, got;
    int i;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = nm;
    pb.ioParam.ioVRefNum = LM_BOOTDRIVE;
    pb.fileParam.ioDirID = 2;                               /* the root directory (fsRtDirID) */
    pb.ioParam.ioPermssn = fsRdPerm;
    if (PBHOpenSync(&pb) != noErr) return 0;
    memset(&io, 0, sizeof io);
    io.ioParam.ioRefNum = pb.ioParam.ioRefNum;
    io.ioParam.ioBuffer = b;
    io.ioParam.ioReqCount = sizeof b - 1;
    io.ioParam.ioPosMode = fsFromStart;
    (void)PBReadSync(&io);
    got = io.ioParam.ioActCount;
    (void)PBCloseSync(&io);
    i = 0;
    if (got > 0 && (b[0] == 'I' || b[0] == 'R')) { g_stop_only = b[0]; i = 1; }
    for (; i < (int)got && b[i] >= '0' && b[i] <= '9'; i++) n = n * 10 + (b[i] - '0');
    return n;
}
#endif

static void do_menu(long mr) {
    short menu = (short)(mr >> 16), item = (short)(mr & 0xFFFF);
    Str255 name;
    if (menu == MN_APPLE) {
        if (item == 1) {
            note("MoofTCP Installer " VERS_STR ". Installs or removes MoofTCP, a TCP/IP stack for System 6 that "
                 "takes the place of MacTCP. It holds MoofTCP and MoofTCP Lite (no DHCP client); the checkbox picks one.");
        } else {
            GetMenuItemText(GetMenuHandle(MN_APPLE), item, name);
            (void)OpenDeskAcc(name);
        }
    } else if (menu == MN_FILE) {
        if (item == 1) g_quit = 1;
    } else if (menu == MN_EDIT) {
        (void)SystemEdit((short)(item - 1));
    }
    HiliteMenu(0);
}

static void setup_menus(void) {
    MenuHandle m;
    m = NewMenu(MN_APPLE, (ConstStringPtr)"\p\x14");
    AppendMenu(m, (ConstStringPtr)"\pAbout MoofTCP Installer...;(-");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(MN_FILE, (ConstStringPtr)"\pFile");
    AppendMenu(m, (ConstStringPtr)"\pQuit/Q");
    InsertMenu(m, 0);
    m = NewMenu(MN_EDIT, (ConstStringPtr)"\pEdit");
    AppendMenu(m, (ConstStringPtr)"\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");   /* for desk accessories */
    InsertMenu(m, 0);
    DrawMenuBar();
}

int main(void) {
    EventRecord ev;
    WindowPtr w;
    DialogPtr d;
    short item, part;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
#if MT_INST_DEBUG
    g_stop = read_debug_count();
#endif
    if (has_run_file()) {
        unsigned char oname[40];
        runid_name("MoofTCPInstall", ".out", oname);        /* no dialog: the run file starts with '!' */
        if (runid_unattended()) { auto_run(oname); return 0; }
    }
    setup_menus();
    g_dlg = GetNewDialog(DLG_MAIN, NULL, (WindowPtr)-1);
    if (!g_dlg) { SysBeep(5); return 0; }
#if MT_INST_DEBUG
    {
        char t[120] = "DEBUG BUILD (not for release): every run stops after ";
        char num[12];
        long v = g_stop;
        int n = 0, k = (int)strlen(t);
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
        while (n > 0) t[k++] = num[--n];
        t[k] = 0;
        strcat(t, !g_stop ? " changes (0 = never)." : g_stop_only == 'I' ? " changes (Install only)."
                  : g_stop_only == 'R' ? " changes (Remove only)." : " changes.");
        set_item_text(IT_INTRO, t);
    }
#endif
    set_lite(show_installed() == INST_VAR_LITE);    /* ticked only when Lite is what is installed now */
    ShowWindow(g_dlg);
    while (!g_quit) {
        SystemTask();
        if (!GetNextEvent(everyEvent, &ev)) {
            if (IsDialogEvent(&ev)) (void)DialogSelect(&ev, &d, &item);   /* caret, idle */
            continue;
        }
        if (ev.what == keyDown || ev.what == autoKey) {
            if (ev.modifiers & cmdKey) { do_menu(MenuKey((short)(ev.message & charCodeMask))); continue; }
        } else if (ev.what == mouseDown) {
            part = FindWindow(ev.where, &w);
            if (part == inMenuBar) { do_menu(MenuSelect(ev.where)); continue; }
            if (part == inSysWindow) { SystemClick(&ev, w); continue; }
            if (part == inDrag) { DragWindow(w, ev.where, &qd.screenBits.bounds); continue; }
            if (part == inGoAway) { if (TrackGoAway(w, ev.where) && w == g_dlg) g_quit = 1; continue; }
            if (part == inContent && w != FrontWindow()) { SelectWindow(w); continue; }
        }
        if (IsDialogEvent(&ev) && DialogSelect(&ev, &d, &item) && d == g_dlg) {
            if (item == IT_INSTALL) run(0);
            else if (item == IT_REMOVE) run(1);
            else if (item == IT_QUIT) g_quit = 1;
            else if (item == IT_LITE) set_lite(!g_lite);
        }
    }
    return 0;
}
