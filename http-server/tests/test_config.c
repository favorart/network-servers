/**
 * @file test_config.c
 * @brief Unit tests for the config, the command line and the master-worker protocol.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http_server.h"

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

static char dir[64];

static int group_setup(void** state)
{
    (void)state;
    strcpy(dir, "/tmp/http-config-test-XXXXXX");
    return mkdtemp(dir) ? 0 : -1;
}
static int group_teardown(void** state)
{
    (void)state;
    return rmdir(dir);
}
//-----------------------------------------
static void test_defaults(void** state)
{
    (void)state;
    srv_conf conf;
    memset(&conf, 0, sizeof(conf));
    assert_int_equal(http_server_config_init(&conf, NULL, NULL, NULL, NULL), 0);
    assert_int_equal(conf.port, 8080);
    assert_string_equal(conf.folder_path, "data/");
    assert_string_equal(conf.ip, "0.0.0.0");
    assert_int_equal(conf.workers, 4);
    assert_null(conf.myself);
    assert_null(conf.child_workers);
    assert_string_equal(conf.ptr_server_name, HTTP_SERVER_NAME);
}
static void test_valid_values(void** state)
{
    (void)state;
    srv_conf conf;
    memset(&conf, 0, sizeof(conf));
    http_server_config_init(&conf, dir, "65535", "127.0.0.1", "16");
    assert_int_equal(conf.port, 65535);
    assert_string_equal(conf.folder_path, dir);
    assert_string_equal(conf.ip, "127.0.0.1");
    assert_int_equal(conf.workers, 16);
    http_server_config_init(&conf, dir, "1", "10.0.0.1", "1");
    assert_int_equal(conf.port, 1);
    assert_int_equal(conf.workers, 1);
}
/** Regression: the range was checked after the truncation to uint16_t. */
static void test_bad_port(void** state)
{
    (void)state;
    const char* ports[] = {"70000", "65536", "0", "-1", "80x", "", " 80", "99999999999999999999"};
    for (size_t i = 0; i < sizeof(ports) / sizeof(*ports); ++i) {
        srv_conf conf;
        memset(&conf, 0, sizeof(conf));
        http_server_config_init(&conf, dir, ports[i], "127.0.0.1", "2");
        assert_int_equal(conf.port, 8080);
    }
}
/** Regression: workers=0 was accepted (rand() % 0), 260 became 4 after truncation. */
static void test_bad_workers(void** state)
{
    (void)state;
    const char* workers[] = {"0", "17", "260", "-3", "two"};
    for (size_t i = 0; i < sizeof(workers) / sizeof(*workers); ++i) {
        srv_conf conf;
        memset(&conf, 0, sizeof(conf));
        http_server_config_init(&conf, dir, "8000", "127.0.0.1", workers[i]);
        assert_int_equal(conf.workers, 4);
    }
}
static void test_bad_ip_and_dir(void** state)
{
    (void)state;
    srv_conf conf;
    memset(&conf, 0, sizeof(conf));
    http_server_config_init(&conf, "/nonexistent/dir", "8000", "1.2.3.4.5", "2");
    assert_string_equal(conf.ip, "0.0.0.0");
    assert_string_equal(conf.folder_path, "data/");
    http_server_config_init(&conf, "/etc/hostname", "8000", "localhost", "2");
    assert_string_equal(conf.ip, "0.0.0.0");
    assert_string_equal(conf.folder_path, "data/");

    char* long_path = malloc(FILENAME_MAX + 16);
    assert_non_null(long_path);
    memset(long_path, 'a', FILENAME_MAX + 15);
    long_path[FILENAME_MAX + 15] = '\0';
    http_server_config_init(&conf, long_path, "8000", "127.0.0.1", "2");
    assert_string_equal(conf.folder_path, "data/");
    free(long_path);
}
//-----------------------------------------
/** @brief Run parse_console_parameters() on a NULL-terminated argument list. */
static void parse_args(srv_conf* conf, char** args)
{
    int argc = 0;
    while (args[argc])
        ++argc;
    optind = 0; /* restart getopt */
    memset(conf, 0, sizeof(*conf));
    assert_int_equal(parse_console_parameters(argc, args, conf), 0);
}
/** Regression: "w:" was missing from the short options, so -w was rejected. */
static void test_short_options(void** state)
{
    (void)state;
    srv_conf conf;
    char* args[] = {"http-server", "-d", dir, "-h", "127.0.0.1", "-p", "9000", "-w", "3", NULL};
    parse_args(&conf, args);
    assert_string_equal(conf.folder_path, dir);
    assert_string_equal(conf.ip, "127.0.0.1");
    assert_int_equal(conf.port, 9000);
    assert_int_equal(conf.workers, 3);
}
static void test_long_options(void** state)
{
    (void)state;
    srv_conf conf;
    char* args[] = {"http-server", "--dir", dir,         "--ip", "127.0.0.2",
                    "--port",      "9001",  "--workers", "2",    NULL};
    parse_args(&conf, args);
    assert_string_equal(conf.folder_path, dir);
    assert_string_equal(conf.ip, "127.0.0.2");
    assert_int_equal(conf.port, 9001);
    assert_int_equal(conf.workers, 2);
}
static void test_version(void** state)
{
    (void)state;
    const char* flags[] = {"-v", "--version"};
    for (size_t i = 0; i < 2; ++i) {
        int out[2];
        assert_int_equal(pipe(out), 0);
        fflush(stdout);
        pid_t pid = fork();
        assert_true(pid >= 0);
        if (pid == 0) {
            dup2(out[1], STDOUT_FILENO);
            close(out[0]);
            char* args[] = {"http-server", (char*)flags[i], NULL};
            optind = 0;
            srv_conf conf;
            memset(&conf, 0, sizeof(conf));
            parse_console_parameters(2, args, &conf);
            _exit(42); /* not reached: -v exits */
        }
        close(out[1]);
        char text[128] = {0};
        size_t n = 0;
        ssize_t r;
        while (n < sizeof(text) - 1 && (r = read(out[0], text + n, sizeof(text) - 1 - n)) > 0)
            n += (size_t)r;
        close(out[0]);
        int status = 0;
        assert_int_equal(waitpid(pid, &status, 0), pid);
        assert_true(WIFEXITED(status));
        assert_int_equal(WEXITSTATUS(status), 0);
        assert_string_equal(text, HTTP_SERVER_NAME " " NS_VERSION "\n");
    }
}
//-----------------------------------------
/** @brief The two ends of a master-worker channel. */
struct channel
{
    chw_t master, worker;
};
static int channel_setup(void** state)
{
    struct channel* c = calloc(1, sizeof(*c));
    int sv[2];
    if (!c || socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) != 0)
        return -1;
    c->master.fd = sv[0];
    c->worker.fd = sv[1];
    *state = c;
    return 0;
}
static int channel_teardown(void** state)
{
    struct channel* c = *state;
    child_worker_free(&c->master);
    child_worker_free(&c->worker);
    free(c);
    return 0;
}
/** @brief Check that @p fd is the read end of a pipe whose write end is @p wr. */
static void assert_pipe_pair(int fd, int wr)
{
    assert_int_equal(write(wr, "x", 1), 1);
    char ch = 0;
    assert_int_equal(read(fd, &ch, 1), 1);
    assert_int_equal(ch, 'x');
}
static void test_pass_fd(void** state)
{
    struct channel* c = *state;
    int p[2];
    assert_int_equal(pipe(p), 0);
    assert_true(child_worker_send(&c->master, CHWMSG_TASK, p[0]) > 0);
    /* the sender closes its copy */
    assert_int_equal(fcntl(p[0], F_GETFD), -1);

    chwmsg_enum msg = CHWMSG_NONE;
    int fd = -1;
    assert_int_equal(child_worker_recv(&c->worker, &msg, &fd), CHWMSG_LENGTH);
    assert_int_equal(msg, CHWMSG_TASK);
    assert_true(fd >= 0);
    assert_pipe_pair(fd, p[1]);
    close(fd);
    close(p[1]);
}
/** Regression: commands glued in the stream ("tasktask") were not recognized. */
static void test_glued_commands(void** state)
{
    struct channel* c = *state;
    int p1[2], p2[2];
    assert_int_equal(pipe(p1), 0);
    assert_int_equal(pipe(p2), 0);
    assert_true(child_worker_send(&c->master, CHWMSG_TASK, p1[0]) > 0);
    assert_true(child_worker_send(&c->master, CHWMSG_TASK, p2[0]) > 0);

    int w[2] = {p1[1], p2[1]};
    for (int i = 0; i < 2; ++i) {
        chwmsg_enum msg = CHWMSG_NONE;
        int fd = -1;
        assert_int_equal(child_worker_recv(&c->worker, &msg, &fd), CHWMSG_LENGTH);
        assert_int_equal(msg, CHWMSG_TASK);
        assert_true(fd >= 0);
        assert_pipe_pair(fd, w[i]);
        close(fd);
        close(w[i]);
    }
}
/** Regression: msg stayed uninitialized when the master had gone (end of stream). */
static void test_end_of_stream(void** state)
{
    struct channel* c = *state;
    child_worker_free(&c->master);
    chwmsg_enum msg = CHWMSG_TASK;
    int fd = 123;
    assert_int_equal(child_worker_recv(&c->worker, &msg, &fd), -1);
    assert_int_equal(msg, CHWMSG_NONE);
}
static void test_unknown_command(void** state)
{
    struct channel* c = *state;
    assert_int_equal(write(c->master.fd, "abcd", 4), 4);
    chwmsg_enum msg = CHWMSG_TASK;
    int fd = 123;
    assert_int_equal(child_worker_recv(&c->worker, &msg, &fd), 4);
    assert_int_equal(msg, CHWMSG_NONE);
    assert_int_equal(fd, -1);

    int p[2];
    assert_int_equal(pipe(p), 0);
    assert_int_equal(child_worker_send(&c->master, CHWMSG_NONE, p[0]), 1);
    close(p[0]);
    close(p[1]);
}
//-----------------------------------------
int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_defaults),
        cmocka_unit_test(test_valid_values),
        cmocka_unit_test(test_bad_port),
        cmocka_unit_test(test_bad_workers),
        cmocka_unit_test(test_bad_ip_and_dir),
        cmocka_unit_test(test_short_options),
        cmocka_unit_test(test_long_options),
        cmocka_unit_test(test_version),
        cmocka_unit_test_setup_teardown(test_pass_fd, channel_setup, channel_teardown),
        cmocka_unit_test_setup_teardown(test_glued_commands, channel_setup, channel_teardown),
        cmocka_unit_test_setup_teardown(test_end_of_stream, channel_setup, channel_teardown),
        cmocka_unit_test_setup_teardown(test_unknown_command, channel_setup, channel_teardown),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
