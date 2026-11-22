/** @file test_unit.c
 *  @brief Unit tests: cache (hash table), work queue, top-10 by weight and prefix search.
 */
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <event2/buffer.h>

#include "srv.h"
#include "srv_cache.h"
#include "srv_que.h"

#ifndef TEST_DICT
#define TEST_DICT "tests/dict_small.txt"
#endif

//-----------------------------------------
/** @brief Record whose key is @p key and whose value is derived from @p key and @p version. */
static ht_rec make_rec(const char* key, int version, int32_t ttl)
{
    ht_rec rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.key.word, sizeof(rec.key.word), "%s", key);
    snprintf(rec.val.words[0], sizeof(rec.val.words[0]), "%s", key);
    rec.val.weights[0] = version;
    rec.val.count = 1;
    rec.ttl = ttl_converted(ttl);
    return rec;
}
/** @brief Look @p key up; @return the stored version, or -1 on a miss. */
static int get_version(hashtable* ht, const char* key)
{
    ht_rec rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.key.word, sizeof(rec.key.word), "%s", key);
    if (hashtable_get(ht, &rec))
        return -1;
    assert_string_equal(rec.val.words[0], key);
    return rec.val.weights[0];
}
/** @brief Find the @p nth key "kN" whose home slot in a table of @p lines is @p slot. */
static void key_with_slot(size_t lines, size_t slot, int nth, char* out)
{
    for (int i = 0;; ++i) {
        ht_key_t key;
        memset(&key, 0, sizeof(key));
        snprintf(key.word, sizeof(key.word), "k%d", i);
        if (ht_hash(&key) % lines == slot && nth-- == 0) {
            strcpy(out, key.word);
            return;
        }
    }
}
//-----------------------------------------
static void test_cache_set_get(void** state)
{
    (void)state;
    hashtable ht;
    hashtable_init(&ht, DICT_CACHELNS, 0, my_key_comp);

    ht_rec rec = make_rec("abc", 7, 100);
    assert_false(hashtable_set(&ht, &rec));
    assert_int_equal(get_version(&ht, "abc"), 7);
    assert_int_equal(get_version(&ht, "abd"), -1);

    /* Update in place: no duplicate, the new value wins. */
    rec = make_rec("abc", 8, 100);
    assert_false(hashtable_set(&ht, &rec));
    assert_int_equal(get_version(&ht, "abc"), 8);

    hashtable_free(&ht);
}
/** Regression: a record whose TTL has passed is a miss, and its slot is reused. */
static void test_cache_ttl(void** state)
{
    (void)state;
    hashtable ht;
    hashtable_init(&ht, 4, 0, my_key_comp);

    ht_rec rec = make_rec("old", 1, -5);
    assert_false(hashtable_set(&ht, &rec));
    assert_int_equal(get_version(&ht, "old"), -1);

    /* Fill the table with expired records: new keys still fit. */
    char key[16];
    for (int i = 0; i < 4; ++i) {
        snprintf(key, sizeof(key), "e%d", i);
        rec = make_rec(key, i, -5);
        hashtable_set(&ht, &rec);
    }
    for (int i = 0; i < 4; ++i) {
        snprintf(key, sizeof(key), "n%d", i);
        rec = make_rec(key, i, 100);
        assert_false(hashtable_set(&ht, &rec));
    }
    for (int i = 0; i < 4; ++i) {
        snprintf(key, sizeof(key), "n%d", i);
        assert_int_equal(get_version(&ht, key), i);
    }
    /* Full of live records: no room. */
    rec = make_rec("more", 1, 100);
    assert_true(hashtable_set(&ht, &rec));

    hashtable_free(&ht);
}
/** Regression: the hash covered the whole key buffer, including garbage after the NUL. */
static void test_cache_hash_ignores_garbage(void** state)
{
    (void)state;
    ht_rec dirty, clean;
    memset(&dirty, 0xAA, sizeof(dirty));
    memset(&clean, 0, sizeof(clean));
    strcpy(dirty.key.word, "prefix");
    strcpy(clean.key.word, "prefix");
    assert_int_equal(ht_hash(&dirty.key), ht_hash(&clean.key));

    hashtable ht;
    hashtable_init(&ht, DICT_CACHELNS, 0, my_key_comp);
    dirty.ttl = ttl_converted(100);
    dirty.val.count = 0;
    assert_false(hashtable_set(&ht, &dirty));
    assert_false(hashtable_get(&ht, &clean));
    assert_string_equal(clean.key.word, "prefix");
    hashtable_free(&ht);
}
/** Regression: probing at the last slot read lines[lines_count] (ASan: heap overflow). */
static void test_cache_last_slot(void** state)
{
    (void)state;
    const size_t lines = 8;
    char a[16], b[16], c[16];
    key_with_slot(lines, lines - 1, 0, a);
    key_with_slot(lines, lines - 1, 1, b);
    key_with_slot(lines, lines - 1, 2, c);

    hashtable ht;
    hashtable_init(&ht, lines, 0, my_key_comp);
    ht_rec rec = make_rec(a, 1, 100);
    assert_false(hashtable_set(&ht, &rec));
    rec = make_rec(b, 2, 100);
    assert_false(hashtable_set(&ht, &rec)); /* wraps to slot 0 */
    rec = make_rec(c, 3, -5);
    assert_false(hashtable_set(&ht, &rec)); /* slot 1, expired */
    assert_int_equal(get_version(&ht, a), 1);
    assert_int_equal(get_version(&ht, b), 2);
    assert_int_equal(get_version(&ht, c), -1);
    assert_int_equal(get_version(&ht, "missing"), -1);
    hashtable_free(&ht);
}
/** Regression: colliding keys got lost (a get shifted records and left holes). */
static void test_cache_model(void** state)
{
    (void)state;
    enum
    {
        LINES = 64,
        KEYS = 48,
        OPS = 20000
    };
    int model[KEYS];
    hashtable ht;
    hashtable_init(&ht, LINES, 0, my_key_comp);
    for (int i = 0; i < KEYS; ++i)
        model[i] = -1;

    srand(12345);
    char key[16];
    for (int op = 0; op < OPS; ++op) {
        int k = rand() % KEYS;
        snprintf(key, sizeof(key), "key%d", k);
        if (rand() % 2) {
            ht_rec rec = make_rec(key, op, 100);
            assert_false(hashtable_set(&ht, &rec));
            model[k] = op;
        } else {
            assert_int_equal(get_version(&ht, key), model[k]);
        }
    }
    hashtable_free(&ht);
}
//-----------------------------------------
struct cache_thread_arg
{
    hashtable* ht;
    int seed;
};
static void* cache_thread(void* ptr)
{
    struct cache_thread_arg* arg = ptr;
    unsigned seed = (unsigned)arg->seed;
    char key[16];
    for (int op = 0; op < 5000; ++op) {
        int k = rand_r(&seed) % 32;
        snprintf(key, sizeof(key), "t%d", k);
        if (rand_r(&seed) % 2) {
            /* The value is a function of the key: any torn record shows up. */
            ht_rec rec = make_rec(key, k, (rand_r(&seed) % 4) ? 100 : -5);
            hashtable_set(arg->ht, &rec);
        } else {
            ht_rec rec;
            memset(&rec, 0, sizeof(rec));
            strcpy(rec.key.word, key);
            if (!hashtable_get(arg->ht, &rec)) {
                if (strcmp(rec.val.words[0], key) || rec.val.weights[0] != k)
                    return (void*)1;
            }
        }
    }
    return NULL;
}
/** Regression: the cache had no mutex (_LOCK was never defined) but is shared by workers. */
static void test_cache_threads(void** state)
{
    (void)state;
    hashtable ht;
    hashtable_init(&ht, 64, 0, my_key_comp);
    pthread_t threads[4];
    struct cache_thread_arg args[4];
    for (int i = 0; i < 4; ++i) {
        args[i].ht = &ht;
        args[i].seed = i + 1;
        assert_int_equal(pthread_create(&threads[i], NULL, cache_thread, &args[i]), 0);
    }
    for (int i = 0; i < 4; ++i) {
        void* ret = NULL;
        pthread_join(threads[i], &ret);
        assert_null(ret);
    }
    hashtable_free(&ht);
}
static void test_cache_free_null(void** state)
{
    (void)state;
    hashtable_free(NULL);
    client_free(NULL);
    struct client* c = calloc(1, sizeof(*c));
    assert_non_null(c);
    client_free(c); /* Regression: NULL base/bufferevent crashed */
}
//-----------------------------------------
/** @brief Shared state of the queue tests. */
struct queue_state
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int order[16];
    bool gate_open;
};
static struct queue_state qs = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, {0}, false};
/** @brief Job: record its number (user_data) and count it. */
static void record_job(job_t* job)
{
    pthread_mutex_lock(&qs.mutex);
    if (qs.done < 16)
        qs.order[qs.done] = (int)(intptr_t)job->user_data;
    ++qs.done;
    pthread_cond_broadcast(&qs.cond);
    pthread_mutex_unlock(&qs.mutex);
    free(job);
}
/** @brief Job: wait until the gate opens. */
static void gate_job(job_t* job)
{
    pthread_mutex_lock(&qs.mutex);
    while (!qs.gate_open)
        pthread_cond_wait(&qs.cond, &qs.mutex);
    pthread_mutex_unlock(&qs.mutex);
    free(job);
}
static void push(wque_t* wq, void (*func)(job_t*), int n)
{
    job_t* job = calloc(1, sizeof(*job));
    assert_non_null(job);
    job->job_func = func;
    job->user_data = (void*)(intptr_t)n;
    wque_push(wq, job);
}
/** @brief Wait (at most 5 s) until @p n jobs are done. */
static void wait_done(int n)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&qs.mutex);
    while (qs.done < n && pthread_cond_timedwait(&qs.cond, &qs.mutex, &until) == 0)
        ;
    int done = qs.done;
    pthread_mutex_unlock(&qs.mutex);
    assert_int_equal(done, n);
}
static int queue_setup(void** state)
{
    (void)state;
    qs.done = 0;
    qs.gate_open = false;
    memset(qs.order, 0, sizeof(qs.order));
    return 0;
}
static void test_queue_runs_all_jobs(void** state)
{
    (void)state;
    wque_t wq;
    assert_int_equal(wque_init(&wq, 4), 0);
    for (int i = 0; i < 200; ++i)
        push(&wq, record_job, i);
    wait_done(200);
    wque_free(&wq);
    assert_null(wq.workers);
}
/** Regression: jobs were taken from the head of the list (the newest client first). */
static void test_queue_fifo(void** state)
{
    (void)state;
    wque_t wq;
    assert_int_equal(wque_init(&wq, 1), 0);
    push(&wq, gate_job, 0);
    for (int i = 1; i <= 5; ++i)
        push(&wq, record_job, i);

    pthread_mutex_lock(&qs.mutex);
    qs.gate_open = true;
    pthread_cond_broadcast(&qs.cond);
    pthread_mutex_unlock(&qs.mutex);

    wait_done(5);
    for (int i = 0; i < 5; ++i)
        assert_int_equal(qs.order[i], i + 1);
    wque_free(&wq);
}
/** Regression: a stopping worker left its loop holding jobs_mutex, so the others deadlocked;
 *  wque_free() now joins every worker (this test hangs on the old code). */
