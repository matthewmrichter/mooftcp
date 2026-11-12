#include "t.h"
#include "mt_int.h"
#include "tune_parse.h"

/* MoofTCPTune's presets and MoofTCPTune.cfg parser (all configurations; the levers themselves are test_tune.c). */

static void test_defaults_match_core(void) {
    tn_vals v;
    tn_defaults(&v);
    T_EQ(v.rto_min, MT_TCP_RTO_MIN); T_EQ(v.rto_init, MT_TCP_RTO_INIT); T_EQ(v.delack, MT_TCP_DELACK_MS);
    T_EQ(v.stall, MT_TCP_STALL_MS); T_EQ(v.quickack, MT_TCP_QUICKACK);
}

static void test_presets(void) {
    tn_vals v, d;
    tn_defaults(&d);
    T_CHECK(tn_preset("default", &v)); T_MEMEQ(&v, &d, sizeof v);
    T_CHECK(tn_preset("delack0", &v)); T_EQ(v.delack, 0); T_EQ(v.stall, 10); T_EQ(v.quickack, 4);
    T_CHECK(tn_preset("delack20", &v)); T_EQ(v.delack, 20);
    T_CHECK(tn_preset("delack50", &v)); T_EQ(v.delack, 50);
    T_CHECK(tn_preset("qack16", &v)); T_EQ(v.quickack, 16); T_EQ(v.delack, 100);
    T_CHECK(tn_preset("qackall", &v)); T_EQ(v.quickack, 255);
    T_CHECK(tn_preset("rto500", &v)); T_EQ(v.rto_min, 500); T_EQ(v.rto_init, 500);
    T_CHECK(tn_preset("rto700", &v)); T_EQ(v.rto_min, 700); T_EQ(v.rto_init, 700);
    T_CHECK(tn_preset("stall2", &v)); T_EQ(v.stall, 2); T_EQ(v.rto_min, 1000);
    T_CHECK(tn_preset("stall5", &v)); T_EQ(v.stall, 5); T_EQ(v.txpump, 0);
    T_CHECK(tn_preset("txpump", &v)); T_EQ(v.txpump, 1); T_EQ(v.delack, 100); T_EQ(v.stall, 10);
    T_CHECK(tn_preset("delack0", &v)); T_EQ(v.txpump, 0);    /* a run never inherits an earlier preset */
    v.delack = 77;
    T_CHECK(!tn_preset("nosuch", &v)); T_EQ(v.delack, 77);      /* untouched */
    T_CHECK(!tn_preset("", &v));
}

/* "a+b": the later steps add their change ("txpump+delack0" = both); "default" restores everything. */
static void test_preset_chain(void) {
    tn_vals v, d;
    tn_defaults(&d);
    v = d; v.delack = 7; v.txpump = 1;
    T_CHECK(tn_preset_step("delack0", 1, &v)); T_EQ(v.delack, 0); T_EQ(v.txpump, 0);   /* first step: defaults + change */
    T_CHECK(tn_preset_step("txpump", 1, &v)); T_EQ(v.txpump, 1);
    T_CHECK(tn_preset_step("delack0", 0, &v)); T_EQ(v.txpump, 1); T_EQ(v.delack, 0); T_EQ(v.quickack, 4);
    T_CHECK(tn_preset_step("default", 0, &v)); T_MEMEQ(&v, &d, sizeof v);
    T_CHECK(tn_preset_step("delack0", 1, &v));
    T_CHECK(tn_preset_step("txpump", 0, &v)); T_EQ(v.txpump, 1); T_EQ(v.delack, 0);
    T_CHECK(!tn_preset_step("nosuch", 0, &v)); T_EQ(v.txpump, 1); T_EQ(v.delack, 0);   /* untouched */
}

static void test_cfg_parse(void) {
    tn_vals v;
    char err[40];
    const char *t = "# sweep\r\ndelack = 20\r\n\r\n  quickack=16 \nrto_min=500\rrto_init=800\nstall=3";
    tn_defaults(&v);
    T_EQ(tn_parse_cfg(t, (long)strlen(t), &v, err, sizeof err), 5);
    T_EQ(v.delack, 20); T_EQ(v.quickack, 16); T_EQ(v.rto_min, 500); T_EQ(v.rto_init, 800); T_EQ(v.stall, 3);
    tn_defaults(&v);                                            /* missing keys keep their value */
    T_EQ(tn_parse_cfg("stall=7\r", 8, &v, err, sizeof err), 1);
    T_EQ(v.stall, 7); T_EQ(v.delack, 100);
    T_EQ(tn_parse_cfg("txpump=1\n", 9, &v, err, sizeof err), 1);
    T_EQ(v.txpump, 1);
    T_EQ(tn_parse_cfg("", 0, &v, err, sizeof err), 0);
}

static void test_cfg_errors(void) {
    tn_vals v;
    char err[40];
    tn_defaults(&v);
    T_EQ(tn_parse_cfg("bogus=1\r", 8, &v, err, sizeof err), -1); T_CHECK(strstr(err, "bogus") != NULL);
    T_EQ(tn_parse_cfg("delack\r", 7, &v, err, sizeof err), -1);
    T_EQ(tn_parse_cfg("delack=x\r", 9, &v, err, sizeof err), -1);
    T_EQ(tn_parse_cfg("delack=\r", 8, &v, err, sizeof err), -1);
    T_EQ(tn_parse_cfg("delack=70000\r", 13, &v, err, sizeof err), -1);
    T_EQ(tn_parse_cfg("delack=-1\r", 10, &v, err, sizeof err), -1);
}

T_MAIN(test_defaults_match_core, test_presets, test_preset_chain, test_cfg_parse, test_cfg_errors)
