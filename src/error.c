#include "squash.h"

const char *sq_strerror(int err)
{
    switch (err) {
    case SQ_OK: return "ok";
    case SQ_ERR_DATA: return "corrupt or truncated input";
    case SQ_ERR_MEM: return "out of memory or output too large";
    case SQ_ERR_ARG: return "invalid argument";
    case SQ_ERR_UNSUPPORTED: return "unsupported feature";
    default: return "unknown error";
    }
}
