#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <bearssl.h>
#include "CdRip.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
}

struct RipDeviceName
{
    USHORT length, maximumLength;
    const char* buffer;
};
extern "C" LONG NTAPI ObCreateSymbolicLink(const RipDeviceName*, const RipDeviceName*);
extern "C" LONG NTAPI ObDeleteSymbolicLink(const RipDeviceName*);

namespace
{
    const unsigned framesPerSector = 588;
    const unsigned batch = 12;
    const unsigned edgeFrames = 5 * framesPerSector;

    // Prevent RXDK's protected store-conditional sequence from being split.
    __attribute__((optnone)) void PublishProgress(volatile LONG& progress, LONG value)
    {
        InterlockedExchange(&progress, value);
    }

    class RipDrive
    {
    public:
        bool Mount()
        {
            const char device[] = "\\Device\\Harddisk0\\Partition1";
            const RipDeviceName partition = {sizeof(device) - 1, sizeof(device), device};
            mounted = ObCreateSymbolicLink(&alias, &partition) >= 0;
            return mounted;
        }
        ~RipDrive() { if (mounted) ObDeleteSymbolicLink(&alias); }

    private:
        const RipDeviceName alias = {sizeof("\\??\\DvdRip:") - 1, sizeof("\\??\\DvdRip:"), "\\??\\DvdRip:"};
        bool mounted = false;
    };

    bool Cancelled(HANDLE stop, HANDLE cancel)
    {
        return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
               WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0;
    }

    int WriteWav(void* opaque, const uint8_t* bytes, int count)
    {
        DWORD written = 0;
        return WriteFile(opaque, bytes, count, &written, NULL) && written == unsigned(count)
            ? count : AVERROR(EIO);
    }

    int64_t SeekWav(void* opaque, int64_t offset, int whence)
    {
        if (whence == AVSEEK_SIZE) return GetFileSize(opaque, NULL);
        whence &= ~AVSEEK_FORCE;
        if (whence < SEEK_SET || whence > SEEK_END ||
            offset < INT32_MIN || offset > INT32_MAX)
            return AVERROR(EINVAL);
        const DWORD position = SetFilePointer(opaque, LONG(offset), NULL, whence);
        return position == INVALID_SET_FILE_POINTER ? AVERROR(EIO) : int64_t(position);
    }

    struct Checksum { uint32_t v1, v2; };

