#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define MAX_EVENTS 32U
#define USR_LIMIT 31U
#define MSG_LIMIT 1025U

#define CLR_IN 1
#define CLR_OUT 2

// #define  DEBUG
#undef DEBUG

/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl()/ioctl(), -1 on error.
 */
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

/** @brief Per-client state: partial input line and pending output. */
struct messager_client
{
    //--------------------------------
    char in_buffer[MSG_LIMIT];
    char* out_buffer;
    //--------------------------------
    size_t in_length;
    size_t out_length;
    size_t out_maxlen;
    //--------------------------------
};

int messager_connection_accept(struct messager_client* User, int EPoll, int SlaveSocket);
int messager_connection_close(struct messager_client* User, struct epoll_event* Event);

int messager_client_initialize(struct messager_client* User);
int messager_client_free(struct messager_client* User);
int messager_client_clear(struct messager_client* User, int what);
int messager_client_receiving(struct messager_client* User, struct epoll_event* Event, int N,
                              struct messager_client* Users, struct epoll_event* Events);
int messager_client_send_other(struct messager_client* Users, struct epoll_event* Events, int N,
                               char* msg_buffer, size_t msg_length);

void logging(int line, struct messager_client* Users, size_t recv_length, char* recv_buffer);
/** @brief Debug dump of a client's input buffer and the last received data. */
void logging(int line, struct messager_client* Users, size_t recv_length, char* recv_buffer)
{
    //--------------------------------
    (void)printf("\n>> !!! %u !!!\n>> buf_in_i_len: '%u'\n>> buf_in_i: '%s'\n>> recv_length: "
                 "'%u'\n>> recv: '%s'\n",
                 line, Users->in_length, Users->in_buffer, recv_length, recv_buffer);
}

/**
 * @brief Register a new client socket in epoll and send it "Welcome".
 * @return 1 if growing an output buffer failed, else 0.
 */
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

    // fprintf (stderr, "accepted connection\n");
    //--------------------------------
    char welcome[] = "Welcome\n";
    return messager_client_send_other(User, &Event, 1, welcome, strlen(welcome));
}
/** @brief Shut down and close the client's socket and clear its buffers. */
int messager_connection_close(struct messager_client* User, struct epoll_event* Event)
{
    //--------------------------------
    // disconnect Users
    shutdown(Event->data.fd, SHUT_RDWR);
    close(Event->data.fd);
    //--------------------------------
    printf("connection terminated\n");
    fflush(stdout);

    // fprintf (stderr, "connection terminated\n");
    //--------------------------------
    messager_client_clear(User, CLR_IN | CLR_OUT);
    //--------------------------------
    return 0;
}

/**
 * @brief Zero the client and allocate its output buffer.
 * @return Non-zero if the allocation failed.
 */
int messager_client_initialize(struct messager_client* User)
{
    memset(User, 0, sizeof(*User));
    //--------------------------------
    User->out_maxlen = MSG_LIMIT;
    User->out_buffer = calloc(User->out_maxlen, sizeof(*User->out_buffer));
    return (User->out_buffer == NULL);
    //--------------------------------
}
/** @brief Free the client's output buffer and zero the structure. */
int messager_client_free(struct messager_client* User)
{
    //--------------------------------
    free(User->out_buffer);
    memset(User, 0, sizeof(*User));
}
/**
 * @brief Clear the client's buffers.
 * @param what CLR_IN, CLR_OUT or both.
 */
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
}

/**
 * @brief Read all available data from a client, split it into lines
 *        and send each complete line to every client.
 * @return 1 on a fatal (allocation) error, else 0.
 */
