#ifndef _WORKQUEUE_H_
#define _WORKQUEUE_H_

#include <pthread.h>
#include <stdbool.h>
//-----------------------------------------
typedef struct worker worker_t;
/** @brief Worker thread of the work queue (item of a doubly linked list). */
struct worker
{
    //-----------------------
    pthread_t thread;
    bool term;
    struct workqueue* wque;
    //-----------------------
    struct worker* prev;
    struct worker* next;
};
//-----------------------------------------
typedef struct job job_t;
/** @brief Job for a worker thread: a function and its data (item of a doubly linked list). */
struct job
{
    //--------------------
    void (*job_func)(struct job* job);
    void* user_data;
    //--------------------
    struct job* prev;
    struct job* next;
};

//-----------------------------------------
typedef struct workqueue wque_t;
/** @brief Work queue: worker threads and waiting jobs, guarded by a mutex and a condition variable.
 */
struct workqueue
{
    struct worker* workers;
    struct job* waiting_jobs;
    //--------------------
    pthread_mutex_t jobs_mutex;
    pthread_cond_t jobs_cond;
};

/**
 * @brief Init the queue and start @p n_workers worker threads (at least 1).
 * @return 0, or -1 if a worker could not be allocated or started (the queue is then freed).
 */
int wque_init(wque_t* wq, int n_workers);
/** @brief Tell all workers to stop, wait for them (each finishes its current job) and
 *        free them; waiting jobs are dropped. */
void wque_free(wque_t* wq);
/** @brief Add a job to the queue and wake up one worker; jobs run in FIFO order. */
void wque_push(wque_t* wq, job_t* job);

//-----------------------------------------
/** @brief Push @p item to the front of the doubly linked @p list. */
#define LL_ADD(item, list)                                                                         \
    {                                                                                              \
        item->prev = NULL;                                                                         \
        item->next = list;                                                                         \
        if (list != NULL)                                                                          \
            list->prev = item;                                                                     \
        list = item;                                                                               \
    }
/** @brief Unlink @p item from the doubly linked @p list. */
#define LL_DEL(item, list)                                                                         \
    {                                                                                              \
        if (item->prev != NULL)                                                                    \
            item->prev->next = item->next;                                                         \
        if (item->next != NULL)                                                                    \
            item->next->prev = item->prev;                                                         \
        if (list == item)                                                                          \
            list = item->next;                                                                     \
        item->prev = item->next = NULL;                                                            \
    }
//-----------------------------------------
#endif //	_WORKQUEUE_H_
