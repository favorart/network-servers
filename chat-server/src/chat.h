/**
 * @file chat.h
 * @brief Chat server internals: per-client buffers, broadcast and the event loop.
 */
#ifndef CHAT_H
#define CHAT_H

#include <stddef.h>
#include <sys/epoll.h>

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

#define CHAT_DEFAULT_PORT 3100

#define MAX_EVENTS 32U
#define USR_LIMIT 31U
#define MSG_LIMIT 1025U

#define CLR_IN 1
#define CLR_OUT 2

/** @brief Per-client state: partial input line and pending output. */
struct messager_client
{
    char in_buffer[MSG_LIMIT];
    char* out_buffer;
    size_t in_length;
    size_t out_length;
    size_t out_maxlen;
};

/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl()/ioctl(), -1 on error.
 */
int set_nonblock(int fd);

/**
 * @brief Register a new client socket in epoll and send it "Welcome".
 * @return 1 if growing an output buffer failed, else 0.
 */
int messager_connection_accept(struct messager_client* User, int EPoll, int SlaveSocket);
/** @brief Shut down and close the client's socket and clear its buffers. */
int messager_connection_close(struct messager_client* User, struct epoll_event* Event);

/**
 * @brief Zero the client and allocate its output buffer.
 * @return Non-zero if the allocation failed.
 */
int messager_client_initialize(struct messager_client* User);
/** @brief Free the client's output buffer and zero the structure. */
int messager_client_free(struct messager_client* User);
/**
 * @brief Clear the client's buffers.
 * @param what CLR_IN, CLR_OUT or both.
 */
int messager_client_clear(struct messager_client* User, int what);
/**
 * @brief Read all available data from a client, split it into lines
 *        and send each complete line to every client.
 * @return 1 on a fatal (allocation) error, else 0.
 */
int messager_client_receiving(struct messager_client* User, struct epoll_event* Event, int N,
                              struct messager_client* Users, struct epoll_event* Events);
/**
 * @brief Append a message to every client's output buffer, flush the pending
 *        output and log the message.
 * @return 1 if growing an output buffer failed, else 0.
 */
int messager_client_send_other(struct messager_client* Users, struct epoll_event* Events, int N,
                               char* msg_buffer, size_t msg_length);

/**
 * @brief Listen on the TCP port and serve clients until a fatal error.
 * @return 0 on a normal stop, 1 on an error.
 */
int chat_server_run(int port);

#endif /* CHAT_H */
