#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>

#include "subs_error.h"
#include "subs_server.h"

//-----------------------------------------
const char* strmyerror(myerr err)
{
    const char* strerr;
    switch (err) {
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
int server_config_init(srv_conf* conf, char* ip, char* port, char* queues)
{
    //-----------------------
    const uint16_t max_port = 65535U;
    if (!port || (conf->port = atoi(port)) > max_port) {
        conf->port = 3131;
    }
    //-----------------------
    if (ip && strlen(ip) < SRV_IPSTRLENGTH) {
        strcpy(conf->ip, ip);
    } else {
        strcpy(conf->ip, "0.0.0.0");
    }
    //-----------------------
    conf->ptr_server_name = (char*)SRV_SERVER_NAME;
    //-----------------------
    ssize_t bytes = readlink("/proc/self/exe", conf->server_path, FILENAME_MAX - 1U);
    if (0 >= bytes) {
        fprintf(stderr, "%s\n", strerror(errno));
        strcpy(conf->server_path, "");
    } else
        conf->server_path[bytes] = '\0';
    //-----------------------
    if (queues) {
        conf->n_queues = atoi(queues);
    } else {
        conf->n_queues = 2U;
    }

    conf->queues_manager = new QueuesManager(conf->n_queues);
    //-----------------------
    return 0;
}
void server_config_print(srv_conf* conf, FILE* stream)
{
    fprintf(stream,
            "Server config:\n\n\tname: '%s'\n\tserver path: '%s'\n"
            "\tport: %u\n\tip: '%s'\n\tqueues: %d\n\n",
            conf->ptr_server_name, conf->server_path, conf->port, conf->ip, conf->n_queues);
}
void server_config_free(srv_conf* conf)
{
    delete conf->queues_manager;
    *conf = server_config();
}
//-----------------------------------------
static void print_usage(FILE* stream)
{
    fprintf(stream,
            "using:\n\t./%s -h|--ip <ip> -p|--port <port> -q|--queues <num>\n"
            "\t-v|--version\n\n",
            SRV_SERVER_NAME);
}

int parse_console_parameters(int argc, char** argv, srv_conf* conf)
{
    char *port_opt = NULL, *ip_opt = NULL, *queue_opt = NULL;
    /* -d, -w and -c are kept for compatibility and ignored */
    static struct option long_options[] = {
        {"dir", required_argument, 0, 'd'},    {"ip", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},   {"workers", required_argument, 0, 'w'},
        {"queues", required_argument, 0, 'q'}, {"conf", required_argument, 0, 'c'},
        {"version", no_argument, 0, 'v'},      {0, 0, 0, 0}};
    //-----------------------
    int c, option_index = 0;
    while (-1 != (c = getopt_long(argc, argv, "d:h:p:w:q:c:v", long_options, &option_index))) {
        switch (c) {
        case 'h':
            ip_opt = optarg;
            break;
        case 'p':
            port_opt = optarg;
            break;
        case 'q':
            queue_opt = optarg;
            break;
        case 'd':
        case 'w':
        case 'c':
            break;
        case 'v':
            printf("%s %s\n", SRV_SERVER_NAME, NS_VERSION);
            exit(EXIT_SUCCESS);

        case '?':
            /* getopt_long already printed an error message. */
            print_usage(stdout);
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
    return server_config_init(conf, ip_opt, port_opt, queue_opt);
}
//-----------------------------------------
