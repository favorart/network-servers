#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "proxy_error.h"
#include "proxy.h"

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
        strerr = "Invalid function argument.";
        break;
    case SRV_ERR_INPUT:
        strerr = "Input incorrect user data.";
        break;
    case SRV_ERR_LIBEV:
        strerr = "Incorrect libevent entity.";
        break;
    case SRV_ERR_RCMMN:
        strerr = "Incorrect request command.";
        break;
    case SRV_ERR_RFREE:
        strerr = "Dispatched a free request.";
        break;
    case SRV_ERR_FCONF:
        strerr = "Config file isn't correct.";
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
int server_config_init(srv_conf* conf, char* path)
{
    bool fail = false;
    FILE* f_conf = NULL;
    char* fileline = NULL;
    //-----------------------
    srand((unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16));
    //-----------------------
    conf->ptr_server_name = SRV_SERVER_NAME;
    //-----------------------
    strcpy(conf->ip, "0.0.0.0");
    //-----------------------
    size_t filesize = 0;
    struct stat sb;
    if (path && strlen(path) < sizeof(conf->config_path) && stat(path, &sb) == 0) {
        strcpy(conf->config_path, path);
        filesize = sb.st_size;
    } else {
        my_errno = SRV_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());

        if (stat(SRV_CONFDEFNAME, &sb) == 0) {
            strcpy(conf->config_path, SRV_CONFDEFNAME);
            filesize = sb.st_size;
        } else {
            my_errno = SRV_ERR_INPUT;
            fprintf(stderr, "%s\n", strmyerror());
            fail = true;
            goto CONF_FREE;
        }
    }
    //-----------------------
    int bytes = 0U;
    if (0 >= (bytes = readlink("/proc/self/exe", conf->server_path, FILENAME_MAX - 1U))) {
        fprintf(stderr, "%s\n", strerror(errno));
        strcpy(conf->server_path, "");
    } else
        conf->server_path[bytes] = '\0';
    //-----------------------
    f_conf = fopen(conf->config_path, "rt");
    if (!f_conf) {
        fprintf(stderr, "%s\n", strerror(errno));
        fail = true;
        goto CONF_FREE;
    }

    fileline = malloc((filesize + 1U) * sizeof(char));
    if (!fileline) {
        fprintf(stderr, "%s\n", strerror(errno));
        fail = true;
        goto CONF_FREE;
    }

    size_t got = fread(fileline, 1U, filesize, f_conf);
    fileline[got] = '\0';

    size_t count = 0;
    char* str = fileline;
    while ((str = strchr(str, ','))) {
        ++str;
        ++count;
    }
    //-----------------------
    conf->remote_servers_count = count;
    conf->remote_servers = calloc(count, sizeof(*conf->remote_servers));
    if (!conf->remote_servers) {
        fprintf(stderr, "%s\n", strerror(errno));
        fail = true;
        goto CONF_FREE;
    }
    //-----------------------

    str = fileline;
    int str_offset = 0;
    const unsigned max_port = 65535U;
    unsigned port = 0U;
    if (1 != sscanf(fileline, "%u,%n", &port, &str_offset) || !port || port > max_port) {
        my_errno = SRV_ERR_FCONF;
        fprintf(stderr, "%s\n", strmyerror());
        port = 8080;
    }
    conf->port = (uint16_t)port;
    str += str_offset;
    //-----------------------
    for (size_t i = 0U; i < count; ++i) {
        unsigned ip1, ip2, ip3, ip4;
        if (5 != sscanf(str, "%u.%u.%u.%u:%u,%n", &ip1, &ip2, &ip3, &ip4, &port, &str_offset) ||
            ip1 > 255U || ip2 > 255U || ip3 > 255U || ip4 > 255U || !port || port > max_port) {
            my_errno = SRV_ERR_FCONF;
            fprintf(stderr, "%s\n", strmyerror());

            conf->remote_servers_count = i;
            break;
        }

        conf->remote_servers[i].port = (uint16_t)port;
        snprintf(conf->remote_servers[i].ip, sizeof(conf->remote_servers[i].ip), "%u.%u.%u.%u", ip1,
                 ip2, ip3, ip4);
        str += str_offset;
    }
    //-----------------------
    if (!conf->remote_servers_count) {
        my_errno = SRV_ERR_FCONF;
        fprintf(stderr, "No remote servers: %s\n", strmyerror());
        fail = true;
    }

    //-----------------------
CONF_FREE:;
    free(fileline);
    if (f_conf)
        fclose(f_conf);
    if (fail)
        server_config_free(conf);
    //-----------------------
    return fail;
}
void server_config_print(srv_conf* conf, FILE* stream)
{
    fprintf(stream,
            ">>> proxy server config:\n\n\tname: '%s'   server path: '%s'\n"
            "\tconf: '%s'\n\tport: %u   ip: '%s'\n",
            conf->ptr_server_name, conf->server_path, conf->config_path, conf->port, conf->ip);
    for (uint16_t i = 0U; i < conf->remote_servers_count; ++i)
        fprintf(stream, "\tremote: %s:%u\n", conf->remote_servers[i].ip,
                conf->remote_servers[i].port);
    fprintf(stream, "\n");
}
void server_config_free(srv_conf* conf)
{
    //-----------------------
    free(conf->remote_servers);
    memset(conf, 0, sizeof(*conf));
}
//-----------------------------------------
int parse_console_parameters(int argc, char** argv, srv_conf* conf)
{
    char* conf_opt = NULL;
    static struct option long_options[] = {
        {"conf", required_argument, 0, 'c'}, {"version", no_argument, 0, 'v'}, {0, 0, 0, 0}};
    //-----------------------
    int c, option_index = 0;
    while (-1 != (c = getopt_long(argc, argv, "c:v", long_options, &option_index))) {
        switch (c) {
        case 0:
            printf("option %s", long_options[option_index].name);
            break;

        case 'c':
            conf_opt = optarg;
            break;

        case 'v':
            printf("%s\n", NS_VERSION);
            return SRV_PARSE_VERSION;

        case '?':
            /* getopt_long already printed an error message. */
            printf("using:\n./proxy-server -c|--conf <conf> | -v|--version\n\n");
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
    return server_config_init(conf, conf_opt);
}
//-----------------------------------------
