/* Unit tests for the command parser and handler (cache_handling). */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <event2/buffer.h>

#include "cache_hash.h"
#include "cache.h"

struct fixture
{
    hashtable ht;
    struct evbuffer* in;
    struct evbuffer* out;
};

static int setup(void** state)
{
    struct fixture* f = calloc(1, sizeof(*f));
    if (!f)
        return -1;
    hashtable_init(&f->ht, 16U, 0U);
    f->in = evbuffer_new();
    f->out = evbuffer_new();
    *state = f;
    return (f->in && f->out) ? 0 : -1;
}
static int teardown(void** state)
{
    struct fixture* f = *state;
    evbuffer_free(f->in);
    evbuffer_free(f->out);
    hashtable_free(&f->ht);
    free(f);
    return 0;
}

/* Feed the input, run the handler and compare everything it replied */
static void run(struct fixture* f, const char* input, const char* expected)
{
    evbuffer_add(f->in, input, strlen(input));
    cache_handling(&f->ht, f->in, f->out);
    size_t len = evbuffer_get_length(f->out);
    char* reply = calloc(1, len + 1U);
    assert_non_null(reply);
    evbuffer_remove(f->out, reply, len);
    assert_string_equal(reply, expected);
    free(reply);
}

static void test_set_and_get(void** state)
{
    struct fixture* f = *state;
    run(f, "set 100 1 2\n", "ok 1 2\n");
    run(f, "get 1\n", "ok 1 2\n");
    run(f, "set 100 -5 -6\r\nget -5\r\n", "ok -5 -6\nok -5 -6\n");
}

static void test_missing_key(void** state)
{
    struct fixture* f = *state;
    run(f, "get 77\n", "error : no such key in table\n");
    run(f, "set -1 8 9\nget 8\n", "ok 8 9\nerror : no such key in table\n");
}

static void test_bad_commands(void** state)
{
    struct fixture* f = *state;
    const char* bad = "error : incorrect command to server\n";
    run(f, "\n", bad);
    run(f, "foo 1\n", bad);
    run(f, "get\n", bad);
    run(f, "get x\n", bad);
    run(f, "set 1 2\n", bad);
    run(f, "set a b c\n", bad);
    /* longer words: only three characters go into the 4-byte buffer (it was 3 bytes) */
    run(f, "setter 1 2 3\n", bad);
    run(f, "gotcha_a_very_long_command_word 1\n", bad);
}

/* Regression: one error made every later reply in the same read an error. */
static void test_error_is_not_sticky(void** state)
{
    struct fixture* f = *state;
    run(f, "get 999\nset 100 1 2\nfoo\nget 1\n",
        "error : no such key in table\nok 1 2\nerror : incorrect command to server\nok 1 2\n");
}

/* Regression: a command split between two reads was dropped. */
static void test_split_command(void** state)
{
    struct fixture* f = *state;
    run(f, "se", "");
    run(f, "t 100 5 6\nget", "ok 5 6\n");
    run(f, " 5\n", "ok 5 6\n");
}

static void test_too_long_line_dropped(void** state)
{
    struct fixture* f = *state;
    char line[CACHE_MAX_LINE + 2U];
    memset(line, '1', sizeof(line) - 1U);
    line[sizeof(line) - 1U] = '\0';
    run(f, line, "error : command too long\n");
    assert_int_equal(evbuffer_get_length(f->in), 0);
    run(f, "set 100 1 1\n", "ok 1 1\n");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_set_and_get, setup, teardown),
        cmocka_unit_test_setup_teardown(test_missing_key, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bad_commands, setup, teardown),
        cmocka_unit_test_setup_teardown(test_error_is_not_sticky, setup, teardown),
        cmocka_unit_test_setup_teardown(test_split_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_too_long_line_dropped, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
