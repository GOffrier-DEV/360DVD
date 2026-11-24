#ifndef DVD360_WINIOCTL_H
#define DVD360_WINIOCTL_H

/* These are only needed to compile libdvdcss's desktop ioctl declarations.
   The Xbox stream backend does not issue these desktop device-control calls. */
#define FILE_DEVICE_DVD 0x00000033
#define FILE_DEVICE_CONTROLLER 0x00000004
#define METHOD_BUFFERED 0
#define FILE_READ_ACCESS 0x0001
#define FILE_WRITE_ACCESS 0x0002
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

#endif
