#include "Types.r"
#include "Dialogs.r"

/* Setup: items 1 Connect, 2 Cancel, 3/5 labels, 4 host, 6 port, 7 text */
resource 'DLOG' (128, "KeyLat") {
    {60, 60, 232, 450},
    dBoxProc,
    visible,
    noGoAway,
    0,
    128,
    "",
    noAutoCenter
};

resource 'DITL' (128) {
    {
        {142, 300, 162, 370}, Button { enabled, "Connect" };
        {142, 210, 162, 280}, Button { enabled, "Cancel" };
        {66, 20, 82, 120}, StaticText { disabled, "Host address:" };
        {64, 124, 80, 280}, EditText { enabled, "" };
        {94, 20, 110, 120}, StaticText { disabled, "Port:" };
        {92, 124, 108, 200}, EditText { enabled, "" };
        {10, 20, 56, 370}, StaticText { disabled,
            "KeyLat connects to the host running scripts/keylatsrv.py, then type any keys. Cmd-Q quits." };
    }
};

resource 'ALRT' (128) {
    {70, 60, 190, 450},
    129,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    noAutoCenter
};

resource 'DITL' (129) {
    {
        {88, 310, 108, 370}, Button { enabled, "OK" };
        {10, 14, 80, 376}, StaticText { disabled, "^0" };
    }
};
