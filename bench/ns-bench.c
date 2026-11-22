/* ns-bench: load generator for the six servers of this repository.
 *
 * Starts every server on a free port, runs short closed-loop scenarios with several client
 * threads and prints one Markdown table: throughput and p50/p99 latency per scenario. Problems
 * (a server that died, did not stop, left processes or IPC objects) go to stderr and make the
 * exit status 1. Run it from the repository root after `make all`:
 *
 *     bench/build/ns-bench [-t seconds per scenario] [-o file for a copy of the table]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/prctl.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IO_TIMEOUT_MS 5000
#define WARMUP_MS 300
#define MAX_ROWS 64
#define CACHE_KEYS 200 /* the table holds 256 entries */
#define DICT_HOT 16
#define ECHO_SIZE 64

#define CACHE_BIN "cache-server/build/cache-server"
#define CHAT_BIN "chat-server/build/chat-server"
#define DICT_BIN "dictionary-server/build/dictionary-server"
#define DICT_FILE "dictionary-server/data/mydict.txt"
#define HTTP_BIN "http-server/build/http-server"
#define PROXY_BIN "proxy-server/build/proxy-server"
#define SUBS_BIN "subscribe-server/build/subscribe-server"

static double bench_seconds = 3.0;
static atomic_int measuring, stopping;
static int problems;
static char tmp_dir[] = "/tmp/ns-bench-XXXXXX";
static volatile sig_atomic_t running_pgid;

/* ------------------------------------------------------------------------------------------ */
/* Helpers */

static void problem(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ns-bench: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    ++problems;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000U + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

/* xorshift64*: a fast per-thread generator */
static uint64_t rnd_next(uint64_t* s)
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Sockets */

static int free_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(a);
    int port = 0;
    if (s >= 0 && !bind(s, (struct sockaddr*)&a, sizeof(a)) &&
        !getsockname(s, (struct sockaddr*)&a, &len))
        port = ntohs(a.sin_port);
    if (s >= 0)
        close(s);
    return port;
}

