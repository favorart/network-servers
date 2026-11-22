/** @file test_unit.cpp
 *  @brief Unit tests: queues, command parsing and replies, configuration parsing. */
#include <getopt.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "subs_server.h"

namespace {

/** @brief Read and remove everything from @p buf as a string. */
std::string take(struct evbuffer* buf)
{
    std::string s(evbuffer_get_length(buf), '\0');
    evbuffer_remove(buf, &s[0], s.size());
    return s;
}

/** @brief A server_config with @p n queues and an event base, plus helpers to add clients. */
class Server : public ::testing::Test
{
protected:
    server_config conf;
    std::vector<client*> owned;

    void SetUp() override
    {
        conf.base = event_base_new();
        ASSERT_NE(conf.base, nullptr);
        conf.queues_manager = new QueuesManager(3);
        conf.n_queues = 3;
    }
    void TearDown() override
    {
        subs_clients_free(&conf);
        event_base_free(conf.base);
        delete conf.queues_manager;
    }
    /** @brief Add a client whose bufferevent has no socket: its output only accumulates. */
    client* add_client()
    {
        client* c = (client*)calloc(1, sizeof(client));
        c->base = conf.base;
        c->config = &conf;
        c->queues_subscribed = new std::set<size_t>();
        c->b_ev = bufferevent_socket_new(conf.base, -1, 0);
        /* a socket bufferevent freezes the outer ends of its buffers; the test is the peer */
        evbuffer_unfreeze(bufferevent_get_input(c->b_ev), 0);
        evbuffer_unfreeze(bufferevent_get_output(c->b_ev), 1);
        conf.clients.push_back(c);
        return c;
    }
    /** @brief Feed @p input to the command handler of @p c, broadcast, return its output. */
    std::string send(client* c, const std::string& input)
    {
        evbuffer_add(bufferevent_get_input(c->b_ev), input.data(), input.size());
        subscribe_handling(c, bufferevent_get_input(c->b_ev), bufferevent_get_output(c->b_ev));
        conf.queues_manager->broadcast(&conf);
        return take(bufferevent_get_output(c->b_ev));
    }
};

} // namespace

//-----------------------------------------
TEST(Queues, StartsWithoutNews)
{
    QueuesManager q(2); // regression: news_ was uninitialized
    EXPECT_EQ(q.n(), 2U);
    EXPECT_FALSE(q.news());
    q.publish(1, "m");
    EXPECT_TRUE(q.news());
}

TEST(Queues, ResizeAllowsPublishToNewQueues)
{
    QueuesManager q(1);
    q.n(5); // regression: reserve() left queues 1..4 nonexistent
    EXPECT_EQ(q.n(), 5U);
    q.publish(4, "m");
    EXPECT_TRUE(q.news());
    q.n(0);
    EXPECT_EQ(q.n(), 1U);
}

//-----------------------------------------
TEST_F(Server, SubscribeReplies)
{
    client* c = add_client();
    EXPECT_EQ(send(c, "subscribe 1\n"), "ok <que 1>\n");
    EXPECT_EQ(c->queues_subscribed->count(1), 1U);
    EXPECT_EQ(send(c, "subscribe 3\n"), "error : no such query to subscribe\n");
    EXPECT_EQ(send(c, "subscribe -1\n"), "error : no such query to subscribe\n");
    EXPECT_EQ(send(c, "subscribe x\n"), "error : incorrect command to server\n");
    EXPECT_EQ(c->queues_subscribed->size(), 1U);
}

TEST_F(Server, PublishAndCommandErrors)
{
    client* c = add_client();
    EXPECT_EQ(send(c, "publish 2 hi\n"), "ok <que 2>\n");
    EXPECT_EQ(send(c, "publish 7 hi\n"), "error : cannot insert the key\n");
    EXPECT_EQ(send(c, "publish\n"), "error : incorrect command to server\n");
    EXPECT_EQ(send(c, "hello 1\n"), "error : incorrect command to server\n");
    EXPECT_EQ(send(c, "   \n"), "error : incorrect command to server\n");
}

TEST_F(Server, ErrorDoesNotStickToNextLines)
{
    client* c = add_client(); // regression: no_answer was not reset between lines
    EXPECT_EQ(send(c, "bogus\nsubscribe 0\npublish 9 x\npublish 1 y\n"),
              "error : incorrect command to server\nok <que 0>\n"
              "error : cannot insert the key\nok <que 1>\n");
}

TEST_F(Server, IncompleteLineWaitsForTheRest)
{
    client* c = add_client(); // regression: a partial line was drained and lost
    EXPECT_EQ(send(c, "subscribe 0\nsubscr"), "ok <que 0>\n");
    EXPECT_EQ(send(c, "ibe 2\n"), "ok <que 2>\n");
    EXPECT_EQ(c->queues_subscribed->size(), 2U);
}

