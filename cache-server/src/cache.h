#ifndef _CACHE_H_
#define _CACHE_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <event.h>
/* ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  v   Run:  ./cache-server [-p port] [-i ip] [-w workers]
 *  v   On start the server creates worker processes (4 by default)
 *      that handle client commands.
 *  v   Passing descriptors to the child processes. (sendmsg)
 *  v   Cache data in shared memory (shmget)
 *  v   Atomicity (semget)
 *  v   Implementation of hash_table with ttl
 */
//-----------------------------------------
#define CHWMSG_MAXLEN 5
/** @brief Message the master process sends to a child worker. */
typedef enum
{
    CHWMSG_NONE = 0,
    CHWMSG_TERM,
    CHWMSG_TASK
} chwmsg_enum;
typedef uint8_t wc_t;
typedef int fd_t;
typedef struct child_worker chw_t;
/** @brief A child worker process and its end of the socketpair to the master. */
struct child_worker
{
    pid_t pid;
    fd_t fd;
};
//-----------------------------------------
/**
 * @brief Create a socketpair and fork a child worker process.
 * @param chw Filled with the pid and this process's end of the socketpair.
 * @return 0 in the child, the child's pid in the parent (exits on failure).
 */
int child_worker_init(chw_t* chw);
/** @brief Close the worker's end of the socketpair (if open) and set it to -1. */
void child_worker_free(chw_t* chw);
/**
 * @brief Send a message and a file descriptor to a child worker.
 * @param msg CHWMSG_TASK or CHWMSG_TERM, sent as text.
 * @param fd  Descriptor passed via SCM_RIGHTS; must not be -1.
 * @return Bytes sent, 0 or -1 on failure, or 1 for an unknown message.
 */
int child_worker_send(chw_t* chw, chwmsg_enum msg, fd_t fd);
/**
 * @brief Receive a message and a file descriptor from the master.
 * @param[out] msg Decoded message (CHWMSG_NONE if unknown or nothing was read).
 * @param[out] fd  Received descriptor, or -1 if none came.
 * @return Bytes read, or -1 on end of file (the master closed the socket).
 */
int child_worker_recv(chw_t* chw, chwmsg_enum* msg, fd_t* fd);
//-----------------------------------------
#define SRV_SERVER_NAME "cache-server-pract5"
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_WORKERS 16U
#define SRV_BUF_LOWMARK 128U

#define SRV_DEFAULT_PORT "8080"
#define SRV_DEFAULT_IP "0.0.0.0"
#define SRV_DEFAULT_WORKERS "4"

typedef struct server_config srv_conf;
/** @brief Server settings, worker processes and the shared cache. */
struct server_config
{
    //-----------------------
    const char* ptr_server_name;
    //-----------------------
    uint16_t port;
    char ip[SRV_IPSTRLENGTH];
    //-----------------------
    wc_t workers;
    chw_t* myself;
    chw_t* child_workers;
    //-----------------------
    /* defines FILENAME_MAX in <stdio.h> */
    char server_path[FILENAME_MAX];
    //-----------------------
    struct event_base* base;
    struct hash_table* ht;
    //-----------------------
};
extern struct server_config server_conf;
//-----------------------------------------
/**
 * @brief Validate the settings and store them in the config (no processes are started).
 * @param port Port number 1..65535.
 * @param ip   IPv4 address in dotted form.
 * @param work Number of worker processes, 1..SRV_MAX_WORKERS.
 * @return 0 on success, -1 (with a message on stderr) if a value is invalid.
 */
int server_config_set(srv_conf* conf, const char* port, const char* ip, const char* work);
/**
 * @brief Fork conf->workers worker processes.
 *
 * Each worker keeps only its own end of its socketpair.
 * @return 0; fatal errors exit. In a child worker conf->myself is set.
 */
int server_config_init(srv_conf* conf);
/** @brief Print the server name, path, port, ip and the cache's IPC ids to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/**
 * @brief Close the worker sockets and free the worker array.
 *
 * In the master the closed sockets tell the workers to exit; it then waits for them.
 */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/** @brief parse_console_parameters() result: run, exit successfully, or exit with an error. */
enum
{
    PARSE_ERROR = -1,
    PARSE_RUN = 0,
    PARSE_EXIT = 1
};
/**
 * @brief Parse -p/--port, -i/--ip, -w/--workers, -v/--version and -h/--help into the config.
 *
 * Missing options take the SRV_DEFAULT_* values.
 * @return PARSE_RUN, PARSE_EXIT after printing the version or help, PARSE_ERROR on bad input.
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Put a socket into non-blocking mode.
 * @return fcntl() result, -1 on error.
 */
int set_nonblock(evutil_socket_t fd);
//-----------------------------------------
typedef struct hash_table hashtable;
/** @brief State of one client connection in a worker. */
struct client
{
    /* bufferevent already has separate
     * two buffers for input and output.
     */
    struct bufferevent* b_ev;
    struct event_base* base;
    //---------------------------
    struct hash_table* ht;
};
//-----------------------------------------
/**
 * @brief Listener callback: accept a connection and pass it to a random worker.
 *
 * The master closes its copy of the client socket once it is sent.
 */
void cache_accept_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/**
 * @brief Worker callback: receive a client fd from the master and start serving it.
 *
 * Stops the worker's event loop on CHWMSG_TERM or when the master closes the socket.
 */
void cache_connect_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Read callback: run the client's get/set commands and write the replies. */
void cache_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief Event callback: free the client on EOF or error. */
void cache_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
/** @brief Longest command line kept while waiting for its end of line. */
#define CACHE_MAX_LINE 1024U
/**
 * @brief Run every complete command line of @p buf_in and write the replies to @p buf_out.
 *
 * Protocol: "get <key>" or "set <ttl> <key> <val>" (integers), one per line.
 * Reply: "ok <key> <val>" or "error : <text>". An incomplete last line stays in @p buf_in
 * (dropped with an error if it grows longer than CACHE_MAX_LINE).
 */
void cache_handling(hashtable* ht, struct evbuffer* buf_in, struct evbuffer* buf_out);
//-----------------------------------------
#endif // _CACHE_H_
