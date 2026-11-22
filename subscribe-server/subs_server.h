#include "stdafx.h"
#include "subs_queues.h"

#ifndef _SUBS_H_
#define _SUBS_H_

/*
 *  ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  v   On start the server creates 4 child
 *      processes that handle client commands.
 *  v   Run:    ./subs
 *                     -d/--dir     <dir>
 *                     -h/--ip      <ip>
 *                     -p/--port    <port>
 *                     -w/--workers <num>
 *                     -q/--queues  <num>
 *                     -c/--conf    <path>
 *
 */
//-----------------------------------------
#define CHWMSG_MAXLEN 5
/** @brief Command the master process sends to a child worker over the socketpair. */
typedef enum
{
    CHWMSG_NONE = 0,
    CHWMSG_TERM,
    CHWMSG_TASK
} chwmsg_enum;

typedef uint8_t wc_t;
typedef int fd_t;
typedef struct child_worker chw_t;
/** @brief A forked worker process and its end of the master-worker socketpair. */
struct child_worker
{
    pid_t pid;
    fd_t fd;
};
//-----------------------------------------
/**
 * @brief Create a socketpair and fork a worker process.
 * @return 0 in the child, the child's pid in the parent (exits on failure).
 */
int child_worker_init(chw_t* chw);
/** @brief Close the worker's socketpair end. */
void child_worker_free(chw_t* chw);
/**
 * @brief Send a command and pass a file descriptor to the worker, then close fd locally.
 * @return Number of bytes sent, or 1 for an unknown command.
 */
int child_worker_send(chw_t* chw, chwmsg_enum msg, fd_t fd);
/**
 * @brief Receive a command and a passed file descriptor from the master.
 * @param[out] msg Received command.
 * @param[out] fd  Received descriptor, or -1 if none was passed.
 * @return Number of bytes read, or -1 on end of stream.
 */
int child_worker_recv(chw_t* chw, chwmsg_enum* msg, fd_t* fd);
//-----------------------------------------
#define SRV_SERVER_NAME "subscribe-server"
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_WORKERS 16U
#define SRV_MAX_NQUEUES 255U
#define SRV_BUF_LOWMARK 128U

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
/** @brief Server configuration and runtime state (event base, clients, queues, workers). */
struct server_config
{
    //-----------------------
    char* ptr_server_name;
    //-----------------------
    uint16_t port;
    char ip[SRV_IPSTRLENGTH];
    //-----------------------
    wc_t n_queues;
    wc_t n_workers;
    chw_t* myself;
    chw_t* child_workers;
    //-----------------------
    /* defines FILENAME_MAX in <stdio.h> */
    char config_path[FILENAME_MAX];
    char server_path[FILENAME_MAX];
    //-----------------------
    struct event_base* base;
    std::list<client*> clients;
    QueuesManager* queues_manager;
    //-----------------------
};
#ifdef MULTI_PROCESS
extern struct server_config server_conf;
#endif
//-----------------------------------------
/**
 * @brief Fill the configuration from option strings (NULL means default), create the
 *        queues manager and, with MULTI_PROCESS, fork the workers.
 * @return 0 on success, non-zero on failure.
 */
int server_config_init(srv_conf* conf, char* dir, char* ip, char* port, char* workers,
                       char* queues);
/** @brief Print the configuration to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Free the queues manager and workers and zero the configuration. */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/**
 * @brief Parse command-line options with getopt_long and initialize @p conf.
 * @return Result of server_config_init().
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl()/ioctl(), -1 on error.
 */
int set_nonblock(evutil_socket_t fd);

//-----------------------------------------
/** @brief Listener callback: accept a client and register it (or hand it to a worker). */
void subs_accept_cb(evutil_socket_t fd, short ev, void* arg);
/** @brief Listener error callback: log the error and stop the event loop. */
void subs_ac_err_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Worker callback: receive a client socket from the master and set up its bufferevent. */
void subs_connect_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Client read callback: handle subscribe/publish commands and broadcast new messages. */
void subs_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief Client event callback: on EOF or error remove and free the client. */
void subs_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
/** @brief SIGINT callback: stop the workers (MULTI_PROCESS) and exit the loop in two seconds. */
void subs_sigint_cb(evutil_socket_t sig, short events, void* arg);
//-----------------------------------------
#endif // _SUBS_H_
