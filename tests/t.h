#ifndef T_H
#define T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int t_failed;

#define T_CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    t_failed = 1; } } while (0)

#define T_EQ(a, b) do { long long t_a = (long long)(a), t_b = (long long)(b); \
    if (t_a != t_b) { \
        fprintf(stderr, "%s:%d: %s == %s failed: %lld vs %lld\n", \
                __FILE__, __LINE__, #a, #b, t_a, t_b); \
        t_failed = 1; } } while (0)

#define T_MEMEQ(a, b, n) do { \
    if (memcmp((a), (b), (size_t)(n)) != 0) { \
        fprintf(stderr, "%s:%d: memory differs: %s vs %s (%d bytes)\n", \
                __FILE__, __LINE__, #a, #b, (int)(n)); \
        t_failed = 1; } } while (0)

#define T_MAIN(...) \
    int main(int argc, char **argv) { \
        static const struct { const char *name; void (*fn)(void); } t_tests[] = { \
            T_LIST(__VA_ARGS__) }; \
        size_t t_i; \
        for (t_i = 0; t_i < sizeof t_tests / sizeof t_tests[0]; t_i++) { \
            if (argc > 1 && !strstr(t_tests[t_i].name, argv[1])) continue; \
            t_tests[t_i].fn(); \
            printf("%s %s\n", t_failed ? "FAIL" : "ok  ", t_tests[t_i].name); \
        } \
        return t_failed ? 1 : 0; \
    }

/* Expand test names to {"name", name} initializers (up to 16 tests per file). */
#define T_CAT_(a, b) a##b
#define T_CAT(a, b) T_CAT_(a, b)
#define T_ENT(n) {#n, n},
#define T_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N
#define T_LIST(...) T_CAT(T_L, T_N(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1))(__VA_ARGS__)
#define T_L1(a) T_ENT(a)
#define T_L2(a,...) T_ENT(a) T_L1(__VA_ARGS__)
#define T_L3(a,...) T_ENT(a) T_L2(__VA_ARGS__)
#define T_L4(a,...) T_ENT(a) T_L3(__VA_ARGS__)
#define T_L5(a,...) T_ENT(a) T_L4(__VA_ARGS__)
#define T_L6(a,...) T_ENT(a) T_L5(__VA_ARGS__)
#define T_L7(a,...) T_ENT(a) T_L6(__VA_ARGS__)
#define T_L8(a,...) T_ENT(a) T_L7(__VA_ARGS__)
#define T_L9(a,...) T_ENT(a) T_L8(__VA_ARGS__)
#define T_L10(a,...) T_ENT(a) T_L9(__VA_ARGS__)
#define T_L11(a,...) T_ENT(a) T_L10(__VA_ARGS__)
#define T_L12(a,...) T_ENT(a) T_L11(__VA_ARGS__)
#define T_L13(a,...) T_ENT(a) T_L12(__VA_ARGS__)
#define T_L14(a,...) T_ENT(a) T_L13(__VA_ARGS__)
#define T_L15(a,...) T_ENT(a) T_L14(__VA_ARGS__)
#define T_L16(a,...) T_ENT(a) T_L15(__VA_ARGS__)

#endif
