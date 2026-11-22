#include "stdafx.h"

#ifndef _SUBS_QUEUES_H_
#define _SUBS_QUEUES_H_
//-----------------------------------------
struct client;

/** @brief Holds N message queues and delivers queued messages to subscribed clients. */
class QueuesManager
{
    size_t n_;
    bool news_;
    std::vector<std::queue<std::string>> queues_;

public:
    /** @brief Create @p n empty queues. */
    QueuesManager(size_t n) : n_(n), queues_(n) {}

    /** @brief Set the number of queues (at least 1). */
    void n(size_t n)
    {
        if (!n)
            n = 1;
        n_ = n;
        queues_.reserve(n);
    }
    /** @brief Number of queues. */
    size_t n() { return n_; }
    /** @brief Whether there are messages not yet broadcast. */
    bool news() { return news_; }

    /** @brief Push @p message into queue @p n and mark that there is news. */
    void publish(size_t n, char* message)
    {
        queues_[n].push(message);
        news_ = true;
    }

    /** @brief Send all queued messages to the clients subscribed to each queue and empty the
     * queues. */
    void broadcast(struct server_config* config);
};
//-----------------------------------------
#endif // _SUBS_QUEUES_H_
