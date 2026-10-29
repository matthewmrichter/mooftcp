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
