/**
 * @file chat_server.c
 * @brief Chat server on epoll: every line a client sends goes to all clients.
 */
#include "chat.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

int set_nonblock(int fd)
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

int messager_connection_accept(struct messager_client* Users, int EPoll, int SlaveSocket)
{
    /* The client table is keyed by socket: take a free slot */
    struct messager_client* User = messager_client_find(Users, -1);
    if (!User) {
        fprintf(stderr, "too many clients\n");
        close(SlaveSocket);
        return 0;
    }
    User->fd = SlaveSocket;
    //--------------------------------
    // Create an epoll entry for it
    struct epoll_event Event;
    Event.data.fd = SlaveSocket;
    Event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    // ET - read until zero and write until zero
    //--------------------------------
    // EPoll - file descriptor of the event data structure
    // EPOLL_CTL_ADD - modify this structure
    epoll_ctl(EPoll, EPOLL_CTL_ADD, SlaveSocket, &Event);
    //--------------------------------
    printf("accepted connection\n");
    fflush(stdout);
    //--------------------------------
    char welcome[] = "Welcome\n";
    return messager_client_send_other(User, 1, welcome, strlen(welcome));
}

int messager_connection_close(struct messager_client* User)
{
    if (User->fd < 0)
        return 0;
    //--------------------------------
    // disconnect Users
    shutdown(User->fd, SHUT_RDWR);
    close(User->fd);
    User->fd = -1;
    //--------------------------------
    printf("connection terminated\n");
    fflush(stdout);
    //--------------------------------
    messager_client_clear(User, CLR_IN | CLR_OUT);
    //--------------------------------
    return 0;
}

int messager_client_initialize(struct messager_client* User)
{
    memset(User, 0, sizeof(*User));
    User->fd = -1;
    //--------------------------------
    User->out_maxlen = MSG_LIMIT;
    User->out_buffer = calloc(User->out_maxlen, sizeof(*User->out_buffer));
    return (User->out_buffer == NULL);
    //--------------------------------
}

int messager_client_free(struct messager_client* User)
{
    //--------------------------------
    free(User->out_buffer);
    memset(User, 0, sizeof(*User));
    User->fd = -1;
    return 0;
}

int messager_client_clear(struct messager_client* User, int what)
{
    if (what & CLR_IN) {
        memset(User->in_buffer, 0, User->in_length);
        User->in_length = 0;
    }
    //--------------------------------
    if (what & CLR_OUT) {
        memset(User->out_buffer, 0, User->out_maxlen);
        User->out_length = 0;
    }
    return 0;
}

struct messager_client* messager_client_find(struct messager_client* Users, int fd)
{
    for (unsigned int j = 0U; j < USR_LIMIT; ++j)
        if (Users[j].fd == fd)
            return &Users[j];
    return NULL;
}

int messager_split_lines(struct messager_client* User, const char* recv_buffer, size_t recv_length,
                         messager_line_cb on_line, void* ctx)
{
    size_t pos = 0U;
    while (pos < recv_length) {
        /* take up to the newline, but no more than fits into the line */
        size_t cur_length = recv_length - pos;
        const char* recv_ptr2nline = memchr(recv_buffer + pos, '\n', cur_length);
        if (recv_ptr2nline)
            cur_length = (size_t)(recv_ptr2nline - (recv_buffer + pos)) + 1U;

        size_t room = (MSG_LIMIT - 1U) - User->in_length;
        if (cur_length > room)
            cur_length = room; /* too long */
        //--------------------------------
        memcpy(User->in_buffer + User->in_length, recv_buffer + pos, cur_length);
        User->in_length += cur_length;
        pos += cur_length;
        //--------------------------------
        if (User->in_buffer[User->in_length - 1U] == '\n' || User->in_length == (MSG_LIMIT - 1U)) {
            char msg_buffer[MSG_LIMIT];
            size_t msg_length = User->in_length;
            memcpy(msg_buffer, User->in_buffer, msg_length);
            messager_client_clear(User, CLR_IN);

            int rc = on_line(ctx, msg_buffer, msg_length);
            if (rc)
                return rc;
        }
    }
    return 0;
}