int messager_client_receiving(struct messager_client* User, struct epoll_event* Event,
                              /* char *msg_buffer, size_t *msg_length,*/ int N,
                              struct messager_client* Users, struct epoll_event* Events)
{
    char recv_buffer[MSG_LIMIT] = {0};
    int recv_length = 0U;
    //--------------------------------
    /* reading from the socket (recv() is read() without signal) */
    while (0 < (recv_length = recv(Event->data.fd, recv_buffer, (MSG_LIMIT - 1U), MSG_NOSIGNAL))) {
        char msg_buffer[MSG_LIMIT] = {0};
        size_t msg_length = 0U;
        //--------------------------------
        /* parse the messages */
        char* recv_ptr2nline;
        while ((recv_ptr2nline = memchr(recv_buffer, '\n', recv_length))) {
            //--------------------------------
            size_t cur_length = (++recv_ptr2nline - recv_buffer);
            if ((cur_length + User->in_length) >= MSG_LIMIT) {
                /* too long */
                cur_length -= (cur_length + User->in_length) - (MSG_LIMIT - 1U);
            }
            //--------------------------------
            memcpy(msg_buffer, User->in_buffer, User->in_length);
            memcpy(msg_buffer + User->in_length, recv_buffer, cur_length);
            msg_length = (User->in_length + cur_length);
            //--------------------------------
            User->in_length = (recv_length - cur_length);
            memcpy(User->in_buffer, recv_buffer + cur_length, User->in_length);
            //--------------------------------
            if (messager_client_send_other(Users, Events, N, msg_buffer, msg_length))
                return 1;
            //--------------------------------
            if ((recv_length - cur_length)) {
                memmove(recv_buffer, recv_buffer + cur_length, (recv_length - cur_length));
                recv_length -= cur_length;
            }
            recv_length -= cur_length;
            //--------------------------------
        } // end if '\n'

        if (recv_length) {
            //--------------------------------
            recv_ptr2nline = recv_buffer;
            if ((recv_length + User->in_length) >= MSG_LIMIT) {
                size_t cur_length = (recv_length + User->in_length) - (MSG_LIMIT - 1U);
                //--------------------------------
                memcpy(msg_buffer, User->in_buffer, User->in_length);
                memcpy(msg_buffer + User->in_length, recv_buffer, cur_length);
                msg_length = (MSG_LIMIT - 1U);
                //--------------------------------
                recv_ptr2nline += cur_length;
                recv_length -= cur_length;
                //--------------------------------
                if (messager_client_send_other(Users, Events, N, msg_buffer, msg_length))
                    return 1;
                //--------------------------------
            }
            //--------------------------------
            memcpy(User->in_buffer, recv_ptr2nline, recv_length);
            User->in_length = recv_length;
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
        // return 0;
    }
    //--------------------------------
    return 0;
}
/**
 * @brief Append a message to every client's output buffer, flush the pending
 *        output and log the message.
 * @return 1 if growing an output buffer failed, else 0.
 */
int messager_client_send_other(struct messager_client* Users, struct epoll_event* Events, int N,
                               char* msg_buffer, size_t msg_length)
{
    if (msg_length) {
        //--------------------------------
        /* Sending to everybody */
        for (unsigned int j = 0U; j < N; ++j) {
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
    for (unsigned int j = 0U; j < N; ++j) {
        if (Users[j].out_length) {
            //--------------------------------
            /* non-block sending */
            int ret =
                send(Events[j].data.fd, Users[j].out_buffer, Users[j].out_length, MSG_NOSIGNAL);
            if (ret <= 0) {
                //--------------------------------
                /* soft and hard disconnect */
                if (errno != EAGAIN)
                    messager_connection_close(&Users[j], &Events[j]);
            } else if (ret > 0) {
                //--------------------------------
                if (ret < Users[j].out_length) {
                    memmove(Users[j].out_buffer, Users[j].out_buffer + ret,
                            Users[j].out_length - ret);
                } else {
                    messager_client_clear(&Users[j], CLR_OUT);
                }
            }
            //--------------------------------
        } // end if out_length
    } // end for
    //--------------------------------

    //--------------------------------
    if (msg_buffer[msg_length - 1] == '\n')
        --msg_length;
    msg_buffer[msg_length] = '\0';
    printf("message = '%s'\n", msg_buffer);
    fflush(stdout);

    // fprintf ( stderr, "message = '%s'\n", msg_buffer);
    //--------------------------------
    return 0;
}

int main(int argc, char** argv)
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
    /* Return IPv4 and IPv6 choices */
    SockAddr.sin_family = AF_INET;
    /* TCP socket */
    // hints.ai_socktype = SOCK_STREAM;
    SockAddr.sin_port = htons(3100);
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

        for (unsigned int i = 0; i < N; ++i) {
            if ((Events[i].events & EPOLLERR) || (Events[i].events & EPOLLHUP)) {
                // fprintf (stderr, "err\n");
                //--------------------------------
                /* error: disconnect */
                messager_connection_close(&Users[i], &Events[i]);
            } else if (Events[i].data.fd == MasterSocket) {
                //--------------------------------
                /* Accept the connection */
                int SlaveSocket;
                while ((SlaveSocket = accept(MasterSocket, 0, 0)) != -1) {
                    // fprintf (stderr, "acc\n");
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
    return 0;
}
