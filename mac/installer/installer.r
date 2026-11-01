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
            "Install puts MoofTCP 1.0rc1 into the System Folder. If MacTCP is there, its files are moved "
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
    0x01, 0x00, beta, 0x01, verUS,
    "1.0rc1",
    "MoofTCP Installer 1.0rc1 (release candidate)"
};

resource 'vers' (2) {
    0x01, 0x00, beta, 0x01, verUS,
    "1.0rc1",
    "MoofTCP 1.0rc1"
};

/* The owner (signature) resource of creator 'mtin'. */
data 'mtin' (0) {
    $"18" "MoofTCP Installer 1.0rc1"
};

resource 'BNDL' (128) {
    'mtin', 0,
    {
        'ICN#', { 0, 128 };
        'FREF', { 0, 128 }
    }
};

resource 'FREF' (128) { 'APPL', 0, "" };

/* An open box with an arrow going in (drawn for MoofTCP). */
data 'ICN#' (128) {
    $"00000000 00000000 0003C000 0003C000"
    $"0003C000 0003C000 0003C000 0003C000"
    $"0003C000 0003C000 0003C000 0003C000"
    $"0003C000 007FFE00 003FFC00 001FF800"
    $"0F8FF1F0 0807E010 0803C010 08000010"
    $"08000010 08000010 08FFFF10 08000010"
    $"08000010 08FFF010 08000010 08000010"
    $"08000010 0FFFFFF0 00000000 00000000"
    /* mask */
    $"00000000 00000000 0003C000 0003C000"
    $"0003C000 0003C000 0003C000 0003C000"
    $"0003C000 0003C000 0003C000 0003C000"
    $"0003C000 007FFE00 003FFC00 001FF800"
    $"0FFFFFF0 0FFFFFF0 0FFFFFF0 0FFFFFF0"
    $"0FFFFFF0 0FFFFFF0 0FFFFFF0 0FFFFFF0"
    $"0FFFFFF0 0FFFFFF0 0FFFFFF0 0FFFFFF0"
    $"0FFFFFF0 0FFFFFF0 00000000 00000000"
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
