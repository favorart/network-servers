/**
 * @file test_chat.c
 * @brief Unit tests: line splitting, buffer clearing and broadcast over socket pairs.
 */
#include "../src/chat.h"

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

/** @brief Messages collected by collect_line(). */
struct collected
{
    char msgs[8][MSG_LIMIT];
    size_t lens[8];
    int count;
    int fail_at; /**< return an error on this message number, -1 never */
};

/** @brief messager_line_cb that stores every message. */
static int collect_line(void* ctx, char* msg_buffer, size_t msg_length)
{
    struct collected* c = ctx;
    assert_true(c->count < 8);
    memcpy(c->msgs[c->count], msg_buffer, msg_length);
    c->msgs[c->count][msg_length] = '\0';
    c->lens[c->count] = msg_length;
    return (c->count++ == c->fail_at) ? 7 : 0;
}

/** @brief Allocate a client before each test. */
static int setup_client(void** state)
{
    static struct messager_client user;
    if (messager_client_initialize(&user))
        return -1;
    *state = &user;
    return 0;
}

/** @brief Free the client after each test. */
static int teardown_client(void** state)
{
    messager_client_free(*state);
    return 0;
}

/** @brief Split the string as one received chunk. */
static int split(struct messager_client* user, const char* data, struct collected* c)
{
    return messager_split_lines(user, data, strlen(data), collect_line, c);
}

static void test_split_single_line(void** state)
{
    struct collected c = {.fail_at = -1};
    assert_int_equal(split(*state, "hello\n", &c), 0);
    assert_int_equal(c.count, 1);
    assert_string_equal(c.msgs[0], "hello\n");
    assert_int_equal(((struct messager_client*)*state)->in_length, 0);
}

static void test_split_partial_line_waits(void** state)
{
    struct collected c = {.fail_at = -1};
    split(*state, "TEST-", &c);
    assert_int_equal(c.count, 0);
    split(*state, "TEST\n", &c);
    assert_int_equal(c.count, 1);
    assert_string_equal(c.msgs[0], "TEST-TEST\n");
}

/** Regression: the rest of a chunk was kept as the partial line and repeated. */
static void test_split_many_lines_in_one_chunk(void** state)
{
    struct collected c = {.fail_at = -1};
    split(*state, "tes", &c);
    split(*state, "t1\ntest2\ntest3\ntail", &c);
    assert_int_equal(c.count, 3);
    assert_string_equal(c.msgs[0], "test1\n");
    assert_string_equal(c.msgs[1], "test2\n");
    assert_string_equal(c.msgs[2], "test3\n");
    struct messager_client* user = *state;
    assert_int_equal(user->in_length, 4);
    assert_memory_equal(user->in_buffer, "tail", 4);
}

static void test_split_long_line_in_parts(void** state)
{
    struct collected c = {.fail_at = -1};
    char data[MSG_LIMIT + 100];
    memset(data, 'x', sizeof(data) - 2);
    data[sizeof(data) - 2] = '\n';
    data[sizeof(data) - 1] = '\0';
    assert_int_equal(split(*state, data, &c), 0);
    assert_int_equal(c.count, 2);
    assert_int_equal(c.lens[0], MSG_LIMIT - 1U);
    assert_int_equal(c.lens[1], sizeof(data) - 1 - (MSG_LIMIT - 1U));
    assert_int_equal(c.msgs[1][c.lens[1] - 1], '\n');
}

static void test_split_long_line_after_partial(void** state)
{
    struct collected c = {.fail_at = -1};
    char data[MSG_LIMIT];
    memset(data, 'y', sizeof(data) - 1);
    data[sizeof(data) - 1] = '\0';
    split(*state, "abc", &c);
    split(*state, data, &c);
    assert_int_equal(c.count, 1);
    assert_int_equal(c.lens[0], MSG_LIMIT - 1U);
    assert_memory_equal(c.msgs[0], "abcyyy", 6);
    assert_int_equal(((struct messager_client*)*state)->in_length, 3);
}

static void test_split_callback_error_stops(void** state)
{
    struct collected c = {.fail_at = 0};
    assert_int_equal(split(*state, "a\nb\n", &c), 7);
    assert_int_equal(c.count, 1);
}

/** Regression: CLR_IN | CLR_OUT cleared nothing. */
static void test_clear_both(void** state)
{
    struct messager_client* user = *state;
    struct collected c = {.fail_at = -1};
    split(user, "partial", &c);
    memcpy(user->out_buffer, "out", 3);
    user->out_length = 3;
    messager_client_clear(user, CLR_IN | CLR_OUT);
    assert_int_equal(user->in_length, 0);
    assert_int_equal(user->out_length, 0);
}

/** Regression: broadcast went to the slots of the current event batch, not to clients. */
static void test_send_other_reaches_every_client(void** state)
{
    (void)state;
    struct messager_client users[3];
    int pairs[2][2];
    for (int i = 0; i < 3; ++i)
        assert_int_equal(messager_client_initialize(&users[i]), 0);
    for (int i = 0; i < 2; ++i) {
        assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]), 0);
        set_nonblock(pairs[i][0]);
    }
    users[0].fd = pairs[0][0];
    users[2].fd = pairs[1][0]; /* users[1] is a free slot */

    char msg[MSG_LIMIT] = "hi all\n";
    assert_int_equal(messager_client_send_other(users, 3, msg, strlen(msg)), 0);

    for (int i = 0; i < 2; ++i) {
        char buf[32] = {0};
        assert_int_equal(read(pairs[i][1], buf, sizeof(buf)), 7);
        assert_string_equal(buf, "hi all\n");
    }
    assert_int_equal(users[1].out_length, 0);
    assert_int_equal(users[0].out_length, 0);

    assert_ptr_equal(messager_client_find(users, pairs[1][0]), &users[2]);
    for (int i = 0; i < 2; ++i) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }
    for (int i = 0; i < 3; ++i)
        messager_client_free(&users[i]);
}

/** A closed peer closes the client once and frees its slot. */
static void test_flush_closes_dead_peer(void** state)
{
    struct messager_client* user = *state;
    int pair[2];
    assert_int_equal(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    close(pair[1]);
    user->fd = pair[0];
    memcpy(user->out_buffer, "x\n", 2);
    user->out_length = 2;
    messager_client_flush(user);
    assert_int_equal(user->fd, -1);
    assert_int_equal(user->out_length, 0);
    assert_int_equal(messager_connection_close(user), 0); /* no second close */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_split_single_line, setup_client, teardown_client),
        cmocka_unit_test_setup_teardown(test_split_partial_line_waits, setup_client,
                                        teardown_client),
        cmocka_unit_test_setup_teardown(test_split_many_lines_in_one_chunk, setup_client,
                                        teardown_client),
        cmocka_unit_test_setup_teardown(test_split_long_line_in_parts, setup_client,
                                        teardown_client),
        cmocka_unit_test_setup_teardown(test_split_long_line_after_partial, setup_client,
                                        teardown_client),
        cmocka_unit_test_setup_teardown(test_split_callback_error_stops, setup_client,
                                        teardown_client),
        cmocka_unit_test_setup_teardown(test_clear_both, setup_client, teardown_client),
        cmocka_unit_test(test_send_other_reaches_every_client),
        cmocka_unit_test_setup_teardown(test_flush_closes_dead_peer, setup_client, teardown_client),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
