#include <stddef.h>

#include "http_error.h"

myerr my_errno = HTTP_ERR_NONE;
//-----------------------------------------
const char* strmyerror(void)
{
    const char* strerr;

    switch (my_errno) {
    default:
        strerr = NULL;
        break;

    case HTTP_ERR_PARAM:
        strerr = "Invalid function argument.";
        break;
    case HTTP_ERR_INPUT:
        strerr = "Input incorrect user data.";
        break;
    case HTTP_ERR_LIBEV:
        strerr = "Incorrect libevent entity.";
        break;
    case HTTP_ERR_RCMMN:
        strerr = "Incorrect request command.";
        break;
    case HTTP_ERR_RFREE:
        strerr = "Dispatched a free request.";
        break;
    case HTTP_ERR_FDTRS:
        strerr = "Failure in transmiting fd.";
        break;
    }

    my_errno = HTTP_ERR_NONE;
    return strerr;
}
