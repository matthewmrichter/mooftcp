/* Layout test for mac/include/mt_mactcp.h.
 *
 * Needs the reference MacTCP header (Apple's, outside the repo, never copied):
 *   MT_REF_MACTCP_H=$HOME/projects/flynn/include/MacTCP.h ctest -R layout
 * Without the variable the test prints "skip" and passes.
 *
 * Method: tests/mactcp_layout_items.h lists every sizeof/offsetof expression and
 * every constant name. We evaluate each expression twice:
 *   ours: natively, from mt_mactcp.h (fixed-width fields, pack(2));
 *   ref:  by writing a probe C file that includes tests/mactcp_ref_shim.h and the
 *         reference header, compiling it with "$CC -m32 -ffreestanding -S" (4-byte
 *         pointers/longs, 2-byte alignment = the 68k ABI layout), and reading the
 *         constants out of the emitted assembly table.
 * Values are stored +100000 in the table so no entry is 0 (the compiler may emit
 * runs of zeros as ".zero"). */
#define _POSIX_C_SOURCE 200809L   /* mkstemp, popen (host-only test) */
#include <stddef.h>
#include <unistd.h>
#include "t.h"
#include "../mac/include/mt_mactcp.h"
#include "mactcp_layout_items.h"

#define BIAS 100000L

static const char *const item_text[] = {
#define X(e) #e,
    LAYOUT_ITEMS(X)
    CONST_ITEMS(X)
#undef X
};
static const long our_val[] = {
#define X(e) ((long)(e)),
    LAYOUT_ITEMS(X)
    CONST_ITEMS(X)
#undef X
};
#define N_ITEMS (sizeof our_val / sizeof our_val[0])

static void shim_path(char *out, size_t n)
{
    const char *f = __FILE__;
    const char *slash = strrchr(f, '/');
    size_t d = slash ? (size_t)(slash - f) : 0;
    if (slash) snprintf(out, n, "%.*s/mactcp_ref_shim.h", (int)d, f);
    else snprintf(out, n, "mactcp_ref_shim.h");
}

static int write_probe(const char *path, const char *ref_h)
{
    char shim[1024];
    size_t i;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    shim_path(shim, sizeof shim);
    fprintf(f, "#include \"%s\"\n#include \"%s\"\n", shim, ref_h);
    /* the reference declares these two without a typedef */
    fprintf(f, "typedef struct IPParamBlock IPParamBlock;\n");
    fprintf(f, "typedef struct ICMPParamBlock ICMPParamBlock;\n");
    fprintf(f, "const int mt_ref_tab[] = {\n");
    for (i = 0; i < N_ITEMS; i++)
        fprintf(f, "  (int)(%s) + %ld,\n", item_text[i], BIAS);
    fprintf(f, "};\n");
    fclose(f);
    return 0;
}

static void layout_matches_reference(void)
{
    const char *ref = getenv("MT_REF_MACTCP_H");
    const char *cc = getenv("CC");
    char probe[] = "mt_layout_probe_XXXXXX";
    char cmd[2048], line[512];
    long ref_val[N_ITEMS];
    size_t n = 0, i;
    int fd, in_tab = 0;
    FILE *p;

    if (!ref || !*ref) {
        printf("skip: MT_REF_MACTCP_H not set\n");
        return;
    }
    if (!cc || !*cc) cc = "gcc";
    fd = mkstemp(probe);                 /* creates "mt_layout_probe_XXXXXX" in cwd */
    T_CHECK(fd >= 0);
    if (fd < 0) return;
    close(fd);
    {
        char probe_c[64];
        snprintf(probe_c, sizeof probe_c, "%s.c", probe);
        T_CHECK(write_probe(probe_c, ref) == 0);
        snprintf(cmd, sizeof cmd,
                 "%s -std=gnu99 -m32 -ffreestanding -fno-pic -fno-asynchronous-unwind-tables "
                 "-w -S -o - %s", cc, probe_c);
        p = popen(cmd, "r");
        T_CHECK(p != NULL);
        if (p) {
            while (fgets(line, sizeof line, p)) {
                long v;
                char *e;
                if (!in_tab) {
                    if (strncmp(line, "mt_ref_tab:", 11) == 0) in_tab = 1;
                    continue;
                }
                if (strncmp(line, "\t.size", 6) == 0) { in_tab = 0; continue; }
                if (strncmp(line, "\t.long\t", 7) != 0) continue;
                v = strtol(line + 7, &e, 10);
                if (n < N_ITEMS) ref_val[n] = v - BIAS;
                n++;
            }
            T_CHECK(pclose(p) == 0);
        }
        remove(probe_c);
        remove(probe);
    }
    if (n != N_ITEMS) {
        fprintf(stderr, "probe produced %zu values, expected %zu (compiler could not "
                "build the reference header with -m32?)\n", n, (size_t)N_ITEMS);
        t_failed = 1;
        return;
    }
    for (i = 0; i < N_ITEMS; i++) {
        if (ref_val[i] != our_val[i]) {
            fprintf(stderr, "MISMATCH %-60s ref %ld ours %ld\n", item_text[i],
                    ref_val[i], our_val[i]);
            t_failed = 1;
        }
    }
    printf("compared %zu values\n", (size_t)N_ITEMS);
}

T_MAIN(layout_matches_reference)
