#include "stdafx.h"

#ifndef _PROXY_H_
#define _PROXY_H_
/*
 *  Create a proxy server.
 *  ! LINUX ARCHITECTURE !
 *
 *  v   Build with make.
 *  +   Run:  ./proxy -c/--conf <filename>
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
 * @param path Config file; falls back to SRV_CONFDEFNAME if missing.
 * @return Non-zero on failure (the config is freed then).
 */
int server_config_init(srv_conf* conf, char* path);
/** @brief Print the config to @p stream. */
void server_config_print(srv_conf* conf, FILE* stream);
/** @brief Free the remote server list and zero the config. */
void server_config_free(srv_conf* conf);
//-----------------------------------------
/**
 * @brief Parse the -c/--conf option and initialize @p conf.
 * @return Result of server_config_init().
 */
int parse_console_parameters(int argc, char** argv, srv_conf* conf);
//-----------------------------------------
/**
 * @brief Put a socket into non-blocking mode.
 * @return Result of fcntl() (or ioctl()), -1 on error.
 */
int set_nonblock(evutil_socket_t fd);
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
/** @brief Handle connect, EOF (half-close; free after both sides) and errors (free both). */
void proxy_error_cb(struct bufferevent* b_ev, short events, void* arg);
//-----------------------------------------
#endif // _PROXY_H_
