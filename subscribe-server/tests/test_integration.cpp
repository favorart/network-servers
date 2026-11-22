/** @file test_integration.cpp
 *  @brief Starts the built server on a free port and talks to it over TCP.
 *
 *  The server binary path comes from the SUBS_SERVER_BIN environment variable. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

const char* server_bin()
{
    const char* bin = getenv("SUBS_SERVER_BIN");
    return bin ? bin : "build/subscribe-server";
}

/** @brief Ask the kernel for a free TCP port on 127.0.0.1. */
int free_port()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
        return -1;
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    close(fd);
    return ntohs(addr.sin_port);
}

/** @brief fork+exec the server with @p args; its stdout goes to @p out_fd (or /dev/null), stderr
 *  stays shared so that sanitizer reports are visible. */
pid_t spawn(const std::vector<std::string>& args, int out_fd = -1)
{
    pid_t pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(out_fd >= 0 ? out_fd : null_fd, STDOUT_FILENO);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(server_bin()));
        for (const std::string& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

/** @brief Wait up to @p ms for @p pid to exit; return its wait status or -1 on timeout. */
int wait_exit(pid_t pid, int ms)
{
    for (int waited = 0; waited < ms; waited += 10) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

/** @brief Connect to 127.0.0.1:@p port; -1 on failure. */
int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** @brief A line-oriented TCP client of the server. */
class Client
{
    int fd_;
    std::string buf_;

public:
    explicit Client(int port) : fd_(connect_to(port)) {}
    ~Client() { close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool ok() const { return fd_ >= 0; }
    void close()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    void send(const std::string& s)
    {
        ASSERT_EQ(::send(fd_, s.data(), s.size(), 0), (ssize_t)s.size());
    }
    /** @brief Next line without "\n", or "<timeout>" if none arrives within @p ms. */
    std::string line(int ms = 3000)
    {
        for (;;) {
            size_t eol = buf_.find('\n');
            if (eol != std::string::npos) {
                std::string l = buf_.substr(0, eol);
                buf_.erase(0, eol + 1);
                return l;
            }
            pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, ms) <= 0)
                return "<timeout>";
            char chunk[512];
            ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return "<closed>";
            buf_.append(chunk, n);
        }
    }
    /** @brief Send one command and return the reply line. */
    std::string ask(const std::string& cmd)
    {
        send(cmd + "\n");
        return line();
    }
};

/** @brief One server process shared by the tests of the suite. */
class ServerTest : public ::testing::Test
{
protected:
    static pid_t pid;
    static int port;

    static void SetUpTestSuite()
    {
        port = free_port();
        ASSERT_GT(port, 0);
        pid = spawn({"-h", "127.0.0.1", "-p", std::to_string(port), "-q", "3"});
        ASSERT_GT(pid, 0);
        for (int i = 0; i < 500; ++i) { // up to 5 s for the server to listen
            int fd = connect_to(port);
            if (fd >= 0) {
                ::close(fd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL() << "server did not start listening";
    }
    static void TearDownTestSuite()
    {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
};
pid_t ServerTest::pid = -1;
int ServerTest::port = -1;

} // namespace

//-----------------------------------------
TEST_F(ServerTest, SubscriberGetsOnlyItsQueue)
{
    Client sub0(port), sub1(port), pub(port);
    ASSERT_TRUE(sub0.ok() && sub1.ok() && pub.ok());
    EXPECT_EQ(sub0.ask("subscribe 0"), "ok <que 0>");
    EXPECT_EQ(sub1.ask("subscribe 1"), "ok <que 1>");

    EXPECT_EQ(pub.ask("publish 0 hello"), "ok <que 0>");
    EXPECT_EQ(sub0.line(), "<que 0>   hello");
    EXPECT_EQ(pub.ask("publish 1 world"), "ok <que 1>");
    EXPECT_EQ(sub1.line(), "<que 1>   world");

    EXPECT_EQ(sub1.line(200), "<timeout>"); // nothing from queue 0
    EXPECT_EQ(sub0.line(200), "<timeout>"); // nothing from queue 1
}

TEST_F(ServerTest, ErrorReplies)
{
    Client c(port);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.ask("subscribe 3"), "error : no such query to subscribe");
    EXPECT_EQ(c.ask("publish 99 x"), "error : cannot insert the key");
    EXPECT_EQ(c.ask("hello"), "error : incorrect command to server");
    EXPECT_EQ(c.ask("subscribe zero"), "error : incorrect command to server");
    // regression: after an error every reply used to be an error
    EXPECT_EQ(c.ask("subscribe 2"), "ok <que 2>");
}

TEST_F(ServerTest, CommandSplitAcrossPackets)
{
    Client c(port);
    ASSERT_TRUE(c.ok());
    c.send("subscr");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.send("ibe 1\n");
    EXPECT_EQ(c.line(), "ok <que 1>");
}

TEST_F(ServerTest, DisconnectedSubscriberIsForgotten)
{
    Client pub(port);
    {
        Client gone(port);
        ASSERT_EQ(gone.ask("subscribe 0"), "ok <que 0>");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(pub.ask("publish 0 nobody"), "ok <que 0>");
    EXPECT_EQ(pub.ask("subscribe 0"), "ok <que 0>");
}

//-----------------------------------------
TEST(Server, StopsCleanlyOnSigint)
{
    int port = free_port();
    pid_t pid = spawn({"-h", "127.0.0.1", "-p", std::to_string(port)});
    ASSERT_GT(pid, 0);
    int fd = -1;
    for (int i = 0; i < 500 && fd < 0; ++i) {
        fd = connect_to(port);
        if (fd < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(fd, 0);
    // a client still connected at shutdown: it must be freed before the event base
    ASSERT_EQ(write(fd, "subscribe 0\n", 12), 12);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    kill(pid, SIGINT);
    int status = wait_exit(pid, 10000);
    close(fd);
    ASSERT_NE(status, -1) << "server did not exit";
    ASSERT_TRUE(WIFEXITED(status)) << "killed by signal " << WTERMSIG(status);
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(Server, BusyPortFailsWithoutCrash)
{
    // regression: event_free(NULL) on the bind error path crashed
    int busy = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(busy, (sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(busy, 1), 0);
    socklen_t len = sizeof(addr);
    getsockname(busy, (sockaddr*)&addr, &len);

    pid_t pid = spawn({"-h", "127.0.0.1", "-p", std::to_string(ntohs(addr.sin_port))});
    int status = wait_exit(pid, 10000);
    close(busy);
    ASSERT_NE(status, -1) << "server did not exit";
    ASSERT_TRUE(WIFEXITED(status)) << "killed by signal " << WTERMSIG(status);
    EXPECT_EQ(WEXITSTATUS(status), 1);
}

TEST(Server, PrintsVersion)
{
    int out[2];
    ASSERT_EQ(pipe(out), 0);
    pid_t pid = spawn({"--version"}, out[1]);
    close(out[1]);
    std::string text;
    char chunk[256];
    ssize_t n;
    while ((n = read(out[0], chunk, sizeof(chunk))) > 0)
        text.append(chunk, n);
    close(out[0]);
    int status = wait_exit(pid, 5000);
    ASSERT_TRUE(status != -1 && WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(text.rfind("subscribe-server ", 0), 0U) << text;
}
