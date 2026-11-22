#ifndef _PROXY_H_
#define _PROXY_H_

#include <stdint.h>
#include <stdio.h>

#include <event.h>
/*
 *  Create a proxy server.
 *  ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  +   Run:  ./proxy-server -c/--conf <filename>, -v/--version
 *            Config: <proxy_port>,<ip1:port1>, <ip2:port2>, ...
 *  +   The server for the outgoing connection is chosen at random
 *      from the list of servers in the config file.
 *  v   Reading and writing are buffered
 *  +   EOF (nc Ctrl+D) --> send all buffered data to the server,
 *                          keep sending the server's data to the client,
 *                          handle the server's data the same way
 *      On connection close or error - drop both connections
 */
//-----------------------------------------
#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

#define SRV_SERVER_NAME "proxy-server-pract3"
#define SRV_CONFDEFNAME "config.txt"
#define SRV_CONFMAXSERV 25U
#define SRV_IPSTRLENGTH 16U
#define SRV_MAX_WORKERS 16U
#define SRV_BUF_LOWMARK 128U

typedef struct server_config srv_conf;
/** @brief Proxy settings: listening port and the list of remote servers. */
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
    struct
    {
        uint16_t port;
        char ip[SRV_IPSTRLENGTH];
    }* remote_servers;
    uint16_t remote_servers_count;
    //-----------------------
};
extern struct server_config server_conf;
//-----------------------------------------
/**
 * @brief Read the config file "<port>,<ip:port>,<ip:port>,...".
 *
 * Ports must be 1..65535, addresses dotted IPv4; parsing stops at the first bad remote server.
 * @param path Config file; falls back to SRV_CONFDEFNAME if missing.
 * @return Non-zero on failure, including no valid remote servers (the config is freed then).
 */
int server_config_init(srv_conf* conf, char* path);
/** @brief Print the config to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Free the remote server list and zero the config. */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/** @brief parse_console_parameters() result: -v/--version was printed, exit with 0. */
#define SRV_PARSE_VERSION 2
/**
 * @brief Parse the -c/--conf and -v/--version options and initialize @p conf.
 * @return Result of server_config_init(), or SRV_PARSE_VERSION after printing NS_VERSION.
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Put a socket into non-blocking mode.
 * @return Result of fcntl() (or ioctl()), -1 on error.
 */
int set_nonblock(evutil_socket_t fd);
//-----------------------------------------
/**
 * @brief Pick a random remote server.
 * @return Index into conf->remote_servers, or -1 if the list is empty.
 */
int proxy_choose_server(const srv_conf* conf);
//-----------------------------------------
/** @brief A proxied pair: the client connection and its remote server connection. */
struct client
{
    /* bufferevent already has separate
     * two buffers for input and output.
     */
    struct bufferevent* b_ev;
    struct event_base* base;
    //---------------------------
    struct bufferevent* server;
    //---------------------------
    /** Number of sides that sent EOF; the pair is freed at 2 with both outputs flushed. */
    int flag_close;
};
//-----------------------------------------
/** @brief Accept a client and connect it to a randomly chosen remote server. */
void proxy_accept_cb(evutil_socket_t fd, short ev, void* arg);
/** @brief Listener error: log it and stop the event loop (@p arg is the event_base). */
void proxy_ac_err_cb(evutil_socket_t fd, short ev, void* arg);
//-----------------------------------------
/** @brief Move all input of one side to the output of the other side. */
void proxy_read_cb(struct bufferevent* b_ev, void* arg);
/**
 * @brief Handle connect, EOF and errors.
 *
 * EOF from one side is passed on as a half-close (shutdown SHUT_WR) of the other side after its
 * output is flushed; the pair is freed after both sides sent EOF. An error frees both sides.
 */
void proxy_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
#endif // _PROXY_H_
