#include <cerrno>
#include <csignal>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "subs_server.h"

//-----------------------------------------
int main(int argc, char** argv)
{
    int result = 0;

    struct server_config server_conf;
    int MasterSocket = -1;
    struct event* sigint_event = NULL;
    //-------------------------------------------
    /* Read the server configuration from the command-line parameters */
    if (parse_console_parameters(argc, argv, &server_conf)) {
        fprintf(stderr, "%s\n", strerror(errno));
        result = 1;
        goto MARK_FREE;
    }

    server_config_print(&server_conf, stdout);
    {
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

        struct sockaddr_in SockAddr = {};
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
    }

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

    sigint_event = evsignal_new(server_conf.base, SIGINT, subs_sigint_cb, (void*)&server_conf);
    if (!sigint_event || event_add(sigint_event, NULL) < 0) {
        fprintf(stderr, "Could not create/add the SIGINT signal event.\n");
        result = 1;
        goto MARK_FREE;
    }

    //-------------------------------------------
    {
        /* Create a new event that reacts to reads from the server socket - a new client
         * connecting; subs_accept_cb() handles it */
        struct event ev;
        event_set(&ev, MasterSocket, EV_READ | EV_PERSIST, subs_accept_cb, &server_conf);
        /* Attach the event to my event base */
        event_base_set(server_conf.base, &ev);
        /* Add server event without timeout */
        event_add(&ev, NULL);

        /* Dispatch events */
        event_base_loop(server_conf.base, 0);
        event_del(&ev);
    }
    //-------------------------------------------

MARK_FREE:
    //-------------------------------------------
    /* Free the clients and events while their event base still exists */
    subs_clients_free(&server_conf);
    if (sigint_event)
        event_free(sigint_event);
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
