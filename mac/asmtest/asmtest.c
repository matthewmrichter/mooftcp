/* asmtest.c - AsmTest: equivalence and timing of mac/glue/mt_cksum68k.s on a real 68000 (Snow).
 *
 * mt_cksum_add / mt_cksum_copy here are the asm versions; cref_cksum_add / cref_cksum_copy are
 * the C versions of core/mt_util.c (cref.c). The sweep is the one of tests/test_util.c: every
 * src offset 0..3, dst offset 0..3, len 0..64 and 1459..1461, four data patterns (random,
 * all 0xFF, 0xFF/0x00, zeros) and carried-in sums that stress the accumulator; results must be
 * bit-identical, the copied bytes equal and the guard bytes around dst untouched. A word or
 * long access to an odd address would bomb (address error) instead of finishing.
 * Then 1000 x 1460 bytes are timed for C and asm (TickCount, 1/60 s). Output: AsmTest.out in
 * the app's folder, and an alert with the summary. */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>
#include <Files.h>
#include <stdio.h>
#include <string.h>
#include "mt_util.h"

uint32_t cref_cksum_add(uint32_t sum, const uint8_t *p, uint32_t len);
uint32_t cref_cksum_copy(uint32_t sum, uint8_t *dst, const uint8_t *src, uint32_t len);

QDGlobals qd;
static short outref = -1;

#define BUF 1480                       /* 1461 + 3 offset + guard, multiple of 4 */
static uint32_t srcw[BUF / 4], dstw[BUF / 4], refw[BUF / 4];
static volatile uint32_t sink;
static unsigned long n_cases, n_bad;
static int n_shown;

static void out(const char *s)
{
    long cnt = (long)strlen(s);
    if (outref >= 0) FSWrite(outref, &cnt, s);
}

static void out_open(void)
{
    Create((ConstStringPtr)"\pAsmTest.out", 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)"\pAsmTest.out", 0, &outref)) outref = -1;
    else SetEOF(outref, 0);
}

static void out_close(void)
{
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
}

/* Independent bytewise reference (end-around carry after every add), canonical result. */
static uint32_t ref_bits(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t t = (sum & 0xFFFFu) + (sum >> 16), i;
    t = (t & 0xFFFFu) + (t >> 16);
    for (i = 0; i < len; i++) {
        t += (i & 1u) ? (uint32_t)p[i] : (uint32_t)p[i] << 8;
        t = (t & 0xFFFFu) + (t >> 16);
    }
    return t;
}

static void fail(const char *fn, unsigned so, unsigned d, unsigned long len, unsigned long sum,
                 unsigned pat, unsigned long got, unsigned long want, int bytes_ok)
{
    char line[160];
    n_bad++;
    if (n_shown++ >= 20) return;
    sprintf(line, "FAIL %s so=%u do=%u len=%lu sum=%08lx pat=%u got=%04lx want=%04lx bytes=%s\r",
            fn, so, d, len, sum, pat, got, want, bytes_ok ? "ok" : "BAD");
    out(line);
}

static void fill_pat(uint8_t *b, unsigned pat)
{
    uint32_t i, x = 777;
    for (i = 0; i < BUF; i++) {
        x = x * 1103515245u + 12345u;
        b[i] = pat == 0 ? (uint8_t)(x >> 16) : pat == 1 ? 0xFF
             : pat == 2 ? (uint8_t)((i & 1u) ? 0 : 0xFF) : 0;
    }
}

static void sweep(void)
{
    static const uint32_t sums[] = {0, 1, 0xFFFFu, 0x10000u, 0x0001FFFEu, 0xFFFFFFF0u,
                                    0xFFFFFFFFu, 0x8000FFFFu};
    uint8_t *sb = (uint8_t *)srcw, *db = (uint8_t *)dstw;
    unsigned pat, so, d, li, si;
    uint32_t i;
    for (pat = 0; pat < 4; pat++) {
        fill_pat(sb, pat);
        for (li = 0; li < 68; li++) {
            uint32_t len = li < 65 ? li : 1459u + (li - 65u);
            for (so = 0; so < 4; so++) {
                const uint8_t *s = sb + so;
                for (si = 0; si < sizeof sums / sizeof sums[0]; si++) {
                    uint32_t want = cref_cksum_add(sums[si], s, len), got;
                    n_cases++;
                    got = mt_cksum_add(sums[si], s, len);
                    if (got != want) fail("add", so, 0, len, sums[si], pat, got, want, 1);
                    for (d = 0; d < 4; d++) {
                        int ok = 1;
                        uint32_t end = d + len + 8 < BUF ? d + len + 8 : BUF;
                        for (i = 0; i < end; i++) db[i] = 0xA5;
                        n_cases++;
                        got = mt_cksum_copy(sums[si], db + d, s, len);
                        if (memcmp(db + d, s, len) != 0) ok = 0;
                        for (i = 0; i < d; i++) if (db[i] != 0xA5) ok = 0;
                        for (i = d + len; i < end; i++) if (db[i] != 0xA5) ok = 0;
                        if (got != want || !ok) fail("copy", so, d, len, sums[si], pat, got, want, ok);
                    }
                }
            }
        }
    }
}

