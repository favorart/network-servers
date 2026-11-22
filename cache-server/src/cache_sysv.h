#ifndef _CACHE_SYSV_H_
#define _CACHE_SYSV_H_

#include <stddef.h>
//-----------------------------------------
typedef int myshm_t;
typedef int mysem_t;
typedef unsigned short mysem_val_t;
typedef unsigned short mysem_inx_t;

/**
 * @brief Create a private set of @p num semaphores with initial values @p vals.
 * @return Semaphore set id (exits on error).
 */
mysem_t mysem_create(mysem_inx_t num, mysem_val_t* vals);
/** @brief Decrement (lock) semaphore @p member, blocking (SEM_UNDO); exits on error. */
void mysem_lock(mysem_t sid, mysem_inx_t member);
/** @brief Increment (unlock) semaphore @p member; exits on error. */
void mysem_unlock(mysem_t sid, mysem_inx_t member);
/** @brief Remove the semaphore set; exits on error. */
void mysem_remove(mysem_t sid);

/**
 * @brief Create a private System V shared memory segment of @p size bytes.
 * @return Segment id (exits on error).
 */
myshm_t myshm_create(size_t size);
/** @brief Mark the shared memory segment for removal. */
void myshm_remove(myshm_t mid);
/**
 * @brief Attach the shared memory segment to this process.
 * @return Address of the segment.
 */
char* myshm_append(myshm_t mid);
//-----------------------------------------
#endif // _CACHE_SYSV_H_
