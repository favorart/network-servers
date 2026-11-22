/**
 * @file test_proxy.c
 * @brief Integration tests: the built proxy (PROXY_BIN) between a client and two echo backends.
 *
 * The backends run in threads of this process on ephemeral ports; the proxy is started with
 * fork + exec on a free port with a temporary config. An echo backend closes the connection when
 * it receives "QUIT".
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <cmocka.h>

#include "proxy.h"

#define TIMEOUT_MS 5000

/** @brief An echo backend: listening socket, acceptor thread and per-connection counters. */
struct backend
{
    int lfd;
    int stop[2];
    uint16_t port;
    pthread_t thread;
    int accepted; /* atomic */
    int eofs;     /* atomic: connections closed by the peer */
    int active;   /* atomic: connections being served */
};

/** @brief A running proxy process with its temporary config. */
struct proxy
{
    pid_t pid;
    uint16_t port;
    char dir[64];
    char conf[128];
};

static struct backend backends[2];
static struct proxy main_proxy;

static int load(int* p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static void add(int* p, int v)
{
    __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

//-----------------------------------------
struct conn
{
    struct backend* b;
    int fd;
};

static void* echo_conn(void* arg)
{
    struct conn* c = arg;
    char buf[16384];
    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n == 0)
            add(&c->b->eofs, 1);
        if (n <= 0 || (n >= 4 && !memcmp(buf, "QUIT", 4)))
            break;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = send(c->fd, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0)
                goto out;
            off += w;
        }
    }
out:
    close(c->fd);
    add(&c->b->active, -1);
    free(c);
    return NULL;
}

static void* backend_loop(void* arg)
{
    struct backend* b = arg;
    struct pollfd pfd[2] = {{b->lfd, POLLIN, 0}, {b->stop[0], POLLIN, 0}};
    while (poll(pfd, 2, -1) >= 0 && !(pfd[1].revents & POLLIN)) {
        if (!(pfd[0].revents & POLLIN))
            continue;
        int fd = accept(b->lfd, NULL, NULL);
        if (fd < 0)
            continue;
        struct conn* c = malloc(sizeof(*c));
        c->b = b;
        c->fd = fd;
        add(&b->accepted, 1);
        add(&b->active, 1);
        pthread_t t;
        pthread_create(&t, NULL, echo_conn, c);
        pthread_detach(t);
    }
    return NULL;
}

/** @brief Listening socket on 127.0.0.1 (port 0 = ephemeral); returns the fd, port in @p port. */
static int listen_local(uint16_t* port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(fd >= 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert_int_equal(bind(fd, (struct sockaddr*)&sa, sizeof(sa)), 0);
    assert_int_equal(listen(fd, SOMAXCONN), 0);
    socklen_t len = sizeof(sa);
    assert_int_equal(getsockname(fd, (struct sockaddr*)&sa, &len), 0);
    *port = ntohs(sa.sin_port);
    return fd;
}

/** @brief A port that is free right now (closed listening socket). */
static uint16_t free_port(void)
{
    uint16_t port;
    close(listen_local(&port));
    return port;
}

static void backend_start(struct backend* b)
{
    memset(b, 0, sizeof(*b));
    b->lfd = listen_local(&b->port);
    assert_int_equal(pipe(b->stop), 0);
    assert_int_equal(pthread_create(&b->thread, NULL, backend_loop, b), 0);
}

static void backend_stop(struct backend* b)
{
    /* Connections end once the proxy is gone */
    for (int t = 0; t < TIMEOUT_MS && load(&b->active) > 0; t += 10)
        sleep_ms(10);
    assert_int_equal(write(b->stop[1], "x", 1), 1);
    pthread_join(b->thread, NULL);
    close(b->lfd);
    close(b->stop[0]);
    close(b->stop[1]);
}

//-----------------------------------------
/** @brief Connect to 127.0.0.1:@p port with receive/send timeouts; -1 on failure. */
static int connect_local(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(fd >= 0);
    struct timeval tv = {TIMEOUT_MS / 1000, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa))) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_all(int fd, const void* data, size_t len)
{
    for (size_t off = 0; off < len;) {
        ssize_t n = send(fd, (const char*)data + off, len - off, MSG_NOSIGNAL);
        assert_true(n > 0);
        off += (size_t)n;
    }
}

/** @brief Receive exactly @p len bytes. */
static void recv_exact(int fd, void* data, size_t len)
{
    for (size_t off = 0; off < len;) {
        ssize_t n = recv(fd, (char*)data + off, len - off, 0);
        assert_true(n > 0);
        off += (size_t)n;
    }
}

/** @brief Expect the connection to be closed: EOF or reset, not a timeout. */
static void expect_closed(int fd)
{
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    assert_true(n == 0 || (n < 0 && errno == ECONNRESET));
}

static void roundtrip(uint16_t port, const char* msg)
{
    int fd = connect_local(port);
    assert_true(fd >= 0);
    char buf[64] = {0};
    send_all(fd, msg, strlen(msg));
    recv_exact(fd, buf, strlen(msg));
    assert_string_equal(buf, msg);
    close(fd);
}

//-----------------------------------------
/** @brief Write a config "<port>,<servers>" and start the proxy; wait until it accepts. */
static void proxy_start(struct proxy* p, const char* servers)
{
    strcpy(p->dir, "/tmp/proxy-test-XXXXXX");
    assert_non_null(mkdtemp(p->dir));
    snprintf(p->conf, sizeof(p->conf), "%s/proxy.conf", p->dir);
    p->port = free_port();
    FILE* fp = fopen(p->conf, "w");
    assert_non_null(fp);
    fprintf(fp, "%u,%s\n", p->port, servers);
    fclose(fp);

    p->pid = fork();
    assert_true(p->pid >= 0);
    if (!p->pid) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        execl(PROXY_BIN, PROXY_BIN, "-c", p->conf, (char*)NULL);
        _exit(127);
    }

    /* The proxy binds the port without printing anything useful: poll it */
    for (int t = 0; t < TIMEOUT_MS; t += 10) {
        assert_int_equal(waitpid(p->pid, NULL, WNOHANG), 0);
        int fd = connect_local(p->port);
        if (fd >= 0) {
            close(fd);
            return;
        }
        sleep_ms(10);
    }
    print_error("proxy did not start on port %u\n", p->port);
    fail();
}

