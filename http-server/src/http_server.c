#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include "http_error.h"
#include "http_server.h"
#include "http_request.h"

struct http_server_config server_conf = {0};
//-----------------------------------------
/*   Working with buffered input/output consists of the following steps:
 *   -  initialize the bufferevent structure by calling bufferevent_new (),
 *      giving the callback functions for the events
 *      read (optional), write (optional) and error (required);
 *   -  Bind this event to the local event base (event_base) by calling bufferevent_base_set ();
 *   -  Enable or disable the read / write handlers by calling bufferevent_enable () /
 * bufferevent_disable ();
 *   -  Read and write by calling bufferevent_read () and bufferevent_write () respectively.
 *   Note that the read handler is called after the whole packet has accumulated in the buffer
 *   or the connection is closed, and the write handler - when the outgoing buffer is sent either
 * fully, or down to the given mark (watermark).
 */

//-----------------------------------------
void http_accept_cb(evutil_socket_t fd, short ev, void* arg)
{
    (void)ev;
    (void)arg;
    //----------------------------------------------------------------------
    int SlaveSocket = accept(fd, 0, 0);
    if (SlaveSocket == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        return;
    }
    //----------------------------------------------------------------------
    set_nonblock(SlaveSocket);
    //----------------------------------------------------------------------
    wc_t r = (rand() % server_conf.workers);
    child_worker_send(&server_conf.child_workers[r], CHWMSG_TASK, SlaveSocket);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection accepted\n");
#endif
}
//-----------------------------------------
/** @brief Free the client: its request, its bufferevent (closes the socket) and itself. */
static void http_client_free(struct client* Client)
{
    http_request_free(&Client->request);
    if (Client->b_ev)
        bufferevent_free(Client->b_ev);
    free(Client);
}
/** @brief No more requests on this connection: close it once the output is sent. */
static void http_client_close(struct client* Client)
{
    Client->closing = true;
    if (!evbuffer_get_length(bufferevent_get_output(Client->b_ev)))
        http_write_cb(Client->b_ev, Client);
}
//-----------------------------------------
void http_connect_cb(evutil_socket_t fd, short ev, void* arg)
{
    (void)fd;
    (void)ev;
    struct event_base* base = (struct event_base*)arg;
    //----------------------------------------------------------------------
    evutil_socket_t SlaveSocket = -1;
    chwmsg_enum msg = CHWMSG_NONE;
    if (child_worker_recv(server_conf.myself, &msg, &SlaveSocket) <= 0 || msg == CHWMSG_TERM) {
        /* The master has gone or asks to stop */
        if (SlaveSocket != -1)
            close(SlaveSocket);
        event_base_loopexit(base, NULL);
        return;
    }
    if (msg != CHWMSG_TASK || SlaveSocket == -1) {
        if (SlaveSocket != -1)
            close(SlaveSocket);
        return;
    }
    //----------------------------------------------------------------------
    /* Making the new client */
    struct client* Client = (struct client*)calloc(1U, sizeof(*Client));
    if (!Client || http_request_init(&Client->request)) {
        fprintf(stderr, "%s\n", strerror(errno));
        if (Client)
            http_client_free(Client);
        close(SlaveSocket);
        return;
    }
    Client->base = base;
    //----------------------------------------------------------------------
    /* Create new bufferized event, linked with client's socket */
    Client->b_ev = bufferevent_socket_new(base, SlaveSocket, BEV_OPT_CLOSE_ON_FREE);
    if (!Client->b_ev) {
        my_errno = HTTP_ERR_LIBEV;
        fprintf(stderr, "%s\n", strmyerror());
        http_client_free(Client);
        close(SlaveSocket);
        return;
    }
    bufferevent_setcb(Client->b_ev, http_read_cb, http_write_cb, http_error_cb, Client);
    /* Ready to get data */
    bufferevent_enable(Client->b_ev, EV_READ | EV_WRITE);

#ifdef _DEBUG
    printf("connection to %d\n", server_conf.workers);
#endif

#ifdef HTTP_SYSLOG
    syslog(LOG_INFO, "connection to %d\n", server_conf.workers);
#endif
}
//-----------------------------------------
void http_error_cb(struct bufferevent* b_ev, short events, void* arg)
{
    struct client* Client = (struct client*)arg;

    if (events & BEV_EVENT_EOF) {
#ifdef _DEBUG
        printf("Got a close. Len = %zu\n", evbuffer_get_length(bufferevent_get_output(b_ev)));
#endif
#ifdef HTTP_SYSLOG
        syslog(LOG_INFO, "Got a close. Len = %zu\n",
               evbuffer_get_length(bufferevent_get_output(b_ev)));
#endif
        Client->eof = true;
        /* The response is still being sent: http_write_cb() frees the client after it */
        if (Client->closing && evbuffer_get_length(bufferevent_get_output(b_ev)) &&
            !(events & BEV_EVENT_ERROR))
            return;
    }

    if (events & BEV_EVENT_ERROR) {
        fprintf(stderr, "Error from bufferevent: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

#ifdef HTTP_SYSLOG
        syslog(LOG_ERR, "Error from bufferevent: '%s'\n",
               evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
#endif
    }

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
        http_client_free(Client);
}
void http_write_cb(struct bufferevent* b_ev, void* arg)
{
    struct client* Client = (struct client*)arg;
    if (!Client->closing || evbuffer_get_length(bufferevent_get_output(b_ev)))
        return;

    if (Client->eof) {
        http_client_free(Client);
        return;
    }
    /* Half-close: the peer gets EOF after the response; whatever it still sends is
     * read and dropped until it closes too (or the read timeout expires), so the
     * response is not lost to a reset.
     */
    shutdown(bufferevent_getfd(b_ev), SHUT_WR);
    struct timeval linger = {HTTP_LINGER_SEC, 0};
    bufferevent_set_timeouts(b_ev, &linger, NULL);
}
void http_read_cb(struct bufferevent* b_ev, void* arg)
{
    struct client* Client = (struct client*)arg;
    struct evbuffer* input = bufferevent_get_input(b_ev);
    /* This callback is invoked when there is data to read on b_ev_read */

#ifdef _DEBUG
    printf("request received\n");
#endif // _DEBUG

#ifdef HTTP_SYSLOG
    syslog(LOG_INFO, "request received\n");
#endif

    if (Client->closing) {
        evbuffer_drain(input, evbuffer_get_length(input));
        return;
    }

    switch (http_request_parse(&Client->request, input)) {
    case HTTP_PARSE_MORE:
        return;
    case HTTP_PARSE_DONE:
        http_response_make(&Client->request, bufferevent_get_output(b_ev));
#ifdef _DEBUG
        printf("response ready\n");
#endif // _DEBUG
        break;
    case HTTP_PARSE_FAIL:
        break;
    }
    /* "Connection: close": one request per connection */
    evbuffer_drain(input, evbuffer_get_length(input));
    http_client_close(Client);
}
//-----------------------------------------
