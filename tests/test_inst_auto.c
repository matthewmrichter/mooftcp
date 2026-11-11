/* test_inst_auto.c - the unattended installer's MoofTCPInstall.cfg parser (mac/installer/inst_auto.[ch]). */
#include "t.h"
#include "inst_auto.h"

static int P(const char *s, InstAuto *a) { return inst_auto_parse(s, (long)strlen(s), a); }

static void check(const char *s, int rc, int action, int lite, int prefs) {
    InstAuto a;
    memset(&a, 0xAA, sizeof a);
    if (P(s, &a) != rc) { fprintf(stderr, "cfg \"%s\": rc differs\n", s); t_failed = 1; }
    T_EQ(a.action, action); T_EQ(a.lite, lite); T_EQ(a.prefs, prefs);
}

static void test_verbs(void) {
    check("install", 0, IA_INSTALL, 0, 0);
    check("remove", 0, IA_REMOVE, 0, 0);
    check("reinstall", 0, IA_REINSTALL, 0, 0);
    check("install lite", 0, IA_INSTALL, 1, 0);
    check("install prefs", 0, IA_INSTALL, 0, 1);
    check("reinstall lite prefs", 0, IA_REINSTALL, 1, 1);
}

static void test_option_order(void) {
    check("install lite prefs", 0, IA_INSTALL, 1, 1);
    check("install prefs lite", 0, IA_INSTALL, 1, 1);
    check("reinstall prefs lite", 0, IA_REINSTALL, 1, 1);
}

static void test_line_ends_and_blanks(void) {
    check("install\r", 0, IA_INSTALL, 0, 0);
    check("install lite\n", 0, IA_INSTALL, 1, 0);
    check("reinstall lite prefs\r\n", 0, IA_REINSTALL, 1, 1);
    check("  reinstall \t prefs  \r\n\r\n", 0, IA_REINSTALL, 0, 1);
}

static void test_bad(void) {
    check("", -1, IA_NONE, 0, 0);
    check("\r", -1, IA_NONE, 0, 0);
    check("   \r\n", -1, IA_NONE, 0, 0);
    check("upgrade", -1, IA_NONE, 0, 0);
    check("install full", -1, IA_NONE, 0, 0);
    check("install lite lite", -1, IA_NONE, 0, 0);
    check("remove lite", -1, IA_NONE, 0, 0);
    check("remove prefs", -1, IA_NONE, 0, 0);
    check("Install", -1, IA_NONE, 0, 0);
    check("lite install", -1, IA_NONE, 0, 0);
    check("install\rremove\r", -1, IA_NONE, 0, 0);
    check("installprefs", -1, IA_NONE, 0, 0);
    check("install aaaaaaaaaaaaaaaaaaaaaaaa", -1, IA_NONE, 0, 0);
}

static void test_not_nul_terminated(void) {
    InstAuto a;
    static const char b[] = { 'i','n','s','t','a','l','l',' ','l','i','t','e','X','X' };
    T_EQ(inst_auto_parse(b, 12, &a), 0);              /* len cuts the buffer: "install lite" */
    T_EQ(a.action, IA_INSTALL); T_EQ(a.lite, 1);
    T_EQ(inst_auto_parse(b, 0, &a), -1);
}

T_MAIN(test_verbs, test_option_order, test_line_ends_and_blanks, test_bad, test_not_nul_terminated)
