/**
 * @file chat.h
 * @brief Chat server internals: per-client buffers, line splitting, broadcast
 *        and the event loop.
 */
#ifndef CHAT_H
#define CHAT_H

#include <stddef.h>

#ifndef NS_VERSION
#define NS_VERSION "0.0.0"
#endif

#define CHAT_DEFAULT_PORT 3100

#define MAX_EVENTS 32U
#define USR_LIMIT 31U
/** @brief A message is at most MSG_LIMIT - 1 bytes, the newline included. */
#define MSG_LIMIT 1025U

#define CLR_IN 1
#define CLR_OUT 2

/** @brief Per-client state: socket, partial input line and pending output. */
struct messager_client
{
    int fd; /**< client socket, -1 if the slot is free */
    char in_buffer[MSG_LIMIT];
    char* out_buffer;
    size_t in_length;
    size_t out_length;
    size_t out_maxlen;
};

/** @brief Called for every complete message; non-zero stops the splitting. */
typedef int (*messager_line_cb)(void* ctx, char* msg_buffer, size_t msg_length);

/**
 * @brief Switch a descriptor to non-blocking mode.
 * @return Result of fcntl()/ioctl(), -1 on error.
 */
int set_nonblock(int fd);

/**
 * @brief Take a free slot for a new client socket, register it in epoll
 *        and send it "Welcome". Closes the socket if all slots are busy.
 * @return 1 if growing an output buffer failed, else 0.
 */
int messager_connection_accept(struct messager_client* Users, int EPoll, int SlaveSocket);
/** @brief Shut down and close the client's socket, clear its buffers, free the slot. */
int messager_connection_close(struct messager_client* User);

/**
 * @brief Zero the client, mark the slot free and allocate its output buffer.
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
/** @brief Find the connected client with the socket among USR_LIMIT slots, or NULL. */
struct messager_client* messager_client_find(struct messager_client* Users, int fd);

/**
 * @brief Split received data into messages: append it to the client's partial
 *        line and call @p on_line for every line ending with '\n'; a line that
 *        does not fit into MSG_LIMIT - 1 bytes is passed on in parts.
 * @return 0, or the first non-zero result of @p on_line.
 */
int messager_split_lines(struct messager_client* User, const char* recv_buffer, size_t recv_length,
                         messager_line_cb on_line, void* ctx);
/**
 * @brief Read all available data from a client, split it into lines
 *        and send each complete line to every client; close the client on EOF
 *        or error.
 * @return 1 on a fatal (allocation) error, else 0.
 */
int messager_client_receiving(struct messager_client* User, struct messager_client* Users);
/**
 * @brief Send as much pending output as the socket takes; close the client on error.
 * @return 0.
 */
int messager_client_flush(struct messager_client* User);
/**
 * @brief Append a message to the output buffer of every connected client among
 *        the first N, flush their pending output and log the message.
 * @return 1 if growing an output buffer failed, else 0.
 */
int messager_client_send_other(struct messager_client* Users, int N, char* msg_buffer,
                               size_t msg_length);

/**
 * @brief Listen on the TCP port and serve clients until SIGINT/SIGTERM
 *        or a fatal error.
 * @return 0 on a normal stop, 1 on an error.
 */
int chat_server_run(int port);

#endif /* CHAT_H */
