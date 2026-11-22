#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srv_cache.h"
#include "srv.h"

//-----------------------------------------
#define TIME_EPS -0.01
/* Relative TTL to absolute TTL (in seconds) */
time_t ttl_converted(int32_t ttl)
{
    return (time(NULL) + ttl);
}
static bool ttl_completed(time_t ttl)
{
    return (difftime(time(NULL), ttl) > TIME_EPS);
}
//-----------------------------------------
void hashtable_init(hashtable* ht, size_t lines_count, size_t size,
                    int (*key_comp)(ht_key_t* a, ht_key_t* b))
{
    //------------------------------------------
    ht->lines_count = lines_count;
    if (sizeof(ht_line) * lines_count > size) {
        if (size) {
            my_errno = SRV_ERR_PARAM;
            fprintf(stderr, "ht size%s\n", strmyerror());
        }
        ht->shm_size = lines_count * sizeof(ht_line);
    } else {
        ht->shm_size = size;
    }
    //-----------------------------------------
    ht->shmemory = calloc(1U, ht->shm_size);
    if (!ht->shmemory) {
        perror("ht shmemory");
        exit(EXIT_FAILURE);
    }
    ht->lines = (ht_line*)ht->shmemory;
    //-----------------------------------------
    pthread_mutex_init(&ht->mutex, NULL);
    ht->key_comp = key_comp;
}
void hashtable_free(hashtable* ht)
{
    if (!ht)
        return;
    free(ht->shmemory);
    ht->shmemory = NULL;
    ht->lines = NULL;
    pthread_mutex_destroy(&ht->mutex);
}
//-----------------------------------------
hash_t ht_hash(const ht_key_t* key)
{
    uint32_t h = 2139062143U;

    /* Only the string: the bytes after its NUL are garbage. */
    for (const char* k = key->word; *k && k < key->word + sizeof(key->word); ++k)
        h = h * 37 + (unsigned char)*k;

    return (hash_t)h;
}
/**
 * @brief Linear probing (with wrap-around) for data->key; call with the mutex held.
 * @param[out] slot Slot of the key (live or expired), else the first expired slot met,
 *                  else the free slot that ended the probe; lines_count if the table is full.
 * @return true if the key itself is at @p slot.
 */
static bool hashtable_walk(hashtable* ht, ht_rec* data, size_t* slot)
{
    size_t n = ht->lines_count;
    size_t h = ht_hash(&data->key) % n;
    size_t expired = n;

    for (size_t i = 0; i < n; ++i, h = (h + 1U) % n) {
        ht_line* line = &ht->lines[h];
        if (!line->busy) {
            *slot = (expired < n) ? expired : h;
            return false;
        }
        if (!ht->key_comp(&line->data.key, &data->key)) {
            *slot = h;
            return true;
        }
        if (expired == n && ttl_completed(line->data.ttl))
            expired = h;
    }
    *slot = expired;
    return false;
}
//-----------------------------------------
bool hashtable_get(hashtable* ht, ht_rec* data)
{
    bool miss = true;
    size_t h;

    pthread_mutex_lock(&ht->mutex);
    if (hashtable_walk(ht, data, &h) && !ttl_completed(ht->lines[h].data.ttl)) {
        *data = ht->lines[h].data;
        miss = false;
    }
    pthread_mutex_unlock(&ht->mutex);
    return miss;
}
bool hashtable_set(hashtable* ht, ht_rec* data)
{
    bool full = true;
    size_t h;

    pthread_mutex_lock(&ht->mutex);
    hashtable_walk(ht, data, &h);
    if (h < ht->lines_count) {
        ht->lines[h].data = *data;
        ht->lines[h].busy = true;
        full = false;
    }
    pthread_mutex_unlock(&ht->mutex);
    return full;
}
//-----------------------------------------
