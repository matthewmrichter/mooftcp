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

/* Item order is cdev_main.c's IT_* enum (Save added in the Task 8 fix round, the diagnostic-log
 * checkbox in Task 11b, the DHCP checkbox in Plan 4, all appended so earlier item numbers stay).
 * Rows: the DHCP box on top, then label at x 95, field at x 186, 22 px apart; Revert, Save and the
 * version along the bottom. */
resource 'DITL' (-4064) {
    {
        {224, 95, 244, 160}, Button { enabled, "Revert" };
        {25, 186, 41, 314}, EditText { enabled, "" };
        {47, 186, 63, 314}, EditText { enabled, "" };
        {69, 186, 85, 314}, EditText { enabled, "" };
        {91, 186, 107, 314}, EditText { enabled, "" };
        {113, 186, 129, 314}, EditText { enabled, "" };
        {25, 95, 41, 180}, StaticText { disabled, "IP address" };
        {47, 95, 63, 180}, StaticText { disabled, "Subnet mask" };
        {69, 95, 85, 180}, StaticText { disabled, "Router" };
        {91, 95, 107, 180}, StaticText { disabled, "DNS server" };
        {113, 95, 129, 180}, StaticText { disabled, "Domain" };
        {152, 95, 168, 316}, StaticText { disabled, "" };
        {168, 95, 200, 316}, StaticText { disabled, "" };
        {202, 95, 218, 320}, StaticText { disabled, "Changes take effect after you restart." };
        {228, 250, 244, 320}, StaticText { disabled, "v1.0rc2" };
        {224, 170, 244, 235}, Button { enabled, "Save" };
        {133, 93, 151, 316}, CheckBox { enabled, "Keep a diagnostic log" };
        {2, 93, 20, 316}, CheckBox { enabled, "Get the address by DHCP" };
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
    $"0E" "MoofTCP 1.0rc2"
};

#include "icons.r"

resource 'vers' (1) {
    0x01, 0x00, beta, 0x02, verUS,
    "1.0rc2",
    "MoofTCP 1.0rc2 (release candidate)"
};

resource 'vers' (2) {
    0x01, 0x00, beta, 0x02, verUS,
    "1.0rc2",
    "MoofTCP 1.0rc2"
};
