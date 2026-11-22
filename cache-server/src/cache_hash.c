#include <stdio.h>
#include <string.h>

#include "cache_error.h"
#include "cache_hash.h"

//-----------------------------------------
#define TIME_EPS 0.01
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
void hashtable_init(hashtable* ht, size_t lines_count, size_t shm_size)
{
    //------------------------------------------
    ht->lines_count = lines_count;
    if (sizeof(ht_line) * lines_count > shm_size) {
        perror("ht size");
        ht->shm_size = lines_count * sizeof(ht_line);
    } else {
        ht->shm_size = shm_size;
    }
    //-----------------------------------------
#ifdef _DEBUG
    static char mem[CACHELINES * sizeof(ht_line)] = {0};
    ht->shmemory = mem;
    ht->lines = (ht_line*)ht->shmemory;
#endif // _DEBUG
    mysem_val_t val = 1;
    /* Create one semaphore, initialized */
    ht->tb_semid = mysem_create(1U, &val);

    /* Create the shared memory */
    ht->tb_shmid = myshm_create(ht->shm_size);
    ht->shmemory = myshm_append(ht->tb_shmid);

    /* Initialize the shared memory */
    memset(ht->shmemory, 0, shm_size);
    //-----------------------------------------
    return;
}
void hashtable_free(hashtable* ht)
{
    myshm_remove(ht->tb_shmid);
    mysem_remove(ht->tb_semid);
}
//-----------------------------------------
// std::hash<string>
static hash_t hash(int32_t key)
{
    char* k = (char*)&key;
    uint32_t h = 2139062143;

    for (size_t i = 0; i < sizeof(key); ++i)
        h = h * 37 + k[i];

    return (hash_t)h;
}
static hash_t hashtable_walk(hashtable* ht, ht_rec* data, hash_t he, bool isget)
{
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
        } else if (ht->lines[h].data.key == data->key) {
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
    mysem_lock(ht->tb_semid, 0);

    hash_t he = hash(data->key) % ht->lines_count;
    //-----------------------------------------
    hash_t h = hashtable_walk(ht, data, he, true);
    if ((h < ht->lines_count) && ht->lines[h].busy && (ht->lines[h].data.key == data->key)) {
        *data = ht->lines[h].data;
    } else {
        h = ht->lines_count;
    }
    //-----------------------------------------
#ifdef _DEBUG
    printf("get he=%d h=%d\n", he, h);
#endif // _DEBUG

    mysem_unlock(ht->tb_semid, 0);
    //-----------------------------------------
    return (h == ht->lines_count);
}
bool hashtable_set(hashtable* ht, ht_rec* data)
{
    mysem_lock(ht->tb_semid, 0);

    hash_t he = hash(data->key) % ht->lines_count;
    //-----------------------------------------
    hash_t h = hashtable_walk(ht, data, he, false);
    if ((h < ht->lines_count) && (!ht->lines[h].busy || (ht->lines[h].data.key == data->key))) {
        ht->lines[h].data = *data;
        ht->lines[h].busy = true;
    } else
        h = ht->lines_count;
    //-----------------------------------------
    mysem_unlock(ht->tb_semid, 0);
    //-----------------------------------------
    return (h == ht->lines_count);
}
//------------------------------------------