static int tcp_connect(int port)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(s, (struct sockaddr*)&a, sizeof(a))) {
        close(s);
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

static int send_all(int fd, const void* data, size_t len)
{
    const char* p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 1: readable, 0: timeout, -1: error */
static int wait_in(int fd, int ms)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int r;
    do
        r = poll(&p, 1, ms);
    while (r < 0 && errno == EINTR);
    return r;
}

static int recv_exact(int fd, char* buf, size_t len)
{
    while (len) {
        if (wait_in(fd, IO_TIMEOUT_MS) != 1)
            return -1;
        ssize_t n = recv(fd, buf, len, 0);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read until EOF; keep the first @p size - 1 bytes. @return bytes read, -1 on error or timeout */
static long recv_to_eof(int fd, char* buf, size_t size)
{
    char scratch[4096];
    long total = 0;
    size_t kept = 0;
    for (;;) {
        if (wait_in(fd, IO_TIMEOUT_MS) != 1)
            return -1;
        char* dst = kept < size - 1U ? buf + kept : scratch;
        size_t room = kept < size - 1U ? size - 1U - kept : sizeof(scratch);
        ssize_t n = recv(fd, dst, room, 0);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        if (dst != scratch)
            kept += (size_t)n;
        total += n;
    }
    buf[kept] = '\0';
    return total;
}

/* Buffered line reader */
struct lines
{
    int fd;
    size_t off, len;
    char buf[16384];
};

/* 1: a line in @p out without '\n', 0: no line within @p ms, -1: EOF or error */
static int read_line(struct lines* r, char* out, size_t size, int ms)
{
    for (;;) {
        char* start = r->buf + r->off;
        char* nl = memchr(start, '\n', r->len);
        if (nl) {
            size_t n = (size_t)(nl - start);
            size_t c = n < size - 1U ? n : size - 1U;
            memcpy(out, start, c);
            out[c] = '\0';
            r->off += n + 1U;
            r->len -= n + 1U;
            return 1;
        }
        if (r->off) {
            memmove(r->buf, start, r->len);
            r->off = 0;
        }
        if (r->len == sizeof(r->buf))
            return -1;
        int p = wait_in(r->fd, ms);
        if (p <= 0)
            return p;
        ssize_t got = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (got <= 0)
            return -1;
        r->len += (size_t)got;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Measurement: worker threads, latency samples, result rows */

struct samples
{
    uint64_t* v;
    size_t n, cap;
};

static void samples_push(struct samples* s, uint64_t x)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2U : 4096U;
        uint64_t* v = realloc(s->v, cap * sizeof(*v));
        if (!v)
            return;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = x;
}

/** One client thread; a scenario fills up to two rows (k = 0, 1). */
struct worker
{
    int id, count;
    void* ctx;
    pthread_barrier_t* barrier;
    uint64_t ops[2], errors;
    struct samples lat[2];
};

static int in_window(void)
{
    return atomic_load(&measuring) && !atomic_load(&stopping);
}

/* Count an operation of row @p k with latency @p ns if it ended inside the measured window */
static void record_lat(struct worker* w, int k, uint64_t ns)
{
    if (!in_window())
        return;
    w->ops[k]++;
    samples_push(&w->lat[k], ns);
}

static void record(struct worker* w, int k, uint64_t start)
{
    record_lat(w, k, now_ns() - start);
}

struct row
{
    char server[16], scenario[64];
    int clients;
    double ops, p50, p99;
    unsigned long long errors;
};

static struct row rows[MAX_ROWS];
static int nrows;

static int cmp_u64(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

/* Run @p nthreads copies of @p fn for the warm-up and the measured time; add a row per label.
 * Every @p fn calls pthread_barrier_wait() exactly once, after its connections are set up. */
static void run(const char* server, const char* const label[2], int clients, int nthreads,
                void* (*fn)(void*), void* ctx)
{
    struct worker* w = calloc((size_t)nthreads, sizeof(*w));
    pthread_t* th = calloc((size_t)nthreads, sizeof(*th));
    pthread_barrier_t barrier;
    if (!w || !th || pthread_barrier_init(&barrier, NULL, (unsigned)nthreads)) {
        fprintf(stderr, "ns-bench: out of memory\n");
        exit(1);
    }
    atomic_store(&measuring, 0);
    atomic_store(&stopping, 0);
    for (int i = 0; i < nthreads; ++i) {
        w[i].id = i;
        w[i].count = nthreads;
        w[i].ctx = ctx;
        w[i].barrier = &barrier;
        if (pthread_create(&th[i], NULL, fn, &w[i])) {
            fprintf(stderr, "ns-bench: cannot create a thread\n");
            exit(1);
        }
    }
    sleep_ms(WARMUP_MS);
    uint64_t t0 = now_ns();
    atomic_store(&measuring, 1);
    sleep_ms((long)(bench_seconds * 1000.0));
    atomic_store(&stopping, 1);
    double elapsed = (double)(now_ns() - t0) / 1e9;
    for (int i = 0; i < nthreads; ++i)
        pthread_join(th[i], NULL);
    pthread_barrier_destroy(&barrier);

    for (int k = 0; k < 2; ++k) {
        if (!label[k] || nrows == MAX_ROWS)
            continue;
        struct row* r = &rows[nrows++];
        memset(r, 0, sizeof(*r));
        snprintf(r->server, sizeof(r->server), "%s", server);
        snprintf(r->scenario, sizeof(r->scenario), "%s", label[k]);
        r->clients = clients;
        struct samples all = {0};
        for (int i = 0; i < nthreads; ++i)
            all.cap += w[i].lat[k].n;
        all.v = malloc((all.cap ? all.cap : 1U) * sizeof(*all.v));
        for (int i = 0; i < nthreads && all.v; ++i) {
            if (w[i].lat[k].n)
                memcpy(all.v + all.n, w[i].lat[k].v, w[i].lat[k].n * sizeof(*all.v));
            all.n += w[i].lat[k].n;
            if (k == 0)
                r->errors += w[i].errors;
        }
        r->ops = (double)all.n / elapsed;
        if (all.n) {
            qsort(all.v, all.n, sizeof(*all.v), cmp_u64);
            r->p50 = (double)all.v[(size_t)(0.50 * (double)(all.n - 1U))] / 1e3;
            r->p99 = (double)all.v[(size_t)(0.99 * (double)(all.n - 1U))] / 1e3;
        }
        free(all.v);
    }
    for (int i = 0; i < nthreads; ++i)
        for (int k = 0; k < 2; ++k)
            free(w[i].lat[k].v);
    free(w);
    free(th);
}

static void print_table(FILE* out)
{
    fprintf(out,
            "| Server | Scenario | Clients | Throughput, ops/s | p50, µs | p99, µs | Errors |\n"
            "|---|---|--:|--:|--:|--:|--:|\n");
    for (int i = 0; i < nrows; ++i)
        fprintf(out, "| %s | %s | %d | %.0f | %.0f | %.0f | %llu |\n", rows[i].server,
                rows[i].scenario, rows[i].clients, rows[i].ops, rows[i].p50, rows[i].p99,
                rows[i].errors);
}

/* ------------------------------------------------------------------------------------------ */
/* Temporary files and server processes */

static char created[16][192];
static int ncreated;

/* Path in the temporary directory, removed at exit (in reverse order) */
static const char* tmp_path(const char* name)
{
    snprintf(created[ncreated], sizeof(created[0]), "%s/%s", tmp_dir, name);
    return created[ncreated++];
}

static int copy_file(const char* from, const char* to)
{
    FILE* in = fopen(from, "rb");
    FILE* out = fopen(to, "wb");
    char buf[65536];
    size_t n;
    int rc = in && out ? 0 : -1;
    while (!rc && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        rc = fwrite(buf, 1, n, out) == n ? 0 : -1;
    if (in)
        fclose(in);
    if (out && fclose(out))
        rc = -1;
    return rc;
}

static void remove_tmp(void)
{
    while (ncreated)
        remove(created[--ncreated]);
    rmdir(tmp_dir);
}

struct server
{
    const char* name;
    pid_t pid;
    int port, stop_sig;
    const char* log;
};

static void dump_log(const struct server* s)
{
    char text[2048];
    int fd = open(s->log, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > (off_t)sizeof(text) - 1 ? size - (off_t)sizeof(text) + 1 : 0;
    ssize_t n = pread(fd, text, sizeof(text) - 1U, from);
    close(fd);
    if (n > 0) {
        text[n] = '\0';
        fprintf(stderr, "--- end of the %s log:\n%s\n---\n", s->name, text);
    }
}

static int read_log(const struct server* s, char* text, size_t size)
{
    int fd = open(s->log, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = pread(fd, text, size - 1U, 0);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    return 0;
}

/* Fork and exec @p argv in its own process group with stdout and stderr in the log; wait until
 * it accepts connections. With @p port_from_log the port is read from "listening on IP:PORT". */
static int server_start(struct server* s, char* const argv[], int port_from_log)
{
    s->log = tmp_path(s->name);
    s->pid = -1;
    int log = open(s->log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0) {
        problem("%s: cannot create the log %s", s->name, s->log);
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(log);
        problem("%s: fork failed", s->name);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDONLY);
        dup2(null, STDIN_FILENO);
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(log);
    s->pid = pid;
    running_pgid = pid;

    for (int i = 0; i < 1000; ++i) {
        if (waitpid(pid, NULL, WNOHANG) == pid) {
            problem("%s exited at start", s->name);
            dump_log(s);
            s->pid = -1;
            running_pgid = 0;
            return -1;
        }
        if (port_from_log) {
            char text[4096];
            const char* key = "listening on 127.0.0.1:";
            const char* at = read_log(s, text, sizeof(text)) ? NULL : strstr(text, key);
            if (at && strchr(at, '\n'))
                s->port = atoi(at + strlen(key));
        }
        int fd = s->port > 0 ? tcp_connect(s->port) : -1;
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        sleep_ms(10);
    }
    problem("%s did not accept connections in 10 s", s->name);
    dump_log(s);
    return -1;
}

/* A server must survive every scenario */
static int server_alive(struct server* s, const char* after)
{
    int status;
    if (s->pid > 0 && waitpid(s->pid, &status, WNOHANG) == s->pid) {
        if (WIFSIGNALED(status))
            problem("%s died with signal %d during \"%s\"", s->name, WTERMSIG(status), after);
        else
            problem("%s exited with %d during \"%s\"", s->name, WEXITSTATUS(status), after);
        dump_log(s);
        s->pid = -1;
    }
    return s->pid > 0;
}

/* Stop the process group with stop_sig; check the exit and that no process is left */
static void server_stop(struct server* s)
{
    if (s->pid <= 0) {
        running_pgid = 0;
        return;
    }
    pid_t pgid = s->pid;
    kill(-pgid, s->stop_sig);
    int status = 0, done = 0;
    for (int i = 0; i < 500 && !done; ++i) {
        done = waitpid(pgid, &status, WNOHANG) == pgid;
        if (!done)
            sleep_ms(10);
    }
    if (!done) {
        problem("%s did not stop within 5 s after signal %d", s->name, s->stop_sig);
        kill(-pgid, SIGKILL);
        waitpid(pgid, &status, 0);
    } else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0) &&
               !(WIFSIGNALED(status) && WTERMSIG(status) == s->stop_sig)) {
        problem("%s stopped with status 0x%x", s->name, status);
        dump_log(s);
    }
    /* workers of the group: this process reaps them as their subreaper */
    int left = 1;
    for (int i = 0; i < 200 && left; ++i) {
        while (waitpid(-pgid, NULL, WNOHANG) > 0) {
        }
        left = !(kill(-pgid, 0) && errno == ESRCH);
        if (left)
            sleep_ms(10);
    }
    if (left) {
        problem("%s left processes behind", s->name);
        kill(-pgid, SIGKILL);
        while (waitpid(-pgid, NULL, 0) > 0) {
        }
    }
    s->pid = -1;
    running_pgid = 0;
}

static void on_signal(int sig)
{
    (void)sig;
    if (running_pgid > 0)
        kill(-running_pgid, SIGTERM);
    _exit(130);
}

/* ------------------------------------------------------------------------------------------ */
/* cache-server: a get/set mix (9:1) over CACHE_KEYS keys on persistent connections */

static void* cache_client(void* arg)
{
    struct worker* w = arg;
    int port = *(int*)w->ctx;
    struct lines* r = calloc(1, sizeof(*r));
    r->fd = tcp_connect(port);
    pthread_barrier_wait(w->barrier);
    uint64_t seed = 0x9E3779B97F4A7C15ULL * (uint64_t)(w->id + 1);
    char req[64], line[256];
    while (r->fd >= 0 && !atomic_load(&stopping)) {
        uint64_t x = rnd_next(&seed);
        unsigned key = (unsigned)(x % CACHE_KEYS);
        int len = (x >> 32) % 10U == 0
                      ? snprintf(req, sizeof(req), "set 3600 %u %u\n", key, (unsigned)(x >> 40))
                      : snprintf(req, sizeof(req), "get %u\n", key);
        uint64_t t0 = now_ns();
        if (send_all(r->fd, req, (size_t)len) ||
            read_line(r, line, sizeof(line), IO_TIMEOUT_MS) != 1)
            break;
        if (strncmp(line, "ok ", 3)) {
            w->errors++;
            continue;
        }
        record(w, 0, t0);
    }
    if (r->fd < 0 || !atomic_load(&stopping))
        w->errors++;
    if (r->fd >= 0)
        close(r->fd);
    free(r);
    return NULL;
}

static void bench_cache(void)
{
    struct server s = {.name = "cache-server", .stop_sig = SIGTERM, .port = free_port()};
    char port[16];
    snprintf(port, sizeof(port), "%d", s.port);
    char* argv[] = {CACHE_BIN, "-p", port, "-i", "127.0.0.1", "-w", "4", NULL};
    if (server_start(&s, argv, 0))
        return;

    /* every key exists: gets hit */
    struct lines* r = calloc(1, sizeof(*r));
    r->fd = tcp_connect(s.port);
    char line[256];
    for (int k = 0; r->fd >= 0 && k < CACHE_KEYS; ++k) {
        char req[64];
        int len = snprintf(req, sizeof(req), "set 3600 %d %d\n", k, k);
        if (send_all(r->fd, req, (size_t)len) ||
            read_line(r, line, sizeof(line), IO_TIMEOUT_MS) != 1) {
            problem("cache-server: filling the keys failed");
            break;
        }
    }
    if (r->fd >= 0)
        close(r->fd);
    free(r);

    const char* label[2] = {"get/set 9:1, 200 keys", NULL};
    int conc[] = {1, 16};
    for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
        run("cache", label, conc[i], conc[i], cache_client, &s.port);
        server_alive(&s, label[0]);
    }
    server_stop(&s);

    /* SIGTERM must remove the shared memory and the semaphore */
    char text[4096];
    const char* ids = read_log(&s, text, sizeof(text)) ? NULL : strstr(text, "shm: ");
    int shmid = -1, semid = -1;
    if (!ids || sscanf(ids, "shm: %d sem: %d", &shmid, &semid) != 2) {
        problem("cache-server: no \"shm: <id> sem: <id>\" in its output");
        return;
    }
    struct shmid_ds ds;
    if (shmctl(shmid, IPC_STAT, &ds) == 0 && !(ds.shm_perm.mode & SHM_DEST)) {
        problem("cache-server left the shared memory segment %d", shmid);
        shmctl(shmid, IPC_RMID, NULL);
    }
    if (semctl(semid, 0, GETVAL) != -1) {
        problem("cache-server left the semaphore %d", semid);
        semctl(semid, 0, IPC_RMID);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* chat-server: every client sends a line and waits until it comes back; all lines reach all
 * clients. The row counts delivered lines; latency is send -> receive of every delivered line. */

static void* chat_client(void* arg)
{
    struct worker* w = arg;
    int port = *(int*)w->ctx;
    struct lines* r = calloc(1, sizeof(*r));
    char line[256];
    r->fd = tcp_connect(port);
    if (r->fd >= 0 &&
        (read_line(r, line, sizeof(line), IO_TIMEOUT_MS) != 1 || strcmp(line, "Welcome"))) {
        close(r->fd);
        r->fd = -1;
    }
    pthread_barrier_wait(w->barrier);
    int ok = r->fd >= 0;
    for (unsigned long long seq = 0; ok && !atomic_load(&stopping); ++seq) {
        char req[96];
        int len =
            snprintf(req, sizeof(req), "%d %llu %llu\n", w->id, seq, (unsigned long long)now_ns());
        ok = !send_all(r->fd, req, (size_t)len);
        while (ok) {
            ok = read_line(r, line, sizeof(line), IO_TIMEOUT_MS) == 1;
            int id;
            unsigned long long s, ts;
            if (ok && sscanf(line, "%d %llu %llu", &id, &s, &ts) == 3) {
                record_lat(w, 0, now_ns() - ts);
                if (id == w->id && s == seq)
                    break;
            }
        }
    }
    if (!ok)
        w->errors++;
    if (r->fd >= 0)
        close(r->fd);
    free(r);
    return NULL;
}

static void bench_chat(void)
{
    struct server s = {.name = "chat-server", .stop_sig = SIGTERM, .port = free_port()};
    char port[16];
    snprintf(port, sizeof(port), "%d", s.port);
    char* argv[] = {CHAT_BIN, "-p", port, NULL};
    if (server_start(&s, argv, 0))
        return;
    const char* label[2] = {"line fan-out, delivered lines", NULL};
    int conc[] = {2, 16};
    for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
        run("chat", label, conc[i], conc[i], chat_client, &s.port);
        server_alive(&s, label[0]);
    }
    server_stop(&s);
}

/* ------------------------------------------------------------------------------------------ */
/* dictionary-server: one prefix per connection (the reply ends with EOF). Fresh prefixes are
 * whole dictionary words, each asked once (cache miss: a scan of the file); hot prefixes are
 * DICT_HOT short prefixes asked again and again (cache hits, TTL 10 s). */

static char** words;
static size_t nwords;
static size_t* order;
static atomic_size_t next_word;
static char hot[DICT_HOT][64];

static int load_words(void)
{
    FILE* f = fopen(DICT_FILE, "r");
    if (!f)
        return -1;
    char* line = NULL;
    size_t cap = 0, have = 0;
    while (getline(&line, &cap, f) > 0) {
        size_t n = strcspn(line, " \t\r\n");
        if (!n)
            continue;
        if (nwords == have) {
            have = have ? have * 2U : 65536U;
            char** p = realloc(words, have * sizeof(*p));
            if (!p)
                break;
            words = p;
        }
        words[nwords] = strndup(line, n);
        if (words[nwords])
            ++nwords;
    }
    free(line);
    fclose(f);
    order = malloc((nwords ? nwords : 1U) * sizeof(*order));
    if (!nwords || !order)
        return -1;
    uint64_t seed = 12345;
    for (size_t i = 0; i < nwords; ++i)
        order[i] = i;
    for (size_t i = nwords - 1U; i > 0; --i) {
        size_t j = (size_t)(rnd_next(&seed) % (i + 1U));
        size_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    /* hot prefixes: the first 3 characters (UTF-8) of random words */
    for (int h = 0; h < DICT_HOT; ++h) {
        const char* wd = words[rnd_next(&seed) % nwords];
        size_t b = 0;
        for (int chars = 0; wd[b] && b < sizeof(hot[0]) - 1U; ++b)
            if (((unsigned char)wd[b] & 0xC0U) != 0x80U && ++chars > 3)
                break;
        memcpy(hot[h], wd, b);
        hot[h][b] = '\0';
    }
    return 0;
}

static void free_words(void)
{
    for (size_t i = 0; i < nwords; ++i)
        free(words[i]);
    free(words);
    free(order);
}

static long dict_query(int port, const char* prefix)
{
    char req[128], reply[1024];
    int fd = tcp_connect(port);
    if (fd < 0)
        return -1;
    int len = snprintf(req, sizeof(req), "%s\n", prefix);
    long got = -1;
    if (!send_all(fd, req, (size_t)len) && !shutdown(fd, SHUT_WR))
        got = recv_to_eof(fd, reply, sizeof(reply));
    close(fd);
    return got;
}

struct dict_ctx
{
    int port, hot;
};

static void* dict_client(void* arg)
{
    struct worker* w = arg;
    const struct dict_ctx* c = w->ctx;
    pthread_barrier_wait(w->barrier);
    uint64_t seed = 0x2545F4914F6CDD1DULL * (uint64_t)(w->id + 1);
    while (!atomic_load(&stopping)) {
        const char* prefix = c->hot ? hot[rnd_next(&seed) % DICT_HOT]
                                    : words[order[atomic_fetch_add(&next_word, 1) % nwords]];
        uint64_t t0 = now_ns();
        if (dict_query(c->port, prefix) < 0) {
            w->errors++;
            break;
        }
        record(w, 0, t0);
    }
    return NULL;
}

static void bench_dictionary(void)
{
    if (load_words()) {
        problem("cannot read the dictionary %s", DICT_FILE);
        free_words();
        return;
    }
    /* the server reads the file on every miss: a copy in /tmp keeps a slow checkout
     * filesystem (a mounted Windows drive) out of the numbers */
    const char* dict = tmp_path("mydict.txt");
    if (copy_file(DICT_FILE, dict)) {
        problem("cannot copy %s to %s", DICT_FILE, dict);
        free_words();
        return;
    }
    struct server s = {.name = "dictionary-server", .stop_sig = SIGTERM};
    char* argv[] = {DICT_BIN, "-p", "0", "-i", "127.0.0.1", "-w", "4", "-d", (char*)dict, NULL};
    if (server_start(&s, argv, 1)) {
        free_words();
        return;
    }
    int conc[] = {1, 16};
    for (int hit = 1; hit >= 0; --hit) {
        const char* label[2] = {hit ? "prefix, cache hit" : "prefix, cache miss", NULL};
        struct dict_ctx c = {.port = s.port, .hot = hit};
        for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
            /* cache entries live 10 s: warm the hot prefixes right before each run */
            for (int h = 0; hit && h < DICT_HOT; ++h)
                if (dict_query(s.port, hot[h]) < 0)
                    problem("dictionary-server: no reply to a warm-up prefix");
            run("dictionary", label, conc[i], conc[i], dict_client, &c);
            server_alive(&s, label[0]);
        }
    }
    server_stop(&s);
    free_words();
}

/* ------------------------------------------------------------------------------------------ */
/* http-server: GET of a small html file, a new connection per request */

static void* http_client(void* arg)
{
    struct worker* w = arg;
    int port = *(int*)w->ctx;
    static const char req[] = "GET /index.html HTTP/1.0\r\n\r\n";
    pthread_barrier_wait(w->barrier);
    while (!atomic_load(&stopping)) {
        char reply[512];
        uint64_t t0 = now_ns();
        int fd = tcp_connect(port);
        long got = -1;
        if (fd >= 0 && !send_all(fd, req, sizeof(req) - 1U))
            got = recv_to_eof(fd, reply, sizeof(reply));
        if (fd >= 0)
            close(fd);
        if (got < 0) {
            w->errors++;
            break;
        }
        const char* eol = strchr(reply, '\n');
        const char* ok = strstr(reply, " 200");
        if (strncmp(reply, "HTTP/1.", 7) || !ok || !eol || ok > eol) {
            w->errors++;
            continue;
        }
        record(w, 0, t0);
    }
    return NULL;
}

static void bench_http(void)
{
    const char* www = tmp_path("www");
    if (mkdir(www, 0700)) {
        problem("cannot create %s", www);
        return;
    }
    const char* index = tmp_path("www/index.html");
    FILE* f = fopen(index, "w");
    if (!f) {
        problem("cannot create %s", index);
        return;
    }
    fputs("<!DOCTYPE html>\n<html><head><title>ns-bench</title></head>\n"
          "<body><p>A small page for the benchmark.</p></body></html>\n",
          f);
    fclose(f);

    struct server s = {.name = "http-server", .stop_sig = SIGTERM, .port = free_port()};
    char port[16];
    snprintf(port, sizeof(port), "%d", s.port);
    char* argv[] = {HTTP_BIN, "-d", (char*)www, "-p", port, "-h", "127.0.0.1", "-w", "4", NULL};
    if (server_start(&s, argv, 0))
        return;
    const char* label[2] = {"GET 118 B html, new connection", NULL};
    int conc[] = {1, 16};
    for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
        run("http", label, conc[i], conc[i], http_client, &s.port);
        server_alive(&s, label[0]);
    }
    server_stop(&s);
}

/* ------------------------------------------------------------------------------------------ */
/* proxy-server: ECHO_SIZE-byte round trips to an echo backend in this process, directly and
 * through the proxy */

static int echo_listen_fd = -1;
static atomic_int echo_stop, echo_conns;
static pthread_t echo_thread;

static void* echo_conn(void* arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    while (!atomic_load(&echo_stop)) {
        int p = wait_in(fd, 100);
        if (p == 0)
            continue;
        ssize_t n = p > 0 ? recv(fd, buf, sizeof(buf), 0) : -1;
        if (n <= 0 || send_all(fd, buf, (size_t)n))
            break;
    }
    close(fd);
    atomic_fetch_sub(&echo_conns, 1);
    return NULL;
}

static void* echo_accept(void* arg)
{
    (void)arg;
    while (!atomic_load(&echo_stop)) {
        if (wait_in(echo_listen_fd, 100) != 1)
            continue;
        int fd = accept4(echo_listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_t t;
        atomic_fetch_add(&echo_conns, 1);
        if (pthread_create(&t, NULL, echo_conn, (void*)(intptr_t)fd)) {
            atomic_fetch_sub(&echo_conns, 1);
            close(fd);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

/* @return the port of the echo backend, 0 on error */
static int echo_start(void)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(a);
    echo_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (echo_listen_fd < 0 || bind(echo_listen_fd, (struct sockaddr*)&a, sizeof(a)) ||
        listen(echo_listen_fd, SOMAXCONN) ||
        getsockname(echo_listen_fd, (struct sockaddr*)&a, &len) ||
        pthread_create(&echo_thread, NULL, echo_accept, NULL)) {
        if (echo_listen_fd >= 0)
            close(echo_listen_fd);
        echo_listen_fd = -1;
        return 0;
    }
    return ntohs(a.sin_port);
}

static void echo_end(void)
{
    atomic_store(&echo_stop, 1);
    pthread_join(echo_thread, NULL);
    close(echo_listen_fd);
    for (int i = 0; i < 100 && atomic_load(&echo_conns); ++i)
        sleep_ms(10);
}

static void* echo_client(void* arg)
{
    struct worker* w = arg;
    int fd = tcp_connect(*(int*)w->ctx);
    pthread_barrier_wait(w->barrier);
    char msg[ECHO_SIZE], back[ECHO_SIZE];
    memset(msg, 'x', sizeof(msg));
    while (fd >= 0 && !atomic_load(&stopping)) {
        uint64_t t0 = now_ns();
        if (send_all(fd, msg, sizeof(msg)) || recv_exact(fd, back, sizeof(back)))
            break;
        record(w, 0, t0);
    }
    if (fd < 0 || !atomic_load(&stopping))
        w->errors++;
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void bench_proxy(void)
{
    int echo_port = echo_start();
    if (!echo_port) {
        problem("cannot start the echo backend");
        return;
    }
    struct server s = {.name = "proxy-server", .stop_sig = SIGTERM, .port = free_port()};
    const char* conf = tmp_path("proxy.conf");
    FILE* f = fopen(conf, "w");
    if (!f) {
        problem("cannot create %s", conf);
        echo_end();
        return;
    }
    fprintf(f, "%d,127.0.0.1:%d\n", s.port, echo_port);
    fclose(f);
    char* argv[] = {PROXY_BIN, "-c", (char*)conf, NULL};
    if (!server_start(&s, argv, 0)) {
        const char* direct[2] = {"echo 64 B, direct to the backend", NULL};
        const char* proxied[2] = {"echo 64 B, through the proxy", NULL};
        int conc[] = {1, 16};
        for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
            run("proxy", direct, conc[i], conc[i], echo_client, &echo_port);
            run("proxy", proxied, conc[i], conc[i], echo_client, &s.port);
            server_alive(&s, proxied[0]);
        }
        server_stop(&s);
    }
    echo_end();
}

/* ------------------------------------------------------------------------------------------ */
/* subscribe-server: one publisher publishes to queue 0 (waiting for each "ok"), K subscribers
 * of queue 0 receive every message. Rows: publish -> ok, and publish -> delivery. */

static void* subs_client(void* arg)
{
    struct worker* w = arg;
    int port = *(int*)w->ctx;
    struct lines* r = calloc(1, sizeof(*r));
    char line[256];
    r->fd = tcp_connect(port);
    if (w->id > 0 && r->fd >= 0 &&
        (send_all(r->fd, "subscribe 0\n", 12) ||
         read_line(r, line, sizeof(line), IO_TIMEOUT_MS) != 1 || strcmp(line, "ok <que 0>"))) {
        close(r->fd);
        r->fd = -1;
    }
    pthread_barrier_wait(w->barrier);
    int ok = r->fd >= 0;
    while (ok && !atomic_load(&stopping)) {
        if (w->id == 0) {
            char req[64];
            uint64_t t0 = now_ns();
            int len = snprintf(req, sizeof(req), "publish 0 %llu\n", (unsigned long long)t0);
            ok = !send_all(r->fd, req, (size_t)len) &&
                 read_line(r, line, sizeof(line), IO_TIMEOUT_MS) == 1 &&
                 !strcmp(line, "ok <que 0>");
            if (ok)
                record(w, 0, t0);
        } else {
            int rc = read_line(r, line, sizeof(line), 100);
            ok = rc >= 0;
            const char* ts = strrchr(line, ' ');
            if (rc == 1 && ts)
                record_lat(w, 1, now_ns() - strtoull(ts + 1, NULL, 10));
        }
    }
    if (!ok)
        w->errors++;
    if (r->fd >= 0)
        close(r->fd);
    free(r);
    return NULL;
}

static void bench_subscribe(void)
{
    /* SIGINT: the server leaves its loop in two seconds and exits */
    struct server s = {.name = "subscribe-server", .stop_sig = SIGINT, .port = free_port()};
    char port[16];
    snprintf(port, sizeof(port), "%d", s.port);
    char* argv[] = {SUBS_BIN, "-h", "127.0.0.1", "-p", port, "-q", "4", NULL};
    if (server_start(&s, argv, 0))
        return;
    const char* label[2] = {"publish -> ok, 1 publisher", "publish -> delivery to subscribers"};
    int subs[] = {1, 16};
    for (int i = 0; i < 2 && server_alive(&s, "start"); ++i) {
        run("subscribe", label, subs[i], subs[i] + 1, subs_client, &s.port);
        server_alive(&s, label[1]);
    }
    server_stop(&s);
}

/* ------------------------------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    const char* out_path = NULL;
    int c;
    while ((c = getopt(argc, argv, "t:o:h")) != -1) {
        if (c == 't' && atof(optarg) > 0) {
            bench_seconds = atof(optarg);
        } else if (c == 'o') {
            out_path = optarg;
        } else {
            fprintf(c == 'h' ? stdout : stderr,
                    "usage: %s [-t seconds per scenario] [-o file]\n"
                    "Run from the repository root after `make all`.\n",
                    argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    const char* bins[] = {CACHE_BIN, CHAT_BIN, DICT_BIN, HTTP_BIN, PROXY_BIN, SUBS_BIN};
    for (size_t i = 0; i < sizeof(bins) / sizeof(bins[0]); ++i)
        if (access(bins[i], X_OK)) {
            fprintf(stderr, "ns-bench: no %s; run `make all` in the repository root\n", bins[i]);
            return 2;
        }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* orphaned server workers are reparented here and can be reaped and counted */
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    if (!mkdtemp(tmp_dir)) {
        fprintf(stderr, "ns-bench: cannot create a temporary directory\n");
        return 2;
    }

    bench_cache();
    bench_chat();
    bench_dictionary();
    bench_http();
    bench_proxy();
    bench_subscribe();
    remove_tmp();

    print_table(stdout);
    if (out_path) {
        FILE* f = fopen(out_path, "w");
        if (f) {
            print_table(f);
            fclose(f);
        } else {
            problem("cannot write %s", out_path);
        }
    }
    return problems ? 1 : 0;
}
