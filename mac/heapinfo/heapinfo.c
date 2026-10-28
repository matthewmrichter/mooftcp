/* HeapInfo: show system-zone FreeMem / MaxMem / size in an alert. */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>

QDGlobals qd;

/* Build Pascal string "\r<label><value>" in out. */
static void line(const char *label, long v, unsigned char *out)
{
    char tmp[16];
    int n = 0, i = 0;
    unsigned long u = (unsigned long)v;
    out[++i] = 13;
    while (*label) out[++i] = (unsigned char)*label++;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = '0' + (char)(u % 10); u /= 10; }
    while (n) out[++i] = (unsigned char)tmp[--n];
    out[0] = (unsigned char)i;
}

int main(void)
{
    THz saved, sys;
    long freeB, maxB, size, grow;
    unsigned char a[48], b[48], c[48];

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    saved = GetZone();
    sys = *(THz *)0x2A6;   /* SysZone low-memory global */
    SetZone(sys);
    freeB = FreeMem();
    maxB = MaxMem(&grow);
    size = (long)sys->bkLim - (long)sys;
    SetZone(saved);

    line("FreeMem: ", freeB, a);
    line("MaxMem: ", maxB, b);
    line("Zone size: ", size, c);
    ParamText("\p system zone (bytes)", a, b, c);
    Alert(128, 0);
    return 0;
}
