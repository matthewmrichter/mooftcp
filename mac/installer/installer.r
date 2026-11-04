/* installer.r - resources of the application "MoofTCP Installer" (Plan 3 Task 10; clean-room).
 * The embedded MoofTCP files ('MTCP'/'MTCD'/'MTCI' 128 = MoofTCP, 129 = MoofTCP Lite) are a separate
 * resource file, embed.rsrc.bin, generated at build time (scripts/mkembed.py, mac/installer/CMakeLists.txt)
 * from build-mac/moof/MoofTCP.bin and build-mac-lite/moof/MoofTCP.bin and merged by add_application.
 * One installer for both variants (docs/dev/DHCP-DESIGN.md section 10): the checkbox picks the file
 * Install writes. */
#include "Types.r"
#include "Dialogs.r"
#include "Processes.r"

/* The window. Mac coordinates {top, left, bottom, right}; on a 512 x 342 screen. The top stays at 50
 * (scripts/installer-test.sh tells the window from a crash by its title bar there). Static texts hold
 * at most 255 characters; each box fits its text at Chicago 12 (16 px a line): intro 4 lines, note 4
 * lines, message 5 lines (inst_message: at most 217 characters, test_inst_exec message_lengths). */
resource 'DLOG' (128, "MoofTCP Installer") {
    {50, 16, 338, 496},
    noGrowDocProc,
    invisible,
    goAway,
    0,
    128,
    "MoofTCP Installer",
    noAutoCenter
};

/* Item order is installer_main.c's IT_* enum (the checkbox and its note appended). */
resource 'DITL' (128) {
    {
        {260, 16, 280, 106}, Button { enabled, "Install" };
        {260, 126, 280, 216}, Button { enabled, "Remove" };
        {260, 374, 280, 464}, Button { enabled, "Quit" };
        {168, 16, 252, 464}, StaticText { disabled, "" };
        {8, 16, 72, 464}, StaticText { disabled,
            "Install puts MoofTCP 1.0rc3 into the System Folder. If MacTCP is there, its files are moved "
            "into the folder \"MacTCP (Disabled)\" unchanged. Remove takes MoofTCP out and puts MacTCP "
            "back. MoofTCP needs an Ethernet driver (.ENET)." };
        {78, 14, 96, 300}, CheckBox { enabled, "Lite version (no DHCP)" };
        {98, 32, 162, 464}, StaticText { disabled,
            "Saves about 5 KB of system memory. Choose it if you set a fixed IP address in the MoofTCP "
            "control panel. Leave it off if your network hands out addresses automatically (DHCP)." };
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

/* The short version string is the release version (scripts/relvers.py checks it, and the two embedded
 * MoofTCP files' long strings: "MoofTCP 1.0rc3 ..." in 128, "MoofTCP Lite 1.0rc3 ..." in 129). */
resource 'vers' (1) {
    0x01, 0x00, beta, 0x03, verUS,
    "1.0rc3",
    "MoofTCP Installer 1.0rc3 (release candidate)"
};

resource 'vers' (2) {
    0x01, 0x00, beta, 0x03, verUS,
    "1.0rc3",
    "MoofTCP 1.0rc3"
};

/* The owner (signature) resource of creator 'mtin'. */
data 'mtin' (0) {
    $"18" "MoofTCP Installer 1.0rc3"
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
 * application carries two MoofTCP files (about 130 KB each, not preloaded); only the chosen one is
 * loaded, for Install, and released afterwards: 384K preferred, 256K minimum, as with one file. */
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
