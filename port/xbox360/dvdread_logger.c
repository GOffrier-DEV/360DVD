#ifdef DVD360_LOGGING
#include "../../deps/libdvdread/src/logger.c"
#else
#include "config.h"
#include "dvdread/dvd_reader.h"

void DVDReadLog(void *priv, const dvd_logger_cb *logcb,
                dvd_logger_level_t level, const char *fmt, ...)
{
}
#endif
