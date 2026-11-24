#ifndef DVD360_CONFIG_H
#define DVD360_CONFIG_H

/* Shared feature-test configuration for the Xbox 360 static-library build. */
#define HAVE_CONFIG_H 1
#define HAVE_DVDCSS_DVDCSS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LARGEFILE_SUPPORT 1
#define HAVE_IO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_TIME_H 1
#define HAVE_WINDOWS_H 1
#define HAVE_WINIOCTL_H 1
#define USE_READDIR 1
#define UNUSED __attribute__((unused))
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef lseek64
#define lseek64 lseek
#endif
#ifndef off64_t
#define off64_t int64_t
#endif
#define WORDS_BIGENDIAN 1

#define PACKAGE_VERSION "xbox360"
#define VERSION "xbox360"

#endif
