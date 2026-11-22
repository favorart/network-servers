
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
    http_type type; // v
    char uri[FILENAME_MAX];
    //------------------
    struct evbuffer* content; // v
    file_ext content_type;    // v
    //------------------
    bool file_exist; // v
                     //------------------
};

#define HTTP_RESPONSE_200 "HTTP/1.0 200 OK"
#define HTTP_RESPONSE_404 "HTTP/1.0 404 Not Found"
//-----------------------------------------
/**
 * @brief Zero the request and allocate its content buffer.
 * @return true on failure (evbuffer_new() returned NULL).
 */
bool http_request_init(struct http_request* req);
/**
 * @brief Take one request head (up to "\r\n\r\n") from @p in_buf and parse it.
 * @return true on failure; the body is not read.
 */
bool http_request_parse(struct http_request* req, struct evbuffer* in_buf);
/**
 * @brief Parse the request line: method, URI (mapped into the served directory)
 *        and file type; check whether the file exists.
 * @return true on failure (the request is freed then).
 */
bool http_reqhead_parse(struct http_request* req, struct evbuffer* in_buf);
/**
 * @brief Write a 200 (with the file for GET) or 404 response to @p out_buf
 *        and clear the request.
 * @return true if the file could not be opened, false otherwise.
 */
bool http_response_make(struct http_request* req, struct evbuffer* out_buf);
/** @brief Reset the request for reuse, keeping its content buffer. */
void http_request_clear(struct http_request* req);
/** @brief Free the content buffer and zero the request. */
void http_request_free(struct http_request* req);
//-----------------------------------------
#endif // _HTTP_REQ_H_
