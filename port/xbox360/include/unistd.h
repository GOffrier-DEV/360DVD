#ifndef DVD360_UNISTD_H
#define DVD360_UNISTD_H

#include_next <unistd.h>
#include <sys/types.h>

ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int origin);

#ifndef lseek64
#define lseek64 lseek
#endif

#endif