TEST_F(Server, OverlongLineIsDropped)
{
    client* c = add_client();
    EXPECT_EQ(send(c, std::string(SRV_MAX_LINE + 1, 'a')), "");
    EXPECT_EQ(evbuffer_get_length(bufferevent_get_input(c->b_ev)), 0U);
    EXPECT_EQ(send(c, "subscribe 0\n"), "ok <que 0>\n");
}

TEST_F(Server, MessagesGoOnlyToSubscribers)
{
    client* sub0 = add_client();
    client* sub1 = add_client();
    client* pub = add_client();
    ASSERT_EQ(send(sub0, "subscribe 0\n"), "ok <que 0>\n");
    ASSERT_EQ(send(sub1, "subscribe 1\n"), "ok <que 1>\n");

    EXPECT_EQ(send(pub, "publish 0 hello\n"), "ok <que 0>\n");
    EXPECT_EQ(take(bufferevent_get_output(sub0->b_ev)), "<que 0>   hello\n");
    EXPECT_EQ(take(bufferevent_get_output(sub1->b_ev)), "");
    EXPECT_FALSE(conf.queues_manager->news());

    EXPECT_EQ(send(pub, "publish 1 a\npublish 1 b\n"), "ok <que 1>\nok <que 1>\n");
    EXPECT_EQ(take(bufferevent_get_output(sub1->b_ev)), "<que 1>   a\n<que 1>   b\n");
    EXPECT_EQ(take(bufferevent_get_output(sub0->b_ev)), "");
}

TEST_F(Server, EmptyMessage)
{
    client* sub = add_client(); // regression: message[size - 1] on an empty message
    ASSERT_EQ(send(sub, "subscribe 0\n"), "ok <que 0>\n");
    EXPECT_EQ(send(sub, "publish 0\n"), "ok <que 0>\n<que 0>  \n");
}

//-----------------------------------------
namespace {
char* s(const char* str)
{
    return const_cast<char*>(str);
}
} // namespace

TEST(Config, Defaults)
{
    server_config conf;
    ASSERT_EQ(server_config_init(&conf, nullptr, nullptr, nullptr), 0);
    EXPECT_EQ(conf.port, 3131);
    EXPECT_STREQ(conf.ip, "0.0.0.0");
    EXPECT_EQ(conf.n_queues, 2);
    EXPECT_EQ(conf.queues_manager->n(), 2U);
    server_config_free(&conf);
    EXPECT_EQ(conf.queues_manager, nullptr);
}

TEST(Config, ValidValues)
{
    server_config conf;
    ASSERT_EQ(server_config_init(&conf, s("127.0.0.1"), s("65535"), s("255")), 0);
    EXPECT_EQ(conf.port, 65535);
    EXPECT_STREQ(conf.ip, "127.0.0.1");
    EXPECT_EQ(conf.queues_manager->n(), 255U);
    server_config_free(&conf);
}

TEST(Config, OutOfRangePortFallsBack)
{
    server_config conf; // regression: 70000 was truncated to 4464 before the range check
    ASSERT_EQ(server_config_init(&conf, nullptr, s("70000"), nullptr), 0);
    EXPECT_EQ(conf.port, 3131);
    server_config_free(&conf);
    for (const char* bad : {"-1", "12ab", ""}) {
        server_config c;
        ASSERT_EQ(server_config_init(&c, nullptr, s(bad), nullptr), 0);
        EXPECT_EQ(c.port, 3131) << bad;
        server_config_free(&c);
    }
}

TEST(Config, OutOfRangeQueuesFallBack)
{
    for (const char* bad : {"0", "256", "300", "x"}) {
        server_config conf; // regression: 300 became 44 queues, 0 gave no queues at all
        ASSERT_EQ(server_config_init(&conf, nullptr, nullptr, s(bad)), 0);
        EXPECT_EQ(conf.n_queues, 2) << bad;
        EXPECT_EQ(conf.queues_manager->n(), 2U) << bad;
        server_config_free(&conf);
    }
}

TEST(Config, TooLongIpFallsBack)
{
    server_config conf;
    ASSERT_EQ(server_config_init(&conf, s("255.255.255.255.1"), nullptr, nullptr), 0);
    EXPECT_STREQ(conf.ip, "0.0.0.0");
    server_config_free(&conf);
}

TEST(Config, CommandLine)
{
    char* argv[] = {s("subscribe-server"), s("-p"), s("1234"), s("--queues"), s("4"), s("--ip"),
                    s("127.0.0.1"),        s("-w"), s("8"),    nullptr};
    optind = 0; // full getopt reset
    server_config conf;
    ASSERT_EQ(parse_console_parameters(9, argv, &conf), 0);
    EXPECT_EQ(conf.port, 1234);
    EXPECT_EQ(conf.n_queues, 4);
    EXPECT_STREQ(conf.ip, "127.0.0.1");
    server_config_free(&conf);
}
