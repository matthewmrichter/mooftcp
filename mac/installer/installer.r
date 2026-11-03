/* installer.r - resources of the application "MoofTCP Installer" (Plan 3 Task 10; clean-room).
 * The embedded MoofTCP file ('MTCP'/'MTCD'/'MTCI' 128) is a separate resource file, embed.rsrc.bin,
 * generated at build time (scripts/mkembed.py, mac/installer/CMakeLists.txt) from
 * build-mac/moof/MoofTCP.bin and merged by add_application. */
#include "Types.r"
#include "Dialogs.r"
#include "Processes.r"

/* The window. Mac coordinates {top, left, bottom, right}; centred on a 512 x 342 screen. */
resource 'DLOG' (128, "MoofTCP Installer") {
    {50, 36, 290, 476},
    noGrowDocProc,
    invisible,
    goAway,
    0,
    128,
    "MoofTCP Installer",
    noAutoCenter
};

resource 'DITL' (128) {
    {
        {204, 20, 224, 110}, Button { enabled, "Install" };
        {204, 130, 224, 220}, Button { enabled, "Remove" };
        {204, 330, 224, 420}, Button { enabled, "Quit" };
        {92, 20, 196, 420}, StaticText { disabled, "" };
        {10, 20, 86, 420}, StaticText { disabled,
            "Install puts MoofTCP 1.0rc2 into the System Folder. If MacTCP is there, its files are moved "
            "into the folder \"MacTCP (Disabled)\" unchanged. Remove takes MoofTCP out and puts MacTCP "
            "back. MoofTCP needs an Ethernet driver (.ENET)." };
    }
};

/* "Restart now?" item 1 Restart (default), item 2 Not now. It covers the intro text, not the result. */
resource 'ALRT' (129) {
    {54, 56, 138, 456},
    129,
    {
        OK, visible, silent,
        OK, visible, silent,
        OK, visible, silent,
        OK, visible, silent
    },
    noAutoCenter
};

resource 'DITL' (129) {
    {
        {56, 300, 76, 380}, Button { enabled, "Restart" };
        {56, 200, 76, 280}, Button { enabled, "Not now" };
        {8, 60, 48, 390}, StaticText { disabled, "Restart now? The change takes effect when the Macintosh restarts." };
    }
};

/* A note (^0): About, and "choose Restart from the Special menu". */
resource 'ALRT' (130) {
    {80, 80, 200, 432},
    130,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    noAutoCenter
};

resource 'DITL' (130) {
    {
        {88, 262, 108, 332}, Button { enabled, "OK" };
        {10, 70, 80, 340}, StaticText { disabled, "^0" };
    }
};

resource 'vers' (1) {
    0x01, 0x00, beta, 0x02, verUS,
    "1.0rc2",
    "MoofTCP Installer 1.0rc2 (release candidate)"
};

resource 'vers' (2) {
    0x01, 0x00, beta, 0x02, verUS,
    "1.0rc2",
    "MoofTCP 1.0rc2"
};

/* The owner (signature) resource of creator 'mtin'. */
data 'mtin' (0) {
    $"18" "MoofTCP Installer 1.0rc2"
};

resource 'BNDL' (128) {
    'mtin', 0,
    {
        'ICN#', { 0, 128 };
        'FREF', { 0, 128 }
    }
};

resource 'FREF' (128) { 'APPL', 0, "" };

/* Cable Tail A (Moof icon concepts, round 2): a dogcow whose tail ends in an RJ45 plug; the mask is the outline filled in. */
data 'ICN#' (128) {
    $"00000000 FF800000 AA800000 AA800000"
    $"FF800000 80800000 80800200 7F000300"
    $"1C001F80 1C002040 08005C3C 08005C8E"
    $"08005C0E 08005C06 08005C78 05FF5C80"
    $"021EC900 041E1100 040C0100 04000100"
    $"0400E100 0401F100 0400E100 02000300"
    $"01FFFC00 01983300 01983300 01983300"
    $"01983300 00000000 00155554 00088888"
    /* mask */
    $"00000000 FF800000 FF800000 FF800000"
    $"FF800000 FF800000 FF800200 7F000300"
    $"1C001F80 1C003FC0 08007FFC 08007FFE"
    $"08007FFE 08007FFE 08007FF8 05FF7F80"
    $"03FFFF00 07FFFF00 07FFFF00 07FFFF00"
    $"07FFFF00 07FFFF00 07FFFF00 03FFFF00"
    $"01FFFC00 01983300 01983300 01983300"
    $"01983300 00000000 00155554 00088888"
};

/* MultiFinder partition (System 6 without MultiFinder gives the application all memory). The
 * bundled MoofTCP file is about 120 KB and is loaded once for Install: 384K preferred, 256K minimum. */
resource 'SIZE' (-1) {
    reserved,
    ignoreSuspendResumeEvents,
    reserved,
    cannotBackground,
    needsActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    384 * 1024,
    256 * 1024
};