/** @brief messager_line_cb that sends the message to every client. */
static int messager_broadcast_line(void* ctx, char* msg_buffer, size_t msg_length)
{
    return messager_client_send_other((struct messager_client*)ctx, USR_LIMIT, msg_buffer,
                                      msg_length);
}

int messager_client_receiving(struct messager_client* User, struct messager_client* Users)
{
    char recv_buffer[MSG_LIMIT];
    ssize_t recv_length = 0;
    //--------------------------------
    /* reading from the socket until EAGAIN (edge-triggered) */
    while (User->fd >= 0 &&
           0 < (recv_length = recv(User->fd, recv_buffer, sizeof(recv_buffer), MSG_NOSIGNAL))) {
        /* parse the messages */
        if (messager_split_lines(User, recv_buffer, (size_t)recv_length, messager_broadcast_line,
                                 Users))
            return 1;
    }
    //--------------------------------
    /* EOF or a hard error: disconnect (the client may be closed already) */
    if (User->fd >= 0 &&
        (recv_length == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)))
        messager_connection_close(User);
    //--------------------------------
    return 0;
}

int messager_client_flush(struct messager_client* User)
{
    while (User->fd >= 0 && User->out_length) {
        /* non-block sending */
        ssize_t ret = send(User->fd, User->out_buffer, User->out_length, MSG_NOSIGNAL);
        if (ret <= 0) {
            /* soft and hard disconnect */
            if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                messager_connection_close(User);
            /* the rest goes out on EPOLLOUT */
            break;
        }
        if ((size_t)ret < User->out_length) {
            memmove(User->out_buffer, User->out_buffer + ret, User->out_length - (size_t)ret);
            User->out_length -= (size_t)ret;
        } else {
            messager_client_clear(User, CLR_OUT);
        }
    }
    return 0;
}

int messager_client_send_other(struct messager_client* Users, int N, char* msg_buffer,
                               size_t msg_length)
{
    if (!msg_length)
        return 0;
    //--------------------------------
    /* Sending to everybody connected */
    for (int j = 0; j < N; ++j) {
        if (Users[j].fd < 0)
            continue;
        if ((Users[j].out_length + msg_length) > Users[j].out_maxlen) {
            //--------------------------------
            char* out_buffer = realloc(Users[j].out_buffer, Users[j].out_maxlen * 2U);
            if (!out_buffer)
                return 1;
            Users[j].out_buffer = out_buffer;
            Users[j].out_maxlen *= 2U;
        }
        //--------------------------------
        memcpy(Users[j].out_buffer + Users[j].out_length, msg_buffer, msg_length);
        Users[j].out_length += msg_length;
    } // end sending to everybody

    //--------------------------------
    for (int j = 0; j < N; ++j)
        messager_client_flush(&Users[j]);

    //--------------------------------
    /* log the message without the newline; msg_buffer holds MSG_LIMIT bytes */
    if (msg_buffer[msg_length - 1U] == '\n')
        --msg_length;
    msg_buffer[msg_length] = '\0';
    printf("message = '%s'\n", msg_buffer);
    fflush(stdout);
    //--------------------------------
    return 0;
}

/** @brief Set by SIGINT/SIGTERM to stop the event loop. */
static volatile sig_atomic_t stop_requested = 0;

/** @brief SIGINT/SIGTERM handler: request a clean stop. */
static void on_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

