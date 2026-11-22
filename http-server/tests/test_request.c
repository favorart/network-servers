/**
 * @file test_request.c
 * @brief Unit tests for the HTTP request parser and the response writer.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <event2/buffer.h>

#include "http_request.h"
#include "http_server.h"

/** @brief Temporary tree: root/www is served, root/secret.html is outside. */
static char root[64];
static char www[128];

/** @brief Create a file with the given contents. */
static void write_file(const char* dir, const char* name, const char* data)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    assert_non_null(f);
    fputs(data, f);
    fclose(f);
}
static void remove_file(const char* dir, const char* name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    remove(path);
}
static int group_setup(void** state)
{
    (void)state;
    strcpy(root, "/tmp/http-request-test-XXXXXX");
    if (!mkdtemp(root))
        return -1;
    snprintf(www, sizeof(www), "%s/www", root);
    if (mkdir(www, 0700) != 0)
        return -1;
    write_file(www, "index.html", "<html>hi</html>");
    write_file(www, "photo.jpg", "JPEGDATA");
    write_file(www, "big.JPEG", "BIG");
    write_file(www, "notes.txt", "text");
    write_file(www, "noext", "none");
    write_file(www, "HEAD.html", "head file");
    write_file(root, "secret.html", "SECRET");
    char path[256];
    snprintf(path, sizeof(path), "%s/dir.html", www);
    if (mkdir(path, 0700) != 0)
        return -1;

    strcpy(server_conf.folder_path, www); /* no trailing slash on purpose */
    server_conf.ptr_server_name = "test-server";
    return 0;
}
static int group_teardown(void** state)
{
    (void)state;
    const char* files[] = {"index.html", "photo.jpg", "big.JPEG",
                           "notes.txt",  "noext",     "HEAD.html"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i)
        remove_file(www, files[i]);
    char path[256];
    snprintf(path, sizeof(path), "%s/dir.html", www);
    rmdir(path);
    rmdir(www);
    remove_file(root, "secret.html");
    rmdir(root);
    return 0;
}
//-----------------------------------------
/** @brief A request and an input buffer for one test. */
struct fixture
{
    struct http_request req;
    struct evbuffer* in;
};
static int setup(void** state)
{
    struct fixture* f = calloc(1, sizeof(*f));
    if (!f || http_request_init(&f->req) || !(f->in = evbuffer_new()))
        return -1;
    *state = f;
    return 0;
}
static int teardown(void** state)
{
    struct fixture* f = *state;
    http_request_free(&f->req);
    evbuffer_free(f->in);
    free(f);
    return 0;
}
/** @brief Append @p text to the input buffer and parse. */
static http_parse_res feed(struct fixture* f, const char* text)
{
    evbuffer_add(f->in, text, strlen(text));
    return http_request_parse(&f->req, f->in);
}
/** @brief Make the response and return it as a NUL-terminated string (free it). */
static char* respond(struct fixture* f, bool* open_failed)
{
    struct evbuffer* out = evbuffer_new();
    assert_non_null(out);
    bool failed = http_response_make(&f->req, out);
    if (open_failed)
        *open_failed = failed;
    size_t length = evbuffer_get_length(out);
    char* text = malloc(length + 1);
    assert_non_null(text);
    assert_int_equal(evbuffer_remove(out, text, length), (int)length);
    text[length] = '\0';
    evbuffer_free(out);
    return text;
}
static void assert_status(const char* response, const char* status)
{
    assert_true(strncmp(response, status, strlen(status)) == 0);
    assert_true(strncmp(response + strlen(status), "\r\n", 2) == 0);
}
//-----------------------------------------
/** Regression: "\r\n\r\n" was searched with the length of a pointer, so nothing matched. */
static void test_get_html(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"),
                     HTTP_PARSE_DONE);
    assert_int_equal(f->req.type, HTTP_REQ_GET);
    assert_int_equal(f->req.content_type, HTTP_EXT_HTML);
    assert_true(f->req.file_exist);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s/index.html", www);
    assert_string_equal(f->req.uri, expected);
    assert_int_equal(evbuffer_get_length(f->in), 0);

    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_200);
    assert_non_null(strstr(response, "\r\nContent-Type: text/html; charset=utf-8\r\n"));
    assert_non_null(strstr(response, "\r\nContent-Length: 15\r\n"));
    assert_non_null(strstr(response, "\r\nServer: test-server\r\n"));
    assert_non_null(strstr(response, "\r\nConnection: close\r\n"));
    assert_non_null(strstr(response, "\r\nDate: "));
    assert_null(strstr(response, "\r\nTime: "));
    const char* body = strstr(response, "\r\n\r\n");
    assert_non_null(body);
    assert_string_equal(body + 4, "<html>hi</html>");
    free(response);
    /* the request is cleared after the response */
    assert_int_equal(f->req.type, HTTP_REQ_NONE);
}
/** Regression: only HTTP/1.1 was accepted, while the replies are HTTP/1.0. */
static void test_http10_accepted(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "GET /index.html HTTP/1.0\r\n\r\n"), HTTP_PARSE_DONE);
    assert_true(f->req.file_exist);
}
static void test_head_has_no_body(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "HEAD /index.html HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    assert_int_equal(f->req.type, HTTP_REQ_HEAD);
    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_200);
    assert_non_null(strstr(response, "\r\nContent-Length: 15\r\n"));
    const char* body = strstr(response, "\r\n\r\n");
    assert_non_null(body);
    assert_string_equal(body + 4, "");
    free(response);
}
static void test_post_leaves_body(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "POST /form HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc"),
                     HTTP_PARSE_DONE);
    assert_int_equal(f->req.type, HTTP_REQ_POST);
    assert_int_equal(evbuffer_get_length(f->in), 3);
    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_200);
    assert_null(strstr(response, "Content-Length"));
    free(response);
}
/** Regression: the method was searched anywhere in the head ("HEAD" in the URI won). */
static void test_method_from_request_line(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "GET /HEAD.html HTTP/1.1\r\nX-Note: POST\r\n\r\n"), HTTP_PARSE_DONE);
    assert_int_equal(f->req.type, HTTP_REQ_GET);
    assert_true(f->req.file_exist);
}
static void test_bad_request_lines(void** state)
{
    struct fixture* f = *state;
    const char* bad[] = {
        "PUT /index.html HTTP/1.1\r\n\r\n",  "GET /index.html HTTP/2.0\r\n\r\n",
        "GET /index.html\r\n\r\n",           "GET\r\n\r\n",
        "get /index.html HTTP/1.1\r\n\r\n",  "GET  HTTP/1.1\r\n\r\n",
        "GET /index.html HTTP/1.1 \r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        assert_int_equal(feed(f, bad[i]), HTTP_PARSE_FAIL);
        /* the whole head is consumed */
        assert_int_equal(evbuffer_get_length(f->in), 0);
    }
}
/** Regression: a head without "\r\n\r\n" yet was dropped from the buffer. */
static void test_head_split_across_reads(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "GET /index.ht"), HTTP_PARSE_MORE);
    assert_int_equal(evbuffer_get_length(f->in), strlen("GET /index.ht"));
    assert_int_equal(feed(f, "ml HTTP/1.1\r\nHost: a\r\n"), HTTP_PARSE_MORE);
    assert_int_equal(feed(f, "\r"), HTTP_PARSE_MORE);
    assert_int_equal(feed(f, "\n"), HTTP_PARSE_DONE);
    assert_int_equal(f->req.type, HTTP_REQ_GET);
    assert_true(f->req.file_exist);
    assert_int_equal(evbuffer_get_length(f->in), 0);
}
static void test_empty_and_leading_lines(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(http_request_parse(&f->req, f->in), HTTP_PARSE_MORE);
    assert_int_equal(feed(f, "\r\n\r\n"), HTTP_PARSE_MORE);
    assert_int_equal(feed(f, "GET /index.html HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    assert_true(f->req.file_exist);
}
static void test_too_long_head(void** state)
{
    struct fixture* f = *state;
    char* chunk = malloc(HTTP_HEAD_MAXLEN + 2);
    assert_non_null(chunk);
    memset(chunk, 'a', HTTP_HEAD_MAXLEN + 1);
    chunk[HTTP_HEAD_MAXLEN + 1] = '\0';
    assert_int_equal(feed(f, "GET /"), HTTP_PARSE_MORE);
    assert_int_equal(feed(f, chunk), HTTP_PARSE_FAIL);
    free(chunk);
}
/** Regression: ".." was not filtered, files outside the directory were served. */
static void test_path_traversal(void** state)
{
    struct fixture* f = *state;
    const char* requests[] = {
        "GET /../secret.html HTTP/1.1\r\n\r\n",
        "GET /dir.html/../../secret.html HTTP/1.1\r\n\r\n",
        "GET ../secret.html HTTP/1.1\r\n\r\n",
        "HEAD /./../secret.html HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        assert_int_equal(feed(f, requests[i]), HTTP_PARSE_DONE);
        assert_false(f->req.file_exist);
        char* response = respond(f, NULL);
        assert_status(response, HTTP_RESPONSE_404);
        assert_null(strstr(response, "SECRET"));
        free(response);
    }
}
/** Regression: JPEG was sent as "image/jpg"; the extension is taken from the end. */
static void test_content_types(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "GET /photo.jpg HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    assert_int_equal(f->req.content_type, HTTP_EXT_JPEG);
    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_200);
    assert_non_null(strstr(response, "\r\nContent-Type: image/jpeg\r\n"));
    free(response);

    assert_int_equal(feed(f, "GET /big.JPEG HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    assert_int_equal(f->req.content_type, HTTP_EXT_JPEG);
    assert_true(f->req.file_exist);
    http_request_clear(&f->req);
}
/** Regression: an unknown or missing extension failed the request with no response. */
static void test_not_found(void** state)
{
    struct fixture* f = *state;
    const char* requests[] = {
        "GET /missing.html HTTP/1.1\r\n\r\n",   "GET /notes.txt HTTP/1.1\r\n\r\n",
        "GET /noext HTTP/1.1\r\n\r\n",          "GET /dir.html HTTP/1.1\r\n\r\n",
        "GET /index.html.txt HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        assert_int_equal(feed(f, requests[i]), HTTP_PARSE_DONE);
        assert_false(f->req.file_exist);
        char* response = respond(f, NULL);
        assert_status(response, HTTP_RESPONSE_404);
        free(response);
    }
}
/** Regression: the URI length was not checked against FILENAME_MAX. */
static void test_long_uri(void** state)
{
    struct fixture* f = *state;
    char request[FILENAME_MAX + 64];
    size_t n = (size_t)snprintf(request, sizeof(request), "GET /");
    memset(request + n, 'a', FILENAME_MAX);
    n += FILENAME_MAX;
    snprintf(request + n, sizeof(request) - n, ".html HTTP/1.1\r\n\r\n");
    assert_int_equal(feed(f, request), HTTP_PARSE_DONE);
    assert_false(f->req.file_exist);
    assert_true(strlen(f->req.uri) < FILENAME_MAX);
    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_404);
    free(response);
}
/** Regression: a parse error freed the request, the next clear drained a NULL buffer. */
static void test_request_usable_after_error(void** state)
{
    struct fixture* f = *state;
    assert_int_equal(feed(f, "BREW /pot HTTP/1.1\r\n\r\n"), HTTP_PARSE_FAIL);
    assert_non_null(f->req.content);
    http_request_clear(&f->req);
    assert_int_equal(feed(f, "GET /index.html HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    char* response = respond(f, NULL);
    assert_status(response, HTTP_RESPONSE_200);
    free(response);
}
/** A file that disappears between the parse and the response gives a 404. */
static void test_file_vanished(void** state)
{
    struct fixture* f = *state;
    write_file(www, "gone.html", "x");
    assert_int_equal(feed(f, "GET /gone.html HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    assert_true(f->req.file_exist);
    remove_file(www, "gone.html");
    bool open_failed = false;
    char* response = respond(f, &open_failed);
    assert_true(open_failed);
    assert_status(response, HTTP_RESPONSE_404);
    free(response);
}
static void test_folder_with_slash(void** state)
{
    struct fixture* f = *state;
    char saved[FILENAME_MAX];
    strcpy(saved, server_conf.folder_path);
    snprintf(server_conf.folder_path, sizeof(server_conf.folder_path), "%s/", www);
    assert_int_equal(feed(f, "GET /index.html HTTP/1.1\r\n\r\n"), HTTP_PARSE_DONE);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s/index.html", www);
    assert_string_equal(f->req.uri, expected);
    strcpy(server_conf.folder_path, saved);
}
//-----------------------------------------
int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_get_html, setup, teardown),
        cmocka_unit_test_setup_teardown(test_http10_accepted, setup, teardown),
        cmocka_unit_test_setup_teardown(test_head_has_no_body, setup, teardown),
        cmocka_unit_test_setup_teardown(test_post_leaves_body, setup, teardown),
        cmocka_unit_test_setup_teardown(test_method_from_request_line, setup, teardown),
        cmocka_unit_test_setup_teardown(test_bad_request_lines, setup, teardown),
        cmocka_unit_test_setup_teardown(test_head_split_across_reads, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_and_leading_lines, setup, teardown),
        cmocka_unit_test_setup_teardown(test_too_long_head, setup, teardown),
        cmocka_unit_test_setup_teardown(test_path_traversal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_content_types, setup, teardown),
        cmocka_unit_test_setup_teardown(test_not_found, setup, teardown),
        cmocka_unit_test_setup_teardown(test_long_uri, setup, teardown),
        cmocka_unit_test_setup_teardown(test_request_usable_after_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_file_vanished, setup, teardown),
        cmocka_unit_test_setup_teardown(test_folder_with_slash, setup, teardown),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
