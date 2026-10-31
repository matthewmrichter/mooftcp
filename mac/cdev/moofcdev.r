/* moofcdev.r - the control panel part of the file "MoofTCP" (Plan 3 Task 8), included by
 * mac/moof/moof.r. Clean-room: the IDs and the resource set follow the Control Panel protocol
 * (Inside Macintosh V, "The Control Panel"): every resource of the panel is -4064 (owned IDs of a
 * cdev run -4064 .. -4033; the alerts are -4048/-4047, fixed IDs: CPanelID is the desk accessory's
 * own base, not ours, see cdev_main.c).
 *
 *   cdev -4064   the code (mac/cdev: cdev_start.s + cdev_main.c, position independent), locked
 *   mach -4064   0000 FFFF: the Control Panel asks the cdev (macDev), which says yes
 *   nrct -4064   the panel's area in the Control Panel window: one rectangle
 *   DITL -4064   the items, appended to the Control Panel's dialog, in window coordinates
 *   ALRT/DITL -4048   stop alert, OK          ALRT/DITL -4047   question, Save / Don't Save
 *   BNDL/FREF -4064, 'ztcp' 0 (signature), ICN#/ics#/icl4/icl8/ics4/ics8 -4064 (icons.r)
 *   vers 1, vers 2
 */
#include "Retro68.r"
#include "Types.r"
#include "Dialogs.r"

type 'cdev' {
    RETRO68_CODE_TYPE
};

resource 'cdev' (-4064, locked) {
    dontBreakAtEntry, $$read("../cdev/MoofCdev.flt");
};

data 'mach' (-4064) {
    $"0000 FFFF"
};

/* count, then {top, left, bottom, right}: the area right of the icon list, -1..255 x 87..322 */
data 'nrct' (-4064) {
    $"0001 FFFF 0057 00FF 0142"
};

/* Item order is cdev_main.c's IT_* enum (Save is last: added in the Task 8 fix round). Rows:
 * label at x 95, field at x 186, 24 px apart; Revert, Save and the version along the bottom. */
resource 'DITL' (-4064) {
    {
        {224, 95, 244, 160}, Button { enabled, "Revert" };
        {9, 186, 25, 314}, EditText { enabled, "" };
        {33, 186, 49, 314}, EditText { enabled, "" };
        {57, 186, 73, 314}, EditText { enabled, "" };
        {81, 186, 97, 314}, EditText { enabled, "" };
        {105, 186, 121, 314}, EditText { enabled, "" };
        {9, 95, 25, 180}, StaticText { disabled, "IP address" };
        {33, 95, 49, 180}, StaticText { disabled, "Subnet mask" };
        {57, 95, 73, 180}, StaticText { disabled, "Router" };
        {81, 95, 97, 180}, StaticText { disabled, "DNS server" };
        {105, 95, 121, 180}, StaticText { disabled, "Domain" };
        {132, 95, 148, 316}, StaticText { disabled, "" };
        {150, 95, 182, 316}, StaticText { disabled, "" };
        {186, 95, 218, 316}, StaticText { disabled, "Changes take effect after you restart." };
        {228, 250, 244, 320}, StaticText { disabled, "v1.0rc1" };
        {224, 170, 244, 235}, Button { enabled, "Save" };
    }
};

resource 'ALRT' (-4048) {
    {50, 40, 196, 472},
    -4048,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    alertPositionMainScreen
};

resource 'DITL' (-4048) {
    {
        {114, 352, 134, 417}, Button { enabled, "OK" };
        {10, 70, 106, 420}, StaticText { disabled, "^0" };
    }
};

resource 'ALRT' (-4047) {
    {50, 40, 196, 472},
    -4047,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    alertPositionMainScreen
};

resource 'DITL' (-4047) {
    {
        {114, 337, 134, 417}, Button { enabled, "Save" };
        {114, 237, 134, 327}, Button { enabled, "Don't Save" };
        {10, 70, 106, 420}, StaticText { disabled, "^0" };
    }
};

resource 'FREF' (-4064) {
    'cdev', 0, ""
};

resource 'BNDL' (-4064) {
    'ztcp', 0,
    {
        'ICN#', { 0, -4064 };
        'FREF', { 0, -4064 }
    }
};

/* The owner (signature) resource of creator 'ztcp': a version string, as is customary. */
data 'ztcp' (0) {
    $"0E" "MoofTCP 1.0rc1"
};

#include "icons.r"

resource 'vers' (1) {
    0x01, 0x00, beta, 0x01, verUS,
    "1.0rc1",
    "MoofTCP 1.0rc1 (release candidate)"
};

resource 'vers' (2) {
    0x01, 0x00, beta, 0x01, verUS,
    "1.0rc1",
    "MoofTCP 1.0rc1"
};
