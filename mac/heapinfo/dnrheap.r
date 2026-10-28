#include "Types.r"
#include "Dialogs.r"

resource 'ALRT' (128) {
    {50, 30, 210, 470},
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
        {130, 360, 150, 430}, Button { enabled, "OK" };
        {10, 10, 125, 430}, StaticText { disabled, "DNR (bytes)^0^1^2^3" };
    }
};
