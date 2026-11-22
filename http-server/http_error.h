#include "stdafx.h"

#ifndef _HTTP_ERR_H_
#define _HTTP_ERR_H_

// #define _DEBUG
#define HTTP_SYSLOG
//-----------------------------------------
/** @brief Server-specific error codes, stored in my_errno. */
typedef enum
{
    HTTP_ERR_NONE,
    HTTP_ERR_PARAM,
    HTTP_ERR_INPUT,
    HTTP_ERR_LIBEV,
    HTTP_ERR_RCMMN,
    HTTP_ERR_RFREE,
    HTTP_ERR_FDTRS
} myerr;
myerr my_errno;
//-----------------------------------------
/**
 * @brief Describe the current my_errno and reset it to HTTP_ERR_NONE.
 * @return Static message, or NULL for HTTP_ERR_NONE.
 */
const char* strmyerror(void);
//-----------------------------------------
#endif // _HTTP_ERR_H_
