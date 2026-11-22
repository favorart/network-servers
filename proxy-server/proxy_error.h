#include "stdafx.h"

#ifndef _PROXY_ERROR_H_
#define _PROXY_ERROR_H_

#define _DEBUG
//-----------------------------------------
/** @brief Proxy-specific error codes, stored in my_errno. */
typedef enum
{
    SRV_ERR_NONE,
    SRV_ERR_PARAM,
    SRV_ERR_INPUT,
    SRV_ERR_LIBEV,
    SRV_ERR_RCMMN,
    SRV_ERR_RFREE,
    SRV_ERR_FCONF
} myerr;
myerr my_errno;
//-----------------------------------------
/**
 * @brief Describe the current my_errno and reset it to SRV_ERR_NONE.
 * @return Static message, or NULL for SRV_ERR_NONE.
 */
const char* strmyerror(void);
//-----------------------------------------
#endif // _PROXY_ERROR_H_
