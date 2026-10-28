#include "t.h"
#include "mt_util.h"

static void test_rd_wr(void) {
    uint8_t b[5] = {0};
    mt_wr16(b + 1, 0x1234); T_EQ(b[1], 0x12); T_EQ(b[2], 0x34);
    mt_wr32(b + 1, 0xA1B2C3D4); T_EQ(mt_rd32(b + 1), 0xA1B2C3D4);
    T_EQ(mt_rd16(b + 3), 0xC3D4);
}
static void test_time_wrap(void) {
    T_CHECK(mt_time_before(0xFFFFFFF0u, 0x00000010u));
    T_CHECK(!mt_time_before(0x00000010u, 0xFFFFFFF0u));
    T_CHECK(mt_seq_lt(0xFFFFFF00u, 0x00000100u));
    T_CHECK(mt_seq_ge(5, 5));
}
T_MAIN(test_rd_wr, test_time_wrap)
