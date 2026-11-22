# network-servers

[![CI](https://github.com/favorart/network-servers/actions/workflows/ci.yml/badge.svg)](https://github.com/favorart/network-servers/actions/workflows/ci.yml)

Six small Linux network servers in C and C++, written in 2015 as homework for the Technosphere (Mail.Ru Group) course and brought into shape in 2026: one build, 145 unit and integration tests, every test also under ASan + UBSan in CI.

| Server | Language | I/O model | What it does |
|---|---|---|---|
| [cache-server](cache-server/) | C | libevent, master + worker processes, SysV shm + semaphore | key-value cache with TTL shared by all workers |
| [chat-server](chat-server/) | C, C++ client | epoll (edge-triggered), select client | line-based chat room: every line goes to every client |
| [dictionary-server](dictionary-server/) | C | libevent, pthread work queue | top-10 words of a Russian dictionary by prefix, with a cache |
| [http-server](http-server/) | C | libevent, master + worker processes, fd passing | static HTTP/1.0 server: GET, HEAD, POST; 200, 404 |
| [proxy-server](proxy-server/) | C | libevent bufferevents | TCP proxy that spreads clients over backend servers |
| [subscribe-server](subscribe-server/) | C++ | libevent | publish-subscribe over N message queues |

## Build and test

Linux only (epoll, SysV IPC, `/proc`). Dependencies are system packages:

```sh
sudo apt-get install libevent-dev libcmocka-dev libgtest-dev
make            # every server into <server>/build/
make test       # unit and integration tests
make test SAN=1 # the same under ASan + UBSan, in <server>/build-san/
make dist       # network-servers-<version>-linux-<arch>.tar.gz with all binaries
```

Each server folder has its own `Makefile` with the same targets. Every binary prints its version with `-v`/`--version`; the version comes from the git tag. Releases with the prebuilt binaries are on the [Releases](https://github.com/favorart/network-servers/releases) page.

## How the servers work

The cache and HTTP servers share one design: the master accepts connections and passes each client socket to a worker process over a UNIX socket pair (`SCM_RIGHTS`); the workers run their own libevent loops.

```mermaid
flowchart LR
    C1[client] -->|connect| M[master: accept]
    C2[client] -->|connect| M
    M -->|socket fd over socketpair| W1[worker 1: libevent loop]
    M -->|socket fd over socketpair| W2[worker N: libevent loop]
    W1 <--> S[(SysV shared memory + semaphore: cache table)]
    W2 <--> S
```

The shared table exists only in the cache server; the HTTP workers read files from the served directory. The other servers run one process:

- **chat-server** keeps a client table keyed by socket, reads lines with edge-triggered epoll and appends each complete line to the output buffer of every client; `EPOLLOUT` sends what the socket did not take at once.
- **dictionary-server** reads requests in libevent and hands them to a pool of threads through a queue; a thread looks the prefix up in the cache (TTL 10 s) or scans the dictionary file and keeps the 10 heaviest words.
- **proxy-server** opens a connection to a randomly chosen backend for each client and pumps data both ways; EOF on one side shuts down writing on the other once its buffer is sent.
- **subscribe-server** keeps N queues; a subscriber gets every message published to its queue after it subscribed.

## Protocols and usage

| Server | Run | Request → reply |
|---|---|---|
| cache | `cache-server -p 8080 -w 4` | `set <ttl> <key> <value>` / `get <key>` → `ok <key> <value>` or `error : <text>` |
| chat | `chat-server -p 3100`, `chat-client -p 3100` | a line → the same line to every client; a new client gets `Welcome` |
| dictionary | `dictionary-server -p 8080 -d data/mydict.txt` | a prefix per line → up to 10 words, one per line, heaviest first |
| http | `http-server -d <dir> -p 8080 -w 4` | `GET`/`HEAD`/`POST` → `200` with an html or jpeg file, or `404` |
| proxy | `proxy-server -c proxy.conf` | bytes both ways; the config is `<listen port>,<ip>:<port>,...` ([example](proxy-server/proxy.conf.example)) |
| subscribe | `subscribe-server -p 3131 -q 4` | `publish <num> text` / `subscribe <num>` → `ok <que num>` or `error : <text>` |

More details: [http-server](http-server/README.md), [subscribe-server](subscribe-server/README.md), and `--help` of the cache, chat and dictionary servers.

## What the tests found

The 2015 code compiled only with warnings, and the tests written now failed on it. The main bugs, each fixed with a regression test:

- **chat-server**: clients were indexed by the epoll event slot, not by socket, so lines went to the wrong clients and even to the listening socket; the original course tests failed.
- **http-server**: the end of headers (`\r\n\r\n`) was searched over `sizeof(pointer)` bytes, so no request was recognized; `..` in the URI escaped the served directory.
- **cache-server**: the "shared" cache lived in a static array, so every worker had its own copy; the hash table lost keys after a miss.
- **dictionary-server**: the cache hashed garbage after the key's NUL and had no mutex; the top-10 was never sorted.
- **proxy-server**: EOF from either side always shut down the client socket; a config without backends divided by zero.
- **subscribe-server**: after the first error every later reply was an error.

## Limitations

- Linux only; no Windows or macOS build.
- Buffers of slow clients grow without a limit (chat, proxy); no timeouts for incomplete requests.
- http-server has no request bodies, percent-decoding or 400 replies; the dictionary server reads the dictionary file on every cache miss.
- The multi-process mode of subscribe-server was never finished in 2015 and is removed: its queues would live in each process separately.
- `--help` is missing in the http, proxy and subscribe servers; they print usage on a wrong option.

## History

Each server was a separate repository; this one keeps their full histories in the subfolders. The servers were written by the author during the Technosphere course in 2015; in 2026 they got one build, CI with releases, English comments, warnings as errors, the bug fixes above and the tests.
