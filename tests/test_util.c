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
/* ---- checksum contract (pinned for the C code, the host model below and the 68k asm) ----
 * mt_cksum_add / mt_cksum_copy return the canonical 16-bit ones-complement sum of the carried-in
 * sum (any uint32) plus the buffer read as big-endian 16-bit words from p (an odd last byte is a
 * high byte): a value in 0..0xFFFF, 0 only if the sum and every byte are 0, otherwise the value
 * in 1..0xFFFF congruent to the exact sum mod 0xFFFF. Equality is exact (bit-identical). */
static uint32_t ref_cksum(uint32_t sum, const uint8_t *p, uint32_t len) {
    uint64_t t = sum;                                   /* exact, no folding until the end */
    uint32_t i;
    for (i = 0; i < len; i++) t += (i & 1u) ? (uint64_t)p[i] : (uint64_t)p[i] << 8;
    return t == 0 ? 0u : (uint32_t)((t - 1u) % 0xFFFFu) + 1u;
}

/* Host model of mac/glue/mt_cksum68k.s, step for step: addx chains over big-endian long words
 * (32-bit add with the carry in X, absorbed once at the end), an odd start handled by one
 * leading byte and a byte swap of the folded rest, the leading word that long-aligns an even
 * destination, and the mixed-parity copy (byte copy, then sum the even side). */
static uint32_t m_fold(uint32_t x) {                   /* fold16 as in the asm: end-around carry */
    x = (x & 0xFFFFu) + (x >> 16);
    return (x & 0xFFFFu) + (x >> 16);
}
static uint32_t m_sum_even(const uint8_t *p, uint32_t len) {   /* p even: returns fold16 */
    uint32_t acc = 0, x = 0, n, t = 0;
    for (n = len >> 2; n; n--, p += 4) {               /* move.l (a0)+,d6 ; addx.l d6,d0 */
        uint64_t v = (uint64_t)acc + mt_rd32(p) + x;
        acc = (uint32_t)v;
        x = (uint32_t)(v >> 32);
    }
    acc += x;                                          /* addx.l zero,d0: never carries again */
    if (len & 2u) { t += mt_rd16(p); p += 2; }
    if (len & 1u) t += (uint32_t)p[0] << 8;
    return m_fold(m_fold(acc) + t);
}
static uint32_t m_finish(uint32_t sum, uint32_t part, uint32_t head, int swap) {
    if (swap) part = ((part << 8) | (part >> 8)) & 0xFFFFu;   /* rol.w #8 */
    return m_fold(part + head + (sum >> 16) + (sum & 0xFFFFu));
}
static uint32_t model_cksum_add(uint32_t sum, const uint8_t *p, uint32_t len) {
    uint32_t head = 0;
    int swap = 0;
    if (len && ((uintptr_t)p & 1u)) { head = (uint32_t)p[0] << 8; p++; len--; swap = 1; }
    return m_finish(sum, m_sum_even(p, len), head, swap);
}
static uint32_t model_cksum_copy(uint32_t sum, uint8_t *dst, const uint8_t *src, uint32_t len) {
    uint32_t head = 0, lead = 0, i, part;
    int swap = 0;
    if (((uintptr_t)src ^ (uintptr_t)dst) & 1u) {      /* mixed parity: bytes, then sum one side */
        for (i = 0; i < len; i++) dst[i] = src[i];
        return m_finish(sum, m_sum_even(((uintptr_t)src & 1u) ? dst : src, len), 0, 0);
    }
    if (len && ((uintptr_t)src & 1u)) {                 /* both odd: one leading byte */
        head = (uint32_t)src[0] << 8; *dst++ = *src++; len--; swap = 1;
    }
    if (len >= 2 && ((uintptr_t)dst & 2u)) {            /* long-align the destination */
        lead = mt_rd16(src); dst[0] = src[0]; dst[1] = src[1];
        src += 2; dst += 2; len -= 2;
    }
    for (i = 0; i < len; i++) dst[i] = src[i];
    part = m_fold(m_sum_even(src, len) + lead);
    return m_finish(sum, part, head, swap);
}

typedef uint32_t (*cks_add_fn)(uint32_t, const uint8_t *, uint32_t);
typedef uint32_t (*cks_copy_fn)(uint32_t, uint8_t *, const uint8_t *, uint32_t);

/* Every src offset 0..3, dst offset 0..3, len 0..64 and 1459..1461, four data patterns
 * (random, all 0xFF, 0xFF/0x00 alternating, zeros) and carried-in sums that stress the
 * accumulator. dst guard bytes must stay untouched. Returns the number of mismatches. */
