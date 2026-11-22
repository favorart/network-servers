#include "stdafx.h"

#ifndef _MYSEM_MYSHM_H_
#define _MYSEM_MYSHM_H_
#define _LOCK
//-----------------------------------------
typedef int myshm_t;
typedef int mysem_t;
typedef unsigned short mysem_val_t;
typedef unsigned short mysem_inx_t;

/**
 * @brief Open an existing System V semaphore set.
 * @return Semaphore set id, -1 on error.
 */
inline mysem_t mysem_open(key_t key);
/**
 * @brief Create a private set of @p num semaphores with initial values @p vals.
 * @return Semaphore set id (exits on error).
 */
inline mysem_t mysem_create(mysem_inx_t num, mysem_val_t* vals);
/** @brief Decrement (lock) semaphore @p member, blocking; exits on error. */
inline void mysem_lock(mysem_t sid, mysem_inx_t member);
/** @brief Increment (unlock) semaphore @p member; exits on error. */
inline void mysem_unlock(mysem_t sid, mysem_inx_t member);
/** @brief Remove the semaphore set; exits on error. */
inline void mysem_remove(mysem_t sid);

/**
 * @brief Create a private System V shared memory segment of @p size bytes.
 * @return Segment id (exits on error).
 */
inline myshm_t myshm_create(size_t size);
/** @brief Mark the shared memory segment for removal. */
inline void myshm_remove(myshm_t mid);
/**
 * @brief Attach the shared memory segment to this process.
 * @return Address of the segment.
 */
inline char* myshm_append(myshm_t mid);
//-----------------------------------------
#endif // _MYSEM_MYSHM_H_
//=========================================
#ifndef _CACHE_HASH_H_
#define _CACHE_HASH_H_
//-----------------------------------------
#define CACHE_SIZE 1024U * 1024U /* bytes */
#define CACHELINES 256U
//-----------------------------------------
typedef int32_t ht_key_t;
typedef int32_t ht_val_t;
typedef uint16_t hash_t;

typedef struct hashtable_record ht_rec;
/** @brief Cache record: key, value and absolute expiry time. */
struct hashtable_record
{
    ht_key_t key;
    ht_val_t val;
    time_t ttl;
};
//-----------------------------------------
typedef struct hashtable_line ht_line;
/** @brief Hash table slot: a record and its busy flag. */
struct hashtable_line
{
    bool /*rip,*/ busy;
    ht_rec data;
};
//-----------------------------------------
/** @brief Hash table with open addressing in shared memory, guarded by a semaphore. */
typedef struct hash_table hashtable;
struct hash_table
{
    char* shmemory;
    size_t shm_size;
    //------------------------
    size_t lines_count;
    ht_line* lines;
    //------------------------
#ifdef _LOCK
    mysem_t ln_semid;
    mysem_t tb_semid;
    myshm_t tb_shmid;
#endif // _LOCK
       //------------------------
};
//-----------------------------------------
/** @brief Convert a relative TTL in seconds to an absolute time. */
clock_t ttl_converted(int32_t ttl);
//-----------------------------------------
/**
 * @brief Create the table in shared memory and its semaphore.
 * @param lines_count Number of slots.
 * @param shm_size    Memory size in bytes, raised to fit all slots.
 */
void hashtable_init(hashtable* ht, size_t lines_count, size_t shm_size);
/** @brief Remove the shared memory segment and the semaphore. */
void hashtable_free(hashtable* ht);

/**
 * @brief Find data->key and copy the stored record into @p data.
 * @return false if found, true if the key is missing or expired.
 */
bool hashtable_get(hashtable* ht, ht_rec* data);
/**
 * @brief Insert or update the record for data->key.
 * @return false on success, true if there is no room for it.
 */
bool hashtable_set(hashtable* ht, ht_rec* data);
//-----------------------------------------
#ifdef _DEBUG_HASH
/** @brief Print all slots to @p f (only with _DEBUG_HASH). */
void hashtable_print_debug(hashtable* ht, FILE* f);
#endif // _DEBUG_HASH
//-----------------------------------------
#endif // _CACHE_HASH_H_
