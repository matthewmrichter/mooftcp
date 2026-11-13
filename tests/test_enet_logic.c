/* test_enet_logic.c - the pure logic of the Ethernet/clock/gate glue (mac/glue/mt_enet_logic.h,
 * frame padding, tick-to-ms conversion. */
#include "t.h"
#include "mt_enet_logic.h"
#include "mt_timer.h"

static void pad_lengths(void)
{
    uint8_t b[64];
    int i;
    T_EQ(mt_enet_padded_len(0), 60);
    T_EQ(mt_enet_padded_len(54), 60);
    T_EQ(mt_enet_padded_len(59), 60);
    T_EQ(mt_enet_padded_len(60), 60);
    T_EQ(mt_enet_padded_len(61), 61);
    T_EQ(mt_enet_padded_len(1514), 1514);
    memset(b, 0xAA, sizeof b);
    T_EQ(mt_enet_pad(b, 54), 60);
    for (i = 0; i < 54; i++) T_EQ(b[i], 0xAA);
    for (i = 54; i < 60; i++) T_EQ(b[i], 0);
    T_EQ(b[60], 0xAA);                 /* nothing past 60 touched */
    memset(b, 0xAA, sizeof b);
    T_EQ(mt_enet_pad(b, 60), 60);
    T_EQ(b[59], 0xAA);
    T_CHECK(!mt_enet_len_ok(13));
    T_CHECK(mt_enet_len_ok(14));
    T_CHECK(mt_enet_len_ok(1514));
    T_CHECK(!mt_enet_len_ok(1515));
}

static void ms_from_ticks(void)
{
    uint32_t t, prev = 0, d;
    T_EQ(mt_ms_from_ticks(0), 0);
    T_EQ(mt_ms_from_ticks(1), 16);       /* 16.625 truncated */
    T_EQ(mt_ms_from_ticks(8), 133);      /* exactly 8 * 16.625 */
    T_EQ(mt_ms_from_ticks(60), 997);     /* 60 ticks = 997.5 ms */
    T_EQ(mt_ms_from_ticks(3600), 59850); /* a minute of 60 Hz ticks */
    /* the step per tick is 16 or 17 ms right up to the last tick before the 32-bit tick count
     * wraps (2.3 years of uptime: that one step is not 16/17, documented in mt_enet_logic.h) */
    for (t = 0xFFFFFF00u; t != 0xFFFFFFFFu; t++) {
        d = mt_ms_from_ticks(t + 1) - mt_ms_from_ticks(t);
        T_CHECK(d == 16 || d == 17);
    }
    for (t = 1u << 28; t < (1u << 28) + 4096; t++) {
        d = mt_ms_from_ticks(t) - prev;
        if (t != (1u << 28)) T_CHECK(d == 16 || d == 17);
        prev = mt_ms_from_ticks(t);
    }
    /* the sum of the steps matches the real rate to within one ms per 8 ticks over 10 days */
    T_EQ(mt_ms_from_ticks(8u * 1000000u) , 133000000u);
}

static void timer_deadline_helper(void)
{
    T_EQ(mt_timer_deadline_ms(MT_TIMER_NONE), MT_TIMER_MAX_MS);   /* never cancel */
    T_EQ(mt_timer_deadline_ms(0x80000000u), MT_TIMER_MAX_MS);
    T_EQ(mt_timer_deadline_ms(MT_TIMER_MAX_MS), MT_TIMER_MAX_MS);
    T_EQ(mt_timer_deadline_ms(MT_TIMER_MAX_MS - 1), MT_TIMER_MAX_MS - 1);
    T_EQ(mt_timer_deadline_ms(0), 17);                             /* due now: one clock step */
    T_EQ(mt_timer_deadline_ms(1), 17);
    T_EQ(mt_timer_deadline_ms(16), 17);
    T_EQ(mt_timer_deadline_ms(17), 17);
    T_EQ(mt_timer_deadline_ms(18), 18);
    T_EQ(mt_timer_deadline_ms(1000), 1000);
    /* the clock step is 16 or 17 ms, so 17 always lets the tick count advance */
    T_CHECK(MT_TIMER_STEP_MS >= 17);
}

