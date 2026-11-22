#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <event2/buffer.h>

#include "http_error.h"
#include "http_server.h"
#include "http_request.h"

//-----------------------------------------
bool http_request_init(struct http_request* req)
{
    memset(req, 0, sizeof(*req));
    //-----------------------------------
    return ((req->content = evbuffer_new()) == NULL);
}
void http_request_free(struct http_request* req)
{
    if (req->content)
        evbuffer_free(req->content);
    memset(req, 0, sizeof(*req));
}
//-----------------------------------------
void http_request_clear(struct http_request* req)
{
    if (req->content)
        evbuffer_drain(req->content, evbuffer_get_length(req->content));
    //-----------------------------------
    req->type = HTTP_REQ_NONE;
    req->file_exist = false;
    req->content_type = HTTP_EXT_NONE;
    memset(req->uri, 0, FILENAME_MAX);
}
http_parse_res http_request_parse(struct http_request* req, struct evbuffer* in_buf)
{
    static const char HTTP_HEAD_TERM[] = "\r\n\r\n";
    const size_t HTTP_HEAD_TERM_LENGTH = sizeof(HTTP_HEAD_TERM) - 1U;
    //-----------------------------------------------------------------
    /* Empty lines before the request line are ignored */
    char lead[2];
    while (evbuffer_copyout(in_buf, lead, sizeof(lead)) == (ev_ssize_t)sizeof(lead) &&
           lead[0] == '\r' && lead[1] == '\n')
        evbuffer_drain(in_buf, sizeof(lead));

    size_t buffer_size = evbuffer_get_length(in_buf);
    if (!buffer_size)
        return HTTP_PARSE_MORE;
    //-----------------------------------------------------------------
    struct evbuffer_ptr head_closer;
    head_closer = evbuffer_search(in_buf, HTTP_HEAD_TERM, HTTP_HEAD_TERM_LENGTH, NULL);
    if (head_closer.pos == -1) {
        if (buffer_size <= HTTP_HEAD_MAXLEN)
            return HTTP_PARSE_MORE; /* the rest of the head is still on its way */
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        return HTTP_PARSE_FAIL;
    }
    if ((size_t)head_closer.pos > HTTP_HEAD_MAXLEN) {
        my_errno = HTTP_ERR_INPUT;
        fprintf(stderr, "%s\n", strmyerror());
        return HTTP_PARSE_FAIL;
    }
    //-----------------------------------------------------------------
    /* The request line ends at the first "\r\n", which is inside the head */
    size_t line_length = 0;
    char* line = evbuffer_readln(in_buf, &line_length, EVBUFFER_EOL_CRLF_STRICT);
    if (!line) {
        my_errno = HTTP_ERR_LIBEV;
        fprintf(stderr, "%s\n", strmyerror());
        return HTTP_PARSE_FAIL;
    }
    /* Headers are skipped; the body (if any) stays in in_buf */
    evbuffer_drain(in_buf, (size_t)head_closer.pos + HTTP_HEAD_TERM_LENGTH - (line_length + 2U));

    bool fail = (strlen(line) != line_length) || http_reqhead_parse(req, line);
    free(line);
    //-----------------------------------------------------------------
    return fail ? HTTP_PARSE_FAIL : HTTP_PARSE_DONE;
}
//-----------------------------------------
/** @brief Whether the relative path @p path of @p length bytes has a ".." segment. */
static bool http_path_has_dotdot(const char* path, size_t length)
{
    const char* end = path + length;
    for (const char* seg = path; seg < end;) {
        const char* slash = memchr(seg, '/', (size_t)(end - seg));
        const char* seg_end = slash ? slash : end;
        if (seg_end - seg == 2 && seg[0] == '.' && seg[1] == '.')
            return true;
        seg = seg_end + 1;
    }
    return false;
}
/** @brief File type by the extension of the last path segment. */
static file_ext http_path_ext(const char* path)
{
    const char* slash = strrchr(path, '/');
    const char* dot = strrchr(slash ? slash : path, '.');
    if (!dot)
        return HTTP_EXT_NONE;
    ++dot;
    if (!strcasecmp(dot, "html") || !strcasecmp(dot, "htm"))
        return HTTP_EXT_HTML;
    if (!strcasecmp(dot, "jpeg") || !strcasecmp(dot, "jpg"))
        return HTTP_EXT_JPEG;
    return HTTP_EXT_NONE;
}
bool http_reqhead_parse(struct http_request* req, const char* line)
{
    static const char* const http_command_strs[] = {"HEAD", "GET", "POST"};
    static const http_type http_command_enum[] = {HTTP_REQ_HEAD, HTTP_REQ_GET, HTTP_REQ_POST};
    const size_t http_command_length = sizeof(http_command_enum) / sizeof(*http_command_enum);

    static const char* const http_versions[] = {"HTTP/1.0", "HTTP/1.1"};
    const size_t http_versions_length = sizeof(http_versions) / sizeof(*http_versions);
    //-----------------------------------------------------------------
    /* METHOD SP URI SP VERSION */
    const char* uri_bgn = strchr(line, ' ');
    const char* uri_end = uri_bgn ? strchr(uri_bgn + 1, ' ') : NULL;
    if (!uri_end || uri_end == uri_bgn + 1) {
        my_errno = HTTP_ERR_RCMMN;
        fprintf(stderr, "%s\n", strmyerror());
        return true;
    }
    const char* version = uri_end + 1;
    ++uri_bgn;

    http_type type = HTTP_REQ_NONE;
    size_t method_length = (size_t)(uri_bgn - 1 - line);
    for (size_t i = 0; i < http_command_length; ++i) {
        if (method_length == strlen(http_command_strs[i]) &&
            !strncmp(line, http_command_strs[i], method_length)) {
            type = http_command_enum[i];
            break;
        }
    }

    bool version_ok = false;
    for (size_t i = 0; i < http_versions_length; ++i)
        version_ok = version_ok || !strcmp(version, http_versions[i]);

    if (type == HTTP_REQ_NONE || !version_ok) {
        my_errno = HTTP_ERR_RCMMN;
        fprintf(stderr, "%s\n", strmyerror());
        return true;
    }
    //-----------------------------------------------------------------
    http_request_clear(req);
    req->type = type;
    if (req->type == HTTP_REQ_POST)
        return false; /* POST is answered with 200 without a file */
    //-----------------------------------------------------------------
    /* receive the exact path to the required file */
    if (*uri_bgn == '/')
        ++uri_bgn;
    size_t uri_length = (size_t)(uri_end - uri_bgn);

    const char* dir = server_conf.folder_path;
    size_t dir_length = strlen(dir);
    size_t slash = (dir_length && dir[dir_length - 1] != '/') ? 1U : 0U;

    /* Paths out of the directory and too long ones are not found */
    if (http_path_has_dotdot(uri_bgn, uri_length) ||
        dir_length + slash + uri_length >= sizeof(req->uri))
        return false;

    memcpy(req->uri, dir, dir_length);
    if (slash)
        req->uri[dir_length] = '/';
    memcpy(req->uri + dir_length + slash, uri_bgn, uri_length);
    req->uri[dir_length + slash + uri_length] = '\0';
    //-----------------------------------------------------------------
    req->content_type = http_path_ext(req->uri);
    if (req->content_type == HTTP_EXT_NONE)
        return false;

    /* Check for existence */
    struct stat sb;
    req->file_exist = (stat(req->uri, &sb) == 0 && S_ISREG(sb.st_mode));
    //-----------------------------------------------------------------
#ifdef _DEBUG
    printf("\nREQUEST: %s\nFilepath: %s\n\n", line, req->uri);
#endif // _DEBUG
    return false;
}
bool http_response_make(struct http_request* req, struct evbuffer* out_buf)
{
    if (req->type == HTTP_REQ_NONE || req->type == HTTP_RESPONSE)
        return false;
    //----------------------------------------------------------------
    const char connection[] = "close";
    char response_date[64];

    time_t timev;
    time(&timev);
    struct tm timeinfo;
    gmtime_r(&timev, &timeinfo);
    strftime(response_date, sizeof(response_date), "%a, %d %b %Y %H:%M:%S GMT", &timeinfo);
    //----------------------------------------------------------------
    bool open_failed = false;
    int fd = -1;
    struct stat sb;
    if ((req->type == HTTP_REQ_HEAD || req->type == HTTP_REQ_GET) && req->file_exist) {
        fd = open(req->uri, O_RDONLY);
        if (fd < 0 || fstat(fd, &sb) != 0) {
            fprintf(stderr, "%s\n", strerror(errno));
            if (fd >= 0)
                close(fd);
            fd = -1;
            open_failed = true;
        }
    }
    //----------------------------------------------------------------
    if (req->type == HTTP_REQ_POST) {
#ifdef _DEBUG
        printf("\nRESPONSE: %s\n"
               "Server: %s\n"
               "Date: %s\n",
               HTTP_RESPONSE_200, server_conf.ptr_server_name, response_date);
#endif // _DEBUG
       //----------------------------------------------------------------
        evbuffer_add_printf(out_buf,
                            "%s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            HTTP_RESPONSE_200, server_conf.ptr_server_name, response_date,
                            connection);

    } else if (fd >= 0) {
        //----------------------------------------------------------------
        const char* content_type = (req->content_type == HTTP_EXT_HTML) ? "text/html; charset=utf-8"
                                   : (req->content_type == HTTP_EXT_JPEG) ? "image/jpeg"
                                                                          : "";
        size_t content_len = (size_t)sb.st_size;
        //----------------------------------------------------------------
        evbuffer_add_printf(out_buf,
                            "%s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            HTTP_RESPONSE_200, server_conf.ptr_server_name, response_date,
                            content_type, content_len, connection);

        if (req->type == HTTP_REQ_GET && content_len) {
            /* The file descriptor will be closed after the data
             * is flushed from the evbuffer, or when the evbuffer
             * is freed.
             */
            if (evbuffer_add_file(out_buf, fd, 0, (ev_off_t)content_len) != 0)
                fprintf(stderr, "evbuffer_add_file failed\n");
        } else {
            close(fd);
        }
        //----------------------------------------------------------------

#ifdef _DEBUG
        printf("\nRESPONSE: %s\n"
               "Server: %s\n"
               "Date: %s\n"
               "Content-Type: %s\n"
               "Content-Length: %zu\n",
               HTTP_RESPONSE_200, server_conf.ptr_server_name, response_date, content_type,
               content_len);
#endif // _DEBUG
    } else {
        evbuffer_add_printf(out_buf,
                            "%s\r\n"
                            "Server: %s\r\n"
                            "Date: %s\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            HTTP_RESPONSE_404, server_conf.ptr_server_name, response_date,
                            connection);
    }
    //----------------------------------------------------------------
    http_request_clear(req);
    return open_failed;
}
//-----------------------------------------
