#include "XboxDvdStream.h"
#ifdef DVD360_LOGGING
#include "FfmpegFileIo.h"
#include <stdio.h>

extern "C" void dvd360_log_css_timing(uint32_t code, uint32_t format, uint32_t ms, int32_t status)
{
    const char* stage = code == 0x335004 && format == 4 ? "title key" : "drive control";
    char message[144];
    sprintf_s(message, sizeof(message), "CSS %s: code=0x%08X format=0x%X %u ms status=0x%08X",
              stage, static_cast<unsigned>(code), static_cast<unsigned>(format),
              static_cast<unsigned>(ms), static_cast<unsigned>(status));
    LogDvdNavStartup(message);
}
#endif

#include <string.h>
#include <stdlib.h>
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
extern "C" LONG NTAPI NtDeviceIoControlFile(HANDLE, HANDLE, void*, void*,
                                          DvdIoStatus*, ULONG, void*, ULONG,
                                          void*, ULONG);

namespace
{
    const DWORD kSectorSize = 2048;
    const DWORD kReadBatch = 128 * 1024;
    const ULONG kCaseInsensitive = 0x40;
    const ULONG kSynchronousNonAlert = 0x20;
    const LONG kInvalidParameter = (LONG)0xC000000D;
    const LONG kInvalidDeviceRequest = (LONG)0xC0000010;
    const LONG kInvalidBufferSize = (LONG)0xC0000206;
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

bool XboxDvdStream::DriveReportsNoMedia()
{
    const ULONG checkVerify = 0x24800; // IOCTL_CDROM_CHECK_VERIFY
    const LONG noMedia = (LONG)0xC0000013; // STATUS_NO_MEDIA_IN_DEVICE
    static const char path[] = "\\Device\\Cdrom0";
    DvdObjectName name = { sizeof(path) - 1, sizeof(path), path };
    DvdObjectAttributes attributes = { NULL, &name, kCaseInsensitive };
    DvdIoStatus io = {};
    HANDLE device = NULL;
    LONG status = NtOpenFile(&device, FILE_READ_DATA | SYNCHRONIZE, &attributes,
                             &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             kSynchronousNonAlert);
    if (status != 0)
        return status == noMedia;
    status = io.status;
    if (status == 0 && device)
    {
        io = {};
        status = NtDeviceIoControlFile(device, NULL, NULL, NULL, &io,
                                       checkVerify, NULL, 0, NULL, 0);
        if (status == 0)
            status = io.status;
    }
    if (device)
        CloseHandle(device);
    return status == noMedia;
}

HRESULT XboxDvdStream::ProbeCdAudio(CdAudioProbe& probe)
{
    probe = {};
    probe.stage = "open";
    if (!Open())
        return HRESULT_FROM_NT(LastStatus());

    const ULONG readToc = 0x24002;
    __declspec(align(16)) BYTE toc[804] = {};
    DvdIoStatus io = {};
    probe.stage = "read TOC";
    LONG status = NtDeviceIoControlFile(m_device, NULL, NULL, NULL, &io,
                                        readToc, NULL, 0, toc, sizeof(toc));
    if (status == 0) status = io.status;
    probe.tocBytes = static_cast<unsigned>(io.information);
    if (status != 0) return HRESULT_FROM_NT(status);
    probe.tocLength = 2 + (toc[0] << 8) + toc[1];
    probe.tocFirst = toc[2];
    probe.tocLast = toc[3];

    probe.stage = "parse TOC";
    const unsigned tocSize = probe.tocLength;
    if (tocSize < 20 || tocSize > sizeof(toc) || tocSize > io.information ||
        toc[2] == 0 || toc[3] < toc[2] || toc[3] > 99 ||
        tocSize < 4 + 8u * (toc[3] - toc[2] + 2))
        return E_FAIL;

    const unsigned trackCount = toc[3] - toc[2] + 1;
    unsigned lbas[100];
    for (unsigned i = 0; i <= trackCount; ++i)
    {
        const BYTE* track = toc + 4 + i * 8;
        const unsigned number = i == trackCount ? 0xAA : toc[2] + i;
        if (track[2] != number || track[6] >= 60 || track[7] >= 75)
            return E_FAIL;
        const unsigned frames = (track[5] * 60u + track[6]) * 75u + track[7];
        if (frames < 150) return E_FAIL;
        lbas[i] = frames - 150;
        if (i && lbas[i] <= lbas[i - 1]) return E_FAIL;
    }
    for (unsigned i = 0; i < trackCount; ++i)
    {
        const BYTE* track = toc + 4 + i * 8;
        if (track[1] & 0x04)
            ++probe.dataTracks;
        else
        {
            if (!probe.audioTracks)
            {
                probe.firstTrack = track[2];
                probe.startLba = lbas[i];
                probe.endLba = lbas[i + 1];
            }
            probe.tracks[probe.audioTracks++] = {lbas[i], lbas[i + 1], track[2]};
        }
    }
    if (!probe.audioTracks)
    {
        probe.stage = "no audio tracks";
        return S_FALSE;
    }

    probe.stage = "raw read";
    __declspec(align(16)) BYTE pcm[2352];
    return ReadCdAudio(probe.startLba, 1, pcm, probe.bytesRead);
}

HRESULT XboxDvdStream::ReadOpticalInfo(const BYTE* cdb, unsigned cdbSize, BYTE* data,
                                      unsigned capacity, unsigned& bytesRead, BYTE (&sense)[18])
{
    bytesRead = 0;
    memset(sense, 0, sizeof(sense));
    if (!IsOpen() || !cdb || !data || !capacity || capacity > 0x20000 ||
        ((reinterpret_cast<ULONG_PTR>(data) | capacity) & 1) ||
        !((cdbSize == 6 && (cdb[0] == 0x12 || cdb[0] == 0x1a)) ||
          (cdbSize == 10 && (cdb[0] == 0x42 || cdb[0] == 0x46 || cdb[0] == 0x5a)) ||
          (cdbSize == 12 && cdb[0] == 0xbe)))
        return E_INVALIDARG;

    struct PassThrough
    {
        USHORT length;
        BYTE reserved[5];
        BYTE dataIn;
        ULONG transferLength, timeout;
        void* buffer;
        BYTE command[16];
    };
    // Verified against SataCdRomStartScsiPassThrough in the Xbox 360 kernel.
    static_assert(sizeof(PassThrough) == 36 && offsetof(PassThrough, dataIn) == 7 &&
                  offsetof(PassThrough, transferLength) == 8 && offsetof(PassThrough, buffer) == 16 &&
                  offsetof(PassThrough, command) == 20, "Xbox SCSI pass-through layout");
    PassThrough request = {};
    request.length = sizeof(request);
    request.dataIn = 1;
    request.transferLength = capacity;
    request.buffer = data;
    memcpy(request.command, cdb, cdbSize);
    DvdIoStatus io = {};
    const ULONG passThroughDirect = 0x4d014;
    LONG status = NtDeviceIoControlFile(m_device, NULL, NULL, NULL, &io, passThroughDirect,
                                       &request, sizeof(request), sense, sizeof(sense));
    if (status == 0) status = io.status;
    if (status) return HRESULT_FROM_NT(status);
    // CHECK CONDITION is returned as sense data with successful NT status.
    if (io.information == sizeof(sense)) return E_FAIL;
    // The kernel does not return a data transfer count in this IOCTL.
    bytesRead = capacity;
    return S_OK;
}

HRESULT XboxDvdStream::ReadCdAudio(unsigned lba, unsigned sectors, void* pcm,
                                 unsigned& bytesRead)
{
    bytesRead = 0;
    if (!IsOpen() || !pcm || !sectors || sectors > 55)
        return E_INVALIDARG;
    const ULONG rawRead = 0x2403E;
    struct RawReadRequest
    {
        LARGE_INTEGER byteOffset;
        ULONG sectors;
        ULONG mode;
    };
    static_assert(sizeof(RawReadRequest) == 16, "Xbox CD raw-read layout");
    RawReadRequest request = {};
    request.byteOffset.QuadPart = static_cast<LONGLONG>(lba) * 2048;
    request.sectors = sectors;
    request.mode = 2; // CDDA
    const unsigned size = sectors * 2352;
    DvdIoStatus io = {};
    LONG status = NtDeviceIoControlFile(m_device, NULL, NULL, NULL, &io, rawRead,
                                       &request, sizeof(request), pcm, size);
    if (status == 0) status = io.status;
    bytesRead = static_cast<unsigned>(io.information);
    if (status != 0) return HRESULT_FROM_NT(status);
    return bytesRead == size ? S_OK : E_FAIL;
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

dvd_reader_stream_cb* XboxDvdStream::ReaderAuthCallbacks()
{
    static dvd_reader_stream_cb callbacks = {
        StreamSeek, StreamRead, ReaderReadVectors
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
    if (callbacks != XboxDvdStream::CssAuthCallbacks() &&
        static_cast<const void*>(callbacks) != XboxDvdStream::ReaderAuthCallbacks())
        return -1;
    return static_cast<XboxDvdStream*>(stream)->CssDeviceFd();
}

XboxDvdStream::XboxDvdStream()
    : m_device(INVALID_HANDLE_VALUE), m_readStorage(NULL), m_readLimit(kReadBatch),
      m_position(0), m_status(0)
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
#ifdef DVD360_LOGGING
    m_firstOpticalRead = true;
    m_opticalReadCalls = 0;
    m_opticalReadBytes = 0;
    m_opticalReadTotalMs = 0;
    m_opticalReadWorstMs = 0;
#endif
    if (IsOpen())
        CloseHandle(m_device);
    m_device = INVALID_HANDLE_VALUE;
    free(m_readStorage);
    m_readStorage = NULL;
    m_readLimit = kReadBatch;
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

    // Keep the aligned staging buffer private to this drive session.
    if (byteCount > (int)kSectorSize && m_readLimit > kSectorSize && !m_readStorage)
    {
        m_readStorage = static_cast<unsigned char*>(malloc(kReadBatch + kSectorSize - 1));
        if (!m_readStorage) m_readLimit = kSectorSize;
    }
    unsigned char storage[2 * kSectorSize - 1];
    unsigned char* base = m_readStorage ? m_readStorage : storage;
    unsigned char* aligned = base + ((0 - (ULONG_PTR)base) & (kSectorSize - 1));
    unsigned char* output = (unsigned char*)buffer;
    int total = 0;
    while (total < byteCount)
    {
        const ULONG remaining = byteCount - total;
        const ULONG limit = m_readStorage ? m_readLimit : kSectorSize;
        const ULONG requested = remaining < limit ? remaining : limit;
        LARGE_INTEGER offset;
        offset.QuadPart = m_position;
        DvdIoStatus io = {};
#ifdef DVD360_LOGGING
        const DWORD readStart = GetTickCount();
#endif
        m_status = NtReadFile(m_device, NULL, NULL, NULL, &io, aligned,
                              requested, &offset);
        if (m_status == 0)
            m_status = io.status;
#ifdef DVD360_LOGGING
        const DWORD readMs = GetTickCount() - readStart;
        ++m_opticalReadCalls;
        m_opticalReadBytes += io.information;
        m_opticalReadTotalMs += readMs;
        if (readMs > m_opticalReadWorstMs) m_opticalReadWorstMs = readMs;
        if ((m_opticalReadCalls % 16) == 0)
        {
            char summary[144];
            sprintf_s(summary, sizeof(summary),
                      "DVD optical read summary: calls=%u bytes=%I64u total=%I64u ms worst=%lu ms",
                      m_opticalReadCalls, m_opticalReadBytes, m_opticalReadTotalMs,
                      m_opticalReadWorstMs);
            LogDvdNavStartup(summary);
        }
        if (m_firstOpticalRead || readMs >= 250)
        {
            char message[144];
            sprintf_s(message, sizeof(message), "DVD %s optical read: LBA=%llu bytes=%u %u ms status=0x%08X",
                      m_firstOpticalRead ? "first" : "slow", m_position / kSectorSize,
                      static_cast<unsigned>(requested), static_cast<unsigned>(readMs),
                      static_cast<unsigned>(m_status));
            LogDvdNavStartup(message);
        }
        m_firstOpticalRead = false;
#endif
        if (m_status != 0)
        {
            if (requested > kSectorSize &&
                (m_status == kInvalidParameter || m_status == kInvalidDeviceRequest ||
                 m_status == kInvalidBufferSize))
            {
                m_readLimit = ((requested / kSectorSize) / 2) * kSectorSize;
                continue; // Retry the same offset with a smaller transfer.
            }
            return total ? total : -1;
        }
        const ULONG_PTR received = io.information;
        if (received == 0)
            break;
        if (received > requested || (received % kSectorSize) != 0)
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