int chat_server_run(int port)
{
    bool was_fail = false;
    int EPoll = -1;
    /* Two private buffers of each connected client */
    struct messager_client Users[USR_LIMIT];
    for (unsigned int i = 0U; i < USR_LIMIT; ++i) {
        Users[i].fd = -1;
        Users[i].out_buffer = NULL;
    }
    //--------------------------------
    /* SIGINT/SIGTERM are blocked except inside epoll_pwait(), so none is missed */
    sigset_t stop_signals, wait_mask;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    sigprocmask(SIG_BLOCK, &stop_signals, &wait_mask);
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    //--------------------------------
    /*  This socket listens and accepts
     *  all incoming connections, which is
     *  what makes it differ from other sockets
     */
    int MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (MasterSocket == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        was_fail = true;
        goto FREE_ALL;
    }

    int so_reuseaddr = 1; /* manipulate the flags set on the socket */
    if (setsockopt(MasterSocket, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr))) {
        fprintf(stderr, "%s\n", strerror(errno));
        was_fail = true;
        goto FREE_ALL;
    }

    //--------------------------------
    struct sockaddr_in SockAddr = {0};
    /* IPv4 */
    SockAddr.sin_family = AF_INET;
    SockAddr.sin_port = htons((unsigned short)port);
    SockAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* bind the address to the socket */
    int Result = bind(MasterSocket, (struct sockaddr*)&SockAddr, sizeof(SockAddr));
    if (Result == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        was_fail = true;
        goto FREE_ALL;
    }

    /* non-blocking mode, for multiplexing */
    set_nonblock(MasterSocket);

    /*  Connection-waiting mode - a kernel flag
     *  and accepting connections, the buffer (backlog)
     *  takes TCP requests
     */
    Result = listen(MasterSocket, SOMAXCONN);
    if (Result == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        was_fail = true;
        goto FREE_ALL;
    }

    /* Event for creating the e-poll object */
    struct epoll_event Event;
    Event.data.fd = MasterSocket;
    Event.events = EPOLLIN | EPOLLET;
    /*  ET - trigger regime - nothing new arrives on the socket
     *       until everything already received is read (accept too).
     */

    struct epoll_event Events[MAX_EVENTS] = {0};
    /* Create the EPoll object to monitor socket changes inside the OS kernel */
    EPoll = epoll_create1(0);
    /* Add the first monitored object - the socket in connection-accepting mode */
    if (EPoll == -1 || epoll_ctl(EPoll, EPOLL_CTL_ADD, MasterSocket, &Event) == -1) {
        fprintf(stderr, "%s\n", strerror(errno));
        was_fail = true;
        goto FREE_ALL;
    }

    //--------------------------------
    for (unsigned int i = 0U; i < USR_LIMIT; ++i) {
        //--------------------------------
        if (messager_client_initialize(&Users[i])) {
            fprintf(stderr, "%s\n", strerror(errno));
            was_fail = true;
            goto FREE_ALL;
        }
    }

    //--------------------------------

    /* Main message-processing loop */
    while (!stop_requested) {
        /*  Multiplexing - when many descriptors wait for different events.
         *  The events need not be uniform, and handling them needs different code.
         */
        int N = epoll_pwait(EPoll, Events, MAX_EVENTS, -1, &wait_mask);
        if (N == -1) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "%s\n", strerror(errno));
            was_fail = true;
            goto FREE_ALL;
        }

        for (int i = 0; i < N; ++i) {
            int fd = Events[i].data.fd;
            if (fd == MasterSocket) {
                //--------------------------------
                /* Accept the connection */
                int SlaveSocket;
                while ((SlaveSocket = accept(MasterSocket, 0, 0)) != -1) {
                    set_nonblock(SlaveSocket);
                    if (messager_connection_accept(Users, EPoll, SlaveSocket)) {
                        fprintf(stderr, "%s\n", strerror(errno));
                        was_fail = true;
                        goto FREE_ALL;
                    }
                }
                continue;
            }
            //--------------------------------
            /* The client may be closed already by an earlier event of this batch */
            struct messager_client* User = messager_client_find(Users, fd);
            if (!User)
                continue;
            /* An error or hang-up shows up in recv() as EOF or an error */
            if (Events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                if (messager_client_receiving(User, Users)) {
                    fprintf(stderr, "%s\n", strerror(errno));
                    was_fail = true;
                    goto FREE_ALL;
                }
            }
            if (Events[i].events & EPOLLOUT)
                messager_client_flush(User);
        } // end for Event [i]
    } // end while

FREE_ALL:;
    //-------------------------------------
    for (unsigned int i = 0U; i < USR_LIMIT; ++i) {
        if (Users[i].fd >= 0)
            close(Users[i].fd);
        messager_client_free(&Users[i]);
    }

    if (EPoll != -1)
        close(EPoll);
    if (MasterSocket != -1) {
        shutdown(MasterSocket, SHUT_RDWR);
        close(MasterSocket);
    }

    //--------------------------------
    return was_fail ? 1 : 0;
}
