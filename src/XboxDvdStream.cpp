#include "XboxDvdStream.h"

#include <string.h>
#include <limits.h>
#include <sys/uio.h>

// Xbox kernel ABI: counted ANSI name and a three-field object descriptor.
struct DvdObjectName
{
    USHORT length, maximumLength;
    const char* buffer;
};
struct DvdObjectAttributes
{
    HANDLE root;
    DvdObjectName* name;
    ULONG attributes;
};
struct DvdIoStatus
{
    LONG status;
    ULONG_PTR information;
};
static_assert(sizeof(DvdObjectName) == 8 && sizeof(DvdObjectAttributes) == 12 &&
              sizeof(DvdIoStatus) == 8, "Unexpected Xbox kernel ABI");

extern "C" LONG NTAPI NtOpenFile(HANDLE*, ULONG, DvdObjectAttributes*,
                                DvdIoStatus*, ULONG, ULONG);
extern "C" LONG NTAPI NtReadFile(HANDLE, HANDLE, void*, void*, DvdIoStatus*,
                                void*, ULONG, LARGE_INTEGER*);

namespace
{
    const DWORD kSectorSize = 2048;
    const ULONG kCaseInsensitive = 0x40;
    const ULONG kSynchronousNonAlert = 0x20;
    const LONG kInvalidParameter = (LONG)0xC000000D;
    const LONG kDeviceDataError = (LONG)0xC000009C;

    int StreamSeek(void* stream, uint64_t offset)
    {
        return static_cast<XboxDvdStream*>(stream)->Seek(offset) ? 0 : -1;
    }

    int StreamRead(void* stream, void* buffer, int bytes)
    {
        return static_cast<XboxDvdStream*>(stream)->Read(buffer, bytes);
    }

    int StreamReadVectors(void* stream, const void* vectors, int count)
    {
        if (count < 0 || (count && !vectors))
            return -1;
        const iovec* entries = static_cast<const iovec*>(vectors);
        int total = 0;
        for (int i = 0; i < count; ++i)
        {
            const size_t length = entries[i].iov_len;
            if (!length)
                continue;
            if (length > (size_t)(INT_MAX - total))
                return total ? total : -1;
            const int bytes = StreamRead(stream, entries[i].iov_base, (int)length);
            if (bytes < 0)
                return total ? total : -1;
            total += bytes;
            if (bytes != (int)length)
                break;
        }
        return total;
    }

    int ReaderReadVectors(void* stream, void* vectors, int count)
    {
        return StreamReadVectors(stream, vectors, count);
    }
}

dvd_reader_stream_cb* XboxDvdStream::ReaderCallbacks()
{
    static dvd_reader_stream_cb callbacks = {
        StreamSeek, StreamRead, ReaderReadVectors
    };
    return &callbacks;
}

dvdcss_stream_cb* XboxDvdStream::CssCallbacks()
{
    static dvdcss_stream_cb callbacks = {
        StreamSeek, StreamRead, StreamReadVectors
    };
    return &callbacks;
}

dvdcss_stream_cb* XboxDvdStream::CssAuthCallbacks()
{
    static dvdcss_stream_cb callbacks = {
        StreamSeek, StreamRead, StreamReadVectors
    };
    return &callbacks;
}

int XboxDvdStream::CssDeviceFd() const
{
    return IsOpen() ? (int)(ULONG_PTR)m_device : -1;
}

extern "C" int dvd360_css_stream_fd(void* stream, const dvdcss_stream_cb* callbacks)
{
    if (callbacks != XboxDvdStream::CssAuthCallbacks())
        return -1;
    return static_cast<XboxDvdStream*>(stream)->CssDeviceFd();
}

XboxDvdStream::XboxDvdStream()
    : m_device(INVALID_HANDLE_VALUE), m_position(0), m_status(0)
{
}

XboxDvdStream::~XboxDvdStream()
{
    Close();
}

bool XboxDvdStream::Open()
{
    if (IsOpen())
        return true;

    static const char path[] = "\\Device\\Cdrom0";
    DvdObjectName name = { sizeof(path) - 1, sizeof(path), path };
    DvdObjectAttributes attributes = { NULL, &name, kCaseInsensitive };
    DvdIoStatus io = {};
    HANDLE device = NULL;
    m_status = NtOpenFile(&device, FILE_READ_DATA | SYNCHRONIZE, &attributes,
                          &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          kSynchronousNonAlert);
    if (m_status == 0)
        m_status = io.status;
    if (m_status == 0)
        m_device = device;
    m_position = 0;
    return IsOpen();
}

void XboxDvdStream::Close()
{
    if (IsOpen())
        CloseHandle(m_device);
    m_device = INVALID_HANDLE_VALUE;
    m_position = 0;
}

bool XboxDvdStream::IsOpen() const
{
    return m_device != INVALID_HANDLE_VALUE && m_device != NULL;
}

bool XboxDvdStream::Seek(unsigned long long byteOffset)
{
    if (!IsOpen() || byteOffset > 0x7FFFFFFFFFFFFFFFULL ||
        (byteOffset % kSectorSize) != 0)
    {
        m_status = kInvalidParameter;
        return false;
    }

    m_position = byteOffset;
    m_status = 0;
    return true;
}

int XboxDvdStream::Read(void* buffer, int byteCount)
{
    if (!IsOpen() || !buffer || byteCount <= 0 ||
        (byteCount % (int)kSectorSize) != 0 ||
        m_position > 0x7FFFFFFFFFFFFFFFULL - (unsigned int)byteCount)
    {
        m_status = kInvalidParameter;
        return -1;
    }

    // Probe one sector at a time; no shared buffer between stream instances.
    unsigned char storage[2 * kSectorSize - 1];
    unsigned char* aligned = storage +
        ((0 - (ULONG_PTR)storage) & (kSectorSize - 1));
    unsigned char* output = (unsigned char*)buffer;
    int total = 0;
    while (total < byteCount)
    {
        LARGE_INTEGER offset;
        offset.QuadPart = m_position;
        DvdIoStatus io = {};
        m_status = NtReadFile(m_device, NULL, NULL, NULL, &io, aligned,
                              kSectorSize, &offset);
        if (m_status == 0)
            m_status = io.status;
        if (m_status != 0)
            return total ? total : -1;
        const ULONG_PTR received = io.information;
        if (received == 0)
            break;
        if (received != kSectorSize)
        {
            m_status = kDeviceDataError;
            return total ? total : -1;
        }

        memcpy(output + total, aligned, received);
        total += (int)received;
        m_position += received;
    }
    return total;
}
