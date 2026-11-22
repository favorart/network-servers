#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include "srv.h"
#include "srv_cache.h"

//-----------------------------------------
void server_job_function(job_t* job)
{
    struct client* Client = (struct client*)job->user_data;
    //----------------------------------------------------------------------
    /* Runs until the client's bufferevent is disabled (EOF or error) and its output is sent. */
    event_base_loop(Client->base, 0);
    client_free(Client);
    free(job);
}
//-----------------------------------------
void client_free(struct client* Client)
{
    if (!Client)
        return;
    if (Client->b_ev)
        bufferevent_free(Client->b_ev);
    if (Client->base)
        event_base_free(Client->base);
    free(Client);
}
//-----------------------------------------
void srv_ac_err_cb(evutil_socket_t fd, short ev, void* arg)
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
void srv_accept_cb(evutil_socket_t fd, short ev, void* arg)
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
    /* Making the new client */
    struct client* Client = (struct client*)calloc(1U, sizeof(*Client));
    if (!Client) {
        perror("client calloc");
        close(SlaveSocket);
        return;
    }

    if (!(Client->base = event_base_new())) {
        my_errno = SRV_ERR_LIBEV;
        fprintf(stderr, "%s\n", strmyerror());

        close(SlaveSocket);
        client_free(Client);
        return;
    }
    Client->ht = server_conf->ht;
    Client->dict_path = server_conf->dict_path;
    //----------------------------------------------------------------------
    /* Create new bufferized event, linked with client's socket */
    if (!(Client->b_ev =
              bufferevent_socket_new(Client->base, SlaveSocket, BEV_OPT_CLOSE_ON_FREE))) {
        my_errno = SRV_ERR_LIBEV;
        fprintf(stderr, "%s\n", strmyerror());

        close(SlaveSocket);
        client_free(Client);
        return;
    }
    bufferevent_setcb(Client->b_ev, srv_read_cb, NULL, srv_error_cb, Client);
    /* Ready to get data */
    bufferevent_enable(Client->b_ev, EV_READ | EV_WRITE | EV_PERSIST);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection to server\n");
#endif // _DEBUG

    job_t* job = NULL;
    /* Create a job object and add it to the work queue. */
    if (!(job = malloc(sizeof(*job)))) {
        perror("job malloc");
        client_free(Client);
        return;
    }

    job->job_func = server_job_function;
    job->user_data = Client;

    wque_push(&server_conf->workqueue, job);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection accepted\n");