/** @brief Stop the proxy: it must still be running (not crashed) until SIGTERM. */
static void proxy_stop(struct proxy* p)
{
    int status = 0;
    assert_int_equal(waitpid(p->pid, &status, WNOHANG), 0);
    kill(p->pid, SIGTERM);
    assert_int_equal(waitpid(p->pid, &status, 0), p->pid);
    assert_true(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    unlink(p->conf);
    rmdir(p->dir);
}

static int group_setup(void** state)
{
    (void)state;
    backend_start(&backends[0]);
    backend_start(&backends[1]);
    char servers[64];
    snprintf(servers, sizeof(servers), "127.0.0.1:%u, 127.0.0.1:%u", backends[0].port,
             backends[1].port);
    proxy_start(&main_proxy, servers);
    return 0;
}

static int group_teardown(void** state)
{
    (void)state;
    proxy_stop(&main_proxy);
    backend_stop(&backends[0]);
    backend_stop(&backends[1]);
    return 0;
}

/** @brief Connections closed by the proxy, over both backends. */
static int total_eofs(void)
{
    return load(&backends[0].eofs) + load(&backends[1].eofs);
}

/** @brief Wait until total_eofs() exceeds @p before or TIMEOUT_MS passes. */
static int wait_eofs_above(int before)
{
    for (int t = 0; t < TIMEOUT_MS; t += 10) {
        if (total_eofs() > before)
            return 1;
        sleep_ms(10);
    }
    return total_eofs() > before;
}

//-----------------------------------------
static void test_version(void** state)
{
    (void)state;
    FILE* fp = popen(PROXY_BIN " --version", "r");
    assert_non_null(fp);
    char line[64] = {0};
    assert_non_null(fgets(line, sizeof(line), fp));
    assert_int_equal(pclose(fp), 0);
    line[strcspn(line, "\n")] = '\0';
    assert_string_equal(line, NS_VERSION);
}

static void test_echo_roundtrip(void** state)
{
    (void)state;
    roundtrip(main_proxy.port, "hello");
}

/* Regression: EOF from the client shut down the client side, cutting the backend's answer */
static void test_client_half_close(void** state)
{
    (void)state;
    enum
    {
        LEN = 256 * 1024
    };
    char* out = malloc(LEN);
    char* in = malloc(LEN);
    for (int i = 0; i < LEN; ++i)
        out[i] = (char)(i * 7 + i / 251);

    int eofs = total_eofs();
    int fd = connect_local(main_proxy.port);
    assert_true(fd >= 0);
    send_all(fd, out, LEN);
    assert_int_equal(shutdown(fd, SHUT_WR), 0);
    recv_exact(fd, in, LEN);
    assert_memory_equal(in, out, LEN);
    expect_closed(fd);
    /* The backend got the EOF too */
    assert_true(wait_eofs_above(eofs));
    close(fd);
    free(out);
    free(in);
}

static void test_spread_over_backends(void** state)
{
    (void)state;
    int before[2] = {load(&backends[0].accepted), load(&backends[1].accepted)};
    for (int i = 0; i < 40; ++i)
        roundtrip(main_proxy.port, "x");
    assert_true(load(&backends[0].accepted) > before[0]);
    assert_true(load(&backends[1].accepted) > before[1]);
}

static void test_client_close_closes_backend(void** state)
{
    (void)state;
    int fd = connect_local(main_proxy.port);
    assert_true(fd >= 0);
    char buf[4];
    send_all(fd, "ping", 4);
    recv_exact(fd, buf, 4);
    int eofs = total_eofs();
    close(fd);
    assert_true(wait_eofs_above(eofs));
}

static void test_backend_close_closes_client(void** state)
{
    (void)state;
    int fd = connect_local(main_proxy.port);
    assert_true(fd >= 0);
    send_all(fd, "QUIT", 4);
    expect_closed(fd);
    close(fd);
}

/* A client that resets the connection while data is still flowing must not kill the proxy */
static void test_client_reset(void** state)
{
    (void)state;
    enum
    {
        LEN = 1024 * 1024
    };
    char* out = calloc(1, LEN);
    for (int i = 0; i < 5; ++i) {
        int fd = connect_local(main_proxy.port);
        assert_true(fd >= 0);
        send_all(fd, out, LEN);
        struct linger lg = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
    }
    free(out);
    sleep_ms(100);
    roundtrip(main_proxy.port, "alive");
}

/* An unreachable backend closes the client connection */
static void test_dead_backend(void** state)
{
    (void)state;
    char servers[32];
    snprintf(servers, sizeof(servers), "127.0.0.1:%u", free_port());
    struct proxy p;
    proxy_start(&p, servers);
    int fd = connect_local(p.port);
    assert_true(fd >= 0);
    expect_closed(fd);
    close(fd);
    proxy_stop(&p);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version),
        cmocka_unit_test(test_echo_roundtrip),
        cmocka_unit_test(test_client_half_close),
        cmocka_unit_test(test_spread_over_backends),
        cmocka_unit_test(test_client_close_closes_backend),
        cmocka_unit_test(test_backend_close_closes_client),
        cmocka_unit_test(test_client_reset),
        cmocka_unit_test(test_dead_backend),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
