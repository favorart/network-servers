/**
 * @file test_config.c
 * @brief Unit tests: config file parsing, command line options, backend selection.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include <cmocka.h>

#include "proxy_error.h"
#include "proxy.h"

/** @brief Per-test temporary directory, also the current directory during the test. */
struct fixture
{
    char dir[64];
    char path[128];
    char old_cwd[FILENAME_MAX];
    srv_conf conf;
};

static int setup(void** state)
{
    struct fixture* f = calloc(1, sizeof(*f));
    assert_non_null(f);
    strcpy(f->dir, "/tmp/proxy-config-XXXXXX");
    assert_non_null(mkdtemp(f->dir));
    snprintf(f->path, sizeof(f->path), "%s/proxy.conf", f->dir);
    assert_non_null(getcwd(f->old_cwd, sizeof(f->old_cwd)));
    /* No config.txt fallback from the source tree */
    assert_int_equal(chdir(f->dir), 0);
    *state = f;
    return 0;
}

static int teardown(void** state)
{
    struct fixture* f = *state;
    server_config_free(&f->conf);
    unlink(f->path);
    unlink(SRV_CONFDEFNAME);
    assert_int_equal(chdir(f->old_cwd), 0);
    rmdir(f->dir);
    free(f);
    return 0;
}

static void write_file(const char* path, const char* text)
{
    FILE* fp = fopen(path, "w");
    assert_non_null(fp);
    fputs(text, fp);
    fclose(fp);
}

/** @brief Write @p text to the fixture config and parse it. */
static int parse_text(struct fixture* f, const char* text)
{
    write_file(f->path, text);
    return server_config_init(&f->conf, f->path);
}

static void test_config_valid(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(parse_text(f, "8080,127.0.0.1:9001, 10.0.0.2:9002\n"), 0);
    assert_int_equal(f->conf.port, 8080);
    assert_int_equal(f->conf.remote_servers_count, 2);
    assert_string_equal(f->conf.remote_servers[0].ip, "127.0.0.1");
    assert_int_equal(f->conf.remote_servers[0].port, 9001);
    assert_string_equal(f->conf.remote_servers[1].ip, "10.0.0.2");
    assert_int_equal(f->conf.remote_servers[1].port, 9002);
    assert_string_equal(f->conf.config_path, f->path);
}

/* Regression: port 65535 was rejected */
static void test_config_port_65535(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(parse_text(f, "65535,127.0.0.1:65535"), 0);
    assert_int_equal(f->conf.port, 65535);
    assert_int_equal(f->conf.remote_servers_count, 1);
    assert_int_equal(f->conf.remote_servers[0].port, 65535);
}

/* A bad listening port falls back to 8080 (kept behavior); out of range no longer wraps */
static void test_config_bad_port_fallback(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(parse_text(f, "70000,127.0.0.1:1"), 0);
    assert_int_equal(f->conf.port, 8080);
    server_config_free(&f->conf);
    assert_int_equal(parse_text(f, "0,127.0.0.1:1"), 0);
    assert_int_equal(f->conf.port, 8080);
}

/* Parsing stops at the first bad remote server; octets > 255 overflowed the ip buffer */
static void test_config_bad_remote(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(parse_text(f, "8080,127.0.0.1:1,300.1.1.1:2,127.0.0.1:3"), 0);
    assert_int_equal(f->conf.remote_servers_count, 1);
    server_config_free(&f->conf);
    assert_int_equal(parse_text(f, "8080,127.0.0.1:1,127.0.0.1:70000"), 0);
    assert_int_equal(f->conf.remote_servers_count, 1);
    server_config_free(&f->conf);
    assert_int_not_equal(parse_text(f, "8080,65535.65535.65535.65535:1"), 0);
}

/* Regression: no remote servers led to `% 0` on the first client */
static void test_config_no_servers(void** state)
{
    struct fixture* f = *state;
    assert_int_not_equal(parse_text(f, "8080\n"), 0);
    assert_int_equal(f->conf.remote_servers_count, 0);
    assert_null(f->conf.remote_servers);
    assert_int_not_equal(parse_text(f, "8080,garbage"), 0);
}

/* Regression: a missing config crashed in fclose() of an unopened file */
static void test_config_missing_file(void** state)
{
    struct fixture* f = *state;
    assert_int_not_equal(server_config_init(&f->conf, "/nonexistent/proxy.conf"), 0);
    assert_int_not_equal(server_config_init(&f->conf, NULL), 0);
}

/* Without -c the config is read from SRV_CONFDEFNAME in the current directory */
static void test_config_default_name(void** state)
{
    struct fixture* f = *state;
    write_file(SRV_CONFDEFNAME, "9000,127.0.0.1:9001");
    assert_int_equal(server_config_init(&f->conf, NULL), 0);
    assert_string_equal(f->conf.config_path, SRV_CONFDEFNAME);
    assert_int_equal(f->conf.port, 9000);
}

/** @brief Run parse_console_parameters() on a NULL-terminated argument list. */
static int parse_args(srv_conf* conf, ...)
{
    char* argv[8] = {"proxy-server"};
    int argc = 1;
    va_list ap;
    va_start(ap, conf);
    for (char* a; argc < 7 && (a = va_arg(ap, char*));)
        argv[argc++] = a;
    va_end(ap);
    optind = 0; /* restart getopt */
    return parse_console_parameters(argc, argv, conf);
}

static void test_args_conf(void** state)
{
    struct fixture* f = *state;
    write_file(f->path, "8081,127.0.0.1:9001");
    assert_int_equal(parse_args(&f->conf, "-c", f->path, NULL), 0);
    assert_int_equal(f->conf.port, 8081);
    server_config_free(&f->conf);
    assert_int_equal(parse_args(&f->conf, "--conf", f->path, NULL), 0);
    assert_string_equal(f->conf.config_path, f->path);
}

static void test_args_version(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(parse_args(&f->conf, "-v", NULL), SRV_PARSE_VERSION);
    assert_int_equal(parse_args(&f->conf, "--version", NULL), SRV_PARSE_VERSION);
    assert_int_equal(f->conf.remote_servers_count, 0);
}

static void test_choose_server(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(proxy_choose_server(&f->conf), -1);

    assert_int_equal(parse_text(f, "8080,127.0.0.1:1,127.0.0.1:2,127.0.0.1:3"), 0);
    int hits[3] = {0};
    for (int i = 0; i < 300; ++i) {
        int id = proxy_choose_server(&f->conf);
        assert_in_range(id, 0, 2);
        ++hits[id];
    }
    for (int i = 0; i < 3; ++i)
        assert_true(hits[i] > 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_config_valid, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_port_65535, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_bad_port_fallback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_bad_remote, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_no_servers, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_missing_file, setup, teardown),
        cmocka_unit_test_setup_teardown(test_config_default_name, setup, teardown),
        cmocka_unit_test_setup_teardown(test_args_conf, setup, teardown),
        cmocka_unit_test_setup_teardown(test_args_version, setup, teardown),
        cmocka_unit_test_setup_teardown(test_choose_server, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
