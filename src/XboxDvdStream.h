#ifndef DVD360_XBOX_DVD_STREAM_H
#define DVD360_XBOX_DVD_STREAM_H

#include <windows.h>
#include <stdint.h>
typedef int64_t off64_t; // Matches the RXDK C library build configuration.
#include <dvdread/dvd_reader.h>
#include <dvdcss/dvdcss.h>

class XboxDvdStream
{
public:
    XboxDvdStream();
    ~XboxDvdStream();

    bool Open();
    void Close();
    bool IsOpen() const;
    bool Seek(unsigned long long byteOffset);
    int Read(void* buffer, int byteCount);
    LONG LastStatus() const { return m_status; }
    static bool DriveReportsNoMedia();
    // The stream must outlive the reader returned by DVDOpenStream.
    static dvd_reader_stream_cb* ReaderCallbacks();
    static dvd_reader_stream_cb* ReaderAuthCallbacks();
    static dvdcss_stream_cb* CssCallbacks();
    static dvdcss_stream_cb* CssAuthCallbacks();
    int CssDeviceFd() const;

private:
    XboxDvdStream(const XboxDvdStream&);
    XboxDvdStream& operator=(const XboxDvdStream&);
    HANDLE m_device;
    unsigned char* m_readStorage;
    DWORD m_readLimit;
    unsigned long long m_position;
    LONG m_status;
};

#endif
