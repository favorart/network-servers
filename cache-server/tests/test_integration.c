/* Integration test: run the built server on a free port and talk to it over TCP. */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#ifndef CACHE_SERVER_BIN
#error "CACHE_SERVER_BIN must be the path of the server binary"
#endif

#define WORKERS "3"

static pid_t server_pid = -1;
static uint16_t server_port;
static char out_path[] = "/tmp/cache-server-test-XXXXXX";
static int out_fd = -1;

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* A port nobody listens on right now: bind to port 0 and ask which one we got */
static uint16_t free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(a);
    if (s < 0 || bind(s, (struct sockaddr*)&a, sizeof(a)) ||
        getsockname(s, (struct sockaddr*)&a, &len))
        return 0;
    close(s);
    return ntohs(a.sin_port);
}

static int connect_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons(server_port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(s, (struct sockaddr*)&a, sizeof(a))) {
        close(s);
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

static void send_all(int s, const char* text)
{
    size_t len = strlen(text);
    while (len) {
        ssize_t n = send(s, text, len, MSG_NOSIGNAL);
        assert_true(n > 0);
        text += n;
        len -= (size_t)n;
    }
}

/* Read until @p lines newlines arrived (5 s timeout) */
static void recv_lines(int s, int lines, char* buf, size_t size)
{
    size_t got = 0;
    int seen = 0;
    while (seen < lines) {
        struct pollfd p = {.fd = s, .events = POLLIN};
        assert_int_equal(poll(&p, 1, 5000), 1);
        ssize_t n = recv(s, buf + got, size - 1U - got, 0);
        assert_true(n > 0);
        for (ssize_t i = 0; i < n; ++i)
            seen += (buf[got + (size_t)i] == '\n');
        got += (size_t)n;
        assert_true(got < size - 1U);
    }
    buf[got] = '\0';
}

/* Send one request on a new connection and check the whole reply */
static void exchange(const char* request, int lines, const char* expected)
{
    char buf[4096];
    int s = connect_server();
    assert_true(s >= 0);
    send_all(s, request);
    recv_lines(s, lines, buf, sizeof(buf));
    assert_string_equal(buf, expected);
    close(s);
}

static int count_fds(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    DIR* d = opendir(path);
    if (!d)
        return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)))
        n += (e->d_name[0] != '.');
    closedir(d);
    return n;
}

static int start_server(void** state)
{
    (void)state;
    out_fd = mkstemp(out_path);
    server_port = free_port();
    if (out_fd < 0 || !server_port)
        return -1;
    char port[8];
    snprintf(port, sizeof(port), "%u", server_port);

    server_pid = fork();
    if (server_pid < 0)
        return -1;
    if (server_pid == 0) {
        /* own process group: the test stops the master and its workers together */
        setpgid(0, 0);
        dup2(out_fd, STDOUT_FILENO);
        execl(CACHE_SERVER_BIN, CACHE_SERVER_BIN, "-p", port, "-i", "127.0.0.1", "-w", WORKERS,
              (char*)NULL);
        perror("execl " CACHE_SERVER_BIN);
        _exit(127);
    }
    setpgid(server_pid, server_pid);
    /* wait until it accepts connections (10 s) */
    for (int i = 0; i < 1000; ++i) {
        int status;
        if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
            server_pid = -1;
            return -1;
        }
        int s = connect_server();
        if (s >= 0) {
            close(s);
            return 0;
        }
        sleep_ms(10);
    }
    return -1;
}

static int stop_server(void** state)
{
    (void)state;
    if (server_pid > 0) {
        kill(-server_pid, SIGKILL);
        waitpid(server_pid, NULL, 0);
        server_pid = -1;
    }
    if (out_fd >= 0) {
        close(out_fd);
        unlink(out_path);
    }
    return 0;
}

static void test_version(void** state)
{
    (void)state;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --version > /dev/null", CACHE_SERVER_BIN);
    assert_int_equal(system(cmd), 0);
    snprintf(cmd, sizeof(cmd), "%s -p 0 2> /dev/null", CACHE_SERVER_BIN);
    assert_int_not_equal(system(cmd), 0);
}

static void test_set_get(void** state)
{
    (void)state;
    exchange("set 3600 1 100\nget 1\nget 2\nbad\n", 4,
             "ok 1 100\nok 1 100\nerror : no such key in table\n"
             "error : incorrect command to server\n");
}

/* Regression: each worker had its own copy of the cache. */
static void test_cache_shared_between_workers(void** state)
{
    (void)state;
    exchange("set 3600 7 70\n", 1, "ok 7 70\n");
    /* new connections go to random workers: every one of them must see the key */
    for (int i = 0; i < 20; ++i)
        exchange("get 7\n", 1, "ok 7 70\n");
}

/* Regression: a command split between TCP segments was dropped. */
static void test_split_command(void** state)
{
    (void)state;
    char buf[256];
    int s = connect_server();
    assert_true(s >= 0);
    send_all(s, "se");
    sleep_ms(50);
    send_all(s, "t 3600 9 90\n");
    recv_lines(s, 1, buf, sizeof(buf));
    assert_string_equal(buf, "ok 9 90\n");
    close(s);
}

/* Regression: the master kept every accepted socket open. */
static void test_master_closes_client_sockets(void** state)
{
    (void)state;
    int before = count_fds(server_pid);
    assert_true(before > 0);
    for (int i = 0; i < 30; ++i)
        exchange("get 7\n", 1, "ok 7 70\n");
    assert_int_equal(count_fds(server_pid), before);
}

/* SIGTERM to the group: the master exits 0 and removes the shared memory and semaphore. */
static void test_clean_shutdown(void** state)
{
    (void)state;
    assert_int_equal(kill(-server_pid, SIGTERM), 0);
    int status = 0;
    pid_t r = 0;
    for (int i = 0; i < 500 && r == 0; ++i) {
        r = waitpid(server_pid, &status, WNOHANG);
        if (r == 0)
            sleep_ms(10);
    }
    assert_int_equal(r, server_pid);
    pid_t pgid = server_pid;
    server_pid = -1;
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    /* no worker is left in the group */
    errno = 0;
    assert_int_equal(kill(-pgid, 0), -1);
    assert_int_equal(errno, ESRCH);

    char out[4096] = {0};
    assert_true(pread(out_fd, out, sizeof(out) - 1U, 0) > 0);
    const char* ids = strstr(out, "shm: ");
    assert_non_null(ids);
    int shmid = -1, semid = -1;
    assert_int_equal(sscanf(ids, "shm: %d sem: %d", &shmid, &semid), 2);
    /* The segment is gone, or marked for removal while a short-lived process (for example a
     * sanitizer helper forked from the master) is still attached: IPC_RMID was done either way. */
    struct shmid_ds ds;
    if (shmctl(shmid, IPC_STAT, &ds) == 0)
        assert_true(ds.shm_perm.mode & SHM_DEST);
    assert_int_equal(semctl(semid, 0, GETVAL), -1);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version),
        cmocka_unit_test(test_set_get),
        cmocka_unit_test(test_cache_shared_between_workers),
        cmocka_unit_test(test_split_command),
        cmocka_unit_test(test_master_closes_client_sockets),
        cmocka_unit_test(test_clean_shutdown),
    };
    return cmocka_run_group_tests(tests, start_server, stop_server);
}
