/** @file test_integration.c
 *  @brief Integration test: start the built server on a free port with a small dictionary,
 *         send real requests over TCP, check the replies and stop it with SIGTERM.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#ifndef SERVER_BIN
#define SERVER_BIN "build/dictionary-server"
#endif
#ifndef TEST_DICT
#define TEST_DICT "tests/dict_small.txt"
#endif
#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

/** @brief The running server. */
static struct
{
    pid_t pid;
    int port;
    char log[64];
} srv = {-1, 0, ""};

//-----------------------------------------
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
/** @brief Read the whole file @p path into @p buf (NUL-terminated). */
static void read_file(const char* path, char* buf, size_t size)
{
    buf[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
}
/** @brief Start the server with -p 0 and wait (at most 10 s) for its "listening on" line. */
static int start_server(void** state)
{
    (void)state;
    strcpy(srv.log, "/tmp/dict-srv-XXXXXX");
    int fd = mkstemp(srv.log);
    if (fd < 0)
        return -1;

    srv.pid = fork();
    if (srv.pid < 0)
        return -1;
    if (srv.pid == 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        execl(SERVER_BIN, SERVER_BIN, "-p", "0", "-i", "127.0.0.1", "-w", "3", "-d", TEST_DICT,
              (char*)NULL);
        _exit(127);
    }
    close(fd);

    static char text[1 << 16];
    for (int i = 0; i < 1000; ++i) {
        read_file(srv.log, text, sizeof(text));
        const char* line = strstr(text, "listening on 127.0.0.1:");
        if (line && strchr(line, '\n')) {
            srv.port = atoi(line + strlen("listening on 127.0.0.1:"));
            return srv.port > 0 ? 0 : -1;
        }
        if (waitpid(srv.pid, NULL, WNOHANG) == srv.pid) {
            fprintf(stderr, "server exited:\n%s\n", text);
            srv.pid = -1;
            return -1;
        }
        sleep_ms(10);
    }
    fprintf(stderr, "server did not start:\n%s\n", text);
    return -1;
}
/** @brief Wait (at most 10 s) for the server to exit; @return its wait status, -1 on timeout. */
static int wait_server(void)
{
    int status = 0;
    for (int i = 0; i < 1000; ++i) {
        if (waitpid(srv.pid, &status, WNOHANG) == srv.pid)
            return status;
        sleep_ms(10);
    }
    return -1;
}
static int stop_server(void** state)
{
    (void)state;
    if (srv.pid > 0) {
        kill(srv.pid, SIGKILL);
        waitpid(srv.pid, NULL, 0);
    }
    if (srv.log[0])
        unlink(srv.log);
    return 0;
}
//-----------------------------------------
/** @brief Send @p request, half-close and read the reply until EOF (timeout 5 s).
 *  @return The reply (caller frees), NULL on error. */
static char* query(const char* request)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return NULL;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)srv.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr))) {
        close(s);
        return NULL;
    }
    size_t len = strlen(request), sent = 0;
    while (sent < len) {
        ssize_t n = send(s, request + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            close(s);
            return NULL;
        }
        sent += (size_t)n;
    }
    shutdown(s, SHUT_WR);

    size_t cap = 4096, got = 0;
    char* reply = malloc(cap);
    while (reply) {
        struct pollfd pfd = {s, POLLIN, 0};
        if (poll(&pfd, 1, 5000) <= 0) {
            free(reply);
            reply = NULL;
            break;
        }
        if (got + 1 >= cap)
            reply = realloc(reply, cap *= 2);
        ssize_t n = recv(s, reply + got, cap - got - 1, 0);
        if (n <= 0) {
            reply[got] = '\0';
            break;
        }
        got += (size_t)n;
    }
    close(s);
    return reply;
}
static void assert_query(const char* request, const char* expected)
{
    char* reply = query(request);
    assert_non_null(reply);
    assert_string_equal(reply, expected);
    free(reply);
}
//-----------------------------------------
static void test_single_prefix(void** state)
{
    (void)state;
    assert_query("x1\n", "x12\nx10\nx11\n");
    assert_query("x1\n", "x12\nx10\nx11\n"); /* cached */
    assert_query("zzz\n", "");
}
static void test_top10(void** state)
{
    (void)state;
    assert_query("x\n", "x04\nx12\nx02\nx10\nx07\nx06\nx11\nx03\nx08\nx09\n");
}
static void test_multi_line_request(void** state)
{
    (void)state;
    assert_query("ap\r\nyes\nap\n", "apricot\napple\nyes\napricot\napple\n");
}
static void test_cyrillic(void** state)
{
    (void)state;
    /* "abaz" in Cyrillic -> abazin (92), abaz (86), abazinets (49) */
    assert_query("\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\n",
                 "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\xd0\xb8\xd0\xbd\n"
                 "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\n"
                 "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\xd0\xb8\xd0\xbd\xd0\xb5\xd1\x86\n");
}
static void* client_thread(void* arg)
{
    (void)arg;
    for (int i = 0; i < 20; ++i) {
        char* reply = query((i % 2) ? "x1\n" : "ap\n");
        const char* expected = (i % 2) ? "x12\nx10\nx11\n" : "apricot\napple\n";
        int ok = reply && !strcmp(reply, expected);
        free(reply);
        if (!ok)
            return (void*)1;
    }
    return NULL;
}
static void test_concurrent_clients(void** state)
{
    (void)state;
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i)
        assert_int_equal(pthread_create(&threads[i], NULL, client_thread, NULL), 0);
    for (int i = 0; i < 8; ++i) {
        void* ret = NULL;
        pthread_join(threads[i], &ret);
        assert_null(ret);
    }
}
/** @brief Last test: SIGTERM stops the server cleanly (exit code 0, workers joined). */
static void test_sigterm(void** state)
{
    (void)state;
    assert_int_equal(kill(srv.pid, SIGTERM), 0);
    int status = wait_server();
    assert_int_not_equal(status, -1);
    srv.pid = -1;
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
}
//-----------------------------------------
static void test_version(void** state)
{
    (void)state;
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        execl(SERVER_BIN, SERVER_BIN, "--version", (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    char out[64] = "";
    ssize_t n = read(fds[0], out, sizeof(out) - 1);
    close(fds[0]);
    out[n > 0 ? n : 0] = '\0';
    int status = 0;
    waitpid(pid, &status, 0);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    assert_string_equal(out, NS_VERSION "\n");
}
//-----------------------------------------
int main(void)
{
    const struct CMUnitTest server_tests[] = {
        cmocka_unit_test(test_single_prefix),      cmocka_unit_test(test_top10),
        cmocka_unit_test(test_multi_line_request), cmocka_unit_test(test_cyrillic),
        cmocka_unit_test(test_concurrent_clients), cmocka_unit_test(test_sigterm),
    };
    const struct CMUnitTest cli_tests[] = {
        cmocka_unit_test(test_version),
    };
    int failed = cmocka_run_group_tests_name("cli", cli_tests, NULL, NULL);
    failed += cmocka_run_group_tests_name("server", server_tests, start_server, stop_server);
    return failed;
}
