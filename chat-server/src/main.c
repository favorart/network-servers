/**
 * @file main.c
 * @brief Chat server entry point: command-line options.
 */
#include "chat.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief Print the command-line help to the stream. */
static void usage(FILE* out, const char* prog)
{
    fprintf(out,
            "Usage: %s [-p PORT] [-v]\n"
            "  -p, --port PORT  TCP port to listen on (default %d)\n"
            "  -v, --version    print the version and exit\n"
            "  -h, --help       print this help and exit\n",
            prog, CHAT_DEFAULT_PORT);
}

int main(int argc, char** argv)
{
    static const struct option options[] = {{"port", required_argument, NULL, 'p'},
                                            {"version", no_argument, NULL, 'v'},
                                            {"help", no_argument, NULL, 'h'},
                                            {NULL, 0, NULL, 0}};
    int port = CHAT_DEFAULT_PORT;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:vh", options, NULL)) != -1) {
        switch (opt) {
        case 'p': {
            char* end = NULL;
            long value = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || value < 1 || value > 65535) {
                fprintf(stderr, "%s: invalid port '%s'\n", argv[0], optarg);
                return 2;
            }
            port = (int)value;
            break;
        }
        case 'v':
            printf("%s\n", NS_VERSION);
            return 0;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (optind < argc) {
        usage(stderr, argv[0]);
        return 2;
    }
    return chat_server_run(port);
}
