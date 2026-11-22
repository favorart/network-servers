# Publish-Subscribe server

A server with N message queues on libevent (Technosphere.mail.ru, 2015). Clients publish messages to the queues or subscribe to listen to them.

## Build and test

Linux only; needs libevent and, for the tests, GoogleTest.

```sh
make                 # build/subscribe-server
make test            # unit and integration tests
make test SAN=1      # the same with ASan and UBSan, in build-san/
```

## Run

```sh
build/subscribe-server -h 127.0.0.1 -p 3131 -q 4
```

- `-h/--ip` — listening address, default `0.0.0.0`.
- `-p/--port` — listening port, default `3131`.
- `-q/--queues` — number of queues (1..255), default `2`.
- `-v/--version` — print the version and exit.

`Ctrl+C` (SIGINT) stops the server in two seconds. `-d`, `-w` and `-c` are accepted and ignored.

## Protocol

A client sends one command per line; `<num>` is a queue number from 0:

- `publish <num> message` — put the message into a queue.
- `subscribe <num>` — receive the messages published to a queue from now on.

The server replies `ok <que num>` or `error : <text>`. A subscriber gets each message as `<que num>  message`.
