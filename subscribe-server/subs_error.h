#include "stdafx.h"

#ifndef _SRV_ERROR_H_
#define _SRV_ERROR_H_

// #define _DEBUG
// #define MULTI_PROCESS
//-----------------------------------------
/** @brief Server error codes. */
typedef enum
{
    SRV_ERR_NONE,
    SRV_ERR_PARAM,
    SRV_ERR_INPUT,
    SRV_ERR_LIBEV,
    SRV_ERR_RCMMN,
    SRV_ERR_RFREE,
    SRV_ERR_FDTRS
} myerr;
/**
 * @brief Human-readable text for a server error code.
 * @return Static string, or NULL for SRV_ERR_NONE/unknown codes.
 */
const char* strmyerror(myerr err);
//-----------------------------------------
#endif // _SRV_ERROR_H_
