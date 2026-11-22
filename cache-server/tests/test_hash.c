/* Unit tests for the shared-memory hash table with TTL. */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmocka.h>

#include "cache_hash.h"

#define LONG_TTL 3600

static int table_setup(void** state, size_t lines)
{
    hashtable* ht = calloc(1, sizeof(*ht));
    if (!ht)
        return -1;
    hashtable_init(ht, lines, 0U);
    *state = ht;
    return 0;
}
static int small_setup(void** state)
{
    return table_setup(state, 8U);
}
static int big_setup(void** state)
{
    return table_setup(state, CACHELINES);
}
static int table_teardown(void** state)
{
    hashtable* ht = *state;
    hashtable_free(ht);
    free(ht);
    return 0;
}

static bool set(hashtable* ht, ht_key_t key, ht_val_t val, int32_t ttl)
{
    ht_rec rec = {.key = key, .val = val, .ttl = ttl_converted(ttl)};
    return !hashtable_set(ht, &rec);
}
/* Returns true and the value if the key is there */
static bool get(hashtable* ht, ht_key_t key, ht_val_t* val)
{
    ht_rec rec = {.key = key};
    if (hashtable_get(ht, &rec))
        return false;
    assert_int_equal(rec.key, key);
    if (val)
        *val = rec.val;
    return true;
}

static void test_set_get(void** state)
{
    hashtable* ht = *state;
    ht_val_t v = 0;
    assert_false(get(ht, 1, NULL));
    assert_true(set(ht, 1, 10, LONG_TTL));
    assert_true(set(ht, -7, 70, LONG_TTL));
    assert_true(get(ht, 1, &v));
    assert_int_equal(v, 10);
    assert_true(get(ht, -7, &v));
    assert_int_equal(v, 70);
    assert_false(get(ht, 2, NULL));
}

static void test_overwrite(void** state)
{
    hashtable* ht = *state;
    ht_val_t v = 0;
    assert_true(set(ht, 5, 1, LONG_TTL));
    assert_true(set(ht, 5, 2, LONG_TTL));
    assert_true(get(ht, 5, &v));
    assert_int_equal(v, 2);
}

static void test_expired_key_is_missing(void** state)
{
    hashtable* ht = *state;
    assert_true(set(ht, 3, 30, -1));
    assert_false(get(ht, 3, NULL));
}

/* Every slot is usable, including the wrap-around past the last one. */
static void test_full_table(void** state)
{
    hashtable* ht = *state;
    ht_val_t v = 0;
    for (ht_key_t k = 0; k < 8; ++k)
        assert_true(set(ht, k * 1000, k, LONG_TTL));
    assert_false(set(ht, 99999, 0, LONG_TTL));
    for (ht_key_t k = 0; k < 8; ++k) {
        assert_true(get(ht, k * 1000, &v));
        assert_int_equal(v, k);
    }
    /* updating a key in a full table works */
    assert_true(set(ht, 3000, 33, LONG_TTL));
    assert_true(get(ht, 3000, &v));
    assert_int_equal(v, 33);
}

/* Expired records free their slots for new keys. */
static void test_expired_slots_reused(void** state)
{
    hashtable* ht = *state;
    for (ht_key_t k = 0; k < 4; ++k)
        assert_true(set(ht, k, k, LONG_TTL));
    for (ht_key_t k = 4; k < 8; ++k)
        assert_true(set(ht, k, k, -1));
    for (ht_key_t k = 100; k < 104; ++k)
        assert_true(set(ht, k, k, LONG_TTL));
    for (ht_key_t k = 0; k < 4; ++k)
        assert_true(get(ht, k, NULL));
    for (ht_key_t k = 100; k < 104; ++k)
        assert_true(get(ht, k, NULL));
    for (ht_key_t k = 4; k < 8; ++k)
        assert_false(get(ht, k, NULL));
}

/* Regression: a lookup of a missing key moved records apart and hid stored keys. */
static void test_missing_get_keeps_keys(void** state)
{
    hashtable* ht = *state;
    for (ht_key_t k = 0; k < 6; ++k)
        assert_true(set(ht, k, k, LONG_TTL));
    for (ht_key_t k = 100; k < 200; ++k)
        assert_false(get(ht, k, NULL));
    for (ht_key_t k = 0; k < 6; ++k)
        assert_true(get(ht, k, NULL));
}

/* Random operations against a simple model, with deletions of expired keys. */
static void test_against_model(void** state)
{
    hashtable* ht = *state;
    enum
    {
        KEYS = 24,
        SLOTS = 8
    };
    struct
    {
        bool present;
        ht_val_t val;
    } model[KEYS] = {{0}};
    int count = 0;
    uint32_t seed = 12345U;
    for (int step = 0; step < 20000; ++step) {
        seed = seed * 1103515245U + 12345U;
        ht_key_t key = (ht_key_t)((seed >> 8) % KEYS);
        unsigned op = (seed >> 20) % 4U;
        ht_val_t v = 0;
        if (op == 0) {
            /* expire the key (if it is there, its slot becomes free) */
            bool ok = set(ht, key, step, -1);
            assert_true(ok || (!model[key].present && count == SLOTS));
            if (model[key].present)
                --count;
            model[key].present = false;
        } else if (op == 1) {
            bool ok = set(ht, key, step, LONG_TTL);
            assert_int_equal(ok, model[key].present || count < SLOTS);
            if (ok) {
                if (!model[key].present)
                    ++count;
                model[key].present = true;
                model[key].val = step;
            }
        } else {
            bool found = get(ht, key, &v);
            assert_int_equal(found, model[key].present);
            if (found)
                assert_int_equal(v, model[key].val);
        }
    }
}

/* Regression: the table lived in static memory, so each worker had its own copy. */
static void test_shared_between_processes(void** state)
{
    hashtable* ht = *state;
    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        ht_rec rec = {.key = 42, .val = 4242, .ttl = ttl_converted(LONG_TTL)};
        _exit(hashtable_set(ht, &rec) ? 1 : 0);
    }
    int status = 0;
    assert_int_equal(waitpid(pid, &status, 0), pid);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    ht_val_t v = 0;
    assert_true(get(ht, 42, &v));
    assert_int_equal(v, 4242);
}

static void test_ttl_converted(void** state)
{
    (void)state;
    time_t now = time(NULL);
    time_t t = ttl_converted(10);
    assert_true(t >= now + 10 && t <= now + 11);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_set_get, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_overwrite, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_expired_key_is_missing, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_full_table, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_expired_slots_reused, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_missing_get_keeps_keys, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_against_model, small_setup, table_teardown),
        cmocka_unit_test_setup_teardown(test_shared_between_processes, big_setup, table_teardown),
        cmocka_unit_test(test_ttl_converted),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
