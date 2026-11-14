/* copied from moofagent 6ca9126; change there first */
/* runid.h - name each run of a Phase 0 tool, so its output file is "<Tool>-<id>.out". */
#ifndef RUNID_H
#define RUNID_H

/* The run file's name (Pascal string): read first; RUNID_FILE_OLD is read when it is missing.
 * MoofAgent writes and restores both. */
#define RUNID_FILE     "\pAgent Run"
#define RUNID_FILE_OLD "\pMoofPort Run"

/* Ask for a run id (or take it from the "Agent Run" file, see runid.c) and build the
 * output file name: "<tool>-<id><ext>", or "<tool><ext>" for an empty id or Cancel. `out` is a
 * Pascal string of at most 31 characters (HFS). Call after InitDialogs. Returns `out`. */
unsigned char *runid_name(const char *tool, const char *ext, unsigned char *out);

/* After runid_name: 1 if the run file started with "!!" (MoofAgent's runs): skip the closing
 * alert and quit, so nothing waits for a click. */
int runid_unattended(void);

#endif