static void test_queue_free_joins(void** state)
{
    (void)state;
    for (int round = 0; round < 20; ++round) {
        wque_t wq;
        assert_int_equal(wque_init(&wq, 4), 0);
        wque_free(&wq);
        assert_null(wq.workers);
    }
}
//-----------------------------------------
/** @brief Check that @p rec holds exactly @p words in this order. */
static void assert_words(const struct dict_rec* rec, const char* const* words, int count)
{
    assert_int_equal(rec->count, count);
    for (int i = 0; i < count; ++i)
        assert_string_equal(rec->words[i], words[i]);
}
/** Regression: a full top-10 replaced the first lighter word and was never sorted. */
static void test_top10_sorted(void** state)
{
    (void)state;
    static const char* const names[] = {"x01", "x02", "x03", "x04", "x05", "x06",
                                        "x07", "x08", "x09", "x10", "x11", "x12"};
    static const int weights[] = {5, 80, 17, 99, 0, 42, 63, 17, 8, 71, 25, 90};
    struct dict_rec rec;
    memset(&rec, 0, sizeof(rec));
    for (int i = 0; i < 12; ++i)
        dict_rec_add(&rec, names[i], weights[i]);

    static const char* const top[] = {"x04", "x12", "x02", "x10", "x07",
                                      "x06", "x11", "x03", "x08", "x09"};
    assert_words(&rec, top, 10);
    for (int i = 1; i < rec.count; ++i)
        assert_true(rec.weights[i - 1] >= rec.weights[i]);
}
static void test_dict_lookup(void** state)
{
    (void)state;
    FILE* f = fopen(TEST_DICT, "r");
    assert_non_null(f);
    struct dict_rec rec;

    dict_lookup(f, "ap", 2, &rec);
    static const char* const ap[] = {"apricot", "apple"};
    assert_words(&rec, ap, 2);

    dict_lookup(f, "x0", 2, &rec);
    static const char* const x0[] = {"x04", "x02", "x07", "x06", "x03", "x08", "x09", "x01", "x05"};
    assert_words(&rec, x0, 9);

    dict_lookup(f, "zzz", 3, &rec);
    assert_int_equal(rec.count, 0);

    /* Regression: a broken line stopped the scan; a too long word overflowed the buffer. */
    dict_lookup(f, "y", 1, &rec);
    static const char* const y[] = {"yes"};
    assert_words(&rec, y, 1);

    dict_lookup(f, "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7", 8, &rec); /* "abaz" in Cyrillic */
    static const char* const abaz[] = {
        "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\xd0\xb8\xd0\xbd", "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7",
        "\xd0\xb0\xd0\xb1\xd0\xb0\xd0\xb7\xd0\xb8\xd0\xbd\xd0\xb5\xd1\x86"};
    assert_words(&rec, abaz, 3);

    /* The empty prefix matches every word. */
    dict_lookup(f, "", 0, &rec);
    assert_int_equal(rec.count, 10);
    assert_string_equal(rec.words[0], "x04");
    fclose(f);
}
/** @brief Run find_words_prefix() on @p request; @return the reply (caller frees). */
static char* answer(hashtable* ht, const char* request)
{
    struct evbuffer* in = evbuffer_new();
    struct evbuffer* out = evbuffer_new();
    evbuffer_add(in, request, strlen(request));
    find_words_prefix(in, out, ht, TEST_DICT);
    assert_int_equal(evbuffer_get_length(in), 0);

    size_t len = evbuffer_get_length(out);
    char* reply = calloc(1, len + 1);
    evbuffer_remove(out, reply, len);
    evbuffer_free(in);
    evbuffer_free(out);
    return reply;
}
/** Regression: the second prefix of a request reused the first one's words and scanned the
 *  dictionary from EOF; a prefix of 101+ bytes overflowed the cache key. */
