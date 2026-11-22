
#ifndef _HTTP_REQ_H_
#define _HTTP_REQ_H_
#include <stdbool.h>
#include <stdio.h>

struct evbuffer;
//-----------------------------------------
/*  CMND URI HTTP/1.0\r\n
 *  Header1: something\r\n
 *  Header2: something\r\n
 *  ...
 *  HeaderN: something\r\n
 *  \r\n
 *  BODY html/jpeg
 */
/** @brief Request method (or a response marker). */
typedef enum
{
    HTTP_REQ_NONE,
    HTTP_REQ_HEAD,
    HTTP_REQ_GET,
    HTTP_REQ_POST,
    HTTP_RESPONSE /* , ... */
} http_type;
/** @brief Type of the requested file, taken from its extension. */
typedef enum
{
    HTTP_EXT_NONE,
    HTTP_EXT_HTML,
    HTTP_EXT_JPEG
} file_ext;

typedef struct http_request http_req;
/** @brief A parsed HTTP request: method, local file path and file info. */
struct http_request
{
    http_type type;
    char uri[FILENAME_MAX]; /**< path of the file in the served directory */
    //------------------
    struct evbuffer* content;
    file_ext content_type;
    //------------------
    bool file_exist; /**< a regular file at @c uri exists */
};

#define HTTP_RESPONSE_200 "HTTP/1.0 200 OK"
#define HTTP_RESPONSE_404 "HTTP/1.0 404 Not Found"
/** @brief Longest accepted request head; a longer one is rejected. */
#define HTTP_HEAD_MAXLEN 8192U

/** @brief Result of http_request_parse(). */
typedef enum
{
    HTTP_PARSE_DONE, /**< a request head was taken and parsed */
    HTTP_PARSE_MORE, /**< the head is not complete yet: wait for more data */
    HTTP_PARSE_FAIL  /**< malformed request line or too long head */
} http_parse_res;
//-----------------------------------------
/**
 * @brief Zero the request and allocate its content buffer.
 * @return true on failure (evbuffer_new() returned NULL).
 */
bool http_request_init(struct http_request* req);
/**
 * @brief Take one request head (up to an empty line) from @p in_buf and parse it.
 *
 * An incomplete head stays in @p in_buf. The body is not read.
 */
http_parse_res http_request_parse(struct http_request* req, struct evbuffer* in_buf);
/**
 * @brief Parse the request line "METHOD URI HTTP/1.x": the method, the URI mapped
 *        into the served directory and the file type; check whether the file exists.
 *
 * A URI with "..", without a known extension or too long is not found (404).
 * @return true on failure: unknown method or version, malformed line.
 */
bool http_reqhead_parse(struct http_request* req, const char* line);
/**
 * @brief Write a 200 (with the file for GET) or 404 response to @p out_buf
 *        and clear the request.
 * @return true if the file could not be opened (a 404 is written then).
 */
bool http_response_make(struct http_request* req, struct evbuffer* out_buf);
/** @brief Reset the request for reuse, keeping its content buffer. */
void http_request_clear(struct http_request* req);
/** @brief Free the content buffer and zero the request. */
void http_request_free(struct http_request* req);
//-----------------------------------------
#endif // _HTTP_REQ_H_