    HRESULT RipTrack(XboxDvdStream& drive, const XboxDvdStream::CdAudioProbe& disc,
                     unsigned track, int offset, HANDLE stop, HANDLE cancel,
                     const char* path, Checksum& crc, unsigned framesCompleted,
                     unsigned discFrames, volatile LONG& progress)
    {
        char temporary[MAX_PATH];
        sprintf_s(temporary, sizeof(temporary), "%s.part", path);
        HANDLE file = CreateFile(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

        AVFormatContext* wav = NULL;
        int result = avformat_alloc_output_context2(&wav, NULL, "wav", NULL);
        AVIOContext* io = NULL;
        if (result >= 0)
        {
            uint8_t* buffer = static_cast<uint8_t*>(av_malloc(32768));
            if (buffer) io = avio_alloc_context(buffer, 32768, 1, file, NULL, WriteWav, SeekWav);
            if (!io) av_free(buffer);
            wav->pb = io;
            wav->flags |= AVFMT_FLAG_CUSTOM_IO;
            AVStream* audio = io ? avformat_new_stream(wav, NULL) : NULL;
            if (!audio) result = AVERROR(ENOMEM);
            else
            {
                audio->time_base = AVRational{1, 44100};
                audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
                audio->codecpar->sample_rate = 44100;
                audio->codecpar->bits_per_coded_sample = 16;
                audio->codecpar->block_align = 4;
                av_channel_layout_default(&audio->codecpar->ch_layout, 2);
                result = avformat_write_header(wav, NULL);
            }
        }

        const auto& range = disc.tracks[track];
        const unsigned length = (range.end - range.start) * framesPerSector;
        const unsigned first = track == 0 ? edgeFrames - 1 : 0;
        const unsigned end = track + 1 == disc.audioTracks
            ? (length > edgeFrames ? length - edgeFrames : 0) : length;
        const int discStart = int((disc.dataTracks ? range.start : disc.tracks[0].start) * framesPerSector);
        const int discEnd = int((disc.dataTracks ? range.end : disc.tracks[disc.audioTracks - 1].end) * framesPerSector);
        __declspec(align(16)) BYTE raw[(batch + 1) * 2352];
        AVPacket* packet = av_packet_alloc();
        HRESULT status = result < 0 || !packet ? E_FAIL : S_OK;
        for (unsigned pos = 0; pos < length && status == S_OK;)
        {
            if (Cancelled(stop, cancel)) { status = S_FALSE; break; }
            // Publish one atomic snapshot for the UI, which runs on another thread.
            const unsigned trackPercent = unsigned(uint64_t(pos) * 100 / length);
            const unsigned discPercent = unsigned(uint64_t(framesCompleted + pos) * 100 / discFrames);
            PublishProgress(progress, (range.number << 16) | (trackPercent << 8) | discPercent);
            const unsigned count = min(length - pos, batch * framesPerSector);
            const int source = int(range.start * framesPerSector + pos) + offset;
            const int start = max(source, discStart);
            const int finish = min(source + int(count), discEnd);
            const unsigned sector = unsigned(start) / framesPerSector;
            if (finish > start)
            {
                const unsigned sectors = (unsigned(finish) + framesPerSector - 1) / framesPerSector - sector;
                unsigned bytes;
                for (unsigned retry = 0; retry < 3; ++retry)
                {
                    status = drive.ReadCdAudio(sector, sectors, raw, bytes);
                    if (status == S_OK || Cancelled(stop, cancel)) break;
                }
                if (status != S_OK) break;
            }
            if (av_new_packet(packet, count * 4) < 0) { status = E_OUTOFMEMORY; break; }
            for (unsigned i = 0; i < count; ++i)
            {
                const int frame = source + int(i);
                BYTE* output = packet->data + i * 4;
                uint32_t sample = 0;
                if (frame >= start && frame < finish)
                {
                    const BYTE* p = raw + (unsigned(frame) - sector * framesPerSector) * 4;
                    // Raw CDDA is already little-endian. Playback swaps it
                    // for XAudio2 on Xenon; PCM_S16LE WAV needs the raw order.
                    memcpy(output, p, 4);
                    sample = AV_RL32(p);
                }
                else
                    memset(output, 0, 4);
                if (pos + i >= first && pos + i < end)
                {
                    const uint64_t product = uint64_t(sample) * (pos + i + 1);
                    crc.v1 += uint32_t(product);
                    crc.v2 += uint32_t(product) + uint32_t(product >> 32);
                }
            }
            packet->pts = packet->dts = pos;
            packet->duration = count;
            if (av_write_frame(wav, packet) < 0) status = E_FAIL;
            av_packet_unref(packet);
            pos += count;
        }
        if (status == S_OK && (av_write_trailer(wav) < 0 || io->error < 0 || !FlushFileBuffers(file)))
            status = E_FAIL;
        av_packet_free(&packet);
        avformat_free_context(wav);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
        CloseHandle(file);
        if (status == S_OK && !MoveFile(temporary, path))
            status = HRESULT_FROM_WIN32(GetLastError());
        if (status != S_OK) DeleteFile(temporary);
        return status;
    }

    struct DiscId { uint32_t first, second, cddb; };

    DiscId Identify(const XboxDvdStream::CdAudioProbe& disc)
    {
        const unsigned leadout = disc.tracks[disc.audioTracks - 1].end;
        DiscId id = {leadout, leadout * (disc.audioTracks + 1), 0};
        unsigned digits = 0;
        for (unsigned i = 0; i < disc.audioTracks; ++i)
        {
            const unsigned lba = disc.tracks[i].start;
            id.first += lba;
            id.second += (lba ? lba : 1) * (i + 1);
            for (unsigned seconds = (lba + 150) / 75; seconds; seconds /= 10)
                digits += seconds % 10;
        }
        const unsigned duration = (leadout + 150) / 75 - (disc.tracks[0].start + 150) / 75;
        id.cddb = ((digits % 255) << 24) | (duration << 8) | disc.audioTracks;
        return id;
    }

    int ConnectDatabase(SOCKET socketFd, const sockaddr_in& address, HANDLE stop,
                        const char*& failure, int& plainTcpError)
    {
        // XexUtils uses this Xbox-specific option for ordinary Internet TCP.
        const int xboxPlainTcp = 0x5801;
        const BOOL enabled = TRUE;
        plainTcpError = 0;
        if (setsockopt(socketFd, SOL_SOCKET, xboxPlainTcp,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            plainTcpError = error;
            failure = error == WSAEACCES ? "console denied Internet socket" : "plain TCP option failed";
            return error;
        }

        u_long nonblocking = 1;
        failure = "nonblocking setup failed";
        if (ioctlsocket(socketFd, FIONBIO, &nonblocking)) return WSAGetLastError();
        failure = "TCP connect failed";
        if (!connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) return 0;
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) return error;

        // A writable Xbox socket can still be unconnected. Confirm the peer
        // before handing the socket to BearSSL.
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < 8000 && WaitForSingleObject(stop, 50) == WAIT_TIMEOUT)
        {
            sockaddr_in peer = {};
            int length = sizeof(peer);
            if (!getpeername(socketFd, reinterpret_cast<sockaddr*>(&peer), &length)) return 0;
            error = WSAGetLastError();
            if (error != WSAENOTCONN && error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
                return error;
        }
        return WSAETIMEDOUT;
    }

    const char accurateRipHost[] = "www.accuraterip.com";

    struct TlsSocket
    {
        SOCKET socket;
        HANDLE stop, cancel;
        DWORD start;
        int& error;
        unsigned sent, received;
    };

    int TransferTls(TlsSocket& io, void* buffer, size_t length, bool writing)
    {
        while (!Cancelled(io.stop, io.cancel) && GetTickCount() - io.start < 15000)
        {
            char* bytes = static_cast<char*>(buffer);
            const int count = writing ? send(io.socket, bytes, int(length), 0)
                                      : recv(io.socket, bytes, int(length), 0);
            if (count > 0)
            {
                (writing ? io.sent : io.received) += count;
                return count;
            }
            if (count == 0)
            {
                io.error = WSAECONNRESET;
                return -1;
            }
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                Sleep(10);
                continue;
            }
            io.error = error;
            return -1;
        }
        if (!Cancelled(io.stop, io.cancel)) io.error = WSAETIMEDOUT;
        return -1;
    }

