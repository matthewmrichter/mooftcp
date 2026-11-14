/* inst_auto.c - parser of MoofTCPInstall.cfg (inst_auto.h). No Toolbox calls. */
#include <string.h>
#include "inst_auto.h"

static int is_blank(char c) { return c == ' ' || c == '\t'; }
static int is_eol(char c) { return c == '\r' || c == '\n'; }

int inst_auto_parse(const char *buf, long len, InstAuto *a) {
    long i = 0;
    int words = 0;
    a->action = IA_NONE; a->lite = 0; a->prefs = 0;
    while (i < len && !is_eol(buf[i]) && buf[i] != 0) {
        char w[16];
        int n = 0;
        if (is_blank(buf[i])) { i++; continue; }
        while (i < len && !is_blank(buf[i]) && !is_eol(buf[i]) && buf[i] != 0) {
            if (n < (int)sizeof w - 1) w[n] = buf[i];
            n++; i++;
        }
        if (n >= (int)sizeof w) goto bad;
        w[n] = 0;
        if (words++ == 0) {
            if (!strcmp(w, "install")) a->action = IA_INSTALL;
            else if (!strcmp(w, "remove")) a->action = IA_REMOVE;
            else if (!strcmp(w, "reinstall")) a->action = IA_REINSTALL;
            else goto bad;
        } else if (a->action == IA_REMOVE) {
            goto bad;                                   /* remove takes no options */
        } else if (!strcmp(w, "lite") && !a->lite) {
            a->lite = 1;
        } else if (!strcmp(w, "prefs") && !a->prefs) {
            a->prefs = 1;
        } else {
            goto bad;
        }
    }
    if (a->action == IA_NONE) goto bad;
    for (; i < len; i++)                                /* only blanks and line ends after the line */
        if (!is_blank(buf[i]) && !is_eol(buf[i]) && buf[i] != 0) goto bad;
    return 0;
bad:
    a->action = IA_NONE; a->lite = 0; a->prefs = 0;
    return -1;
}
