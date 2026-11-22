/**
 * @file chat_client.cpp
 * @brief Console chat client on select(): sends stdin lines, prints server messages.
 */
#include <iostream>
#include <algorithm>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

#define PORT 3100U
#define BUFSIZE 1024U

/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl()/ioctl(), -1 on error.
 */
static int set_nonblock(int fd)
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
        flags = 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

/** @brief Print the command-line help to the stream. */
static void usage(std::ostream& out, const char* prog)
{
    out << "Usage: " << prog << " [-p PORT] [-v]\n"
        << "  -p, --port PORT  server port on localhost (default " << PORT << ")\n"
        << "  -v, --version    print the version and exit\n"
        << "  -h, --help       print this help and exit\n";
}

int main(int argc, char** argv)
{
    static const struct option options[] = {{"port", required_argument, NULL, 'p'},
                                            {"version", no_argument, NULL, 'v'},
                                            {"help", no_argument, NULL, 'h'},
                                            {NULL, 0, NULL, 0}};
    unsigned short port = PORT;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:vh", options, NULL)) != -1) {
        switch (opt) {
        case 'p': {
            char* end = NULL;
            long value = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || value < 1 || value > 65535) {
                std::cerr << argv[0] << ": invalid port '" << optarg << "'\n";
                return 2;
            }
            port = static_cast<unsigned short>(value);
            break;
        }
        case 'v':
            std::cout << NS_VERSION << std::endl;
            return 0;
        case 'h':
            usage(std::cout, argv[0]);
            return 0;
        default:
            usage(std::cerr, argv[0]);
            return 2;
        }
    }

    int sendSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sendSocket == -1) {
        std::cout << strerror(errno) << std::endl;
        return 1;
    }

    sockaddr_in SockAddr = {};
    SockAddr.sin_family = AF_INET;
    SockAddr.sin_port = htons(port);
    SockAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sendSocket, (struct sockaddr*)(&SockAddr), sizeof(SockAddr)) == -1) {
        std::cout << strerror(errno) << std::endl;
        return 1;
    }

    set_nonblock(sendSocket);
    set_nonblock(STDIN_FILENO);

    char buffer[BUFSIZE + 1U];

    while (true) {
        fd_set readset;

        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(sendSocket, &readset);

        if (select(std::max(0, sendSocket) + 1, &readset, NULL, NULL, NULL) == -1) {
            std::cout << "Error occured\n";
            return 1;
        }

        if (FD_ISSET(sendSocket, &readset)) {
            while (true) {
                ssize_t recvSize = recv(sendSocket, buffer, BUFSIZE, MSG_NOSIGNAL);
                if (recvSize == 0) {
                    std::cout << "Server closed the connection" << std::endl;
                    close(sendSocket);
                    return 0;
                }
                if (recvSize < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        break;
                    }
                    std::cout << strerror(errno) << std::endl;
                    close(sendSocket);
                    return 1;
                }
                buffer[recvSize] = 0;
                std::cout << "Message from server: " << buffer;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            if (fgets(buffer, BUFSIZE, stdin) == NULL) {
                break;
            }
            size_t len = strlen(buffer);
            std::cout << "\x1b[1A\x1b[2K";
            send(sendSocket, buffer, len, MSG_NOSIGNAL);
        }
    }

    close(sendSocket);
    return 0;
}
