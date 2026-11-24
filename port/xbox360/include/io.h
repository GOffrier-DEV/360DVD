#ifndef DVD360_IO_H
#define DVD360_IO_H

/* libdvdcss's POSIX backend only needs the read/write declarations here. */
#include <unistd.h>
#include <sys/uio.h>

static inline ssize_t readv(int fd, const struct iovec *iov, int count)
{
    ssize_t total = 0;
    int i;
    for (i = 0; i < count; ++i) {
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((size_t)n != iov[i].iov_len)
            break;
    }
    return total;
}

#endif
