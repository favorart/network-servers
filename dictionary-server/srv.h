#include "header.h"
#include "srv_que.h"

/*  Practice on threads (pthreads, std::threads, boost::threads)
 *  ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  v   Run:  ./dict ...
 *  v   On start the server creates 4 threads that handle client commands.

 *  v  A text file with words (each word is followed, after a space, by its int weight (0-99)) is
 the shared database.
 *  v  The file is searched linearly for words with the prefix received over the network.
 *  v  Replies are given in descending order of weight, but no more than 10.
 *  v  1 connection - 1 request.
 *
 *  v  One thread accepts connections - the others handle them (with an even load).
 *  v  Between them: mutexes and conditional_variables.
 *
 *  -  There is also a cache that keeps the most recently requested words.
 *  -  The cache has a TTL and is shared (read/write lock)
 */
#ifndef _SRV_DICT_H_
#define _SRV_DICT_H_

#define _DEBUG
//-----------------------------------------
/** @brief Server error codes, stored in my_errno. */
typedef enum
{
    SRV_ERR_NONE,
    SRV_ERR_PARAM,
    SRV_ERR_INPUT,
    SRV_ERR_LIBEV,
    SRV_ERR_RCMMN,
    SRV_ERR_RFREE,
    SRV_ERR_FDTRS
} myerr;
myerr my_errno;
/** @brief Text for the current my_errno (NULL if none); resets my_errno. */
const char* strmyerror();
//-----------------------------------------
#define SRV_SERVER_NAME "dict-server-pract6"
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_WORKERS 16U
#define SRV_BUF_LOWMARK 128U
#define SRV_DICT_FLNAME "mydict.txt"

typedef uint8_t wc_t;
typedef struct hash_table hashtable;
typedef struct server_config srv_conf;
/** @brief Server settings, the work queue and the shared cache. */
struct server_config
{
    //-----------------------
    char* ptr_server_name;
    //-----------------------
    uint16_t port;
    char ip[SRV_IPSTRLENGTH];
    //-----------------------
    /* defines FILENAME_MAX in <stdio.h> */
    char config_path[FILENAME_MAX];
    char server_path[FILENAME_MAX];
    //-----------------------
    wc_t workers_count;
    worker_t* workers;
    wque_t workqueue;
    //-----------------------
    struct event_base* base;
    struct hash_table* ht;
};
extern struct server_config server_conf;
//-----------------------------------------
/**
 * @brief Fill the config (defaults for NULL arguments), start the worker threads
 *        and create the cache.
 * @param port Port string, default 8080.
 * @param ip   IPv4 address string, default "0.0.0.0".
 * @param work Number of worker threads, default 4.
 * @return 0; fatal errors exit.
 */
int server_config_init(srv_conf* conf, char* port, char* ip, char* work);
/** @brief Print the server name, path, port and ip to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Free the cache, stop the worker threads and zero the config. */
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
/** @brief State of one client connection: its own event base, bufferevent and the shared cache. */
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
/** @brief Free the client's bufferevent (closing the socket), its event base and the client. */
void client_free(struct client* Client);
//-----------------------------------------
/** @brief Listener callback: accept a connection, set up a client and queue a job for it. */
void srv_accept_cb(evutil_socket_t fd, short ev, void* arg);
/** @brief Listener error callback: log the error and stop the event loop. */
void srv_ac_err_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Read callback: answer each prefix line with up to 10 matching dictionary words. */
void srv_read_cb(struct bufferevent* b_ev, void* arg);
/** @brief Event callback: log EOF or an error and shut the socket down. */
void srv_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
#define DICT_CACHELNS 256
#define DICT_WORD_LEN 101
#define DICT_WORD_CNT 10
/** @brief Cache value: up to DICT_WORD_CNT words found for a prefix, with their weights. */
struct dict_rec
{
    char words[DICT_WORD_CNT][DICT_WORD_LEN];
    int weights[DICT_WORD_CNT];
    int count;
};
/** @brief Cache key: the requested prefix. */
struct key_rec
{
    char word[DICT_WORD_LEN];
};

typedef struct dict_rec ht_val_t;
typedef struct key_rec ht_key_t;
/** @brief Compare two cache keys as strings. @return strcmp() result, 0 if equal. */
static inline int my_key_comp(ht_key_t* a, ht_key_t* b)
{
    return strcmp(a->word, b->word);
}
//-----------------------------------------
#endif // _SRV_DICT_H_
