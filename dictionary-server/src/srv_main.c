#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <event2/event.h>

#include "srv.h"

//-----------------------------------------
/** @brief Signal callback: stop the main event loop (@p arg is its event base). */
static void srv_signal_cb(evutil_socket_t sig, short ev, void* arg)
{
    (void)ev;
    printf("got signal %d, shutting down\n", (int)sig);
    event_base_loopexit((struct event_base*)arg, NULL);
}
//-----------------------------------------
int main(int argc, char** argv)
{
    int result = 0;
    int MasterSocket = -1;
    struct event* ev = NULL;
    struct event* ev_term = NULL;
    struct event* ev_int = NULL;
    //-------------------------------------------
    /* Read the server configuration from the command-line parameters */
    struct server_config server_conf = {0};
    int parsed = parse_console_parameters(argc, argv, &server_conf);
    if (parsed)
        return (parsed > 0) ? 0 : 2;
    server_config_print(&server_conf, stdout);
    /* A client that closes early must not kill the server with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* Create the listening socket */
    MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (MasterSocket == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }

    int so_reuseaddr = 1; /* Manipulate the flags set on the socket */
    if (setsockopt(MasterSocket, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr))) {
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
    /* Report the real port (useful with -p 0) */
    socklen_t addr_len = sizeof(SockAddr);
    if (!getsockname(MasterSocket, (struct sockaddr*)&SockAddr, &addr_len))
        server_conf.port = ntohs(SockAddr.sin_port);

    //-------------------------------------------
    /* Initialize new event_base to do not overlay the global context */
    server_conf.base = event_base_new();
    if (!server_conf.base) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }
    //-------------------------------------------
    /* A read event on the listening socket means a new client: srv_accept_cb() accepts it */
    ev = event_new(server_conf.base, MasterSocket, EV_READ | EV_PERSIST, srv_accept_cb,
                   &server_conf);
    if (!ev || event_add(ev, NULL)) {
        fprintf(stderr, "listener event failed\n");
        result = 1;
        goto MARK_FREE;
    }
    /* SIGTERM and SIGINT stop the loop; the workers are joined in server_config_free() */
    ev_term = evsignal_new(server_conf.base, SIGTERM, srv_signal_cb, server_conf.base);
    ev_int = evsignal_new(server_conf.base, SIGINT, srv_signal_cb, server_conf.base);
    if (!ev_term || !ev_int || event_add(ev_term, NULL) || event_add(ev_int, NULL)) {
        fprintf(stderr, "signal event failed\n");
        result = 1;
        goto MARK_FREE;
    }
    printf("listening on %s:%u\n", server_conf.ip, server_conf.port);
    fflush(stdout);
    /* Dispatch events */
    event_base_loop(server_conf.base, 0);
    //-------------------------------------------

MARK_FREE:
    //-------------------------------------------
    if (ev)
        event_free(ev);
    if (ev_term)
        event_free(ev_term);
    if (ev_int)
        event_free(ev_int);
    if (server_conf.base)
        event_base_free(server_conf.base);

    if (MasterSocket != -1) {
        shutdown(MasterSocket, SHUT_RDWR);
        close(MasterSocket);
    }

    server_config_free(&server_conf);
    //-------------------------------------------
    return result;
}
//-----------------------------------------
