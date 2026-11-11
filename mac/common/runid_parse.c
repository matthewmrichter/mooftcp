/* copied from moofagent 6ca9126; change there first */
/* runid_parse.c - the run file's text format. No Toolbox, so it is host-tested
 * (tests/test_runid.c) and can be shared verbatim with other tools that read the run file. */
#include "runid_parse.h"

int runid_parse(const char *buf, long len, char *id, int cap, int *unattended)
{
    long i = 0, end;
    int bangs = 0, n = 0;

    *unattended = 0;
    if (!buf || len <= 0 || cap < 1 || buf[0] != '!') return 0;
    while (i < len && i < 2 && buf[i] == '!') { i++; bangs++; }
    for (end = i; end < len && buf[end] != '\r' && buf[end] != '\n'; end++) {}
    while (i < end && buf[i] == ' ') i++;
    while (end > i && buf[end - 1] == ' ') end--;
    if (end == i || end - i > cap - 1) return 0;
    for (; i < end; i++) id[n++] = buf[i];
    id[n] = 0;
    *unattended = bangs == 2;
    return 1;
}
