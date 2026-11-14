/* tune_parse.h - MoofTCPTune's portable part: the presets and the MoofTCPTune.cfg text (no Toolbox;
 * host-tested by tests/test_tune_parse.c). */
#ifndef TUNE_PARSE_H
#define TUNE_PARSE_H

typedef struct { unsigned rto_min, rto_init, delack, stall, quickack, txpump; } tn_vals;

/* The release defaults (mt_int.h: MT_TCP_RTO_MIN / RTO_INIT / DELACK_MS / STALL_MS / QUICKACK; txpump 0: off). */
void tn_defaults(tn_vals *v);

/* Preset named by a run id: default, delack0, delack20, delack50, qack16, qackall, rto500, rto700, stall2, stall5,
 * txpump (the glue's transmit pump on, task 17).
 * Fills *v with the defaults plus that preset's one change (so a run never inherits an earlier preset) and
 * returns 1; returns 0 (v untouched) for any other name. */
int tn_preset(const char *id, tn_vals *v);

/* One step of a run id "a_b_c": the first step is tn_preset (defaults plus a's change); every later step applies
 * only its preset's change on top of *v ("default" restores all the defaults). So "txpump_delack0" sets both, and
 * "delack0_default" ends at the defaults. Returns 0 (v untouched) for an unknown name. */
int tn_preset_step(const char *id, int first, tn_vals *v);

/* The steps of a run id are joined by '_' (MoofAgent refuses '+' in run ids); '+' is still accepted (Snow runs).
 * Returns the length of tok's first step; *rest is the text after its separator, or NULL when it is the last. */
unsigned tn_step_len(const char *tok, const char **rest);

/* MoofTCPTune.cfg: lines "key=value" with key rto_min, rto_init, delack, stall, quickack or txpump, value a decimal
 * number; CR, LF or CRLF end a line; blank lines and lines starting with '#' are skipped; spaces around the
 * key and value are ignored. Keys the text lacks keep their value in *v. Returns the number of keys applied,
 * or -1 with a short message in err (unknown key, bad number, no '='). */
int tn_parse_cfg(const char *text, long len, tn_vals *v, char *err, int errcap);

#endif