    int ReadTls(void* context, unsigned char* buffer, size_t length)
    {
        return TransferTls(*static_cast<TlsSocket*>(context), buffer, length, false);
    }

    int WriteTls(void* context, const unsigned char* buffer, size_t length)
    {
        return TransferTls(*static_cast<TlsSocket*>(context), const_cast<unsigned char*>(buffer), length, true);
    }

    struct DatabaseTls
    {
        br_ssl_client_context client;
        br_x509_minimal_context validator;
        unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
    };

    int ReadDatabaseHttps(TlsSocket& socket, const char* request, char* response, int capacity,
                          const char*& failure)
    {
        // ISRG Root YR, the CA above Let's Encrypt YR1 in AccurateRip's chain.
        static unsigned char dn[] =
            "\x30\x2e\x31\x0b\x30\x09\x06\x03\x55\x04\x06\x13\x02" "US"
            "\x31\x0d\x30\x0b\x06\x03\x55\x04\x0a\x13\x04" "ISRG"
            "\x31\x10\x30\x0e\x06\x03\x55\x04\x03\x13\x07" "Root YR";
        static unsigned char exponent[] = {1, 0, 1};
        static const char modulusHex[] =
            "dbc626737bf024c97562f7f9e19fb0b3794eb34126cf951fd8515ea45bc31bbd"
            "b06362074043d5f70ec5b494402248335c44d770dbfb90b0d70d2cd04058b2fb"
            "883ffea3a05d30f1cb8899b811d4e1a061b490e0ea7323e0c6f121ae4e5704"
            "f3bdce092fa4877b2b968eaf978dcce4e260e007a8d6c7c7a7a913243a088850"
            "4d24063e38a7d7fc552f60aba18d3fa7a38f659aa9aea52048e4f901422aab10"
            "6b56539bb153f7105871aef234a3141ce766abdb34f2cc5cc2c5c426f75937eb"
            "2415d78eabd61bebf86e3cf18e3780b4e954eea6ab443bcd3b202e4182e59fdd"
            "3833e7da2b1f219cb0a9275e59a9a12070d958fbd30c59c0b9caf2f60368f7"
            "97dfb66ee820657f9d384f75cd898875ac13bd266e56f95b46b0252f1201ea9c"
            "0c0639de5c28d843cfb70420a3b2c6d2369654de14dc0437617c09440e562bea"
            "911ab6731d9834f906a9ca0bdd9b7741213357db2c78a82dc70b0ae89ebd3f19"
            "90f859aeaecc8a2ee035701c504011f3e97361a97ef734e0ec2e4300bc9539e1"
            "bf0aa7d100c9f52921c247435203a3fcb67fe619196709cb79aed5422e757249"
            "5a54e87d6a1a0dc38133aeb247fc79a85332261b2397b33c998ee5eaed6a5b"
            "24435bada7be3e783cbdd8f2387ed1ca4dbabd216847e1864a8775a724422dfa"
            "842f95865b63cb6912ba0aac507a3c51b3ac9203472b6ce07aafb87e764458f6ceffc1";
        unsigned char modulus[(sizeof(modulusHex) - 1) / 2];
        for (size_t i = 0; i < sizeof(modulus); ++i)
        {
            unsigned value = 0;
            sscanf_s(modulusHex + 2 * i, "%2x", &value);
            modulus[i] = static_cast<unsigned char>(value);
        }
        br_x509_trust_anchor anchor = {};
        anchor.dn.data = dn;
        anchor.dn.len = sizeof(dn) - 1;
        anchor.flags = BR_X509_TA_CA;
        anchor.pkey.key_type = BR_KEYTYPE_RSA;
        anchor.pkey.key.rsa.n = modulus;
        anchor.pkey.key.rsa.nlen = sizeof(modulus);
        anchor.pkey.key.rsa.e = exponent;
        anchor.pkey.key.rsa.elen = sizeof(exponent);

        DatabaseTls* tls = static_cast<DatabaseTls*>(malloc(sizeof(DatabaseTls)));
        failure = "out of memory";
        if (!tls) return 0;
        br_ssl_client_init_full(&tls->client, &tls->validator, &anchor, 1);
        br_ssl_engine_set_buffer(&tls->client.eng, tls->buffer, sizeof(tls->buffer), 1);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const uint64_t seconds = ((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) / 10000000;
        br_x509_minimal_set_time(&tls->validator, uint32_t(seconds / 86400 + 584754),
                                 uint32_t(seconds % 86400));
        unsigned char seed[32] = {};
        const int seedError = XNetRandom(seed, sizeof(seed));
        if (!seedError) br_ssl_engine_inject_entropy(&tls->client.eng, seed, sizeof(seed));
        SecureZeroMemory(seed, sizeof(seed));
        failure = "TLS setup failed";
        int size = 0;
        if (!seedError && br_ssl_client_reset(&tls->client, accurateRipHost, 0))
        {
            br_sslio_context io;
            br_sslio_init(&io, &tls->client.eng, ReadTls, &socket, WriteTls, &socket);
            failure = "TLS handshake failed";
            const int written = br_sslio_write_all(&io, request, strlen(request));
            if (!written) failure = "HTTPS send failed";
            if (!written && !br_sslio_flush(&io))
            {
                failure = "incomplete HTTPS response";
                int expected = 0;
                while (size < capacity)
                {
                    const int count = br_sslio_read(&io, response + size, capacity - size);
                    if (count <= 0) break;
                    size += count;
                    response[size] = '\0';
                    if (!expected)
                    {
                        const char* body = strstr(response, "\r\n\r\n");
                        if (!body) continue;
                        const char* status = strchr(response, ' ');
                        if (!status || strncmp(response, "HTTP/", 5)) break;
                        if (atoi(status + 1) != 200)
                            expected = int(body + 4 - response);
                        else
                        {
                            const char* header = strstr(response, "\r\n") + 2;
                            while (header < body && _strnicmp(header, "Content-Length:", 15))
                                header = strstr(header, "\r\n") + 2;
                            if (header >= body)
                            {
                                failure = "missing HTTPS content length";
                                break;
                            }
                            char* end;
                            const unsigned long length = strtoul(header + 15, &end, 10);
                            const int headerSize = int(body + 4 - response);
                            if (end == header + 15 || *end != '\r' || length > unsigned(capacity - headerSize))
                            {
                                failure = "invalid HTTPS content length";
                                break;
                            }
                            expected = headerSize + int(length);
                        }
                    }
                    if (size >= expected)
                    {
                        free(tls);
                        return expected;
                    }
                }
            }
            if (!socket.error) socket.error = br_ssl_engine_last_error(&tls->client.eng);
            if (socket.error == WSAETIMEDOUT && socket.sent && !socket.received)
                failure = "no TLS reply after ClientHello";
        }
        free(tls);
        return 0;
    }

    int GetDatabase(const char* request, char* response, int capacity, HANDLE stop, HANDLE cancel,
                    const char*& failure, int& socketError)
    {
        XNetStartupParams params = {};
        params.cfgSizeOfStruct = sizeof(params);
        params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        failure = "network startup failed";
        if (XNetStartup(&params)) return 0;
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data))
        {
            XNetCleanup();
            return 0;
        }
        // Give XNet time to acquire its title address before opening the socket.
        XNADDR titleAddress = {};
        const DWORD start = GetTickCount();
        DWORD addressStatus = XNetGetTitleXnAddr(&titleAddress);
        while (addressStatus == XNET_GET_XNADDR_PENDING && GetTickCount() - start < 6000 &&
               WaitForSingleObject(stop, 100) == WAIT_TIMEOUT)
            addressStatus = XNetGetTitleXnAddr(&titleAddress);
        if (addressStatus == XNET_GET_XNADDR_PENDING || Cancelled(stop, cancel))
        {
            failure = "network address not ready";
            WSACleanup();
            XNetCleanup();
            return 0;
        }
        WSAEVENT event = WSACreateEvent();
        XNDNS* dns = NULL;
        SOCKET socketFd = INVALID_SOCKET;
        int size = 0;
        failure = "DNS lookup failed or timed out";
        if (event != WSA_INVALID_EVENT && !XNetDnsLookup(accurateRipHost, event, &dns))
        {
            HANDLE events[] = {event, stop, cancel};
            if (WaitForMultipleObjects(stop == cancel ? 2 : 3, events, FALSE, 5000) == WAIT_OBJECT_0 &&
                !dns->iStatus && dns->cina)
            {
                failure = "server connection failed";
                sockaddr_in address = {};
                address.sin_family = AF_INET;
                address.sin_port = htons(443);
                int plainTcpError = 0;
                for (int i = 0; i < dns->cina && !Cancelled(stop, cancel); ++i)
                {
                    address.sin_addr = dns->aina[i];
                    failure = "socket creation failed";
                    socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    socketError = socketFd == INVALID_SOCKET ? WSAGetLastError() :
                        ConnectDatabase(socketFd, address, stop, failure, plainTcpError);
                    if (!socketError) break;
                    if (!Cancelled(stop, cancel))
                    {
                        char localIp[16] = {};
                        char remoteIp[16] = {};
                        XNetInAddrToString(titleAddress.ina, localIp, sizeof(localIp));
                        XNetInAddrToString(address.sin_addr, remoteIp, sizeof(remoteIp));
                        char message[192];
                        sprintf_s(message, sizeof(message),
                                  "AccurateRip TCP %s -> %s:443: %s (%d), XNet flags=0x%08X, TCP override=%d",
                                  localIp, remoteIp, failure, socketError, unsigned(addressStatus), plainTcpError);
                        LogAccurateRipFailure(message);
                    }
                    if (socketFd != INVALID_SOCKET) closesocket(socketFd);
                    socketFd = INVALID_SOCKET;
                }
                if (socketFd != INVALID_SOCKET && !Cancelled(stop, cancel))
                {
                    TlsSocket io = {socketFd, stop, cancel, GetTickCount(), socketError, 0, 0};
                    size = ReadDatabaseHttps(io, request, response, capacity, failure);
                    if (!size && !Cancelled(stop, cancel))
                    {
                        char message[160];
                        sprintf_s(message, sizeof(message),
                                  "AccurateRip %u.%u.%u.%u:443: %s (%d), TCP override=%d, TLS sent=%u received=%u",
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b1),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b2),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b3),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b4), failure, socketError,
                                  plainTcpError, io.sent, io.received);
                        LogAccurateRipFailure(message);
                    }
                }
            }
        }
        if (socketFd != INVALID_SOCKET) closesocket(socketFd);
        if (dns) XNetDnsRelease(dns);
        if (event != WSA_INVALID_EVENT) WSACloseEvent(event);
        WSACleanup();
        XNetCleanup();
        return size;
    }

    void Verify(const DiscId& id, unsigned tracks, const Checksum* crc, unsigned* confidence,
                const AccurateRipLookup& lookup, char (&summary)[96])
    {
        if (!lookup.size)
        {
            sprintf_s(summary, sizeof(summary), "WAVs saved; %s", lookup.status);
            return;
        }
        const char* body = strstr(lookup.response, "\r\n\r\n") + 4;
        const char* end = lookup.response + lookup.size;
        const unsigned recordSize = 13 + 9 * tracks;
        for (; body < end; body += recordSize)
        {
            const BYTE* record = reinterpret_cast<const BYTE*>(body);
            if (record[0] != tracks || AV_RL32(record + 1) != id.first ||
                AV_RL32(record + 5) != id.second || AV_RL32(record + 9) != id.cddb)
                continue;
            for (unsigned i = 0; i < tracks; ++i)
            {
                const BYTE* entry = record + 13 + 9 * i;
                const uint32_t expected = AV_RL32(entry + 1);
                if ((expected == crc[i].v1 || expected == crc[i].v2) && entry[0] > confidence[i])
                    confidence[i] = entry[0];
            }
        }
        unsigned matched = 0;
        for (unsigned i = 0; i < tracks; ++i) if (confidence[i]) ++matched;
        sprintf_s(summary, sizeof(summary), "WAVs saved; AccurateRip matched %u/%u tracks", matched, tracks);
    }
}

