#ifndef DVD360_SYS_TIME_H
#define DVD360_SYS_TIME_H

/* Avoid winsockx.h here: its select/fd_set declarations conflict with RXDK libc. */
#include <time.h>
struct timeval {
    long tv_sec;
    long tv_usec;
};

#endif
