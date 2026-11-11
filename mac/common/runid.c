/* copied from moofagent 6ca9126; change there first */
/* runid.c - the "name this run" dialog shared by the Phase 0 tools.
 *
 * The tools write their results next to themselves, and every run used to replace the last
 * one's file. On launch each tool now asks for a run id (DLOG 200, runid.r) and writes
 * "<Tool>-<id>.out". The id is remembered in the text file "Agent Run" in the same folder and
 * offered next time ("MoofPort Run", the old name, is read when there is no "Agent Run").
 * If that file's first line starts with '!', the rest of the line is used as
 * the id without asking: that keeps unattended runs (Snow) unattended. "!!" also tells the tool
 * to skip its closing alert and quit (runid_unattended), so MoofAgent (mac/moofagent) sees it end. Cancel or an empty id
 * gives the old "<Tool>.out". ':' (the HFS path separator) and control characters become '-',
 * and the id is cut so the whole name fits HFS's 31 characters.
 */
#include <Dialogs.h>
#include <Files.h>
#include <string.h>
#include "runid.h"
#include "runid_parse.h"

enum { kDlog = 200, kOK = 1, kCancel = 2, kEdit = 4 };

static int g_unattended;

int runid_unattended(void) { return g_unattended; }

/* Read the run file's text into buf (at most 63 bytes): "Agent Run", else the old name. */
static long read_run_file(char *buf)
{
    static const char *const names[2] = { RUNID_FILE, RUNID_FILE_OLD };
    int k;
    for (k = 0; k < 2; k++) {
        short r;
        long len = 63;
        if (FSOpen((ConstStr255Param)names[k], 0, &r) != noErr) continue;
        if (FSRead(r, &len, (Ptr)buf) != noErr && len <= 0) len = 0;
        FSClose(r);
        return len;
    }
    return 0;
}

static void read_last(unsigned char *id, int *quiet)
{
    long len;
    char buf[64], s[64];
    int i, n = 0, un = 0;
    id[0] = 0;
    *quiet = 0;
    len = read_run_file(buf);
    if (runid_parse(buf, len, s, sizeof s, &un)) {
        *quiet = 1;
        g_unattended = un;
        for (i = 0; s[i]; i++) id[++n] = (unsigned char)s[i];
    } else if (len > 0 && buf[0] == '!') {
        /* "!" or "!!" with no usable id (empty, or too long): still quiet, no id, so an
           unattended run never opens a dialog or waits at its closing alert. */
        *quiet = 1;
        g_unattended = len > 1 && buf[1] == '!';
    } else {                              /* no '!': offer the first line as the default */
        for (i = 0; i < len && buf[i] != '\r' && buf[i] != '\n'; i++) id[++n] = (unsigned char)buf[i];
    }
    id[0] = (unsigned char)n;
}

static void save_last(const unsigned char *id)
{
    short r;
    long len = id[0];
    Create((ConstStr255Param)RUNID_FILE, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)RUNID_FILE, 0, &r) != noErr) return;
    SetEOF(r, 0);
    FSWrite(r, &len, (Ptr)(id + 1));
    FSClose(r);
}

static void ask(unsigned char *id)
{
    DialogPtr d = GetNewDialog(kDlog, 0, (WindowPtr)-1);
    short item = 0, type;
    Handle h;
    Rect box;
    if (!d) return;                       /* no dialog resource: keep the remembered id */
    GetDialogItem(d, kEdit, &type, &h, &box);
    SetDialogItemText(h, id);
    SelectDialogItemText(d, kEdit, 0, 32767);
    while (item != kOK && item != kCancel) ModalDialog(0, &item);
    if (item == kOK) GetDialogItemText(h, id);
    else id[0] = 0;
    DisposeDialog(d);
}

unsigned char *runid_name(const char *tool, const char *ext, unsigned char *out)
{
    unsigned char id[256];
    int quiet, i, n = 0, room;
    size_t tl = strlen(tool), el = strlen(ext);

    read_last(id, &quiet);
    if (!quiet) {
        ask(id);
        if (id[0]) save_last(id);
    }
    for (i = 1; i <= id[0]; i++)          /* trim leading and trailing spaces */
        if (id[i] != ' ') break;
    while (id[0] >= i && id[id[0]] == ' ') id[0]--;

    memcpy(out + 1, tool, tl);
    n = (int)tl;
    room = 31 - (int)tl - (int)el - 1;
    if (id[0] >= i && room > 0) {
        out[++n] = '-';
        for (; i <= id[0] && room > 0; i++, room--) {
            unsigned char c = id[i];
            out[++n] = (c == ':' || c < 0x20 || c == 0x7F) ? '-' : c;
        }
    }
    memcpy(out + 1 + n, ext, el);
    out[0] = (unsigned char)(n + el);
    return out;
}