/* 1.1d1 F5: a deadline re-arms the Time Manager task only when it is earlier than the pending shot. */
static void timer_rearm_only_when_earlier(void)
{
    MtTimer t;
    memset(&t, 0, sizeof t);
    T_CHECK(!mt_timer_keep(&t, 1100, 1000, 500));                  /* not armed (or fired: inside the callback) */
    t.armed = 1;
    T_CHECK(mt_timer_keep(&t, 1100, 1000, 500));                   /* later: the shot at 1100 stays */
    T_CHECK(mt_timer_keep(&t, 1100, 1000, 100));                   /* the same time: stays */
    T_CHECK(!mt_timer_keep(&t, 1100, 1000, 99));                   /* earlier: re-arm */
    T_CHECK(!mt_timer_keep(&t, 1100, 1000, 17));
    T_CHECK(mt_timer_keep(&t, 1100, 1000, MT_TIMER_MAX_MS));       /* no deadline: the longest shot */
    T_CHECK(mt_timer_keep(&t, 0x10u, 0xFFFFFFF0u, 0x20u));         /* across the 2^32 wrap */
    T_CHECK(!mt_timer_keep(&t, 0x10u, 0xFFFFFFF0u, 0x1Fu));
    t.in_cb = 1;                                                   /* armed again in the same callback */
    T_CHECK(mt_timer_keep(&t, 1100, 1000, 500));
    t.armed = 0;
    T_CHECK(!mt_timer_keep(&t, 1100, 1000, 500));                  /* fired: PrimeTime from the callback */
}

/* 1.1d2 fix: a pending shot overdue by more than MT_TIMER_LOST_MS is taken as lost (armed stays 1 but it never
 * fired): re-arm, whatever the new deadline. Within the slack it is still kept (the coarse clock may pass it a
 * little before the exact-ms shot fires). */
static void timer_overdue_shot_rearmed(void)
{
    MtTimer t;
    memset(&t, 0, sizeof t);
    t.armed = 1;
    T_CHECK(mt_timer_keep(&t, 1000, 1000, 500));                   /* due now: kept */
    T_CHECK(mt_timer_keep(&t, 1000 - MT_TIMER_LOST_MS, 1000, 500)); /* at the slack: kept */
    T_CHECK(!mt_timer_keep(&t, 1000 - MT_TIMER_LOST_MS - 1u, 1000, 500));   /* beyond: lost, re-arm */
    T_CHECK(!mt_timer_keep(&t, 1000 - 600000u, 1000, MT_TIMER_MAX_MS));     /* long lost, no deadline: re-arm */
    T_CHECK(!mt_timer_keep(&t, 0xFFFFFF00u, 0x100u, 500));         /* lost across the 2^32 wrap */
}

/* 1.1d2 fix 2: a lost shot is counted (with the clock time) when it is found, so MoofDiag can show it after the next
 * gate session has healed it; a live or a fired shot is not. */
static void timer_lost_shot_counted(void)
{
    MtTimer t;
    MtTimerShot sh;
    memset(&t, 0, sizeof t);
    memset(&sh, 0, sizeof sh);
    sh.due = 1000;
    T_CHECK(!mt_timer_note_lost(&t, &sh, 5000));                   /* not armed: fired, nothing lost */
    t.armed = 1;
    T_CHECK(!mt_timer_note_lost(&t, &sh, 1000 + MT_TIMER_LOST_MS)); /* within the slack */
    T_EQ(sh.lost, 0);
    T_CHECK(mt_timer_note_lost(&t, &sh, 1001 + MT_TIMER_LOST_MS));
    T_EQ(sh.lost, 1);
    T_EQ(sh.lost_at, 1001 + MT_TIMER_LOST_MS);
    T_CHECK(mt_timer_note_lost(&t, &sh, 9000));
    T_EQ(sh.lost, 2);
    T_EQ(sh.lost_at, 9000);
    T_CHECK(!mt_timer_keep(&t, sh.due, 9000, 500));                /* and the same shot is re-armed */
}

/* 1.1d2 fix: the Time Manager steps of mt_timer_arm (mt_timer_arm_steps), run against a model of the task: every arm,
 * outside the callback, once inside it and again inside it, leaves the task inserted and primed with armed = 1. */
static void timer_arm_steps_always_prime(void)
{
    MtTimer t;
    int ins = 0, primed = 0, k, seq;
    for (seq = 0; seq < 4; seq++) {
        memset(&t, 0, sizeof t);
        t.inserted = 1; ins = 1; primed = 0;                         /* mt_timer_init: InsTime */
        for (k = 0; k < 6; k++) {
            unsigned st;
            int in_cb = (seq & 1) ? (k >= 2) : (k % 2);              /* mixes of task-time and in-callback arms */
            if (k == 3 && (seq & 2)) { t.armed = 0; primed = 0; }    /* the shot fired in between */
            t.in_cb = (uint8_t)in_cb;
            st = mt_timer_arm_steps(&t);
            if (st & MT_TIMER_RMV) { T_CHECK(ins); ins = 0; primed = 0; t.inserted = 0; }
            if (st & MT_TIMER_INS) { T_CHECK(!ins); ins = 1; t.inserted = 1; }
            T_CHECK(ins);                                            /* PrimeTime needs an inserted task */
            if (primed) T_CHECK(0);                                  /* never PrimeTime an active task */
            primed = 1; t.armed = 1;
        }
    }
}

T_MAIN(pad_lengths, ms_from_ticks, timer_deadline_helper, timer_rearm_only_when_earlier, timer_overdue_shot_rearmed,
       timer_arm_steps_always_prime, timer_lost_shot_counted)
