#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "proxy_error.h"
#include "proxy.h"

//-----------------------------------------
int main(int argc, char** argv)
{
    int result = 0;
    int MasterSocket = -1;
    struct event_base* base = NULL;

    /* A write to a closed peer must fail with EPIPE, not kill the proxy */
    signal(SIGPIPE, SIG_IGN);

    /* Read the server configuration from the command line parameters */
    int parsed = parse_console_parameters(argc, argv, &server_conf);
    if (parsed == SRV_PARSE_VERSION)
        return 0;
    if (parsed) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }
    server_config_print(&server_conf, stdout);

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

    /*  A server that serves several clients at the same time.
     *  It needs a listening socket that can accept connections
     *  asynchronously, a connection handler and the client logic.
     */

    /* Initialize new event_base to do not overlay the global context */
    base = event_base_new();
    if (!base) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }
    /* Init events */
    struct event ev;
    /* Create a new event, that react to read drom server's socket - event of connecting the new
     * client */
    /* Set connection callback (on_connect()) to read event on server socket */
    event_set(&ev, MasterSocket, EV_READ | EV_PERSIST, proxy_accept_cb /* on_accept */, base);
    /* Attach ev_connect event to my event base */
    event_base_set(base, &ev);
    /* Add server event without timeout */
    event_add(&ev, NULL);
    /* Dispatch events */
    event_base_loop(base, 0);
    //-------------------------------------------

MARK_FREE:
    //-------------------------------------------
    if (base)
        event_base_free(base);

    if (MasterSocket != -1) {
        shutdown(MasterSocket, SHUT_RDWR);
        close(MasterSocket);
    }

    server_config_free(&server_conf);
    //-------------------------------------------
    return result;
}
