#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http_error.h"
#include "http_server.h"

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

//-----------------------------------------
int set_nonblock(evutil_socket_t fd)
{
    int flags;
#ifdef O_NONBLOCK
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}
//-----------------------------------------
/**
 * @brief Parse a decimal number in [@p min, @p max] (the whole string must be a number).
 * @return true on success, @p value is set then.
 */
static bool parse_number(const char* str, unsigned long min, unsigned long max,
                         unsigned long* value)
{
    if (!str || *str < '0' || *str > '9')
        return false;
    char* end = NULL;
    errno = 0;
    unsigned long v = strtoul(str, &end, 10);
    if (errno || *end || v < min || v > max)
        return false;
    *value = v;
    return true;
}
int http_server_config_init(srv_conf* conf, const char* path, const char* port, const char* ip,
                            const char* work)
{
    unsigned long number = 0;
    //-----------------------
    if (parse_number(port, 1U, 65535U, &number)) {
        conf->port = (uint16_t)number;
    } else {
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        conf->port = 8080;
    }
    //-----------------------
    struct stat sb;
    if (path && strlen(path) < sizeof(conf->folder_path) && stat(path, &sb) == 0 &&
        S_ISDIR(sb.st_mode)) {
        strcpy(conf->folder_path, path);
    } else {
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        strcpy(conf->folder_path, "data/");
    }
    //-----------------------
    struct in_addr addr;
    if (ip && strlen(ip) < HTTP_IPSTRLENGTH && inet_pton(AF_INET, ip, &addr) == 1) {
        strcpy(conf->ip, ip);
    } else {
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        strcpy(conf->ip, "0.0.0.0");
    }
    //-----------------------
    if (parse_number(work, 1U, HTTP_MAX_WORKERS, &number)) {
        conf->workers = (wc_t)number;
    } else {
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        conf->workers = 4U;
    }
    conf->myself = NULL;
    conf->child_workers = NULL;
    //-----------------------
    /* exec working directory */
    ssize_t bytes = readlink("/proc/self/exe", conf->server_path, FILENAME_MAX - 1U);
    if (bytes <= 0) {
        fprintf(stderr, "%s\n", strerror(errno));
        strcpy(conf->server_path, "");
    } else
        conf->server_path[bytes] = '\0';
    //-----------------------
    conf->ptr_server_name = HTTP_SERVER_NAME;
    //-----------------------
    return 0;
}
int http_server_workers_start(srv_conf* conf)
{
    if (!(conf->child_workers = calloc(conf->workers, sizeof(*conf->child_workers)))) {
        perror("child_workers");
        exit(EXIT_FAILURE);
    }

    conf->myself = NULL;
    for (wc_t i = 0U; i < conf->workers; ++i) {
        if (!child_worker_init(&conf->child_workers[i])) {
            /* A worker keeps only its own end of the socketpairs */
            for (wc_t k = 0U; k < i; ++k) {
                child_worker_free(&conf->child_workers[k]);
            }
            conf->myself = &conf->child_workers[i];
            conf->workers = (i + 1);
            break;
        }
    }
    return 0;
}
void http_server_workers_stop(srv_conf* conf)
{
    if (!conf->child_workers)
        return;
    if (conf->myself) {
        child_worker_free(conf->myself);
    } else {
        /* Workers see the end of stream and leave their event loops */
        for (wc_t i = 0U; i < conf->workers; ++i)
            child_worker_free(&conf->child_workers[i]);
        for (wc_t i = 0U; i < conf->workers; ++i)
            waitpid(conf->child_workers[i].pid, NULL, 0);
    }
    free(conf->child_workers);
    conf->child_workers = NULL;
    conf->myself = NULL;
}
void http_server_config_print(srv_conf* conf, FILE* stream)
{
    fprintf(stream,
            ">>> http server config:\n\n\tname: '%s'   server path: '%s'\n"
            "\tpath: '%s'\n\tport: %u   ip: '%s'\n\tworkers count: %u\n\n",
            conf->ptr_server_name, conf->server_path, conf->folder_path, conf->port, conf->ip,
            conf->workers);
}
void http_server_config_free(srv_conf* conf)
{
    //-----------------------
    memset(conf, 0, sizeof(*conf));
}
//-----------------------------------------
int parse_console_parameters(int argc, char** argv, srv_conf* conf)
{
    char *dir_opt = NULL, *port_opt = NULL, *ip_opt = NULL, *work_opt = NULL;
    static struct option long_options[] = {
        {"dir", required_argument, 0, 'd'},  {"ip", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'}, {"workers", required_argument, 0, 'w'},
        {"version", no_argument, 0, 'v'},    {0, 0, 0, 0}};
    //-----------------------
    int c;
    while (-1 != (c = getopt_long(argc, argv, "d:h:p:w:v", long_options, NULL))) {
        switch (c) {
        case 'd':
            dir_opt = optarg;
            break;
        case 'h':
            ip_opt = optarg;
            break;
        case 'p':
            port_opt = optarg;
            break;
        case 'w':
            work_opt = optarg;
            break;
        case 'v':
            printf("%s %s\n", HTTP_SERVER_NAME, NS_VERSION);
            exit(EXIT_SUCCESS);

        case '?':
            /* getopt_long already printed an error message. */
            printf("using:\n./http-server -d|--dir <dir> -h|--ip <ip> -p|--port <port> "
                   "-w|--workers <num> | -v|--version\n\n");
            break;

        default:
            printf("?? getopt returned character code 0%o ??\n", c);
            break;
        }
    }
    //-----------------------
    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
    }
    //-----------------------
    return http_server_config_init(conf, dir_opt, port_opt, ip_opt, work_opt);
}
//-----------------------------------------
