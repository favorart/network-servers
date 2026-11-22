/**
 * @file test_integration.c
 * @brief Starts the built server on a free port and talks HTTP to it.
 *
 * Usage: test_integration <path to http-server>
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS "2"
#define IO_TIMEOUT_MS 5000
#define BIG_SIZE (8U << 20)

static const char* server_bin;
static char root[64]; /* root/www is served, root/secret.html is outside */
static char www[128];
static char log_path[128];
static pid_t server_pid = -1;
static int server_port;

static const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',
                                     'F',  'I',  'F',  0x00, 0x01, 0xFF, 0xD9};
//-----------------------------------------
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static int write_file(const char* dir, const char* name, const void* data, size_t length)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t written = fwrite(data, 1, length, f);
    fclose(f);
    return written == length ? 0 : -1;
}
static void remove_file(const char* dir, const char* name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    remove(path);
}
/** @brief A port that was free a moment ago. */
static int free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (s < 0 || bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        getsockname(s, (struct sockaddr*)&addr, &len) != 0) {
        if (s >= 0)
            close(s);
        return -1;
    }
    close(s);
    return ntohs(addr.sin_port);
}
static int connect_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}
/** @brief Wait until @p pid exits (true) or @p timeout_ms passes (false). */
static bool wait_exit(pid_t pid, int* status, long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    do {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid)
            return true;
        if (r < 0)
            return false;
        sleep_ms(10);
    } while (now_ms() < deadline);
    return false;
}
/** @brief Whether any process of the server's group is still alive. */
static bool group_alive(pid_t pgid)
{
    /* reap orphaned workers (this process is their subreaper) */
    while (waitpid(-pgid, NULL, WNOHANG) > 0) {
    }
    return kill(-pgid, 0) == 0 || errno != ESRCH;
}
/** @brief Start the server; false if it could not listen (port taken). */
static bool start_server(void)
{
    server_port = free_port();
    if (server_port <= 0)
        return false;
    char port[16];
    snprintf(port, sizeof(port), "%d", server_port);

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setpgid(0, 0);
        int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        int null = open("/dev/null", O_RDONLY);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        if (null >= 0)
            dup2(null, STDIN_FILENO);
        execl(server_bin, server_bin, "-d", www, "-p", port, "-h", "127.0.0.1", "-w", WORKERS,
              (char*)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    server_pid = pid;

    long deadline = now_ms() + IO_TIMEOUT_MS;
    while (now_ms() < deadline) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            server_pid = -1;
            group_alive(pid);
            return false;
        }
        int s = connect_server();
        if (s >= 0) {
            close(s);
            return true;
        }
        sleep_ms(20);
    }
    return false;
}
static void kill_server(void)
{
    if (server_pid > 0) {
        kill(-server_pid, SIGKILL);
        int status;
        wait_exit(server_pid, &status, IO_TIMEOUT_MS);
        long deadline = now_ms() + IO_TIMEOUT_MS;
        while (group_alive(server_pid) && now_ms() < deadline)
            sleep_ms(10);
        server_pid = -1;
    }
}
//-----------------------------------------
static int group_setup(void** state)
{
    (void)state;
    signal(SIGPIPE, SIG_IGN);
    /* orphaned workers are reparented to this process, so none can hide */
    prctl(PR_SET_CHILD_SUBREAPER, 1);

    strcpy(root, "/tmp/http-integration-XXXXXX");
    if (!mkdtemp(root))
        return -1;
    snprintf(www, sizeof(www), "%s/www", root);
    snprintf(log_path, sizeof(log_path), "%s/server.log", root);
    if (mkdir(www, 0700) != 0)
        return -1;
    const char* html = "<html><body>hello</body></html>\n";
    char* big = malloc(BIG_SIZE);
    if (!big)
        return -1;
    memset(big, 'b', BIG_SIZE);
    int rc = write_file(www, "index.html", html, strlen(html)) |
             write_file(www, "photo.jpeg", jpeg, sizeof(jpeg)) |
             write_file(www, "big.html", big, BIG_SIZE) | write_file(www, "notes.txt", "text", 4) |
             write_file(root, "secret.html", "SECRET", 6);
    free(big);
    if (rc != 0)
        return -1;

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (start_server())
            return 0;
        kill_server();
    }
    fprintf(stderr, "cannot start %s\n", server_bin);
    return -1;
}
/** @brief Fail if a sanitizer reported anything in the server log. */
static int check_log(void)
{
    FILE* f = fopen(log_path, "r");
    if (!f)
        return 0;
    char line[1024];
    int bad = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Sanitizer") || strstr(line, "runtime error:")) {
            fprintf(stderr, "server log: %s", line);
            bad = -1;
        }
    }
    fclose(f);
    return bad;
}
static int group_teardown(void** state)
{
    (void)state;
    kill_server();
    int rc = check_log();
    remove_file(www, "index.html");
    remove_file(www, "photo.jpeg");
    remove_file(www, "big.html");
    remove_file(www, "notes.txt");
    rmdir(www);
    remove_file(root, "secret.html");
    remove(log_path);
    rmdir(root);
    return rc;
}
//-----------------------------------------
/** @brief A response read until the server closed the connection. */
struct response
{
    char* data;
    size_t length;
    int status;
    const char* body;
    size_t body_length;
};
static void response_free(struct response* r)
{
    free(r->data);
    memset(r, 0, sizeof(*r));
}
/** @brief Read until EOF; fails the test on a timeout (the server must close). */
static void read_to_eof(int s, struct response* r)
{
    size_t capacity = 4096;
    r->data = malloc(capacity + 1);
    assert_non_null(r->data);
    r->length = 0;
    for (;;) {
        struct pollfd p = {s, POLLIN, 0};
        int ready = poll(&p, 1, IO_TIMEOUT_MS);
        assert_true(ready == 1); /* the server closes the connection */
        if (capacity - r->length < 4096) {
            capacity *= 2;
            r->data = realloc(r->data, capacity + 1);
            assert_non_null(r->data);
        }
        ssize_t n = recv(s, r->data + r->length, capacity - r->length, 0);
        assert_true(n >= 0);
        if (n == 0)
            break;
        r->length += (size_t)n;
    }
    r->data[r->length] = '\0';
    r->status = 0;
    sscanf(r->data, "HTTP/1.0 %d", &r->status);
    char* end = strstr(r->data, "\r\n\r\n");
    r->body = end ? end + 4 : NULL;
    r->body_length = end ? r->length - (size_t)(end + 4 - r->data) : 0;
}
/** @brief Send @p parts (NULL-terminated) with a pause between them, read the response. */
static void exchange_parts(const char* const* parts, struct response* r)
{
    int s = connect_server();
    assert_true(s >= 0);
    for (size_t i = 0; parts[i]; ++i) {
        if (i)
            sleep_ms(50);
        size_t len = strlen(parts[i]);
        assert_int_equal(send(s, parts[i], len, MSG_NOSIGNAL), (ssize_t)len);
    }
    read_to_eof(s, r);
    close(s);
}
static void exchange(const char* request, struct response* r)
{
    const char* parts[] = {request, NULL};
    exchange_parts(parts, r);
}
static void assert_header(const struct response* r, const char* header)
{
    if (!strstr(r->data, header)) {
        print_error("no header '%s' in:\n%s\n", header, r->data);
        fail();
    }
}
//-----------------------------------------
static void test_get_html(void** state)
{
    (void)state;
    struct response r;
    exchange("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n", &r);
    assert_int_equal(r.status, 200);
    assert_header(&r, "\r\nContent-Type: text/html; charset=utf-8\r\n");
    assert_header(&r, "\r\nContent-Length: 32\r\n");
    assert_header(&r, "\r\nConnection: close\r\n");
    assert_header(&r, "\r\nDate: ");
    assert_non_null(r.body);
    assert_int_equal(r.body_length, 32);
    assert_memory_equal(r.body, "<html><body>hello</body></html>\n", 32);
    response_free(&r);
}
static void test_get_jpeg(void** state)
{
    (void)state;
    struct response r;
    exchange("GET /photo.jpeg HTTP/1.0\r\n\r\n", &r);
    assert_int_equal(r.status, 200);
    assert_header(&r, "\r\nContent-Type: image/jpeg\r\n");
    assert_int_equal(r.body_length, sizeof(jpeg));
    assert_memory_equal(r.body, jpeg, sizeof(jpeg));
    response_free(&r);
}
static void test_get_big_file(void** state)
{
    (void)state;
    struct response r;
    exchange("GET /big.html HTTP/1.1\r\n\r\n", &r);
    assert_int_equal(r.status, 200);
    assert_int_equal(r.body_length, BIG_SIZE);
    for (size_t i = 0; i < BIG_SIZE; i += 4096)
        assert_int_equal(r.body[i], 'b');
    response_free(&r);
}
static void test_head(void** state)
{
    (void)state;
    struct response r;
    exchange("HEAD /index.html HTTP/1.1\r\n\r\n", &r);
    assert_int_equal(r.status, 200);
    assert_header(&r, "\r\nContent-Length: 32\r\n");
    assert_non_null(r.body);
    assert_int_equal(r.body_length, 0);
    response_free(&r);
}
static void test_post(void** state)
{
    (void)state;
    struct response r;
    exchange("POST /form HTTP/1.1\r\nContent-Length: 7\r\n\r\na=1&b=2", &r);
    assert_int_equal(r.status, 200);
    assert_int_equal(r.body_length, 0);
    response_free(&r);
}
static void test_not_found(void** state)
{
    (void)state;
    const char* requests[] = {"GET /missing.html HTTP/1.1\r\n\r\n",
                              "GET /notes.txt HTTP/1.1\r\n\r\n",
                              "HEAD /nothing.jpg HTTP/1.0\r\n\r\n"};
    for (size_t i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        struct response r;
        exchange(requests[i], &r);
        assert_int_equal(r.status, 404);
        assert_int_equal(r.body_length, 0);
        response_free(&r);
    }
}
/** Regression: "GET /../secret.html" served a file outside the directory. */
static void test_path_traversal(void** state)
{
    (void)state;
    const char* requests[] = {"GET /../secret.html HTTP/1.1\r\n\r\n",
                              "GET /x/../../secret.html HTTP/1.1\r\n\r\n"};
    for (size_t i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        struct response r;
        exchange(requests[i], &r);
        assert_int_equal(r.status, 404);
        assert_null(strstr(r.data, "SECRET"));
        response_free(&r);
    }
}
/** Regression: a head that came in several reads was dropped. */
static void test_split_request(void** state)
{
    (void)state;
    const char* parts[] = {"GET /index", ".html HTTP/1.1\r\nHo", "st: a\r\n\r", "\n", NULL};
    struct response r;
    exchange_parts(parts, &r);
    assert_int_equal(r.status, 200);
    assert_int_equal(r.body_length, 32);
    response_free(&r);
}
/** A bad request line closes the connection without a response. */
static void test_bad_request(void** state)
{
    (void)state;
    struct response r;
    exchange("BREW /pot HTTP/1.1\r\n\r\n", &r);
    assert_int_equal(r.length, 0);
    response_free(&r);
}
/** Clients that reset the connection in the middle of a response do not kill workers. */
static void test_client_reset(void** state)
{
    (void)state;
    for (int i = 0; i < 8; ++i) {
        int s = connect_server();
        assert_true(s >= 0);
        const char* request = "GET /big.html HTTP/1.1\r\n\r\n";
        assert_int_equal(send(s, request, strlen(request), MSG_NOSIGNAL), (ssize_t)strlen(request));
        char buf[1024];
        assert_true(recv(s, buf, sizeof(buf), 0) > 0);
        struct linger lg = {1, 0}; /* close with RST */
        setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(s);
    }
    for (int i = 0; i < 10; ++i) {
        struct response r;
        exchange("GET /index.html HTTP/1.1\r\n\r\n", &r);
        assert_int_equal(r.status, 200);
        response_free(&r);
    }
}
/** SIGTERM to the master stops it and all its workers; no process is left behind. */
static void test_graceful_stop(void** state)
{
    (void)state;
    pid_t pgid = server_pid;
    assert_true(group_alive(pgid));
    assert_int_equal(kill(server_pid, SIGTERM), 0);
    int status = 0;
    assert_true(wait_exit(server_pid, &status, IO_TIMEOUT_MS));
    server_pid = -1;
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);

    long deadline = now_ms() + IO_TIMEOUT_MS;
    while (group_alive(pgid) && now_ms() < deadline)
        sleep_ms(10);
    assert_false(group_alive(pgid));
    assert_true(connect_server() < 0);
}
//-----------------------------------------
int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <http-server binary>\n", argv[0]);
        return 2;
    }
    server_bin = argv[1];
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_get_html),       cmocka_unit_test(test_get_jpeg),
        cmocka_unit_test(test_get_big_file),   cmocka_unit_test(test_head),
        cmocka_unit_test(test_post),           cmocka_unit_test(test_not_found),
        cmocka_unit_test(test_path_traversal), cmocka_unit_test(test_split_request),
        cmocka_unit_test(test_bad_request),    cmocka_unit_test(test_client_reset),
        cmocka_unit_test(test_graceful_stop),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