/* 1460 bytes of 0xFF at every alignment, carried-in sums near the top of the accumulator:
 * asm, C and the bytewise reference must all agree. */
static void ff1460(void)
{
    static const uint32_t sums[] = {0, 0xFFFFu, 0xFFFFFFFFu, 0xFFFF0000u};
    uint8_t *sb = (uint8_t *)srcw, *db = (uint8_t *)dstw;
    unsigned so, d, si;
    fill_pat(sb, 1);
    for (so = 0; so < 4; so++)
        for (si = 0; si < 4; si++) {
            uint32_t r = ref_bits(sums[si], sb + so, 1460), c = cref_cksum_add(sums[si], sb + so, 1460);
            n_cases++;
            if (c != r) fail("cref", so, 0, 1460, sums[si], 1, c, r, 1);
            for (d = 0; d < 4; d++) {
                uint32_t a = mt_cksum_copy(sums[si], db + d, sb + so, 1460);
                n_cases++;
                if (a != r) fail("ff", so, d, 1460, sums[si], 1, a, r, memcmp(db + d, sb + so, 1460) == 0);
            }
        }
}

typedef uint32_t (*copy_fn)(uint32_t, uint8_t *, const uint8_t *, uint32_t);
typedef uint32_t (*add_fn)(uint32_t, const uint8_t *, uint32_t);

static unsigned long time_copy(copy_fn f, uint8_t *dst, const uint8_t *src)
{
    unsigned long t0 = (unsigned long)TickCount();
    int i;
    for (i = 0; i < 1000; i++) sink += f(0, dst, src, 1460);
    return (unsigned long)TickCount() - t0;
}

static unsigned long time_add(add_fn f, const uint8_t *src)
{
    unsigned long t0 = (unsigned long)TickCount();
    int i;
    for (i = 0; i < 1000; i++) sink += f(0, src, 1460);
    return (unsigned long)TickCount() - t0;
}

static void timings(void)
{
    uint8_t *sb = (uint8_t *)srcw, *db = (uint8_t *)dstw, *rb = (uint8_t *)refw;
    char line[160];
    static const struct { const char *what; unsigned so, d; } cp[] = {
        {"copy src even, dst 2 mod 4 (txbuf+54)", 0, 2},
        {"copy src even, dst 0 mod 4", 0, 0},
        {"copy both odd", 1, 1},
        {"copy mixed parity (src even, dst odd)", 0, 1},
    };
    unsigned k;
    (void)rb;
    fill_pat(sb, 0);
    out("timing: ticks (1/60 s) for 1000 x 1460 bytes, C (-O2) vs asm\r");
    for (k = 0; k < sizeof cp / sizeof cp[0]; k++) {
        unsigned long tc = time_copy(cref_cksum_copy, db + cp[k].d, sb + cp[k].so);
        unsigned long ta = time_copy(mt_cksum_copy, db + cp[k].d, sb + cp[k].so);
        sprintf(line, "time %s: C %lu asm %lu\r", cp[k].what, tc, ta);
        out(line);
    }
    {
        unsigned long tc = time_add(cref_cksum_add, sb), ta = time_add(mt_cksum_add, sb);
        sprintf(line, "time add src even: C %lu asm %lu\r", tc, ta);
        out(line);
        tc = time_add(cref_cksum_add, sb + 1);
        ta = time_add(mt_cksum_add, sb + 1);
        sprintf(line, "time add src odd: C %lu asm %lu\r", tc, ta);
        out(line);
    }
    {
        unsigned long t0 = (unsigned long)TickCount(), tm;
        int i;
        for (i = 0; i < 1000; i++) memcpy(db, sb, 1460);
        tm = (unsigned long)TickCount() - t0;
        sprintf(line, "time memcpy (ring copies) both even: %lu\r", tm);
        out(line);
    }
}

int main(void)
{
    char line[160];
    Str255 ps;
    size_t n;
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    out_open();
    out("AsmTest: mac/glue/mt_cksum68k.s vs the C of core/mt_util.c\r");
    sweep();
    ff1460();
    sprintf(line, "%s %lu/%lu cases\r", n_bad ? "FAIL" : "PASS", n_cases - n_bad, n_cases);
    out(line);
    FlushVol(0, 0);
    timings();
    out(n_bad ? "RESULT FAIL\r" : "RESULT PASS\r");
    out_close();

    sprintf(line, "AsmTest %s %lu/%lu. Results are in AsmTest.out", n_bad ? "FAIL" : "PASS",
            n_cases - n_bad, n_cases);
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
