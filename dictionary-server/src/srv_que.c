#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "srv_que.h"

//-----------------------------------------
static void* worker_function(void* ptr)
{
    worker_t* worker = (worker_t*)ptr;
    wque_t* wq = worker->wque;

    pthread_mutex_lock(&wq->jobs_mutex);
    while (true) {
        /* Wait until there is a job or we are told to terminate. */
        while (!wq->waiting_jobs && !worker->term)
            pthread_cond_wait(&wq->jobs_cond, &wq->jobs_mutex);

        if (worker->term)
            break;

        /* Jobs are pushed to the front: take the oldest one from the tail. */
        job_t* job = wq->waiting_jobs;
        while (job->next)
            job = job->next;
        LL_DEL(job, wq->waiting_jobs);

        pthread_mutex_unlock(&wq->jobs_mutex);
        job->job_func(job);
        pthread_mutex_lock(&wq->jobs_mutex);
    }
    pthread_mutex_unlock(&wq->jobs_mutex);

    return NULL;
}
//-----------------------------------------
int wque_init(wque_t* wq, int n_workers)
{
    worker_t* worker;

    if (n_workers < 1) {
        n_workers = 1;
    }

    memset(wq, 0, sizeof(*wq));
    pthread_mutex_init(&wq->jobs_mutex, NULL);
    pthread_cond_init(&wq->jobs_cond, NULL);

    for (int i = 0; i < n_workers; i++) {
        if (!(worker = calloc(1U, sizeof(worker_t)))) {
            perror("failed to allocate all workers");
            wque_free(wq);
            return -1;
        }
        worker->wque = wq;

        if (pthread_create(&worker->thread, NULL, worker_function, (void*)worker)) {
            perror("failed to start all worker threads");
            free(worker);
            wque_free(wq);
            return -1;
        }

        LL_ADD(worker, wq->workers);
    }

    return 0;
}
void wque_free(wque_t* wq)
{
    /* Tell all workers to terminate and wake them up; waiting jobs are dropped. */
    pthread_mutex_lock(&wq->jobs_mutex);
    for (worker_t* worker = wq->workers; worker; worker = worker->next) {
        worker->term = true;
    }
    wq->waiting_jobs = NULL;
    pthread_cond_broadcast(&wq->jobs_cond);
    pthread_mutex_unlock(&wq->jobs_mutex);

    /* Wait for each worker to finish its current job and exit. */
    worker_t* worker = wq->workers;
    while (worker) {
        worker_t* next = worker->next;
        pthread_join(worker->thread, NULL);
        free(worker);
        worker = next;
    }
    wq->workers = NULL;

    pthread_cond_destroy(&wq->jobs_cond);
    pthread_mutex_destroy(&wq->jobs_mutex);
}
void wque_push(wque_t* wq, job_t* job)
{
    /* Add a job to the queue and notify a worker */
    pthread_mutex_lock(&wq->jobs_mutex);

    LL_ADD(job, wq->waiting_jobs);

    pthread_cond_signal(&wq->jobs_cond);
    pthread_mutex_unlock(&wq->jobs_mutex);
}
//-----------------------------------------
