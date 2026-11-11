/* tune_parse.h - MoofTCPTune's portable part: the presets and the MoofTCPTune.cfg text (no Toolbox;
 * host-tested by tests/test_tune_parse.c). */
#ifndef TUNE_PARSE_H
#define TUNE_PARSE_H

typedef struct { unsigned rto_min, rto_init, delack, stall, quickack; } tn_vals;

/* The release defaults (mt_int.h: MT_TCP_RTO_MIN / RTO_INIT / DELACK_MS / STALL_MS / QUICKACK). */
void tn_defaults(tn_vals *v);

/* Preset named by a run id: default, delack0, delack20, delack50, qack16, qackall, rto500, rto700, stall2, stall5.
 * Fills *v with the defaults plus that preset's one change (so a run never inherits an earlier preset) and
 * returns 1; returns 0 (v untouched) for any other name. */
int tn_preset(const char *id, tn_vals *v);

/* MoofTCPTune.cfg: lines "key=value" with key rto_min, rto_init, delack, stall or quickack, value a decimal
 * number; CR, LF or CRLF end a line; blank lines and lines starting with '#' are skipped; spaces around the
 * key and value are ignored. Keys the text lacks keep their value in *v. Returns the number of keys applied,
 * or -1 with a short message in err (unknown key, bad number, no '='). */
int tn_parse_cfg(const char *text, long len, tn_vals *v, char *err, int errcap);

#endif
