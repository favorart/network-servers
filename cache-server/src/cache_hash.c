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
        ht->shm_size = lines_count * sizeof(ht_line);
    } else {
        ht->shm_size = shm_size;
    }
    //-----------------------------------------
    mysem_val_t val = 1;
    /* Create one semaphore, initialized */
    ht->tb_semid = mysem_create(1U, &val);

    /* Create the shared memory: the forked workers share the table through it */
    ht->tb_shmid = myshm_create(ht->shm_size);
    ht->shmemory = myshm_append(ht->tb_shmid);
    ht->lines = (ht_line*)ht->shmemory;

    /* Initialize the shared memory */
    memset(ht->shmemory, 0, ht->shm_size);
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
/* Home slot of a key */
static size_t hashtable_home(const hashtable* ht, ht_key_t key)
{
    return hash(key) % ht->lines_count;
}
/* Free slot i (linear probing, backward-shift deletion): the later records of the
 * same cluster move back so that each stays reachable from its home slot. */
static void hashtable_erase(hashtable* ht, size_t i)
{
    const size_t n = ht->lines_count;
    size_t j = i;
    while (true) {
        ht->lines[i].busy = false;
        while (true) {
            j = (j + 1U) % n;
            if (!ht->lines[j].busy)
                return;
            size_t k = hashtable_home(ht, ht->lines[j].data.key);
            /* the record at j stays if its home k is cyclically in (i, j] */
            bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!stays)
                break;
        }
        ht->lines[i] = ht->lines[j];
        i = j;
    }
}
/* Slot holding the key, or the free slot for it, or lines_count if the table is full.
 * Expired records met on the way are removed. */
static size_t hashtable_find(hashtable* ht, ht_key_t key)
{
    const size_t n = ht->lines_count;
    const size_t home = hashtable_home(ht, key);
    size_t i = home;
    for (size_t probes = 0U; probes < n;) {
        ht_line* line = &ht->lines[i];
        if (!line->busy)
            return i;
        if (ttl_completed(line->data.ttl)) {
            /* the records of a wrapped cluster may have moved: probe again from the start */
            hashtable_erase(ht, i);
            i = home;
            probes = 0U;
            continue;
        }
        if (line->data.key == key)
            return i;
        i = (i + 1U) % n;
        ++probes;
    }
    return n;
}
//-----------------------------------------
bool hashtable_get(hashtable* ht, ht_rec* data)
{
    mysem_lock(ht->tb_semid, 0);

    size_t h = hashtable_find(ht, data->key);
    bool missing = (h == ht->lines_count) || !ht->lines[h].busy;
    if (!missing)
        *data = ht->lines[h].data;
        //-----------------------------------------
#ifdef _DEBUG
    printf("get key=%d h=%zu found=%d\n", data->key, h, !missing);
#endif // _DEBUG

    mysem_unlock(ht->tb_semid, 0);
    //-----------------------------------------
    return missing;
}
bool hashtable_set(hashtable* ht, ht_rec* data)
{
    mysem_lock(ht->tb_semid, 0);

    size_t h = hashtable_find(ht, data->key);
    bool full = (h == ht->lines_count);
    if (!full) {
        ht->lines[h].data = *data;
        ht->lines[h].busy = true;
    }
    //-----------------------------------------
    mysem_unlock(ht->tb_semid, 0);
    //-----------------------------------------
    return full;
}
//------------------------------------------
