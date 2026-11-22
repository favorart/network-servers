#include "header.h"
#include "srv.h"

#ifndef _CACHE_HASH_H_
#define _CACHE_HASH_H_
//-----------------------------------------
typedef uint16_t hash_t;
// typedef   int32_t ht_key_t;
// typedef   int32_t ht_val_t;

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
typedef struct hash_table hashtable;
/** @brief Hash table with open addressing; the mutex is used only with _LOCK. */
struct hash_table
{
    char* shmemory;
    size_t shm_size;
    size_t lines_count;
    ht_line* lines;
    //------------------------
    pthread_mutex_t mutex;
    //------------------------
    int (*key_comp)(ht_key_t* a, ht_key_t* b);
};
//-----------------------------------------
/** @brief Convert a relative TTL in seconds to an absolute time. */
time_t ttl_converted(int32_t ttl);
//-----------------------------------------
/**
 * @brief Allocate and zero the table and set the key comparator.
 * @param lines_count Number of slots.
 * @param size        Memory size in bytes, raised to fit all slots.
 * @param key_comp    Returns 0 when two keys are equal.
 */
void hashtable_init(hashtable* ht, size_t lines_count, size_t size,
                    int (*key_comp)(ht_key_t* a, ht_key_t* b));
/** @brief Free the table memory. */
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
