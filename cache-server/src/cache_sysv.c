#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "cache_sysv.h"

#define MY_PERM (S_IRUSR | S_IWUSR) // (|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
//-----------------------------------------
#define MYSEM_NMBR_MAX 257U

mysem_t mysem_create(mysem_inx_t num, mysem_val_t* vals)
{
    if (num > MYSEM_NMBR_MAX) {
        perror("max sems");
        exit(EXIT_FAILURE);
    }
    //-----------------------------------------
    mysem_t sid = semget(IPC_PRIVATE, num, IPC_CREAT | IPC_EXCL | MY_PERM);
    if (sid == -1) {
        perror("mysem exists");
        exit(EXIT_FAILURE);
    }
    //-----------------------------------------
    /* Initialize all the elements */
    int rc = semctl(sid, num, SETALL, vals);
    if (rc == -1) {
        perror("setall");
        exit(EXIT_FAILURE);
    }
    //-----------------------------------------
    return sid;
}
void mysem_lock(mysem_t sid, mysem_inx_t inx)
{
    struct sembuf lock = {.sem_num = inx, .sem_op = -1, .sem_flg = 0 /* IPC_NOWAIT */};
    /* Try to lock the resource */
    if (0 > semop(sid, &lock, 1)) {
        perror("semop lock");
        exit(EXIT_FAILURE);
    }
}
void mysem_unlock(mysem_t sid, mysem_inx_t inx)
{
    struct sembuf release = {.sem_num = inx, .sem_op = 1, .sem_flg = 0 /* IPC_NOWAIT */};
    /* Try to release (unlock) the semaphore set */
    if (0 > semop(sid, &release, 1)) {
        perror("semop release");
        exit(EXIT_FAILURE);
    }
}
void mysem_remove(mysem_t sid)
{
    if (0 > semctl(sid, 0, IPC_RMID, 0)) {
        perror("shm_remid");
        exit(EXIT_FAILURE);
    }
}
//-----------------------------------------
myshm_t myshm_create(size_t size)
{
    myshm_t mid = shmget(IPC_PRIVATE, size, IPC_CREAT | MY_PERM);
    if (mid == -1) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }
    return mid;
}
void myshm_remove(myshm_t mid)
{
    shmctl(mid, IPC_RMID, 0);
}
char* myshm_append(myshm_t mid)
{
    void* shm = shmat(mid, NULL, SHM_RND);
    if (!shm) {
        perror("shmat");
        exit(EXIT_FAILURE);
    }
    return (char*)shm;
}
//-----------------------------------------
