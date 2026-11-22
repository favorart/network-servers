#ifndef _CACHE_H_
#define _CACHE_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <event.h>
/* ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  v   Run:  ./cache ...
 *  v   On start the server creates 4 child processes
 *      that handle client commands.
 *  v   Passing descriptors to the child processes. (sndmsg)
 *  v   Cache data in shared memory (shm_open | schmget)
 *  v   Atomicity (semget | posix)
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
/** @brief Close the worker's end of the socketpair. */
void child_worker_free(chw_t* chw);
/**
 * @brief Send a message and a file descriptor to a child worker.
 * @param msg CHWMSG_TASK or CHWMSG_TERM, sent as text.
 * @param fd  Descriptor passed via SCM_RIGHTS; must not be -1.
 * @return Bytes sent, or 1 for an unknown message.
 */
int child_worker_send(chw_t* chw, chwmsg_enum msg, fd_t fd);
/**
 * @brief Receive a message and a file descriptor from the master.
 * @param[out] msg Decoded message (left unchanged if unknown).
 * @param[out] fd  Received descriptor, or -1 if none came.
 * @return Bytes read, or -1 if nothing was read.
 */
int child_worker_recv(chw_t* chw, chwmsg_enum* msg, fd_t* fd);
//-----------------------------------------
#define SRV_SERVER_NAME "cache-server-pract5"
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_WORKERS 16U
#define SRV_BUF_LOWMARK 128U

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
    char config_path[FILENAME_MAX];
    char server_path[FILENAME_MAX];
    //-----------------------
    struct event_base* base;
    struct hash_table* ht;
    //-----------------------
};
extern struct server_config server_conf;
//-----------------------------------------
/**
 * @brief Fill the config (defaults for NULL arguments) and fork the workers.
 * @param port Port string, default 8080.
 * @param ip   IPv4 address string, default "0.0.0.0".
 * @param work Number of worker processes, default 4.
 * @return 0; fatal errors exit. In a child worker conf->myself is set.
 */
int server_config_init(srv_conf* conf, char* port, char* ip, char* work);
/** @brief Print the server name, path, port and ip to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Close the worker sockets, free the worker array and zero the config. */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/**
 * @brief Parse -c/--conf (the value is not used yet) and init the config with defaults.
 * @return Result of server_config_init().
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
/** @brief Listener callback: accept a connection and pass it to a random worker. */
void cache_accept_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/**
 * @brief Worker callback: receive a client fd from the master and start serving it.
 *
 * Stops the worker's event loop on CHWMSG_TERM.
 */
void cache_connect_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Read callback: run the client's get/set commands and write the replies. */
void cache_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief Event callback: free the client on EOF or error. */
void cache_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
#endif // _CACHE_H_
