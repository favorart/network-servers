#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cache_error.h"
#include "cache_hash.h"
#include "cache.h"

//-----------------------------------------
void cache_accept_cb(evutil_socket_t fd, short ev, void* arg)
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
    wc_t r = (rand() % server_conf->workers);
    if (0 >= child_worker_send(&server_conf->child_workers[r], CHWMSG_TASK, SlaveSocket))
        fprintf(stderr, "cannot pass the connection to worker %u\n", r);
    /* The worker has its own copy now (or the connection is dropped) */
    close(SlaveSocket);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection accepted\n");
#endif
    //----------------------------------------------------------------------
}
//-----------------------------------------
void cache_connect_cb(evutil_socket_t fd, short ev, void* arg)
{
    (void)fd;
    (void)ev;
    struct server_config* server_conf = (struct server_config*)arg;
    //----------------------------------------------------------------------
    evutil_socket_t SlaveSocket = -1;
    chwmsg_enum msg = CHWMSG_NONE;
    if (0 > child_worker_recv(server_conf->myself, &msg, &SlaveSocket) || msg == CHWMSG_TERM) {
        /* The master closed its end or asked to stop */
        if (SlaveSocket != -1)
            close(SlaveSocket);
        event_base_loopexit(server_conf->base, NULL);
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
    if (!Client) {
        fprintf(stderr, "%s\n", strerror(errno));
        close(SlaveSocket);
        return;
    }
    Client->base = server_conf->base;
    Client->ht = server_conf->ht;
    //----------------------------------------------------------------------
    /* Create new bufferized event, linked with client's socket */
    Client->b_ev = bufferevent_socket_new(server_conf->base, SlaveSocket, BEV_OPT_CLOSE_ON_FREE);
    if (!Client->b_ev) {
        fprintf(stderr, "bufferevent_socket_new failed\n");
        close(SlaveSocket);
        free(Client);
        return;
    }
    bufferevent_setcb(Client->b_ev, cache_read_cb, NULL, cache_error_cb, Client);
    /* Ready to get data */
    bufferevent_enable(Client->b_ev, EV_READ | EV_WRITE | EV_PERSIST);
    //----------------------------------------------------------------------
    bufferevent_setwatermark(Client->b_ev, EV_WRITE, SRV_BUF_LOWMARK, 0);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection to worker # %d\n", server_conf->workers);
#endif
}
//-----------------------------------------
void cache_error_cb(struct bufferevent* b_ev, short events, void* arg)
{
    (void)b_ev;
    /*   An event structure becomes a timeout trigger via the function timeout_set().
     *   Naturally, the trigger has to be "armed" - the function timeout_add() does that.
     *   To "disarm" the trigger and cancel the timeout event, use
     *   the function timeout_del().
     */
    /*
     *   The header <event2/bufferevent.h> defines flags
     *   that tell the reason of an exceptional situation:
     *   1. The event happened while reading (BEV_EVENT_READING 0x01) or writing (BEV_EVENT_WRITING
     * 0x02).
     *   2. End of file was reached (BEV_EVENT_EOF 0x10), a critical failure occurred
     * (BEV_EVENT_ERROR 0x20), the given interval expired (BEV_EVENT_TIMEOUT 0x40), the connection
     * was established (BEV_EVENT_CONNECTED 0x80).
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

        shutdown(bufferevent_getfd(Client->b_ev), SHUT_RDWR);
        bufferevent_free(Client->b_ev);
        free(Client);

#ifdef _DEBUG
        printf("connection closed\n");
#endif // _DEBUG

    } else if (events & BEV_EVENT_ERROR) {
        struct client* Client = (struct client*)arg;

        fprintf(stderr, "Error from bufferevent: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

        bufferevent_free(Client->b_ev);
        free(Client);

#ifdef _DEBUG
        printf("connection closed");
#endif // _DEBUG
    }
}
void cache_read_cb(struct bufferevent* b_ev, void* arg)
{

#ifdef _DEBUG
    printf("data reseived\n");
#endif // _DEBUG

    /* This callback is invoked when there is data to read on b_ev_read */
    struct client* Client = (struct client*)arg;

    struct evbuffer* buf_in = bufferevent_get_input(b_ev);
    struct evbuffer* buf_out = bufferevent_get_output(b_ev);

    cache_handling(Client->ht, buf_in, buf_out);

#ifdef _DEBUG
    printf("response ready\n");
#endif // _DEBUG
}
//-----------------------------------------
/*  Protocol:  get <key> || set <TTL> <key> <val>
 *  Reply:      ok <key> <val> OR error <err_text>
 *
 *  key - no spaces,  val and key - no '\n'
 */
void cache_handling(hashtable* ht, struct evbuffer* buf_in, struct evbuffer* buf_out)
{
    const char* const err_prefix = "cache handling";
    const char* const cmd_get = "get";
    const char* const cmd_set = "set";
    //-----------------------------------------------------------------
    char* cmd = NULL;
    size_t cmd_length = 0U;

    while ((cmd = evbuffer_readln(buf_in, &cmd_length, EVBUFFER_EOL_ANY))) {
        /* every command gets its own answer */
        const char* err_answer = "";
        int no_answer = 0;
        char* cmd_cur_char = cmd;
        int n = 0;
        //-----------------------------------------------------------------
        ht_rec hd = {0};
        char cmd_head[4];

        /* try to read the command */
        if (1 == sscanf(cmd_cur_char, "%3s%n", cmd_head, &n)) {
            cmd_cur_char += n;

            if (!strcmp(cmd_head, cmd_get)) {
                if (1 == sscanf(cmd_cur_char, "%d%n", &hd.key, &n)) {
                    cmd_cur_char += n;

                    if (hashtable_get(ht, &hd)) {
                        no_answer = 1;
                        err_answer = "no such key in table";
                    }
                } else {
                    no_answer = 2;
                    err_answer = "incorrect command to server";
                }
            } else if (!strcmp(cmd_head, cmd_set)) {
                int32_t ttl = 0;
                /* try to read the parameters */
                if (3 == sscanf(cmd_cur_char, "%d%d%d%n", &ttl, &hd.key, &hd.val, &n)) {
                    cmd_cur_char += n;

                    hd.ttl = ttl_converted(ttl);
                    if (hashtable_set(ht, &hd)) {
                        no_answer = 1;
                        err_answer = "cannot insert the key";
                    }
                } else {
                    no_answer = 2;
                    err_answer = "incorrect command to server";
                } // end else
            } // end if strcmp
            else {
                no_answer = 2;
                err_answer = "incorrect command to server";
            }
        } // end if scanf
        else {
            no_answer = 2;
            err_answer = "incorrect command to server";
        }
        //-----------------------------------------------------------------
#ifdef _DEBUG
        printf("key=% 4d ttl=% 3.2lf answ=%d\n", hd.key,
               !hd.ttl ? 0. : difftime(time(NULL), hd.ttl), !no_answer);
#endif // _DEBUG
       //-----------------------------------------------------------------
        if (!no_answer) {
            evbuffer_add_printf(buf_out, "ok %d %d\n", hd.key, hd.val);
        } else {
            evbuffer_add_printf(buf_out, "error : %s\n", err_answer);
            //-----------------------------------------------------------------
            if (no_answer != 1) {
                my_errno = SRV_ERR_RCMMN;
                fprintf(stderr, "%s%s", err_prefix, strmyerror());
            }
        }
        //-----------------------------------------------------------------
        free(cmd);
    }
    //-----------------------------------------------------------------
    /* An incomplete line waits for the rest of it, unless it is too long */
    size_t rest = evbuffer_get_length(buf_in);
    if (rest > CACHE_MAX_LINE) {
        evbuffer_drain(buf_in, rest);
        evbuffer_add_printf(buf_out, "error : command too long\n");
    }
    return;
}
//-----------------------------------------
