#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>

#include "subs_server.h"

//-----------------------------------------
void subs_ac_err_cb(evutil_socket_t fd, short ev, void* arg)
{
    (void)fd;
    (void)ev;
    struct server_config* server_conf = (struct server_config*)arg;
    //----------------------------------------------------------------------
    int err = EVUTIL_SOCKET_ERROR();
    fprintf(stderr,
            "Got an error %d (%s) on the listener. "
            "Shutting down.\n",
            err, evutil_socket_error_to_string(err));
    //----------------------------------------------------------------------
    event_base_loopexit(server_conf->base, NULL);
}
void subs_accept_cb(evutil_socket_t fd, short ev, void* arg)
{
    (void)ev;
    struct server_config* server_conf = (struct server_config*)arg;
    //----------------------------------------------------------------------
    int SlaveSocket = accept(fd, 0, 0);
    if (SlaveSocket == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        return;
    }
    //----------------------------------------------------------------------
    set_nonblock(SlaveSocket);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection accepted\n");
#endif
    //----------------------------------------------------------------------
    /* Making the new client */
    struct client* Client = (struct client*)calloc(1U, sizeof(*Client));
    if (!Client) {
        fprintf(stderr, "%s\n", strerror(errno));
        return;
    }

    Client->base = server_conf->base;
    Client->config = server_conf;
    Client->queues_subscribed = new std::set<size_t>();
    server_conf->clients.push_back(Client);

    /* Create new bufferized event, linked with client's socket */
    Client->b_ev = bufferevent_socket_new(Client->base, SlaveSocket, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(Client->b_ev, subs_read_cb, NULL, subs_error_cb, Client);
    /* Ready to get data */
    bufferevent_enable(Client->b_ev, EV_READ | EV_WRITE | EV_PERSIST);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("event created\n");
#endif // _DEBUG
}
//-----------------------------------------
void subs_error_cb(struct bufferevent* b_ev, short events, void* arg)
{
    (void)b_ev;
    /*   The header <event2/bufferevent.h> defines the flags:
     *   the connection was established (BEV_EVENT_CONNECTED 0x80),
     *   end of file was detected (BEV_EVENT_EOF 0x10),
     *   a critical failure happened (BEV_EVENT_ERROR 0x20),
     *   the given interval expired (BEV_EVENT_TIMEOUT 0x40).
     */
    if (events & BEV_EVENT_CONNECTED) {
#ifdef _DEBUG
        printf("connection established\n");
#endif
    } else if (events & BEV_EVENT_EOF) {
        struct client* Client = (struct client*)arg;

#ifdef _DEBUG
        printf("got a close. length = %zu\n",
               evbuffer_get_length(bufferevent_get_input(Client->b_ev)));
#endif // _DEBUG

        struct server_config* server_conf = Client->config;
        server_conf->clients.remove(Client);
        delete Client->queues_subscribed;

        shutdown(bufferevent_getfd(Client->b_ev), SHUT_RDWR);
        bufferevent_free(Client->b_ev);
        free(Client);

#ifdef _DEBUG
        printf("connection closed\n");
#endif // _DEBUG

    } else if (events & BEV_EVENT_ERROR) {
        struct client* Client = (struct client*)arg;

        struct server_config* server_conf = Client->config;
        server_conf->clients.remove(Client);
        delete Client->queues_subscribed;

        fprintf(stderr, "Error from bufferevent: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

        bufferevent_free(Client->b_ev);
        free(Client);

#ifdef _DEBUG
        printf("connection closed");
#endif // _DEBUG
    }
}
void subs_clients_free(struct server_config* conf)
{
    for (client* Client : conf->clients) {
        delete Client->queues_subscribed;
        bufferevent_free(Client->b_ev);
        free(Client);
    }
    conf->clients.clear();
}
void subs_read_cb(struct bufferevent* b_ev, void* arg)
{
#ifdef _DEBUG
    printf("data reseived\n");
#endif // _DEBUG

    /* This callback is invoked when there is data to read on b_ev_read */
    struct client* Client = (struct client*)arg;

    struct evbuffer* buf_in = bufferevent_get_input(b_ev);
    struct evbuffer* buf_out = bufferevent_get_output(b_ev);

    /* The main call */
    subscribe_handling(Client, buf_in, buf_out);
    Client->config->queues_manager->broadcast(Client->config);

#ifdef _DEBUG
    printf("response ready\n");
#endif // _DEBUG
}
//-----------------------------------------
void subs_sigint_cb(evutil_socket_t sig, short events, void* arg)
{
    (void)sig;
    (void)events;
    struct server_config* conf = (struct server_config*)arg;
    struct timeval delay = {2, 0};

    printf("\nCaught an interrupt signal;\n");
    //-------------------------------
    printf("Exiting cleanly in two seconds.\n");
    event_base_loopexit(conf->base, &delay);
}
