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
        my_errno = SRV_ERR_PARAM;
        fprintf(stderr, "ht size%s", strmyerror());
        ht->shm_size = lines_count * sizeof(ht_line);
    } else {
        ht->shm_size = size;
    }
    //-----------------------------------------
    ht->shmemory = malloc(ht->shm_size);
    if (!ht->shmemory) {
        perror("ht shmemory");
        exit(EXIT_FAILURE);
    }
    /* Initialize the shared memory */
    memset(ht->shmemory, 0, ht->shm_size);

    ht->lines = (ht_line*)ht->shmemory;
    //-----------------------------------------
    pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
    memcpy(&ht->mutex, &blank_mutex, sizeof(ht->mutex));
    //-----------------------------------------
    ht->key_comp = key_comp;

    return;
}
void hashtable_free(hashtable* ht)
{
    free(ht->shmemory);
}
//-----------------------------------------
static hash_t hash(ht_key_t key)
{
    char* k = (char*)&key;
    uint32_t h = 2139062143U;

    for (size_t i = 0; i < sizeof(key); ++i)
        h = h * 37 + k[i];

    return (hash_t)h;
}
static hash_t hashtable_walk(hashtable* ht, ht_rec* data, bool isget)
{
    hash_t he = hash(data->key) % ht->lines_count;
    hash_t h = he, h_ret = h;
    //-----------------------------------------
    /* walk along the open addressing */
    while ((h < ht->lines_count) && ht->lines[h].busy) {
        hash_t hn = hash(ht->lines[h + 1U].data.key) % ht->lines_count;
        /* if we meet an expired ttl */
        if (ttl_completed(ht->lines[h].data.ttl)) {
            /* if there is nothing ahead in the open addressing */
            if (((h + 1U) >= ht->lines_count) || !ht->lines[h + 1U].busy ||
                ((h + 1U) == hn)
                /* or the hash of the next element is greater than the hash of the current one */
                || ((he < hn) && !isget)) {
                ht->lines[h].busy = false;
                h_ret = h;
                break;
            } /* found the required index - finish */

            hash_t hc = ++h;
            /* while in the open addressing: the index of the next element is greater than its hash
             */
            while ((h < ht->lines_count) && ht->lines[h].busy && (h > hn)) {
                hn = hash(ht->lines[++h].data.key) % ht->lines_count;
            }

            /* shift the next one into this place */
            memmove(&ht->lines[hc - 1U], &ht->lines[hc], (h - hc) * sizeof(ht_line));

            ht->lines[h - 1U].busy = false;
            h = hc;
        } else if (!ht->key_comp(&ht->lines[h].data.key, &data->key)) {
            h_ret = h;
            break;
        } else if (he < h) {
            /* move the records apart */
            hash_t hc = h;
            while ((h < ht->lines_count) && ht->lines[h].busy) {
                if (ttl_completed(ht->lines[h].data.ttl)) {
                    ht->lines[h].busy = false;
                    break;
                }
                ++h;
            }

            if (h >= ht->lines_count) {
                break;
            }

            /* moving the records apart */
            memmove(&ht->lines[hc + 1U], &ht->lines[hc], (h - hc) * sizeof(ht_line));
            ht->lines[hc].busy = false;

            h_ret = hc;
            break;
        }

        ++h;
        h_ret = h;
    } // end while
    //-----------------------------------------
    return h_ret;
}
//-----------------------------------------
bool hashtable_get(hashtable* ht, ht_rec* data)
{
#ifdef _LOCK
    pthread_mutex_lock(&ht->mutex);
#endif // _LOCK
    //-----------------------------------------
    hash_t h = hashtable_walk(ht, data, true);
    if ((h < ht->lines_count) && ht->lines[h].busy &&
        !ht->key_comp(&ht->lines[h].data.key, &data->key)) {
        *data = ht->lines[h].data;
    } else {
        h = ht->lines_count;
    }
    //-----------------------------------------
#ifdef _LOCK
    pthread_mutex_unlock(&ht->mutex);
#endif // _LOCK
    //-----------------------------------------
    return (h == ht->lines_count);
}
bool hashtable_set(hashtable* ht, ht_rec* data)
{
#ifdef _LOCK
    pthread_mutex_lock(&ht->mutex);
#endif // _LOCK
    //-----------------------------------------
    hash_t h = hashtable_walk(ht, data, false);
    if ((h < ht->lines_count) &&
        (!ht->lines[h].busy || !ht->key_comp(&ht->lines[h].data.key, &data->key))) {
        ht->lines[h].data = *data;
        ht->lines[h].busy = true;
    } else
        h = ht->lines_count;
        //-----------------------------------------
#ifdef _LOCK
    pthread_mutex_unlock(&ht->mutex);
#endif // _LOCK
    //-----------------------------------------
    return (h == ht->lines_count);
}
//-----------------------------------------
