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

int messager_connection_accept(struct messager_client* User, int EPoll, int SlaveSocket)
{
    //--------------------------------
    // Create an epoll entry for it
    struct epoll_event Event;
    Event.data.fd = SlaveSocket;
    Event.events = EPOLLIN | EPOLLET;
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
    return messager_client_send_other(User, &Event, 1, welcome, strlen(welcome));
}

int messager_connection_close(struct messager_client* User, struct epoll_event* Event)
{
    //--------------------------------
    // disconnect Users
    shutdown(Event->data.fd, SHUT_RDWR);
    close(Event->data.fd);
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
    return 0;
}

int messager_client_clear(struct messager_client* User, int what)
{
    if (what == CLR_IN) {
        memset(User->in_buffer, 0, User->in_length);
        User->in_length = 0;
    }
    //--------------------------------
    if (what == CLR_OUT) {
        memset(User->out_buffer, 0, User->out_maxlen);
        User->out_length = 0;
    }
    return 0;
}

int messager_client_receiving(struct messager_client* User, struct epoll_event* Event, int N,
                              struct messager_client* Users, struct epoll_event* Events)
{
    char recv_buffer[MSG_LIMIT] = {0};
    ssize_t recv_length = 0;
    //--------------------------------
    /* reading from the socket (recv() is read() without signal) */
    while (0 < (recv_length = recv(Event->data.fd, recv_buffer, (MSG_LIMIT - 1U), MSG_NOSIGNAL))) {
        char msg_buffer[MSG_LIMIT] = {0};
        size_t msg_length = 0U;
        //--------------------------------
        /* parse the messages */
        char* recv_ptr2nline;
        while ((recv_ptr2nline = memchr(recv_buffer, '\n', (size_t)recv_length))) {
            //--------------------------------
            size_t cur_length = (size_t)(++recv_ptr2nline - recv_buffer);
            if ((cur_length + User->in_length) >= MSG_LIMIT) {
                /* too long */
                cur_length -= (cur_length + User->in_length) - (MSG_LIMIT - 1U);
            }
            //--------------------------------
            memcpy(msg_buffer, User->in_buffer, User->in_length);
            memcpy(msg_buffer + User->in_length, recv_buffer, cur_length);
            msg_length = (User->in_length + cur_length);
            //--------------------------------
            User->in_length = ((size_t)recv_length - cur_length);
            memcpy(User->in_buffer, recv_buffer + cur_length, User->in_length);
            //--------------------------------
            if (messager_client_send_other(Users, Events, N, msg_buffer, msg_length))
                return 1;
            //--------------------------------
            if (((size_t)recv_length - cur_length)) {
                memmove(recv_buffer, recv_buffer + cur_length, ((size_t)recv_length - cur_length));
                recv_length -= (ssize_t)cur_length;
            }
            recv_length -= (ssize_t)cur_length;
            //--------------------------------
        } // end if '\n'

        if (recv_length) {
            //--------------------------------
            recv_ptr2nline = recv_buffer;
            if (((size_t)recv_length + User->in_length) >= MSG_LIMIT) {
                size_t cur_length = ((size_t)recv_length + User->in_length) - (MSG_LIMIT - 1U);
                //--------------------------------
                memcpy(msg_buffer, User->in_buffer, User->in_length);
                memcpy(msg_buffer + User->in_length, recv_buffer, cur_length);
                msg_length = (MSG_LIMIT - 1U);
                //--------------------------------
                recv_ptr2nline += cur_length;
                recv_length -= (ssize_t)cur_length;
                //--------------------------------
                if (messager_client_send_other(Users, Events, N, msg_buffer, msg_length))
                    return 1;
                //--------------------------------
            }
            //--------------------------------
            memcpy(User->in_buffer, recv_ptr2nline, (size_t)recv_length);
            User->in_length = (size_t)recv_length;
            //--------------------------------
        }

        //--------------------------------
        memset(recv_buffer, 0, MSG_LIMIT);
        recv_length = 0;
        //--------------------------------
    } // end while

    //--------------------------------
    if (recv_length <= 0) { /* disconnect */
        if (errno != EAGAIN)
            messager_connection_close(User, Event);
    }
    //--------------------------------
    return 0;
}