void LookupAccurateRip(const XboxDvdStream::CdAudioProbe& disc, HANDLE stop, AccurateRipLookup& lookup)
{
    if (disc.dataTracks)
    {
        strcpy_s(lookup.status, sizeof(lookup.status), "Mixed-mode AccurateRip lookup unsupported");
        return;
    }
    const DiscId id = Identify(disc);
    const unsigned tracks = disc.audioTracks;
    char request[256];
    sprintf_s(request, sizeof(request),
              "GET /accuraterip/%x/%x/%x/dBAR-%03u-%08x-%08x-%08x.bin HTTP/1.0\r\n"
              "Host: www.accuraterip.com\r\nConnection: close\r\n\r\n",
              id.first & 15, (id.first >> 4) & 15, (id.first >> 8) & 15,
              tracks, id.first, id.second, id.cddb);
    const int capacity = 256 * 1024;
    char* response = static_cast<char*>(malloc(capacity + 1));
    const char* failure = "out of memory";
    int socketError = 0;
    const int size = response ? GetDatabase(request, response, capacity, stop, stop, failure, socketError) : 0;
    if (response) response[size] = '\0';
    const char* body = size ? strstr(response, "\r\n\r\n") : NULL;
    const char* code = size ? strchr(response, ' ') : NULL;
    const int httpStatus = code && !strncmp(response, "HTTP/", 5) ? atoi(code + 1) : 0;
    const unsigned recordSize = 13 + 9 * tracks;
    if (body) body += 4;
    if (httpStatus == 200 && body && body < response + size &&
        (response + size - body) % recordSize == 0)
    {
        lookup.response = response;
        lookup.size = size;
        strcpy_s(lookup.status, sizeof(lookup.status), "AccurateRip database record downloaded");
        return;
    }
    if (httpStatus == 404)
        strcpy_s(lookup.status, sizeof(lookup.status), "Disc not in AccurateRip");
    else if (httpStatus && httpStatus != 200)
        sprintf_s(lookup.status, sizeof(lookup.status), "AccurateRip HTTP %d", httpStatus);
    else if (socketError)
        sprintf_s(lookup.status, sizeof(lookup.status), "AccurateRip: %s (%d)", failure, socketError);
    else
        sprintf_s(lookup.status, sizeof(lookup.status), "AccurateRip: %s",
                  size ? "invalid database response" : failure);
    free(response);
}

