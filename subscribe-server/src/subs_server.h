#ifndef _SUBS_H_
#define _SUBS_H_

#include <cstdint>
#include <cstdio>
#include <list>
#include <set>

#include <event.h>

#include "subs_queues.h"

/*
 *  ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  v   A single process serves all clients with libevent.
 *  v   Run:    ./subscribe-server
 *                     -h/--ip      <ip>
 *                     -p/--port    <port>
 *                     -q/--queues  <num>
 *                     -v/--version
 *              (-d/--dir, -w/--workers and -c/--conf are accepted and ignored)
 */
//-----------------------------------------
#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

#define SRV_SERVER_NAME "subscribe-server"
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_NQUEUES 255U
#define SRV_MAX_LINE 4096U

typedef uint8_t wc_t;
typedef struct server_config srv_conf;
//-----------------------------------------
/** @brief A connected client: its bufferevent and the set of subscribed queues. */
struct client
{
    /* bufferevent already has separate
     * two buffers for input and output.
     */
    struct bufferevent* b_ev;
    struct event_base* base;
    //---------------------------
    std::set<size_t>* queues_subscribed;
    struct server_config* config;
};
//-----------------------------------------
/** @brief Server configuration and runtime state (event base, clients, queues). */
struct server_config
{
    //-----------------------
    char* ptr_server_name = nullptr;
    //-----------------------
    uint16_t port = 0;
    char ip[SRV_IPSTRLENGTH] = {};
    //-----------------------
    wc_t n_queues = 0;
    //-----------------------
    /* defines FILENAME_MAX in <stdio.h> */
    char server_path[FILENAME_MAX] = {};
    //-----------------------
    struct event_base* base = nullptr;
    std::list<client*> clients;
    QueuesManager* queues_manager = nullptr;
    //-----------------------
};
//-----------------------------------------
/**
 * @brief Fill the configuration from option strings and create the queues manager.
 *
 * NULL or invalid values mean defaults: port 3131 (valid 0..65535), ip 0.0.0.0,
 * 2 queues (valid 1..SRV_MAX_NQUEUES).
 * @return 0 on success, non-zero on failure.
 */
int server_config_init(srv_conf* conf, char* ip, char* port, char* queues);
/** @brief Print the configuration to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Free the queues manager and reset the configuration. */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/**
 * @brief Parse command-line options with getopt_long and initialize @p conf.
 *        On -v/--version prints NS_VERSION and exits.
 * @return Result of server_config_init().
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl(), -1 on error.
 */
int set_nonblock(evutil_socket_t fd);

//-----------------------------------------
/** @brief Listener callback: accept a client and register it. */
void subs_accept_cb(evutil_socket_t fd, short ev, void* arg);
/** @brief Listener error callback: log the error and stop the event loop. */
void subs_ac_err_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Client read callback: handle subscribe/publish commands and broadcast new messages. */
void subs_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief Client event callback: on EOF or error remove and free the client. */
void subs_error_cb(struct bufferevent* b_ev, short events, void* arg);
/** @brief Free every connected client (call before freeing the event base). */
void subs_clients_free(struct server_config* conf);
/**
 * @brief Execute every complete command line in @p buf_in and write the replies to @p buf_out.
 *
 * An incomplete last line stays in @p buf_in until the rest arrives, unless it grows longer
 * than SRV_MAX_LINE bytes; then it is dropped.
 *
 * Protocol: `publish <num> message` or `subscribe <num>`; replies `ok <que num>` or
 * `error : text`.
 */
void subscribe_handling(client* Client, struct evbuffer* buf_in, struct evbuffer* buf_out);
//-----------------------------------------
/** @brief SIGINT callback: exit the event loop in two seconds. */
void subs_sigint_cb(evutil_socket_t sig, short events, void* arg);
//-----------------------------------------
#endif // _SUBS_H_
