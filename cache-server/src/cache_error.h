#ifndef _CACHE_ERROR_H_
#define _CACHE_ERROR_H_
//-----------------------------------------
/** @brief Server error codes, stored in my_errno. */
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
/** @brief Last server error code (defined in cache_config.c). */
extern myerr my_errno;
//-----------------------------------------
/** @brief Text for the current my_errno (NULL if none); resets my_errno. */
const char* strmyerror(void);
//-----------------------------------------
#endif // _CACHE_ERROR_H_
