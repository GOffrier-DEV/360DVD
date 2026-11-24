#ifndef DVD360_SYS_UIO_H
#define DVD360_SYS_UIO_H

#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
struct iovec {
    void *iov_base;
    size_t iov_len;
};

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
