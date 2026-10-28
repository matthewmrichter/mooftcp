#include "Types.r"
#include "Dialogs.r"

resource 'ALRT' (128) {
    {80, 80, 180, 400},
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
        {70, 230, 90, 300}, Button { enabled, "OK" };
        {10, 70, 60, 300}, StaticText { disabled, "MoofTCP pipeline OK" };
    }
};
