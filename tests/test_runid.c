/* copied from moofagent 6ca9126; change there first */
/* test_runid.c - runid_parse (mac/common/runid_parse.c): the run file's text format. */
#include "t.h"
#include "runid_parse.h"

static int parse(const char *s, char *id, int cap, int *un)
{
    return runid_parse(s, (long)strlen(s), id, cap, un);
}

static void test_double_bang_is_unattended(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("!!r1\r", id, sizeof id, &un), 1);
    T_CHECK(strcmp(id, "r1") == 0);
    T_EQ(un, 1);
}

static void test_single_bang_is_quiet_only(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("!r2\n", id, sizeof id, &un), 1);
    T_CHECK(strcmp(id, "r2") == 0);
    T_EQ(un, 0);
}

static void test_no_bang_means_ask(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("abc", id, sizeof id, &un), 0);
    T_EQ(un, 0);
}

static void test_empty_means_ask(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("", id, sizeof id, &un), 0);
    T_EQ(runid_parse(0, 0, id, sizeof id, &un), 0);
    T_EQ(parse("!!\r", id, sizeof id, &un), 0);
}

static void test_too_long_is_refused(void)
{
    char id[8];
    int un = -1;
    T_EQ(parse("!!1234567", id, sizeof id, &un), 1);   /* 7 chars fit in cap 8 */
    T_CHECK(strcmp(id, "1234567") == 0);
    T_EQ(parse("!!12345678", id, sizeof id, &un), 0);  /* 8 do not */
    T_EQ(un, 0);
}

static void test_crlf_and_spaces_stripped(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("!!run 7  \r\nsecond line\r\n", id, sizeof id, &un), 1);
    T_CHECK(strcmp(id, "run 7") == 0);
    T_EQ(un, 1);
    T_EQ(parse("!!  \r\n", id, sizeof id, &un), 0);
}

/* A '!'-prefixed file with nothing usable returns 0 (mac/runid.c then runs quiet, no id). */
static void test_bang_only_files_return_zero(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("!", id, sizeof id, &un), 0);
    T_EQ(un, 0);
    T_EQ(parse("!!", id, sizeof id, &un), 0);
    T_EQ(un, 0);
    T_EQ(parse("!\r", id, sizeof id, &un), 0);
}

/* Only two bangs count; a third is part of the id. */
static void test_third_bang_is_part_of_id(void)
{
    char id[32];
    int un = -1;
    T_EQ(parse("!!!x", id, sizeof id, &un), 1);
    T_CHECK(strcmp(id, "!x") == 0);
    T_EQ(un, 1);
}

T_MAIN(test_double_bang_is_unattended, test_single_bang_is_quiet_only, test_no_bang_means_ask,
       test_empty_means_ask, test_too_long_is_refused, test_crlf_and_spaces_stripped,
       test_bang_only_files_return_zero, test_third_bang_is_part_of_id)
