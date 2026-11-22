/* Unit tests for the command-line and config parsing and the worker channel. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "cache.h"

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])) - 1)

static int parse(int argc, char** argv, srv_conf* conf)
{
    memset(conf, 0, sizeof(*conf));
    return parse_console_parameters(argc, argv, conf);
}

static void test_defaults(void** state)
{
    (void)state;
    srv_conf conf;
    char* argv[] = {"cache-server", NULL};
    assert_int_equal(parse(ARGC(argv), argv, &conf), PARSE_RUN);
    assert_int_equal(conf.port, 8080);
    assert_string_equal(conf.ip, "0.0.0.0");
    assert_int_equal(conf.workers, 4);
}

static void test_short_and_long_options(void** state)
{
    (void)state;
    srv_conf conf;
    char* a1[] = {"cache-server", "-p", "1234", "-i", "127.0.0.1", "-w", "2", NULL};
    assert_int_equal(parse(ARGC(a1), a1, &conf), PARSE_RUN);
    assert_int_equal(conf.port, 1234);
    assert_string_equal(conf.ip, "127.0.0.1");
    assert_int_equal(conf.workers, 2);

    char* a2[] = {"cache-server", "--port=65535", "--ip", "10.1.2.3", "--workers=16", NULL};
    assert_int_equal(parse(ARGC(a2), a2, &conf), PARSE_RUN);
    assert_int_equal(conf.port, 65535);
    assert_string_equal(conf.ip, "10.1.2.3");
    assert_int_equal(conf.workers, 16);
}

/* Regression: the port was stored in uint16_t before the range check, so it never failed. */
static void test_invalid_values(void** state)
{
    (void)state;
    srv_conf conf;
    const char* bad[][2] = {
        {"-p", "70000"}, {"-p", "0"},         {"-p", "12ab"},      {"-p", ""},
        {"-i", "1.2.3"}, {"-i", "999.1.1.1"}, {"-i", "1.2.3.4.5"}, {"-i", "localhost"},
        {"-w", "0"},     {"-w", "17"},        {"-w", "300"},       {"-w", "abc"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char* argv[] = {"cache-server", (char*)bad[i][0], (char*)bad[i][1], NULL};
        assert_int_equal(parse(ARGC(argv), argv, &conf), PARSE_ERROR);
    }
    char* extra[] = {"cache-server", "stray", NULL};
    assert_int_equal(parse(ARGC(extra), extra, &conf), PARSE_ERROR);
    char* unknown[] = {"cache-server", "-x", NULL};
    assert_int_equal(parse(ARGC(unknown), unknown, &conf), PARSE_ERROR);
}

static void test_version_and_help(void** state)
{
    (void)state;
    srv_conf conf;
    char* v[] = {"cache-server", "-v", NULL};
    assert_int_equal(parse(ARGC(v), v, &conf), PARSE_EXIT);
    char* version[] = {"cache-server", "--version", NULL};
    assert_int_equal(parse(ARGC(version), version, &conf), PARSE_EXIT);
    char* help[] = {"cache-server", "--help", NULL};
    assert_int_equal(parse(ARGC(help), help, &conf), PARSE_EXIT);
}

/* A task message carries a descriptor; end of file stops the worker. */
static void test_worker_channel(void** state)
{
    (void)state;
    int sv[2], pipefd[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    assert_int_equal(pipe(pipefd), 0);
    chw_t master = {.pid = 0, .fd = sv[0]};
    chw_t worker = {.pid = 0, .fd = sv[1]};

    assert_true(child_worker_send(&master, CHWMSG_TASK, pipefd[1]) > 0);
    close(pipefd[1]);
    chwmsg_enum msg = CHWMSG_NONE;
    int fd = -1;
    assert_true(child_worker_recv(&worker, &msg, &fd) > 0);
    assert_int_equal(msg, CHWMSG_TASK);
    assert_true(fd >= 0);
    assert_int_equal(write(fd, "x", 1), 1);
    close(fd);
    char c = 0;
    assert_int_equal(read(pipefd[0], &c, 1), 1);
    assert_int_equal(c, 'x');
    close(pipefd[0]);

    /* Regression: end of file left msg uninitialized and the worker served fd -1 */
    child_worker_free(&master);
    assert_int_equal(master.fd, -1);
    msg = CHWMSG_TASK;
    fd = 0;
    assert_int_equal(child_worker_recv(&worker, &msg, &fd), -1);
    assert_int_equal(msg, CHWMSG_NONE);
    assert_int_equal(fd, -1);
    child_worker_free(&worker);
    child_worker_free(&worker); /* closing twice is harmless */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_defaults),       cmocka_unit_test(test_short_and_long_options),
        cmocka_unit_test(test_invalid_values), cmocka_unit_test(test_version_and_help),
        cmocka_unit_test(test_worker_channel),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
