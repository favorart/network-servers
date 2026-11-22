#ifndef _CACHE_HASH_H_
#define _CACHE_HASH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cache_sysv.h"
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
    bool busy;
    ht_rec data;
};
//-----------------------------------------
/** @brief Hash table with linear probing in shared memory, guarded by a semaphore. */
typedef struct hash_table hashtable;
struct hash_table
{
    char* shmemory;
    size_t shm_size;
    //------------------------
    size_t lines_count;
    ht_line* lines;
    //------------------------
    mysem_t tb_semid;
    myshm_t tb_shmid;
};
//-----------------------------------------
/** @brief Convert a relative TTL in seconds to an absolute time. */
time_t ttl_converted(int32_t ttl);
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
 * Expired records are dropped on the way, so their slots are reused.
 * @return false on success, true if the table is full.
 */
bool hashtable_set(hashtable* ht, ht_rec* data);
//-----------------------------------------
#endif // _CACHE_HASH_H_
