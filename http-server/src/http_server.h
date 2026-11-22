#ifndef _HTTP_SERVER_H_
#define _HTTP_SERVER_H_
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <event2/util.h>

#include "http_request.h"

struct bufferevent;
struct event_base;
/*
 *  v   Create an HTTP server.
 *  v   Build with make.
 *  v   Run:  ./http-server -d/--dir <dir> -h/--ip <ip> -p/--port <port> -w/--workers <num>
 *  v   Implement HEAD/GET/POST
 *  v   Statuses 200 and 404
 *  v   The <dir> directory holds html and jpeg files
 */
//-----------------------------------------
#define CHWMSG_MAXLEN 5
/** @brief Command sent from the master process to a child worker. */
typedef enum
{
    CHWMSG_NONE = 0,
    CHWMSG_TERM,
    CHWMSG_TASK
} chwmsg_enum;
typedef int fd_t;
typedef uint8_t wc_t;
typedef struct child_worker chw_t;
/** @brief A forked worker process and its end of the socketpair to the master. */
struct child_worker
{
    pid_t pid;
    fd_t fd;
};
//-----------------------------------------
/**
 * @brief Create a socketpair and fork a worker process.
 * @param chw Filled with the pid and the socket end of the calling side.
 * @return 0 in the child, the child's pid in the parent (exits on failure).
 */
int child_worker_init(chw_t* chw);
/** @brief Close the worker's socketpair end. */
void child_worker_free(chw_t* chw);
/**
 * @brief Send a command with a client socket to a worker, then close @p fd locally.
 * @return Bytes sent, or 1 for an unknown command.
 */
int child_worker_send(chw_t* chw, chwmsg_enum msg, fd_t fd);
/**
 * @brief Receive a command and a passed file descriptor from the master.
 * @param msg Set to the received command.
 * @param fd  Set to the received descriptor, or -1 if none was passed.
 * @return Bytes received, or -1 on end of stream.
 */
int child_worker_recv(chw_t* chw, chwmsg_enum* msg, fd_t* fd);
//-----------------------------------------
#define HTTP_SERVER_NAME "http-server-hw"
#define HTTP_IPSTRLENGTH 16U
#define HTTP_MAX_WORKERS 16U

typedef struct http_server_config srv_conf;
/** @brief Server settings from the command line plus the worker table. */
struct http_server_config
{
    uint16_t port;
    char ip[HTTP_IPSTRLENGTH];
    //-----------------------
    /* defines FILENAME_MAX in <stdio.h> */
    char folder_path[FILENAME_MAX];
    char server_path[FILENAME_MAX];
    //-----------------------
    char* ptr_server_name;
    //-----------------------
    wc_t workers;
    chw_t* myself;
    chw_t* child_workers;
    //-----------------------
};
extern struct http_server_config server_conf;
//-----------------------------------------
/**
 * @brief Fill the config (invalid values fall back to defaults) and fork the workers.
 * @param path    Directory with the served files (default "data/").
 * @param port    Listening port (default 8080).
 * @param ip      Listening address (default "0.0.0.0").
 * @param workers Number of worker processes (default 4).
 * @return Always 0; in a child process @c conf->myself points to its worker.
 */
int http_server_config_init(srv_conf* conf, char* path, char* port, char* ip, char* workers);
/** @brief Print the config to @p stream. */
void http_server_config_print(srv_conf* conf, FILE* stream);
/** @brief Zero the config (the worker table is not freed). */
void http_server_config_free(srv_conf* conf);
//-----------------------------------------
/**
 * @brief Parse -d/-h/-p/-w options and initialize @p conf.
 * @return Result of http_server_config_init().
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Put a socket into non-blocking mode.
 * @return Result of fcntl() (or ioctl()), -1 on error.
 */
int set_nonblock(evutil_socket_t fd);
//-----------------------------------------
/** @brief One client connection served by a worker. */
struct client
{
    /* bufferevent already has separate
     * two buffers for input and output.
     */
    struct bufferevent* b_ev;
    struct event_base* base;
    //---------------------------
    struct http_request request;
};
//-----------------------------------------
/** @brief Master: accept a connection and pass its socket to a random worker. */
void http_accept_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Worker: receive a client socket from the master and set up its bufferevent. */
void http_connect_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Parse buffered requests and write the responses (@p arg is the client). */
void http_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief On EOF or error: free the request, the bufferevent and the client. */
void http_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
#endif // _HTTP_SERVER_H_
