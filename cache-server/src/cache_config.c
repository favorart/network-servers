#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cache_error.h"
#include "cache_hash.h"
#include "cache.h"

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

//-----------------------------------------
myerr my_errno = SRV_ERR_NONE;
//-----------------------------------------
const char* strmyerror(void)
{
    const char* strerr;
    switch (my_errno) {
    default:
        strerr = NULL;
        break;
    case SRV_ERR_PARAM:
        strerr = " : Invalid function argument.\n";
        break;
    case SRV_ERR_INPUT:
        strerr = " : Input incorrect user data.\n";
        break;
    case SRV_ERR_LIBEV:
        strerr = " : Incorrect libevent entity.\n";
        break;
    case SRV_ERR_RCMMN:
        strerr = " : Incorrect request command.\n";
        break;
    case SRV_ERR_RFREE:
        strerr = " : Dispatched a free request.\n";
        break;
    case SRV_ERR_FDTRS:
        strerr = " : Failure in transmiting fd.\n";
        break;
    }
    my_errno = SRV_ERR_NONE;
    return strerr;
}
//-----------------------------------------
int set_nonblock(evutil_socket_t fd)
{
    int flags;
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
//-----------------------------------------
/* Parse the whole string as a decimal number in [min, max] */
static bool parse_number(const char* str, long min, long max, long* value)
{
    if (!str || !*str)
        return false;
    char* end = NULL;
    errno = 0;
    long v = strtol(str, &end, 10);
    if (errno || *end || v < min || v > max)
        return false;
    *value = v;
    return true;
}
int server_config_set(srv_conf* conf, const char* port, const char* ip, const char* work)
{
    long value = 0;
    //-----------------------
    if (!parse_number(port, 1L, 65535L, &value)) {
        fprintf(stderr, "invalid port '%s': expected 1..65535\n", port ? port : "");
        return -1;
    }
    conf->port = (uint16_t)value;
    //-----------------------
    struct in_addr addr;
    if (!ip || strlen(ip) >= SRV_IPSTRLENGTH || 1 != inet_pton(AF_INET, ip, &addr)) {
        fprintf(stderr, "invalid ip '%s': expected an IPv4 address\n", ip ? ip : "");
        return -1;
    }
    strcpy(conf->ip, ip);
    //-----------------------
    if (!parse_number(work, 1L, (long)SRV_MAX_WORKERS, &value)) {
        fprintf(stderr, "invalid number of workers '%s': expected 1..%u\n", work ? work : "",
                SRV_MAX_WORKERS);
        return -1;
    }
    conf->workers = (wc_t)value;
    //-----------------------
    conf->ptr_server_name = SRV_SERVER_NAME;
    //-----------------------
    ssize_t bytes = 0;
    if (0 >= (bytes = readlink("/proc/self/exe", conf->server_path, FILENAME_MAX - 1U))) {
        fprintf(stderr, "%s\n", strerror(errno));
        strcpy(conf->server_path, "");
    } else
        conf->server_path[bytes] = '\0';
    //-----------------------
    return 0;
}
int server_config_init(srv_conf* conf)
{
    if (!(conf->child_workers = calloc(conf->workers, sizeof(*conf->child_workers)))) {
        perror("child_workers");
        exit(EXIT_FAILURE);
    }

    conf->myself = NULL;
    for (wc_t i = 0U; i < conf->workers; ++i) {
        if (!child_worker_init(&conf->child_workers[i])) {
            /* The child inherited the master's ends of the earlier workers' socketpairs:
             * close them, or those workers would not see the master close its end. */
            for (wc_t j = 0U; j < i; ++j)
                child_worker_free(&conf->child_workers[j]);
            conf->myself = &conf->child_workers[i];
            conf->workers = (i + 1);
            break;
        }
    }
    //-----------------------
    return 0;
}
void server_config_print(srv_conf* conf, FILE* stream)
{
    fprintf(stream,
            ">>> cache server config:\n\n\tname: '%s'   server path: '%s'\n"
            "\tport: %u   ip: '%s'   workers: %u\n",
            conf->ptr_server_name, conf->server_path, conf->port, conf->ip, conf->workers);
    if (conf->ht)
        fprintf(stream, "\tshm: %d   sem: %d\n", conf->ht->tb_shmid, conf->ht->tb_semid);
    fprintf(stream, "\n");
}
void server_config_free(srv_conf* conf)
{
    if (!conf->child_workers)
        return;
    //-----------------------
    for (wc_t i = 0U; i < conf->workers; ++i)
        child_worker_free(&conf->child_workers[i]);
    /* Master: the workers see end of file on their sockets and exit */
    if (!conf->myself) {
        for (wc_t i = 0U; i < conf->workers; ++i) {
            while (0 > waitpid(conf->child_workers[i].pid, NULL, 0) && errno == EINTR) {
            }
        }
    }
    free(conf->child_workers);
    //-----------------------
    conf->child_workers = NULL;
    conf->myself = NULL;
    conf->workers = 0U;
}
//-----------------------------------------
static void print_usage(FILE* stream, const char* name)
{
    fprintf(stream,
            "usage: %s [-p|--port <port>] [-i|--ip <ipv4>] [-w|--workers <1..%u>]\n"
            "       %s -v|--version | -h|--help\n"
            "defaults: port %s, ip %s, %s workers\n",
            name, SRV_MAX_WORKERS, name, SRV_DEFAULT_PORT, SRV_DEFAULT_IP, SRV_DEFAULT_WORKERS);
}
int parse_console_parameters(int argc, char** argv, srv_conf* conf)
{
    static const struct option long_options[] = {
        {"port", required_argument, 0, 'p'},    {"ip", required_argument, 0, 'i'},
        {"workers", required_argument, 0, 'w'}, {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},          {0, 0, 0, 0}};
    const char* name = (argc > 0 && argv[0]) ? argv[0] : "cache-server";
    const char* port = SRV_DEFAULT_PORT;
    const char* ip = SRV_DEFAULT_IP;
    const char* work = SRV_DEFAULT_WORKERS;
    //-----------------------
    int c;
    optind = 0; /* full rescan: the function may be called more than once */
    while (-1 != (c = getopt_long(argc, argv, "p:i:w:vh", long_options, NULL))) {
        switch (c) {
        case 'p':
            port = optarg;
            break;
        case 'i':
            ip = optarg;
            break;
        case 'w':
            work = optarg;
            break;
        case 'v':
            printf("%s\n", NS_VERSION);
            return PARSE_EXIT;
        case 'h':
            print_usage(stdout, name);
            return PARSE_EXIT;
        default:
            /* getopt_long already printed an error message. */
            print_usage(stderr, name);
            return PARSE_ERROR;
        }
    }
    //-----------------------
    if (optind < argc) {
        fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
        print_usage(stderr, name);
        return PARSE_ERROR;
    }
    //-----------------------
    return server_config_set(conf, port, ip, work) ? PARSE_ERROR : PARSE_RUN;
}
//-----------------------------------------
