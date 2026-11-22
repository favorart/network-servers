#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "srv.h"
#include "srv_cache.h"
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
        strerr = " : Invalid function argument.";
        break;
    case SRV_ERR_INPUT:
        strerr = " : Input incorrect user data.";
        break;
    case SRV_ERR_LIBEV:
        strerr = " : Incorrect libevent entity.";
        break;
    case SRV_ERR_RCMMN:
        strerr = " : Incorrect request command.";
        break;
    case SRV_ERR_RFREE:
        strerr = " : Dispatched a free request.";
        break;
    case SRV_ERR_FDTRS:
        strerr = " : Failure in transmiting fd.";
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
void resolve_dict_path(const char* dict, const char* exe_path, char* out)
{
    if (dict) {
        snprintf(out, FILENAME_MAX, "%s", dict);
        return;
    }
    if (!access(SRV_DICT_FLNAME, R_OK)) {
        snprintf(out, FILENAME_MAX, "%s", SRV_DICT_FLNAME);
        return;
    }
    /* Directory of the binary, with the trailing slash ("" if unknown). */
    const char* slash = strrchr(exe_path, '/');
    int dir_len = slash ? (int)(slash - exe_path + 1) : 0;

    snprintf(out, FILENAME_MAX, "%.*s%s", dir_len, exe_path, SRV_DICT_FLNAME);
    if (!access(out, R_OK))
        return;
    snprintf(out, FILENAME_MAX, "%.*s../data/%s", dir_len, exe_path, SRV_DICT_FLNAME);
}
//-----------------------------------------
int server_config_init(srv_conf* conf, uint16_t port, const char* ip, int work, const char* dict)
{
    if (!ip || strlen(ip) >= SRV_IPSTRLENGTH || work < 1 || work > (int)SRV_MAX_WORKERS) {
        my_errno = SRV_ERR_INPUT;
        fprintf(stderr, "config%s\n", strmyerror());
        return -1;
    }
    conf->port = port;
    strcpy(conf->ip, ip);
    conf->workers_count = (wc_t)work;
    //-----------------------
    conf->ptr_server_name = SRV_SERVER_NAME;
    //-----------------------
    ssize_t bytes = readlink("/proc/self/exe", conf->server_path, FILENAME_MAX - 1U);
    if (bytes <= 0) {
        fprintf(stderr, "%s\n", strerror(errno));
        strcpy(conf->server_path, "");
    } else
        conf->server_path[bytes] = '\0';
    //-----------------------
    resolve_dict_path(dict, conf->server_path, conf->dict_path);
    //-----------------------
    if (wque_init(&conf->workqueue, conf->workers_count)) {
        perror("work queue init");
        exit(EXIT_FAILURE);
    }

    if (!(conf->ht = malloc(sizeof(*conf->ht)))) {
        perror("ht malloc");
        exit(EXIT_FAILURE);
    }

    hashtable_init(conf->ht, DICT_CACHELNS, 0, my_key_comp);
    //-----------------------
    return 0;
}
void server_config_print(srv_conf* conf, FILE* stream)
{
    fprintf(stream,
            ">>> dictionary server config:\n\n\tname: '%s'   server path: '%s'\n"
            "\tport: %u   ip: '%s'\n\tdictionary: '%s'\n\n",
            conf->ptr_server_name, conf->server_path, conf->port, conf->ip, conf->dict_path);
}
void server_config_free(srv_conf* conf)
{
    /* Stop the workers first: their clients use the cache. */
    wque_free(&conf->workqueue);
    //-----------------------
    hashtable_free(conf->ht);
    free(conf->ht);
    //-----------------------
    memset(conf, 0, sizeof(*conf));
}
//-----------------------------------------
static void print_usage(FILE* stream, const char* prog)
{
    fprintf(stream,
            "usage: %s [-p PORT] [-i IP] [-w WORKERS] [-d DICT] [-v] [-h]\n"
            "  -p, --port PORT      TCP port to listen on (default 8080; 0: any free port;\n"
            "                       the server prints \"listening on IP:PORT\")\n"
            "  -i, --ip IP          IPv4 address to bind (default 0.0.0.0)\n"
            "  -w, --workers N      worker threads, 1..%u (default 4)\n"
            "  -d, --dict FILE      dictionary file (default: ./" SRV_DICT_FLNAME " if it exists,\n"
            "                       else <binary dir>/" SRV_DICT_FLNAME " if it exists,\n"
            "                       else <binary dir>/../data/" SRV_DICT_FLNAME ")\n"
            "  -v, --version        print the version and exit\n"
            "  -h, --help           print this help and exit\n",
            prog, SRV_MAX_WORKERS);
}
/** @brief Parse a decimal integer in [min, max]. @return false on success. */
static bool parse_int(const char* s, long min, long max, long* out)
{
    char* end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max)
        return true;
    *out = v;
    return false;
}
int parse_console_parameters(int argc, char** argv, srv_conf* conf)
{
    static struct option long_options[] = {{"port", required_argument, 0, 'p'},
                                           {"ip", required_argument, 0, 'i'},
                                           {"workers", required_argument, 0, 'w'},
                                           {"dict", required_argument, 0, 'd'},
                                           {"version", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};
    long port = 8080, work = 4;
    const char* ip = "0.0.0.0";
    const char* dict = NULL;
    //-----------------------
    int c;
    while (-1 != (c = getopt_long(argc, argv, "p:i:w:d:vh", long_options, NULL))) {
        switch (c) {
        case 'p':
            if (parse_int(optarg, 0, 65535, &port)) {
                fprintf(stderr, "invalid port '%s'\n", optarg);
                return -1;
            }
            break;
        case 'i':
            ip = optarg;
            break;
        case 'w':
            if (parse_int(optarg, 1, SRV_MAX_WORKERS, &work)) {
                fprintf(stderr, "invalid number of workers '%s'\n", optarg);
                return -1;
            }
            break;
        case 'd':
            dict = optarg;
            break;
        case 'v':
            printf("%s\n", NS_VERSION);
            return 1;
        case 'h':
            print_usage(stdout, argv[0]);
            return 1;
        default:
            /* getopt_long already printed an error message. */
            print_usage(stderr, argv[0]);
            return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
        print_usage(stderr, argv[0]);
        return -1;
    }
    //-----------------------
    return server_config_init(conf, (uint16_t)port, ip, (int)work, dict);
}
//-----------------------------------------
