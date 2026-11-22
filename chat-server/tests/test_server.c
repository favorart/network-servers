/**
 * @file test_server.c
 * @brief Integration tests: start the chat-server binary on a free port, read its
 *        log from a pipe and talk to it over TCP (the scenarios of the old test.py).
 *
 * The server binary path comes from the CHAT_SERVER environment variable.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#define LOG_SIZE 65536
#define WAIT_MS 2000

/** @brief A running server: process, port and the log read so far. */
struct server
{
    pid_t pid;
    int port;
    int log_fd;
    char log[LOG_SIZE];
    size_t log_len;
};

/** @brief Milliseconds of a monotonic clock. */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/** @brief Sleep for the milliseconds. */
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/** @brief Ask the kernel for a free TCP port on localhost. */
static int free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    assert_int_equal(bind(s, (struct sockaddr*)&addr, sizeof(addr)), 0);
    assert_int_equal(getsockname(s, (struct sockaddr*)&addr, &len), 0);
    close(s);
    return ntohs(addr.sin_port);
}

/** @brief Read the server log that arrives within the timeout. */
static void read_log(struct server* srv, int timeout_ms)
{
    struct pollfd pfd = {.fd = srv->log_fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return;
    ssize_t n;
    while ((n = read(srv->log_fd, srv->log + srv->log_len, LOG_SIZE - 1 - srv->log_len)) > 0) {
        srv->log_len += (size_t)n;
        srv->log[srv->log_len] = '\0';
    }
}

/** @brief Count the string in the log read so far. */
static int count_in_log(const struct server* srv, const char* what)
{
    int count = 0;
    for (const char* p = srv->log; (p = strstr(p, what)) != NULL; p += strlen(what))
        ++count;
    return count;
}

/** @brief Wait until the string occurs exactly @p n times in the log. */
static int wait_for_count(struct server* srv, const char* what, int n)
{
    long deadline = now_ms() + WAIT_MS;
    while (count_in_log(srv, what) != n) {
        long left = deadline - now_ms();
        if (left <= 0)
            return 0;
        read_log(srv, (int)left);
    }
    return 1;
}

/** @brief Count the string after the server had time to log everything pending. */
static int settled_count(struct server* srv, const char* what)
{
    long deadline = now_ms() + 50;
    long left;
    while ((left = deadline - now_ms()) > 0)
        read_log(srv, (int)left);
    return count_in_log(srv, what);
}

/** @brief Connect a client with a receive timeout; -1 if the connection is refused. */
static int try_connect(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((unsigned short)port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    struct timeval tv = {WAIT_MS / 1000, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return s;
}

/** @brief Connect a new client to the server. */
static int new_client(struct server* srv)
{
    int s = try_connect(srv->port);
    assert_true(s >= 0);
    return s;
}

/** @brief Send the whole string. */
static void send_str(int s, const char* str)
{
    assert_int_equal(send(s, str, strlen(str), MSG_NOSIGNAL), (ssize_t)strlen(str));
}

/** @brief Read one line (with '\n') into the buffer; fails on timeout. */
static void read_line(int s, char* buf, size_t size)
{
    size_t len = 0;
    while (len + 1 < size) {
        ssize_t n = recv(s, buf + len, 1, 0);
        assert_true(n == 1);
        if (buf[len++] == '\n')
            break;
    }
    buf[len] = '\0';
}

/** @brief Read one line and compare it with the expected one. */
static void expect_line(int s, const char* expected)
{
    char buf[2048];
    read_line(s, buf, sizeof(buf));
    assert_string_equal(buf, expected);
}

/** @brief Start the server on a free port and wait until it accepts connections. */
static int start_server(void** state)
{
    const char* bin = getenv("CHAT_SERVER");
    if (!bin)
        return -1;
    struct server* srv = calloc(1, sizeof(*srv));
    int fds[2];
    if (!srv || pipe(fds) != 0)
        return -1;
    srv->port = free_port();

    srv->pid = fork();
    if (srv->pid == 0) {
        char port[16];
        snprintf(port, sizeof(port), "%d", srv->port);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(bin, bin, "-p", port, (char*)NULL);
        _exit(127);
    }
    close(fds[1]);
    srv->log_fd = fds[0];
    fcntl(srv->log_fd, F_SETFL, O_NONBLOCK);
    *state = srv;

    /* the probe connection is logged too: count it out */
    long deadline = now_ms() + WAIT_MS;
    int probe;
    while ((probe = try_connect(srv->port)) < 0) {
        if (now_ms() > deadline || waitpid(srv->pid, NULL, WNOHANG) != 0)
            return -1;
        sleep_ms(5);
    }
    close(probe);
    if (!wait_for_count(srv, "connection terminated", 1))
        return -1;
    srv->log_len = 0;
    srv->log[0] = '\0';
    return 0;
}

/** @brief Stop the server with SIGTERM; it must exit with status 0. */
static int stop_server(void** state)
{
    struct server* srv = *state;
    int status = -1;
    kill(srv->pid, SIGTERM);
    long deadline = now_ms() + WAIT_MS * 2;
    while (waitpid(srv->pid, &status, WNOHANG) == 0) {
        if (now_ms() > deadline) {
            kill(srv->pid, SIGKILL);
            waitpid(srv->pid, &status, 0);
            break;
        }
        sleep_ms(5);
    }
    close(srv->log_fd);
    free(srv);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "server exit status %d\n", status);
        return -1;
    }
    return 0;
}

static void test_connect(void** state)
{
    struct server* srv = *state;
    close(new_client(srv));
    assert_true(wait_for_count(srv, "accepted connection", 1));
}

static void test_accepted_count(void** state)
{
    struct server* srv = *state;
    int c1 = new_client(srv);
    int c2 = new_client(srv);
    close(c2);
    assert_true(wait_for_count(srv, "accepted connection", 2));
    assert_int_equal(settled_count(srv, "accepted connection"), 2);

    int c3 = new_client(srv);
    int c4 = new_client(srv);
    close(c3);
    close(c4);
    close(c1);
    assert_true(wait_for_count(srv, "accepted connection", 4));
}

static void test_read_one(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    send_str(c, "TEST-TEST\n");
    close(c);
    assert_true(wait_for_count(srv, "message = 'TEST-TEST'", 1));
}

static void test_no_split(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    send_str(c, "TEST-");
    assert_int_equal(settled_count(srv, "TEST-"), 0);
    send_str(c, "TEST\n");
    close(c);
    assert_true(wait_for_count(srv, "TEST-TEST", 1));
}

static void test_read_multi(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    send_str(c, "TEST-TEST\n");
    int c2 = new_client(srv);
    send_str(c2, "TEST-TEST\n");
    send_str(c, "TEST-TEST\n");
    assert_true(wait_for_count(srv, "TEST-TEST", 3));
    close(c);
    close(c2);
}

static void test_disconnect(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    assert_int_equal(settled_count(srv, "connection terminated"), 0);
    close(c);
    assert_true(wait_for_count(srv, "connection terminated", 1));
}

static void test_disconnect_many(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    int c1 = new_client(srv);
    int c2 = new_client(srv);
    send_str(c1, "Some message");
    assert_int_equal(settled_count(srv, "connection terminated"), 0);
    close(c);
    assert_true(wait_for_count(srv, "connection terminated", 1));
    close(c1);
    close(c2);
    assert_true(wait_for_count(srv, "connection terminated", 3));
    assert_int_equal(settled_count(srv, "Some message"), 0);
}

static void test_greeting(void** state)
{
    struct server* srv = *state;
    int c1 = new_client(srv);
    int c2 = new_client(srv);
    expect_line(c1, "Welcome\n");
    expect_line(c2, "Welcome\n");
    close(c1);
    close(c2);
}

static void test_echo(void** state)
{
    struct server* srv = *state;
    int c = new_client(srv);
    expect_line(c, "Welcome\n");
    send_str(c, "tes");
    send_str(c, "t1\ntest2\ntest3\n");
    expect_line(c, "test1\n");
    expect_line(c, "test2\n");
    expect_line(c, "test3\n");
    close(c);
}

/** Every message reaches every client, also the idle ones. */
static void test_broadcast(void** state)
{
    struct server* srv = *state;
    int c[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = new_client(srv);
        expect_line(c[i], "Welcome\n");
    }
    send_str(c[0], "test1\ntest2\n");
    assert_true(wait_for_count(srv, "test2", 1));
    send_str(c[1], "test");
    send_str(c[1], "3\ntest4\n");

    const char* msgs[] = {"test1\n", "test2\n", "test3\n", "test4\n"};
    for (int i = 0; i < 3; ++i)
        for (int m = 0; m < 4; ++m)
            expect_line(c[i], msgs[m]);
    for (int i = 0; i < 3; ++i)
        close(c[i]);
}

/** Regression: a client that joins after another left gets the broadcast. */
static void test_broadcast_after_disconnect(void** state)
{
    struct server* srv = *state;
    int c1 = new_client(srv);
    int c2 = new_client(srv);
    expect_line(c1, "Welcome\n");
    expect_line(c2, "Welcome\n");
    close(c1);
    assert_true(wait_for_count(srv, "connection terminated", 1));

    int c3 = new_client(srv);
    expect_line(c3, "Welcome\n");
    send_str(c2, "hello\n");
    expect_line(c2, "hello\n");
    expect_line(c3, "hello\n");
    close(c2);
    close(c3);
}

int main(void)
{
#define T(f) cmocka_unit_test_setup_teardown(f, start_server, stop_server)
    const struct CMUnitTest tests[] = {
        T(test_connect),
        T(test_accepted_count),
        T(test_read_one),
        T(test_no_split),
        T(test_read_multi),
        T(test_disconnect),
        T(test_disconnect_many),
        T(test_greeting),
        T(test_echo),
        T(test_broadcast),
        T(test_broadcast_after_disconnect),
    };
#undef T
    return cmocka_run_group_tests(tests, NULL, NULL);
}