static int sweep(cks_add_fn add, cks_copy_fn copy) {
    static const uint32_t sums[] = {0, 1, 0xFFFFu, 0x10000u, 0x0001FFFEu, 0xFFFFFFF0u,
                                    0xFFFFFFFFu, 0x8000FFFFu};
    static uint32_t sw[1472 / 4], dw[1472 / 4];
    uint8_t *sb = (uint8_t *)sw, *db = (uint8_t *)dw;
    uint32_t pat, so, d, li, si, i, x = 777;
    int bad = 0;
    for (pat = 0; pat < 4; pat++) {
        for (i = 0; i < 1472; i++) {
            x = x * 1103515245u + 12345u;
            sb[i] = pat == 0 ? (uint8_t)(x >> 16) : pat == 1 ? 0xFF : pat == 2 ? (uint8_t)((i & 1u) ? 0 : 0xFF) : 0;
        }
        for (li = 0; li < 68; li++) {
            uint32_t len = li < 65 ? li : 1459u + (li - 65u);
            for (so = 0; so < 4; so++) {
                const uint8_t *s = sb + so;
                for (si = 0; si < sizeof sums / sizeof sums[0]; si++) {
                    uint32_t want = ref_cksum(sums[si], s, len);
                    if (add(sums[si], s, len) != want) bad++;
                    for (d = 0; d < 4; d++) {
                        memset(db, 0xA5, 1472);
                        if (copy(sums[si], db + d, s, len) != want) bad++;
                        if (memcmp(db + d, s, len) != 0) bad++;
                        for (i = 0; i < d; i++) if (db[i] != 0xA5) bad++;
                        for (i = d + len; i < 1472; i++) if (db[i] != 0xA5) { bad++; break; }
                    }
                }
            }
        }
    }
    return bad;
}

static void test_cksum_matches_rfc1071_vectors(void) {
    /* RFC 1071 section 3 example: words 0001 f203 f4f5 f6f7 sum to 2ddf0 -> ddf2 */
    const uint8_t d[8] = {0x00,0x01,0xf2,0x03,0xf4,0xf5,0xf6,0xf7};
    uint8_t buf[10], out[10];
    uint32_t o;
    T_EQ(mt_cksum_add(0, d, 8), 0xddf2);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, d, 8)), 0x220d);
    for (o = 0; o < 2; o++) {                          /* same answer from an odd address */
        memcpy(buf + o, d, 8);
        T_EQ(mt_cksum_add(0, buf + o, 8), 0xddf2);
        T_EQ(mt_cksum_copy(0, out + 1 - o, buf + o, 8), 0xddf2);
        T_MEMEQ(out + 1 - o, d, 8);
    }
    /* RFC 1071 byte-order independence: the swapped data sums to the swapped result */
    {
        const uint8_t e[8] = {0x01,0x00,0x03,0xf2,0xf5,0xf4,0xf7,0xf6};
        T_EQ(mt_cksum_add(0, e, 8), 0xf2dd);
    }
    /* canonical result: 0 only for an all-zero input, 0xFFFF for a nonzero multiple */
    T_EQ(mt_cksum_add(0, d, 0), 0);
    T_EQ(mt_cksum_add(0xFFFFu, d, 0), 0xFFFF);
    T_EQ(mt_cksum_add(0x10000u, d, 0), 1);
    T_EQ(mt_cksum_add(0xFFFFFFFFu, d, 0), 0xFFFF);
    T_EQ(mt_cksum_add(0, (const uint8_t *)"\xff\xff", 2), 0xFFFF);
    T_EQ(mt_cksum_add(1, (const uint8_t *)"\xff\xfe", 2), 0xFFFF);
    T_EQ(mt_cksum_add(0, (const uint8_t *)"\x00\x00\x00", 3), 0);
    /* carry out of 32 bits in the carried-in sum is not lost */
    T_EQ(mt_cksum_add(0xFFFFFFFFu, (const uint8_t *)"\x00\x01", 2), 1);
}
static void test_cksum_copy_all_alignments_and_lengths(void) {
    T_EQ(sweep(mt_cksum_add, mt_cksum_copy), 0);
}
static void test_cksum_model_of_asm(void) {          /* the 68k algorithm, run on the host */
    T_EQ(sweep(model_cksum_add, model_cksum_copy), 0);
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
       test_cksum_copy_matches, test_cksum_matches_rfc1071_vectors,
       test_cksum_copy_all_alignments_and_lengths, test_cksum_model_of_asm, test_ring_wrap, test_ring_edges, test_ring_write_at)
