#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cache_error.h"
#include "cache_hash.h"
#include "cache.h"

//-----------------------------------------
struct server_config server_conf = {0};

/* SIGTERM or SIGINT in the master: leave the event loop and shut down cleanly */
static void cache_signal_cb(evutil_socket_t sig, short ev, void* arg)
{
    (void)sig;
    (void)ev;
    event_base_loopexit((struct event_base*)arg, NULL);
}

int main(int argc, char** argv)
{
    int result = 0;

    //-------------------------------------------
    /* Read the server configuration from the command-line parameters */
    switch (parse_console_parameters(argc, argv, &server_conf)) {
    case PARSE_RUN:
        break;
    case PARSE_EXIT:
        return EXIT_SUCCESS;
    default:
        return EXIT_FAILURE;
    }
    /* A client or a worker that went away must not kill the process on write */
    signal(SIGPIPE, SIG_IGN);

    //-------------------------------------------
    /* Master Process: the cache is created before the workers, so they share it */
    hashtable* ht = malloc(sizeof(struct hash_table));
    if (!ht) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    hashtable_init(ht, CACHELINES, CACHE_SIZE);
    server_conf.ht = ht;

    server_config_init(&server_conf);
    bool is_master = !server_conf.myself;

    int MasterSocket = -1;
    struct event* ev_term = NULL;
    struct event* ev_int = NULL;
    struct event ev;
    bool ev_added = false;

    //-------------------------------------------
    /*  A server that serves several clients at the same time.
     *  It needs a listening socket that can accept connections
     *  asynchronously, a connection handler and the client logic.
     */

    /* Initialize new event_base to do not overlay the global context */
    server_conf.base = event_base_new();
    if (!server_conf.base) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }

    /* Master Process */
    if (is_master) {
        /* Catch the signals before the socket accepts anybody */
        ev_term = evsignal_new(server_conf.base, SIGTERM, cache_signal_cb, server_conf.base);
        ev_int = evsignal_new(server_conf.base, SIGINT, cache_signal_cb, server_conf.base);
        if (!ev_term || !ev_int || evsignal_add(ev_term, NULL) || evsignal_add(ev_int, NULL)) {
            fprintf(stderr, "cannot set the signal handlers\n");
            result = 1;
            goto MARK_FREE;
        }

        server_config_print(&server_conf, stdout);
        fflush(stdout);

        /* Create the listening socket */
        if (-1 == (MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
            fprintf(stderr, "%s\n", strerror(errno));
            result = 1;
            goto MARK_FREE;
        }

        int so_reuseaddr = 1; /* Manipulate the flags set on the socket */
        if (setsockopt(MasterSocket, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr,
                       sizeof(so_reuseaddr))) {
            fprintf(stderr, "%s\n", strerror(errno));
            result = 1;
            goto MARK_FREE;
        }

        struct sockaddr_in SockAddr = {0};
        SockAddr.sin_family = AF_INET;
        SockAddr.sin_port = htons(server_conf.port);
        SockAddr.sin_addr.s_addr = inet_addr(server_conf.ip);

        int Result = bind(MasterSocket, (struct sockaddr*)&SockAddr, sizeof(SockAddr));
        if (Result == -1) {
            fprintf(stderr, "%s\n", strerror(errno));
            result = 1;
            goto MARK_FREE;
        }

        set_nonblock(MasterSocket);

        Result = listen(MasterSocket, SOMAXCONN);
        if (Result == -1) {
            fprintf(stderr, "%s\n", strerror(errno));
            result = 1;
            goto MARK_FREE;
        }

        /* Create a new event, that react to read drom server's socket - event of connecting the new
         * client */
        /* Set connection callback (on_connect()) to read event on server socket */
        event_set(&ev, MasterSocket, EV_READ | EV_PERSIST, cache_accept_cb, &server_conf);
    } else {
        /* Worker: wait for client sockets from the master */
        event_set(&ev, server_conf.myself->fd, EV_READ | EV_PERSIST, cache_connect_cb,
                  &server_conf);
    }
    /* Attach the event to my event base and add it without timeout */
    event_base_set(server_conf.base, &ev);
    event_add(&ev, NULL);
    ev_added = true;

    /* Dispatch events */
    event_base_loop(server_conf.base, 0);
    //-------------------------------------------

MARK_FREE:
    //-------------------------------------------
    if (ev_added)
        event_del(&ev);
    if (ev_term)
        event_free(ev_term);
    if (ev_int)
        event_free(ev_int);
    if (server_conf.base)
        event_base_free(server_conf.base);
    server_conf.base = NULL;

    if (MasterSocket != -1) {
        shutdown(MasterSocket, SHUT_RDWR);
        close(MasterSocket);
    }

    /* The master closes the workers' sockets and waits for them to exit */
    server_config_free(&server_conf);
    /* Only the master owns the shared memory and the semaphore */
    if (is_master)
        hashtable_free(ht);
    free(ht);
    server_conf.ht = NULL;
    //-------------------------------------------
    return result;
}
