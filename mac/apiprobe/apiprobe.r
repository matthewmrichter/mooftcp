#include "Types.r"
#include "Dialogs.r"

resource 'ALRT' (128) {
    {60, 60, 160, 440},
    128,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    alertPositionMainScreen
};

resource 'DITL' (128) {
    {
        {70, 300, 90, 370}, Button { enabled, "OK" };
        {10, 10, 60, 370}, StaticText { disabled, "ApiProbe finished. Results are in ApiProbe.out" };
    }
};

/* Launch dialog: 1 OK, 2 Quit, 3/5 labels, 4 target host IP, 6 groups, 7 help text, 8 message line (key help / validation message) */
resource 'DLOG' (129, "ApiProbe") {
    {50, 60, 236, 450},
    dBoxProc,
    visible,
    noGoAway,
    0,
    129,
    "",
    noAutoCenter
};

resource 'DITL' (129) {
    {
        {152, 310, 172, 370}, Button { enabled, "OK" };
        {152, 220, 172, 290}, Button { enabled, "Quit" };
        {70, 20, 86, 130}, StaticText { disabled, "Target host IP:" };
        {68, 134, 84, 280}, EditText { enabled, "" };
        {98, 20, 114, 130}, StaticText { disabled, "Groups:" };
        {96, 134, 112, 280}, EditText { enabled, "" };
        {8, 20, 62, 370}, StaticText { disabled,
            "IP of the computer running scripts/testnet.py (172.17.0.1 = the Snow test setup). Groups: digits, D, Q; empty = all." };
        {118, 20, 150, 370}, StaticText { disabled, "" };
    }
};
