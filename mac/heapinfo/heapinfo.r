#include "Types.r"
#include "Dialogs.r"

resource 'ALRT' (128) {
    {60, 60, 200, 440},
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
        {110, 300, 130, 370}, Button { enabled, "OK" };
        {10, 10, 100, 370}, StaticText { disabled, "System heap^0^1^2^3" };
    }
};
