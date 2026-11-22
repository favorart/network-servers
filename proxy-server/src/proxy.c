#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "proxy_error.h"
#include "proxy.h"

struct server_config server_conf = {0};
//-----------------------------------------
int proxy_choose_server(const srv_conf* conf)
{
    if (!conf->remote_servers_count)
        return -1;
    return rand() % conf->remote_servers_count;
}
//-----------------------------------------
void proxy_ac_err_cb(evutil_socket_t fd, short ev, void* arg)
{
    struct event_base* base = (struct event_base*)arg;
    (void)fd;
    (void)ev;

    int err = EVUTIL_SOCKET_ERROR();
    fprintf(stderr,
            "Got an error %d (%s) on the listener. "
            "Shutting down.\n",
            err, evutil_socket_error_to_string(err));

    event_base_loopexit(base, NULL);
}
void proxy_accept_cb(evutil_socket_t fd, short ev, void* arg)
{
    /* A new connection! Set up a bufferevent for it */
    struct event_base* base = (struct event_base*)arg;
    (void)ev;
    //----------------------------------------------------------------------
    int SlaveSocket = accept(fd, 0, 0);
    if (SlaveSocket == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        return;
    }

    set_nonblock(SlaveSocket);

#ifdef _DEBUG
    printf("connection accepted\n");
#endif
    //----------------------------------------------------------------------
    /* Making the new client */
    struct client* Client = (struct client*)calloc(1U, sizeof(*Client));
    if (!Client) {
        fprintf(stderr, "%s\n", strerror(errno));
        close(SlaveSocket);
        return;
    }

    Client->flag_close = 0;
    Client->base = base;
    //----------------------------------------------------------------------
    /* Random server choosing */
    int srv_id = proxy_choose_server(&server_conf);
    if (srv_id < 0) {
        fprintf(stderr, "No remote servers configured\n");
        free(Client);
        close(SlaveSocket);
        return;
    }
    //----------------------------------------------------------------------
    struct sockaddr_in SockAddr = {0};
    SockAddr.sin_family = AF_INET;
    SockAddr.sin_port = htons(server_conf.remote_servers[srv_id].port);
    SockAddr.sin_addr.s_addr = inet_addr(server_conf.remote_servers[srv_id].ip);

    Client->server = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(Client->server, proxy_read_cb, NULL, proxy_error_cb, Client);
    //----------------------------------------------------------------------
    /*  The arguments are the same as those of the standard connect() system call.
     *  If no socket was set in the bufferevent before, the call creates a new
     *  stream socket and makes it non-blocking. A connection is started,
     *  which may succeed (0) or fail (-1).
     */
    if (0 >
        bufferevent_socket_connect(Client->server, (struct sockaddr*)&SockAddr, sizeof(SockAddr))) {
        // fprintf (stderr, "%s\n", strerror (errno));
        fprintf(stderr, "Error while connecting to server: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

        /* The connection attempt failed */
        bufferevent_free(Client->server);
        free(Client);
        close(SlaveSocket);

        return;
    }

#ifdef _DEBUG
    printf("connection to server established\n");
#endif
    /*  BEV_OPT_CLOSE_ON_FREE    - the socket is closed automatically when the bufferevent object is
     * freed. BEV_OPT_THREADSAFE       - protects operations on the data with locks.
     *  BEV_OPT_DEFER_CALLBACKS  - prevents stack overflow when a series of interdependent callbacks
     * is generated, by keeping a queue of deferred callbacks. BEV_OPT_UNLOCK_CALLBACKS - the
     * bufferevent object's callbacks run without taking locks.
     */
    //----------------------------------------------------------------------
    /* Create new bufferized event, linked with client's socket */
    Client->b_ev = bufferevent_socket_new(base, SlaveSocket, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(Client->b_ev, proxy_read_cb, NULL, proxy_error_cb, Client);
    /* Ready to get data */
    bufferevent_enable(Client->b_ev, EV_READ | EV_WRITE | EV_PERSIST);
    bufferevent_enable(Client->server, EV_READ | EV_WRITE | EV_PERSIST);
    //----------------------------------------------------------------------
    // bufferevent_setwatermark (Client->b_ev,   EV_WRITE, SRV_BUF_LOWMARK, 0);
    // bufferevent_setwatermark (Client->server, EV_WRITE, SRV_BUF_LOWMARK, 0);
}
//-----------------------------------------
/** @brief The other side of the pair. */
static struct bufferevent* proxy_peer(struct client* Client, struct bufferevent* b_ev)
{
    return (Client->b_ev == b_ev) ? Client->server : Client->b_ev;
}
/** @brief Free the pair after both sides sent EOF and both outputs are flushed. */
static void proxy_try_close(struct client* Client)
{
    if (Client->flag_close < 2 || evbuffer_get_length(bufferevent_get_output(Client->b_ev)) ||
        evbuffer_get_length(bufferevent_get_output(Client->server)))
        return;

    bufferevent_free(Client->server);
    bufferevent_free(Client->b_ev);

#ifdef _DEBUG
    printf("connection closed\n");
#endif // _DEBUG

    free(Client);
}
/** @brief The peer sent EOF and @p b_ev output is flushed: pass the EOF on. */
static void proxy_drained_cb(struct bufferevent* b_ev, void* arg)
{
    struct client* Client = (struct client*)arg;

    bufferevent_setcb(b_ev, proxy_read_cb, NULL, proxy_error_cb, Client);
    shutdown(bufferevent_getfd(b_ev), SHUT_WR);
    proxy_try_close(Client);
}
void proxy_error_cb(struct bufferevent* b_ev, short events, void* arg)
{
    /*   An event structure becomes a timeout trigger with the timeout_set() function.
     *   Of course the trigger has to be "armed" - the timeout_add() function does that.
     *   To "disarm" the trigger and cancel the timeout event, use
     *   the timeout_del() function.
     */

    /*
     *   The header <event2/bufferevent.h> defines flags
     *   that tell why the exceptional condition happened:
     *   1. The event happened while reading (BEV_EVENT_READING 0x01) or writing (BEV_EVENT_WRITING
     * 0x02).
     *   2. End of file was found (BEV_EVENT_EOF 0x10), a critical failure happened (BEV_EVENT_ERROR
     * 0x20), the given interval expired (BEV_EVENT_TIMEOUT 0x40), the connection was established
     * (BEV_EVENT_CONNECTED 0x80).
     */
    if (events & BEV_EVENT_CONNECTED) {
#ifdef _DEBUG
        printf("connection established\n");
#endif
    } else if (events & BEV_EVENT_EOF) {
        struct client* Client = (struct client*)arg;

        struct bufferevent* peer = proxy_peer(Client, b_ev);

        struct evbuffer* buf_in = bufferevent_get_input(b_ev);
        struct evbuffer* buf_out = bufferevent_get_output(peer);

        /* Copy all the data from the input buffer to the output buffer */
        evbuffer_remove_buffer(buf_in, buf_out, evbuffer_get_length(buf_in));

#ifdef _DEBUG
        printf("Got a close. Length = %zu\n", evbuffer_get_length(buf_out));
#endif // _DEBUG

        ++Client->flag_close;
        /* Half-close the peer once everything queued for it is written */
        if (evbuffer_get_length(buf_out))
            bufferevent_setcb(peer, proxy_read_cb, proxy_drained_cb, proxy_error_cb, Client);
        else {
            shutdown(bufferevent_getfd(peer), SHUT_WR);
            proxy_try_close(Client);
        }
    } else if (events & BEV_EVENT_ERROR) // (BEV_EVENT_EOF | BEV_EVENT_ERROR)
    {
        struct client* Client = (struct client*)arg;

        fprintf(stderr, "Error from bufferevent: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

        bufferevent_free(Client->server);
        bufferevent_free(Client->b_ev);

#ifdef _DEBUG
        printf("connection closed");
#endif // _DEBUG

        free(Client);
    }
}
void proxy_read_cb(struct bufferevent* b_ev, void* arg)
{

#ifdef _DEBUG
    printf("data reseived\n");
#endif // _DEBUG

    /* This callback is invoked when there is data to read on b_ev_read */
    struct client* Client = (struct client*)arg;

    struct evbuffer* buf_in = bufferevent_get_input(b_ev);
    struct evbuffer* buf_out = bufferevent_get_output(proxy_peer(Client, b_ev));

    /* Copy all the data from the input buffer to the output buffer. */
    evbuffer_remove_buffer(buf_in, buf_out, evbuffer_get_length(buf_in));

#ifdef _DEBUG
    printf("response ready\n");
#endif // _DEBUG
}
//-----------------------------------------
