#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <event2/event.h>

#include "srv.h"

//-----------------------------------------
int main(int argc, char** argv)
{
    int result = 0;
    int MasterSocket = -1;
    struct event* ev = NULL;
    //-------------------------------------------
    /* Read the server configuration from the command-line parameters */
    struct server_config server_conf = {0};
    int parsed = parse_console_parameters(argc, argv, &server_conf);
    if (parsed)
        return (parsed > 0) ? 0 : 2;
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
    /* Dispatch events */
    event_base_loop(server_conf.base, 0);
    //-------------------------------------------

MARK_FREE:
    //-------------------------------------------
    if (ev)
        event_free(ev);
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
