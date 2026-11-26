#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

    int ConnectDatabase(SOCKET socketFd, const sockaddr_in& address, HANDLE stop, HANDLE cancel,
                        const char*& failure)
    {
        // XexUtils uses this Xbox-specific option for ordinary Internet TCP.
        const int xboxPlainTcp = 0x5801;
        const BOOL enabled = TRUE;
        if (setsockopt(socketFd, SOL_SOCKET, xboxPlainTcp,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSAEACCES)
            {
                failure = "plain TCP option failed";
                return error;
            }
            // Some kernels reject the optional override. Try their default
            // transport settings and report whether connect itself succeeds.
        }

        u_long nonblocking = 1;
        failure = "nonblocking setup failed";
        if (ioctlsocket(socketFd, FIONBIO, &nonblocking)) return WSAGetLastError();
        failure = "TCP connect failed";
        if (!connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) return 0;
        const int pending = WSAGetLastError();
        if (pending != WSAEWOULDBLOCK && pending != WSAEINPROGRESS) return pending;

        const DWORD start = GetTickCount();
        while (GetTickCount() - start < 8000 && !Cancelled(stop, cancel))
        {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(socketFd, &writable);
            FD_SET(socketFd, &failed);
            timeval timeout = {0, 250000};
            const int ready = select(0, NULL, &writable, &failed, &timeout);
            if (ready == SOCKET_ERROR)
            {
                failure = "TCP select failed";
                return WSAGetLastError();
            }
            if (failed.fd_count)
            {
                // Xbox Winsock does not expose SO_ERROR.
                return WSAECONNREFUSED;
            }
            if (writable.fd_count) return 0;
        }
        return WSAETIMEDOUT;
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
        WSAEVENT event = WSACreateEvent();
        XNDNS* dns = NULL;
        SOCKET socketFd = INVALID_SOCKET;
        int size = 0;
        failure = "DNS lookup failed or timed out";
        if (event != WSA_INVALID_EVENT && !XNetDnsLookup("www.accuraterip.com", event, &dns))
        {
            HANDLE events[] = {event, stop, cancel};
            if (WaitForMultipleObjects(stop == cancel ? 2 : 3, events, FALSE, 5000) == WAIT_OBJECT_0 &&
                !dns->iStatus && dns->cina)
            {
                failure = "server connection failed";
                sockaddr_in address = {};
                address.sin_family = AF_INET;
                address.sin_port = htons(80);
                for (int i = 0; i < dns->cina && !Cancelled(stop, cancel); ++i)
                {
                    address.sin_addr = dns->aina[i];
                    failure = "socket creation failed";
                    socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    socketError = socketFd == INVALID_SOCKET ? WSAGetLastError() :
                        ConnectDatabase(socketFd, address, stop, cancel, failure);
                    if (!socketError) break;
                    if (socketFd != INVALID_SOCKET) closesocket(socketFd);
                    socketFd = INVALID_SOCKET;
                }
                if (socketFd != INVALID_SOCKET && !Cancelled(stop, cancel) && !request)
                    size = 1;
                if (socketFd != INVALID_SOCKET && !Cancelled(stop, cancel) && request)
                {
                    const int requestSize = int(strlen(request));
                    int sent = 0;
                    bool complete = false;
                    failure = "HTTP send failed";
                    const DWORD start = GetTickCount();
                    while (size < capacity && GetTickCount() - start < 5000 && !Cancelled(stop, cancel))
                    {
                        const bool sending = sent < requestSize;
                        if (!sending) failure = "HTTP receive failed";
                        fd_set ready;
                        FD_ZERO(&ready);
                        FD_SET(socketFd, &ready);
                        timeval timeout = {0, 250000};
                        const int selected = select(0, sending ? NULL : &ready,
                                                    sending ? &ready : NULL, NULL, &timeout);
                        if (selected == 0) continue;
                        if (selected == SOCKET_ERROR)
                        {
                            socketError = WSAGetLastError();
                            break;
                        }
                        const int count = sending ? send(socketFd, request + sent, requestSize - sent, 0)
                                                  : recv(socketFd, response + size, capacity - size, 0);
                        if (count == SOCKET_ERROR)
                        {
                            const int error = WSAGetLastError();
                            if (error == WSAEWOULDBLOCK) continue;
                            socketError = error;
                            break;
                        }
                        if (count == 0)
                        {
                            complete = !sending && size > 0;
                            break;
                        }
                        if (sending) sent += count;
                        else size += count;
                    }
                    if (!complete)
                    {
                        if (!socketError && GetTickCount() - start >= 5000) socketError = WSAETIMEDOUT;
                        size = 0;
                    }
                }
            }
        }
        if (socketFd != INVALID_SOCKET) closesocket(socketFd);
        if (dns) XNetDnsRelease(dns);
        if (event != WSA_INVALID_EVENT) WSACloseEvent(event);
        WSACleanup();
        XNetCleanup();
        return size < capacity ? size : 0;
    }

    void Verify(const DiscId& id, unsigned tracks, const Checksum* crc,
                unsigned* confidence, HANDLE stop, HANDLE cancel, char (&summary)[96])
    {
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
        const int size = response ? GetDatabase(request, response, capacity, stop, cancel, failure, socketError) : 0;
        if (response) response[size] = '\0';
        const char* body = size ? strstr(response, "\r\n\r\n") : NULL;
        const char* code = size ? strchr(response, ' ') : NULL;
        const unsigned recordSize = 13 + 9 * tracks;
        if (body) body += 4;
        if (!body || !code || strncmp(code, " 200 ", 5) ||
            (response + size - body) % recordSize || body == response + size)
        {
            const int httpStatus = code && !strncmp(response, "HTTP/", 5) ? atoi(code + 1) : 0;
            if (httpStatus == 404)
                strcpy_s(summary, sizeof(summary), "WAVs saved; disc not in AccurateRip");
            else if (httpStatus && httpStatus != 200)
                sprintf_s(summary, sizeof(summary), "WAVs saved; AccurateRip HTTP %d", httpStatus);
            else if (socketError)
                sprintf_s(summary, sizeof(summary), "WAVs saved; AccurateRip: %s (%d)", failure, socketError);
            else
                sprintf_s(summary, sizeof(summary), "WAVs saved; AccurateRip: %s",
                          size ? "invalid database response" : failure);
        }
        else
        {
            for (; body < response + size; body += recordSize)
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
        free(response);
    }
}

void ProbeAccurateRipConnection(HANDLE stop, char (&summary)[96])
{
    const char* failure = "network startup failed";
    int socketError = 0;
    if (GetDatabase(NULL, NULL, 2, stop, stop, failure, socketError))
        strcpy_s(summary, sizeof(summary), "AccurateRip TCP connected   Y: Rip Disc");
    else if (socketError)
        sprintf_s(summary, sizeof(summary), "AccurateRip: %s (%d)   Y: Rip", failure, socketError);
    else
        sprintf_s(summary, sizeof(summary), "AccurateRip: %s   Y: Rip", failure);
}

HRESULT RipAudioCd(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                   HANDLE stop, HANDLE cancel, volatile LONG& progress, char (&summary)[96])
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
        else
        {
            PublishProgress(progress, CdRipVerifying);
            Verify(id, disc.audioTracks, crc, confidence, stop, cancel, summary);
        }
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
