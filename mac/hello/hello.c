#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Events.h>

QDGlobals qd;

int main(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    /* ALRT/DITL 128 (hello.r) show "MoofTCP pipeline OK". */
    Alert(128, 0);
    return 0;
}