int messager_client_send_other(struct messager_client* Users, struct epoll_event* Events, int N,
                               char* msg_buffer, size_t msg_length)
{
    if (msg_length) {
        //--------------------------------
        /* Sending to everybody */
        for (int j = 0; j < N; ++j) {
            if ((Users[j].out_length + msg_length) > Users[j].out_maxlen) {
                //--------------------------------
                Users[j].out_buffer = realloc(Users[j].out_buffer, Users[j].out_maxlen *= 2U);
                if (!Users[j].out_buffer)
                    return 1;
            }

            //--------------------------------
            memcpy(Users[j].out_buffer + Users[j].out_length, msg_buffer, msg_length);
            Users[j].out_length += msg_length;
            //--------------------------------
        } // end sending to everybody
    }

    //--------------------------------
    for (int j = 0; j < N; ++j) {
        if (Users[j].out_length) {
            //--------------------------------
            /* non-block sending */
            ssize_t ret =
                send(Events[j].data.fd, Users[j].out_buffer, Users[j].out_length, MSG_NOSIGNAL);
            if (ret <= 0) {
                //--------------------------------
                /* soft and hard disconnect */
                if (errno != EAGAIN)
                    messager_connection_close(&Users[j], &Events[j]);
            } else {
                //--------------------------------
                if ((size_t)ret < Users[j].out_length) {
                    memmove(Users[j].out_buffer, Users[j].out_buffer + ret,
                            Users[j].out_length - (size_t)ret);
                } else {
                    messager_client_clear(&Users[j], CLR_OUT);
                }
            }
            //--------------------------------
        } // end if out_length
    } // end for

    //--------------------------------
    if (msg_buffer[msg_length - 1] == '\n')
        --msg_length;
    msg_buffer[msg_length] = '\0';
    printf("message = '%s'\n", msg_buffer);
    fflush(stdout);
    //--------------------------------
    return 0;
}

int chat_server_run(int port)
{
    bool was_fail = false;
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
    int EPoll = epoll_create1(0);
    /* Add the first monitored object - the socket in connection-accepting mode */
    epoll_ctl(EPoll, EPOLL_CTL_ADD, MasterSocket, &Event);

    //--------------------------------

    /* Two private buffers of each connected client */
    struct messager_client Users[USR_LIMIT] = {0};
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
    while (true) {
        /*  Multiplexing - when many descriptors wait for different events.
         *  The events need not be uniform, and handling them needs different code.
         *  If all the cases need to be told apart..
         */
        int N = epoll_wait(EPoll, Events, MAX_EVENTS, -1);

        for (int i = 0; i < N; ++i) {
            if ((Events[i].events & EPOLLERR) || (Events[i].events & EPOLLHUP)) {
                //--------------------------------
                /* error: disconnect */
                messager_connection_close(&Users[i], &Events[i]);
            } else if (Events[i].data.fd == MasterSocket) {
                //--------------------------------
                /* Accept the connection */
                int SlaveSocket;
                while ((SlaveSocket = accept(MasterSocket, 0, 0)) != -1) {
                    //--------------------------------
                    /* Listen for reading and writing */
                    set_nonblock(SlaveSocket);
                    messager_connection_accept(&Users[i], EPoll, SlaveSocket);
                }
            } else if ((Events[i].events & EPOLLOUT)) {
                //--------------------------------
                if (messager_client_send_other(Users, Events, N, NULL, 0)) {
                    fprintf(stderr, "%s\n", strerror(errno));
                    was_fail = true;
                    goto FREE_ALL;
                }
            } // end else if EPOLLOUT
            else {
                //--------------------------------
                if (messager_client_receiving(&Users[i], &Events[i], N, Users, Events)) {
                    fprintf(stderr, "%s\n", strerror(errno));
                    was_fail = true;
                    goto FREE_ALL;
                }
            } // end else

        } // end for Event [i]
    } // end while true

FREE_ALL:;
    //-------------------------------------
    for (unsigned int i = 0U; i < USR_LIMIT; ++i)
        messager_client_free(&Users[i]);

    shutdown(MasterSocket, SHUT_RDWR);
    close(MasterSocket);

    //--------------------------------
    return was_fail ? 1 : 0;
}