HRESULT RipAudioCd(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                   HANDLE stop, HANDLE cancel, HANDLE lookupDone, const AccurateRipLookup& lookup,
                   volatile LONG& progress, char (&summary)[96])
{
    strcpy_s(summary, sizeof(summary), "CD rip failed: HDD unavailable");
    RipDrive drive;
    if (!drive.Mount()) return E_FAIL;
    const char ripRoot[] = "DvdRip:\\360DVD-Rips";
    if (!CreateDirectory(ripRoot, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return E_FAIL;
    int offset = 0;
    FILE* config = fopen("DvdRip:\\360DVD-Rips\\drive-offset.txt", "r");
    if (config)
    {
        char text[32] = {};
        const bool read = fgets(text, sizeof(text), config) != NULL;
        fclose(config);
        char* end;
        const long value = strtol(text, &end, 10);
        if (!read || end == text || value < -long(edgeFrames) || value > long(edgeFrames) ||
            end[strspn(end, " \t\r\n")])
        {
            strcpy_s(summary, sizeof(summary), "CD rip failed: invalid drive-offset.txt");
            return E_INVALIDARG;
        }
        offset = int(value);
    }

    const DiscId id = Identify(disc);
    char directory[MAX_PATH];
    for (unsigned attempt = 1;; ++attempt)
    {
        sprintf_s(directory, sizeof(directory), "%s\\%08x-%08x-%08x-%u",
                  ripRoot, id.first, id.second, id.cddb, attempt);
        if (CreateDirectory(directory, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS || attempt == 999) return E_FAIL;
    }

    Checksum crc[99] = {};
    unsigned confidence[99] = {};
    HRESULT status = S_OK;
    unsigned completed = 0;
    unsigned framesCompleted = 0;
    unsigned discFrames = 0;
    for (unsigned i = 0; i < disc.audioTracks; ++i)
        discFrames += (disc.tracks[i].end - disc.tracks[i].start) * framesPerSector;
    for (; completed < disc.audioTracks && status == S_OK;)
    {
        char path[MAX_PATH];
        sprintf_s(path, sizeof(path), "%s\\Track %02u.wav", directory, disc.tracks[completed].number);
        status = RipTrack(stream, disc, completed, offset, stop, cancel, path, crc[completed],
                          framesCompleted, discFrames, progress);
        if (status == S_OK)
        {
            framesCompleted += (disc.tracks[completed].end - disc.tracks[completed].start) * framesPerSector;
            ++completed;
        }
    }
    if (status == S_OK)
    {
        if (disc.dataTracks)
            strcpy_s(summary, sizeof(summary), "WAVs saved; mixed-mode AccurateRip lookup unsupported");
        else if (lookupDone)
        {
            PublishProgress(progress, CdRipVerifying);
            HANDLE events[] = {stop, cancel, lookupDone};
            if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) == WAIT_OBJECT_0 + 2)
                Verify(id, disc.audioTracks, crc, confidence, lookup, summary);
            else
                strcpy_s(summary, sizeof(summary), "WAVs saved; AccurateRip lookup unavailable");
        }
        else
            strcpy_s(summary, sizeof(summary), "WAVs saved; AccurateRip lookup unavailable");
        if (Cancelled(stop, cancel))
            strcpy_s(summary, sizeof(summary), "WAVs saved; AccurateRip lookup cancelled");
    }
    else
    {
        strcpy_s(summary, sizeof(summary), status == S_FALSE ? "CD rip cancelled" : "CD rip failed: read or write error");
        if (!completed) RemoveDirectory(directory);
    }
    if (completed)
    {
        char path[MAX_PATH];
        sprintf_s(path, sizeof(path), "%s\\rip.txt", directory);
        FILE* report = fopen(path, "w");
        if (report)
        {
            fprintf(report, "%s\nDrive sample offset: %+d\n", summary, offset);
            for (unsigned i = 0; i < completed; ++i)
                fprintf(report, "Track %02u: ARv1 %08x ARv2 %08x confidence %u\n",
                        disc.tracks[i].number, crc[i].v1, crc[i].v2, confidence[i]);
            fclose(report);
        }
    }
    return status;
}
