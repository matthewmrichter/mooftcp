/* copied from moofagent 6ca9126; change there first */
/* runid_parse.h - the run file's text format, no Toolbox (host-tested by tests/test_runid.c). */
#ifndef RUNID_PARSE_H
#define RUNID_PARSE_H

/* Parse the contents of a run file ("Agent Run" / "MoofPort Run"): "!<id>" or "!!<id>", first
 * line only (CR or LF ends it), trailing spaces dropped. Returns 1 and fills `id` (NUL
 * terminated, at most cap-1 characters) when the line starts with '!' and holds an id;
 * *unattended is 1 for "!!" (skip the closing alert and quit), else 0. Returns 0 (ask for an
 * id) when there is no '!', the id is empty, or it does not fit in `cap`; *unattended is then 0 (a caller that sees '!' first
 * still treats the run as quiet, with no id; mac/runid.c does, so unattended runs never ask). */
int runid_parse(const char *buf, long len, char *id, int cap, int *unattended);

#endif