#endif // _DEBUG
}
//-----------------------------------------
void srv_error_cb(struct bufferevent* b_ev, short events, void* arg)
{
    (void)arg;
    //----------------------------------------------------------------------
#ifdef _DEBUG
    if (events & BEV_EVENT_CONNECTED) {
        printf("connection established\n");
        return;
    }
    if (events & BEV_EVENT_EOF) {
        printf("got a close. length = %zu\n", evbuffer_get_length(bufferevent_get_input(b_ev)));
    }
#endif // _DEBUG
    //----------------------------------------------------------------------
    if (events & BEV_EVENT_ERROR) {
        fprintf(stderr, "Error from bufferevent: '%s'\n",
                evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
        bufferevent_disable(b_ev, EV_READ | EV_WRITE);
    } else {
        /* EOF: stop reading; the loop ends once the pending output is written. */
        bufferevent_disable(b_ev, EV_READ);
    }
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("connection closed\n");
#endif // _DEBUG
}
void srv_read_cb(struct bufferevent* b_ev, void* arg)
{
#ifdef _DEBUG
    printf("data reseived\n");
#endif // _DEBUG
    //----------------------------------------------------------------------
    /* This callback is invoked when there is data to read on b_ev_read */
    struct client* Client = (struct client*)arg;

    struct evbuffer* buf_in = bufferevent_get_input(b_ev);
    struct evbuffer* buf_out = bufferevent_get_output(b_ev);

    find_words_prefix(buf_in, buf_out, Client->ht, Client->dict_path);
    //----------------------------------------------------------------------
#ifdef _DEBUG
    printf("response ready\n");
#endif // _DEBUG
}
//-----------------------------------------
void dict_rec_add(struct dict_rec* rec, const char* word, int weight)
{
    /* Position after all words with a weight >= this one: equal weights keep file order. */
    int pos = 0;
    while (pos < rec->count && rec->weights[pos] >= weight)
        ++pos;
    if (pos >= DICT_WORD_CNT)
        return;

    /* Shift the lighter words down; the last one falls out when the record is full. */
    int last = (rec->count < DICT_WORD_CNT) ? rec->count : DICT_WORD_CNT - 1;
    memmove(rec->words[pos + 1], rec->words[pos], (size_t)(last - pos) * DICT_WORD_LEN);
    memmove(&rec->weights[pos + 1], &rec->weights[pos], (size_t)(last - pos) * sizeof(int));

    snprintf(rec->words[pos], DICT_WORD_LEN, "%s", word);
    rec->weights[pos] = weight;
    if (rec->count < DICT_WORD_CNT)
        ++rec->count;
}
void dict_lookup(FILE* fdict, const char* prefix, size_t prefix_length, struct dict_rec* rec)
{
    char* line = NULL;
    size_t line_size = 0U;
    char word[DICT_WORD_LEN];
    int weight = 0, word_end = 0;

    memset(rec, 0, sizeof(*rec));
    rewind(fdict);
    while (getline(&line, &line_size, fdict) != -1) {
        /* "%100s" matches DICT_WORD_LEN; longer words and broken lines are skipped. */
        if (2 != sscanf(line, " %100s%n %d", word, &word_end, &weight))
            continue;
        if (!isspace((unsigned char)line[word_end]))
            continue;
        if (!strncmp(word, prefix, prefix_length))
            dict_rec_add(rec, word, weight);
    }
    free(line);
}
//-----------------------------------------
void find_words_prefix(struct evbuffer* buf_inc, struct evbuffer* buf_out, struct hash_table* ht,
                       const char* dict_path)
{
    char* prefix = NULL;
    size_t prefix_length = 0U;
    ht_rec rec;
    //-----------------------------------------------------------------
    FILE* fdict = fopen(dict_path, "r");
    if (!fdict) {
        perror("dict file");
        goto HNDL_FREE;
    }

    while ((prefix = evbuffer_readln(buf_inc, &prefix_length, EVBUFFER_EOL_ANY))) {
#ifdef _DEBUG
        printf("\n\n--> %s\n", prefix);
#endif // _DEBUG
       //-----------------------------------------------------------------
        if (prefix_length >= DICT_WORD_LEN) {
            my_errno = SRV_ERR_RCMMN;
            fprintf(stderr, "prefix longer than %d bytes%s\n", DICT_WORD_LEN - 1, strmyerror());
            free(prefix);
            continue;
        }
        /* Each prefix starts from an empty record. */
        memset(&rec, 0, sizeof(rec));
        strcpy(rec.key.word, prefix);
        //-----------------------------------------------------------------
        if (hashtable_get(ht, &rec)) {
            dict_lookup(fdict, prefix, prefix_length, &rec.val);
        }
        rec.ttl = ttl_converted(10);
        hashtable_set(ht, &rec);

        if (buf_out) {
            for (int j = 0; j < rec.val.count; ++j) {
#ifdef _DEBUG
                printf("%s %d\n", rec.val.words[j], rec.val.weights[j]);
#endif // _DEBUG
                evbuffer_add_printf(buf_out, "%s\n", rec.val.words[j]);
            }
        }
        //-----------------------------------------------------------------
        free(prefix);
    }
    //-----------------------------------------------------------------
HNDL_FREE:;
    if (fdict)
        fclose(fdict);

    evbuffer_drain(buf_inc, evbuffer_get_length(buf_inc));
    //----------------------------------------------------------------------
}
//-----------------------------------------