static void test_find_words_prefix(void** state)
{
    (void)state;
    hashtable ht;
    hashtable_init(&ht, DICT_CACHELNS, 0, my_key_comp);

    char* reply = answer(&ht, "ap\nx1\r\nzzz\nap\n");
    assert_string_equal(reply, "apricot\napple\nx12\nx10\nx11\napricot\napple\n");
    free(reply);

    /* From the cache now. */
    reply = answer(&ht, "x1\n");
    assert_string_equal(reply, "x12\nx10\nx11\n");
    free(reply);

    char request[256];
    memset(request, 'a', 200);
    strcpy(request + 200, "\nap\n");
    reply = answer(&ht, request);
    assert_string_equal(reply, "apricot\napple\n");
    free(reply);

    hashtable_free(&ht);
}
//-----------------------------------------
int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cache_set_get),
        cmocka_unit_test(test_cache_ttl),
        cmocka_unit_test(test_cache_hash_ignores_garbage),
        cmocka_unit_test(test_cache_last_slot),
        cmocka_unit_test(test_cache_model),
        cmocka_unit_test(test_cache_threads),
        cmocka_unit_test(test_cache_free_null),
        cmocka_unit_test_setup(test_queue_runs_all_jobs, queue_setup),
        cmocka_unit_test_setup(test_queue_fifo, queue_setup),
        cmocka_unit_test_setup(test_queue_free_joins, queue_setup),
        cmocka_unit_test(test_top10_sorted),
        cmocka_unit_test(test_dict_lookup),
        cmocka_unit_test(test_find_words_prefix),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
