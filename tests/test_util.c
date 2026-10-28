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
    T_CHECK(mt_seq_le(5, 5));
    T_CHECK(mt_seq_le(0xFFFFFF00u, 0x00000100u));
    T_CHECK(!mt_seq_le(0x00000100u, 0xFFFFFF00u));
    T_CHECK(mt_seq_gt(0x00000100u, 0xFFFFFF00u));
    T_CHECK(!mt_seq_gt(5, 5));
    /* +/-2^31 boundary: a distance of exactly 2^31 reads as "before" both ways */
    T_CHECK(mt_time_before(0u, 0x80000000u));
    T_CHECK(mt_time_before(0x80000000u, 0u));
    T_CHECK(mt_time_before(0u, 0x7FFFFFFFu));
    T_CHECK(!mt_time_before(0x7FFFFFFFu, 0u));
}
static void test_cksum_rfc1071(void) {
    const uint8_t d[8] = {0x00,0x01,0xf2,0x03,0xf4,0xf5,0xf6,0xf7};
    T_EQ(mt_cksum_fold(mt_cksum_add(0, d, 8)), (uint16_t)~0xddf2);
}
static void test_cksum_odd_and_chained(void) {
    const uint8_t d[5] = {1,2,3,4,5};
    T_EQ(mt_cksum_fold(mt_cksum_add(mt_cksum_add(0, d, 4), d + 4, 1)),
         mt_cksum_fold(mt_cksum_add(0, d, 5)));
    T_EQ(mt_cksum_fold(mt_cksum_add(0, d, 5)), (uint16_t)~0x0906);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, d, 0)), 0xFFFF);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, d, 1)), (uint16_t)~0x0100);
}
static void test_cksum_copy_matches(void) {
    uint8_t srcbuf[1501], dst[1500];
    uint8_t *src = srcbuf + 1;               /* odd address */
    uint32_t x = 12345, i;
    for (i = 0; i < 1500; i++) { x = x * 1103515245u + 12345u; src[i] = (uint8_t)(x >> 16); }
    memset(dst, 0, sizeof dst);
    uint32_t a = mt_cksum_add(0, src, 1500);
    uint32_t c = mt_cksum_copy(0, dst, src, 1500);
    T_MEMEQ(dst, src, 1500);
    T_EQ(mt_cksum_fold(c), mt_cksum_fold(a));
    /* odd length copy */
    memset(dst, 0, sizeof dst);
    c = mt_cksum_copy(0, dst, src, 1499);
    T_MEMEQ(dst, src, 1499);
    T_EQ(mt_cksum_fold(c), mt_cksum_fold(mt_cksum_add(0, src, 1499)));
}
static void test_ring_wrap(void) {
    uint8_t mem[8]; mt_ring r; mt_ring_init(&r, mem, 8);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"abcdef", 6), 6);
    uint8_t o[8]; T_EQ(mt_ring_read(&r, o, 4), 4);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"ghijkl", 6), 6);   /* wraps */
    T_EQ(mt_ring_free(&r), 0);
    const uint8_t *p; T_EQ(mt_ring_peek(&r, 0, &p), 4); T_MEMEQ(p, "efgh", 4);
    T_EQ(mt_ring_read(&r, o, 8), 8); T_MEMEQ(o, "efghijkl", 8);
}
static void test_ring_edges(void) {
    uint8_t mem[8], o[8]; mt_ring r; const uint8_t *p;
    mt_ring_init(&r, mem, 8);
    T_EQ(mt_ring_read(&r, o, 4), 0);                 /* empty read */
    T_EQ(mt_ring_peek(&r, 0, &p), 0);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"0123456789", 10), 8);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"x", 1), 0); /* full */
    T_EQ(mt_ring_peek(&r, 3, &p), 5); T_MEMEQ(p, "34567", 5);
    T_EQ(mt_ring_peek(&r, 8, &p), 0);
    mt_ring_consume(&r, 6);
    T_EQ(mt_ring_free(&r), 6);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"ab", 2), 2);  /* wraps to start */
    T_EQ(mt_ring_peek(&r, 0, &p), 2); T_MEMEQ(p, "67", 2);
    T_EQ(mt_ring_peek(&r, 1, &p), 1); T_MEMEQ(p, "7", 1);
    T_EQ(mt_ring_peek(&r, 2, &p), 2); T_MEMEQ(p, "ab", 2);
    T_EQ(mt_ring_read(&r, o, 3), 3); T_MEMEQ(o, "67a", 3);
}
static void test_ring_write_at(void) {
    uint8_t mem[8], o[8]; mt_ring r; const uint8_t *p;
    mt_ring_init(&r, mem, 8);
    mt_ring_write(&r, (const uint8_t*)"abcdef", 6);
    mt_ring_consume(&r, 5);                          /* head=5, used=1 */
    T_EQ(mt_ring_write_at(&r, 2, (const uint8_t*)"XYZW", 4), 4); /* wraps */
    T_EQ(r.used, 1);
    T_EQ(mt_ring_write_at(&r, 6, (const uint8_t*)"123", 3), 1);  /* only 1 fits */
    T_EQ(mt_ring_write_at(&r, 8, (const uint8_t*)"1", 1), 0);
    T_EQ(mt_ring_write(&r, (const uint8_t*)"pq", 2), 2);
    T_EQ(mt_ring_peek(&r, 0, &p), 3); T_MEMEQ(p, "fpq", 3);
    T_EQ(mt_ring_read(&r, o, 8), 3);
    /* data written ahead is intact after growing used over it */
    mt_ring_init(&r, mem, 8);
    mt_ring_write_at(&r, 2, (const uint8_t*)"cd", 2);
    mt_ring_write_at(&r, 0, (const uint8_t*)"ab", 2);
    r.used = 4;                                      /* adopt the ahead-written bytes */
    T_EQ(mt_ring_read(&r, o, 4), 4); T_MEMEQ(o, "abcd", 4);
}
T_MAIN(test_rd_wr, test_time_wrap, test_cksum_rfc1071, test_cksum_odd_and_chained,
       test_cksum_copy_matches, test_ring_wrap, test_ring_edges, test_ring_write_at)
