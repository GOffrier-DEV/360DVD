#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <bearssl.h>
#include <string>
#include <vector>
#include "CdRip.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/base64.h>
#include <libavutil/sha.h>
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
    const unsigned offsetSearchSectors = 10;
    const unsigned offsetSearchFrames = offsetSearchSectors * framesPerSector;
    const char offsetTablePath[] = "game:\\drive-offsets.json";
    const char defaultOffsetTable[] =
        "{\r\n"
        "  \"PLDS / DG-16D4S / 0401\": 6,\r\n"
        "  \"PBDS / VAD6038-64930C / *\": 6\r\n"
        "}\r\n";

    bool ReadCdOffset(const CdDriveInfo& drive, int& offset)
    {
        if (!drive.vendor[0] || !drive.model[0]) return false;
        FILE* file = fopen(offsetTablePath, "rb");
        char text[8192];
        size_t size = sizeof(defaultOffsetTable) - 1;
        if (file)
        {
            size = fread(text, 1, sizeof(text) - 1, file);
            const bool complete = feof(file) && !ferror(file);
            fclose(file);
            if (!complete || memchr(text, 0, size)) return false;
            text[size] = '\0';
        }
        else
            memcpy(text, defaultOffsetTable, sizeof(defaultOffsetTable));
        char exact[40], wildcard[40];
        sprintf_s(exact, sizeof(exact), "%s / %s / %s", drive.vendor, drive.model, drive.revision);
        sprintf_s(wildcard, sizeof(wildcard), "%s / %s / *", drive.vendor, drive.model);
        char* cursor = text;
        if (size >= 3 && !memcmp(cursor, "\xef\xbb\xbf", 3)) cursor += 3;
        cursor += strspn(cursor, " \t\r\n");
        if (*cursor++ != '{') return false;
        cursor += strspn(cursor, " \t\r\n");
        if (*cursor == '}') return false;
        int priority = 0, selected = 0;
        for (;;)
        {
            if (*cursor++ != '"') return false;
            char* key = cursor;
            cursor += strcspn(cursor, "\"\\\r\n\t");
            if (*cursor != '"') return false;
            *cursor++ = '\0';
            cursor += strspn(cursor, " \t\r\n");
            if (*cursor++ != ':') return false;
            cursor += strspn(cursor, " \t\r\n");
            const char* digits = *cursor == '-' ? cursor + 1 : cursor;
            if (*digits < '0' || *digits > '9' ||
                (*digits == '0' && digits[1] >= '0' && digits[1] <= '9'))
                return false;
            char* end;
            const long value = strtol(cursor, &end, 10);
            if (value < -long(offsetSearchFrames) || value > long(offsetSearchFrames)) return false;
            const int rank = !_stricmp(key, exact) ? 2 : !_stricmp(key, wildcard) ? 1 : 0;
            if (rank && rank == priority) return false;
            if (rank > priority)
            {
                priority = rank;
                selected = int(value);
            }
            cursor = end + strspn(end, " \t\r\n");
            if (*cursor == '}') break;
            if (*cursor++ != ',') return false;
            cursor += strspn(cursor, " \t\r\n");
        }
        cursor += 1 + strspn(cursor + 1, " \t\r\n");
        if (*cursor || !priority) return false;
        offset = selected;
        return true;
    }
    // Raw reads allow up to 55 sectors. Leave one for a shifted read window.
    const unsigned batch = 48;
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

    void ReadTrackIsrc(XboxDvdStream& drive, unsigned track, char (&isrc)[13])
    {
        const BYTE cdb[] = {0x42, 0, 0x40, 3, 0, 0, BYTE(track), 0, 24, 0};
        __declspec(align(16)) BYTE response[24] = {};
        BYTE sense[18];
        unsigned bytes;
        if (drive.ReadOpticalInfo(cdb, sizeof(cdb), response, sizeof(response), bytes, sense) != S_OK ||
            bytes < 24 || AV_RB16(response + 2) < 20 ||
            response[4] != 3 || response[6] != track || !(response[8] & 0x80))
            return;
        for (unsigned i = 0; i < 12; ++i)
        {
            const BYTE value = response[9 + i];
            const bool letter = value >= 'A' && value <= 'Z';
            const bool digit = value >= '0' && value <= '9';
            if (!(i < 2 ? letter : i < 5 ? letter || digit : digit)) return;
        }
        memcpy(isrc, response + 9, 12);
    }

    void AddId3Text(std::vector<BYTE>& tag, const char (&frame)[5], const char* text,
                    const char* description = NULL)
    {
        if (!text[0]) return;
        WCHAR wide[257];
        const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, ARRAYSIZE(wide));
        if (count <= 1) return;
        const unsigned prefix = description ? unsigned(strlen(description)) * 2 + 4 : 0;
        const unsigned length = 3 + prefix + (count - 1) * 2;
        const size_t start = tag.size();
        tag.resize(start + 10 + length);
        BYTE* output = tag.data() + start;
        memcpy(output, frame, 4);
        AV_WB32(output + 4, length);
        output[10] = 1;
        output[11] = 0xff;
        output[12] = 0xfe;
        BYTE* value = output + 13;
        if (description)
        {
            while (*description)
            {
                AV_WL16(value, BYTE(*description++));
                value += 2;
            }
            AV_WL16(value, 0);
            AV_WL16(value + 2, 0xfeff);
            value += 4;
        }
        for (int i = 0; i < count - 1; ++i)
            AV_WL16(value + i * 2, wide[i]);
    }

    bool AppendId3Tag(AVIOContext* io, const CdMetadata& metadata, unsigned track, unsigned total,
                      const char* isrc)
    {
        const CdTrackMetadata& entry = metadata.tracks[track];
        std::vector<BYTE> frames;
        AddId3Text(frames, "TIT2", entry.title);
        AddId3Text(frames, "TPE1", entry.artist);
        AddId3Text(frames, "TPE2", metadata.artist);
        AddId3Text(frames, "TALB", metadata.album);
        AddId3Text(frames, "TPUB", metadata.label);
        AddId3Text(frames, "TXXX", metadata.catalog, "CATALOGNUMBER");
        AddId3Text(frames, "TXXX", metadata.date, "RELEASEDATE");
        char number[20];
        if (metadata.album[0])
        {
            sprintf_s(number, sizeof(number), "%u/%u", track + 1, total);
            AddId3Text(frames, "TRCK", number);
            sprintf_s(number, sizeof(number), "%u", metadata.discNumber);
            AddId3Text(frames, "TPOS", number);
        }
        if (strlen(metadata.date) >= 4)
        {
            char year[5] = {};
            memcpy(year, metadata.date, 4);
            AddId3Text(frames, "TYER", year);
        }
        AddId3Text(frames, "TSRC", isrc);
        if (metadata.coverSize)
        {
            const char* mime = metadata.coverPng ? "image/png" : "image/jpeg";
            const unsigned mimeSize = unsigned(strlen(mime)) + 1;
            const unsigned length = 1 + mimeSize + 2 + metadata.coverSize;
            const size_t start = frames.size();
            frames.resize(start + 10 + length);
            BYTE* output = frames.data() + start;
            memcpy(output, "APIC", 4);
            AV_WB32(output + 4, length);
            memcpy(output + 11, mime, mimeSize);
            output[11 + mimeSize] = 3; // Front cover; empty description follows.
            memcpy(output + 13 + mimeSize, metadata.cover, metadata.coverSize);
        }
        if (frames.empty()) return true;
        const int64_t end = avio_size(io);
        const unsigned frameBytes = unsigned(frames.size());
        const unsigned tagSize = 10 + frameBytes;
        if (end < 8 || end > INT32_MAX - int64_t(tagSize) - 9 || avio_seek(io, end, SEEK_SET) != end)
            return false;
        const BYTE header[] = {'I', 'D', '3', 3, 0, 0, BYTE((frameBytes >> 21) & 127),
                               BYTE((frameBytes >> 14) & 127), BYTE((frameBytes >> 7) & 127),
                               BYTE(frameBytes & 127)};
        avio_write(io, reinterpret_cast<const unsigned char*>("ID3 "), 4);
        avio_wl32(io, tagSize);
        avio_write(io, header, sizeof(header));
        avio_write(io, frames.data(), frameBytes);
        if (tagSize & 1) avio_w8(io, 0);
        const int64_t taggedEnd = avio_tell(io);
        if (avio_seek(io, 4, SEEK_SET) != 4) return false;
        avio_wl32(io, uint32_t(taggedEnd - 8));
        avio_flush(io);
        return io->error >= 0;
    }

    HRESULT RipTrack(XboxDvdStream& drive, const XboxDvdStream::CdAudioProbe& disc,
                     unsigned track, int offset, HANDLE stop, HANDLE cancel,
                     const char* path, Checksum& crc, unsigned framesCompleted,
                     unsigned discFrames, volatile LONG& progress, const CdMetadata& metadata)
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
                av_dict_set(&wav->metadata, "title", metadata.tracks[track].title, 0);
                av_dict_set(&wav->metadata, "artist", metadata.tracks[track].artist, 0);
                av_dict_set(&wav->metadata, "album", metadata.album, 0);
                av_dict_set(&wav->metadata, "date", metadata.date, 0);
                result = avformat_write_header(wav, NULL);
            }
        }

        const auto& range = disc.tracks[track];
        char isrc[13] = {};
        if (result >= 0 && !Cancelled(stop, cancel))
            ReadTrackIsrc(drive, range.number, isrc);
        if (!isrc[0]) strcpy_s(isrc, sizeof(isrc), metadata.tracks[track].isrc);
        const unsigned length = (range.end - range.start) * framesPerSector;
        const unsigned first = track == 0 ? edgeFrames - 1 : 0;
        const unsigned end = track + 1 == disc.audioTracks
            ? (length > edgeFrames ? length - edgeFrames : 0) : length;
        const int discStart = int((disc.dataTracks ? range.start : disc.tracks[0].start) * framesPerSector);
        const int discEnd = int((disc.dataTracks ? range.end : disc.tracks[disc.audioTracks - 1].end) * framesPerSector);
        BYTE* raw = static_cast<BYTE*>(av_malloc((batch + 1) * 2352));
        BYTE* outputBuffer = static_cast<BYTE*>(av_malloc(batch * 2352 + AV_INPUT_BUFFER_PADDING_SIZE));
        AVPacket* packet = av_packet_alloc();
        HRESULT status = result < 0 ? E_FAIL : !raw || !outputBuffer || !packet ? E_OUTOFMEMORY : S_OK;
        DWORD started = GetTickCount();
        unsigned batchFrames = batch * framesPerSector;
        for (unsigned pos = 0; pos < length && status == S_OK;)
        {
            if (Cancelled(stop, cancel)) { status = S_FALSE; break; }
            // Publish one atomic snapshot for the UI, which runs on another thread.
            const unsigned trackPercent = unsigned(uint64_t(pos) * 100 / length);
            const unsigned discPercent = unsigned(uint64_t(framesCompleted + pos) * 100 / discFrames);
            PublishProgress(progress, (range.number << 16) | (trackPercent << 8) | discPercent);
            const int source = int(range.start * framesPerSector + pos) + offset;
            const unsigned count = min(length - pos, batchFrames);
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
                if (status != S_OK)
                {
                    // Retain the known smaller request size on drives that reject larger reads.
                    if (batchFrames > 12 * framesPerSector && !Cancelled(stop, cancel))
                    {
                        batchFrames = 12 * framesPerSector;
                        status = S_OK;
                        continue;
                    }
                    break;
                }
            }
            packet->data = outputBuffer;
            packet->size = count * 4;
            memset(outputBuffer + packet->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
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
        if (status == S_OK && (av_write_trailer(wav) < 0 || io->error < 0 ||
                              !AppendId3Tag(io, metadata, track, disc.audioTracks, isrc) ||
                              !FlushFileBuffers(file)))
            status = E_FAIL;
        av_packet_free(&packet);
        av_free(outputBuffer);
        av_free(raw);
        avformat_free_context(wav);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
        CloseHandle(file);
        if (status == S_OK && !MoveFile(temporary, path))
            status = HRESULT_FROM_WIN32(GetLastError());
        if (status == S_OK)
        {
            char line[144];
            sprintf_s(line, sizeof(line), "CD rip track %02u: %u sectors in %u ms, batch=%u sectors, ISRC=%s",
                      range.number, length / framesPerSector, unsigned(GetTickCount() - started),
                      batchFrames / framesPerSector, isrc[0] ? isrc : "unavailable");
            LogCdDiagnostic(line);
        }
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
    const char musicBrainzHost[] = "musicbrainz.org";
    const char coverArtHost[] = "coverartarchive.org";

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

    int ReadDatabaseHttps(TlsSocket& socket, const char* host, const char* request, char* response, int capacity,
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
        br_x509_trust_anchor anchors[2] = {};
        br_x509_trust_anchor& anchor = anchors[0];
        unsigned anchorCount = 1;
        anchor.dn.data = dn;
        anchor.dn.len = sizeof(dn) - 1;
        anchor.flags = BR_X509_TA_CA;
        anchor.pkey.key_type = BR_KEYTYPE_RSA;
        anchor.pkey.key.rsa.n = modulus;
        anchor.pkey.key.rsa.nlen = sizeof(modulus);
        anchor.pkey.key.rsa.e = exponent;
        anchor.pkey.key.rsa.elen = sizeof(exponent);
        unsigned char serviceDn[140], serviceKey[526];
        const char* encodedDn = NULL;
        const char* encodedKey = NULL;
        if (!strcmp(host, musicBrainzHost))
        {
            // DigiCert Global Root G2, the trust anchor for MusicBrainz.
            encodedDn = "MGExCzAJBgNVBAYTAlVTMRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcy";
            encodedKey = "MIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQAB";
        }
        else if (!strcmp(host, coverArtHost))
        {
            // ISRG Root X1, above the cross-signed X2 in the Cover Art Archive chain.
            encodedDn = "ME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBTZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgx";
            encodedKey = "MIICCgKCAgEAregkc/QUN/ObnitXKByHvty33ziQjG485legePd1wqL+9Wpu9gBPKNveaIZsRJO2sWP9FBJrvx/S6jGbIX7RMzy6SPXded+zuP8S8SGaS8GKhnFpSmZmbI9+PHC/rSkiBvPkwOaAruJLj7eZfpQDn9NHl3yZSCNT6DiuTwpvgy7RSVeMgHS22i/QOI17A3AhG3XyMDz6j67d2mOr6xZPwo4RS37PC+j/tXcu9LJ7SuBMEiUMcI0DKaDhUyTsE9nuGb8Qs0qMP4mjYVHerIcHlPRjcewu4m9bmIHhiVw0eWx27zuQYnnm26SaLybF0BDhDt7ZEI4W+7f3qPfH5QIHmI82CJXn4jeWDTZ1nvsOcrEdm7wD+UkF2IHdBbQq1kHprAF2lQoP2N/VvRIfNS8oF2zSmMGoCWR3bkc3us6sWV5onX9y1onFBkEpPlk+3Sb1JMkRp1qjTEAfRqGZtac6UW6GO559cqcSBXhZ7T5ReBULA4+N0C8Fsj57ShxLcwUS/Mbq4FATfEOTdLPKdOeOHwEI0DDUW3E2tAe6wTAwXEi3gjuYpn1giqKjKYLMur2DBBuigwNBodYF8RvCtvCofIY7RqhIKojcdpp2vx9qpT0Zj+s482TeyCsNCij/99viFULUItAnXeF5/hjncIitTubZizrG3SdRbv+8ZPUzQ08CAwEAAQ==";
        }
        else if (strcmp(host, accurateRipHost))
        {
            // Go Daddy Root G2 for the archive.org image redirects.
            encodedDn = "MIGDMQswCQYDVQQGEwJVUzEQMA4GA1UECBMHQXJpem9uYTETMBEGA1UEBxMKU2NvdHRzZGFsZTEaMBgGA1UEChMRR29EYWRkeS5jb20sIEluYy4xMTAvBgNVBAMTKEdvIERhZGR5IFJvb3QgQ2VydGlmaWNhdGUgQXV0aG9yaXR5IC0gRzI=";
            encodedKey = "MIIBCgKCAQEAv3FiCPH6WTT3G8kYo/eASVjpIoMTpsUgQwE7hPHmhUmfJ+r2hBtOoLTbcJjHMgGxBT4HTu70+k8vWTAi56sZVmvigAf88xZ1gDlRe+X5NbZ0TqmNghPktj+pA4P6or6KFWp/3gvDthkUBcrqw6gElDtGfDIN8wBmIsiNaW02jBEYt9OyHGC0OPoCjM7T3UYH3go+6118yHz7sCtTpJJiaVElBWEaRIGMLKlDliPfrDqBmg4pxRyp6V0etp6eMAo5zvGIgPtLXcwy7IViQyU0AlYnAZG0O3AqP26x6JyIAX2f1PnbU21gnb8s51iruF9G/M7EGwM8CetJMVxpRrPgRwIDAQAB";
        }
        if (encodedDn)
        {
            const int dnSize = av_base64_decode(serviceDn, encodedDn, sizeof(serviceDn));
            const int keySize = av_base64_decode(serviceKey, encodedKey, sizeof(serviceKey));
            if (dnSize <= 0 || (keySize != 270 && keySize != 526)) return 0;
            anchor.dn.data = serviceDn;
            anchor.dn.len = dnSize;
            anchor.pkey.key.rsa.n = serviceKey + 9;
            anchor.pkey.key.rsa.nlen = keySize - 14;
        }

        unsigned char x2Dn[81], x2Key[97];
        if (strcmp(host, accurateRipHost) && strcmp(host, musicBrainzHost))
        {
            // Some Internet Archive image servers use Let's Encrypt rather than Go Daddy.
            const int dnSize = av_base64_decode(x2Dn,
                "ME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBTZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgy",
                sizeof(x2Dn));
            const int keySize = av_base64_decode(x2Key,
                "BM2b1Z+AgwrsCUrzFko+XM93rN5nBQ0dB7bcFvtaixTb4nFgxLpFlRGJjuoG3/cqFhykucXFMuAD4B6CGDiL10XYCmpu5gB3+wJRfSLYCm6aW3ff8PpB7DncdcpoBwwf6g==",
                sizeof(x2Key));
            if (dnSize != sizeof(x2Dn) || keySize != sizeof(x2Key)) return 0;
            br_x509_trust_anchor& x2 = anchors[anchorCount++];
            x2.dn.data = x2Dn;
            x2.dn.len = dnSize;
            x2.flags = BR_X509_TA_CA;
            x2.pkey.key_type = BR_KEYTYPE_EC;
            x2.pkey.key.ec.curve = BR_EC_secp384r1;
            x2.pkey.key.ec.q = x2Key;
            x2.pkey.key.ec.qlen = keySize;
        }
        DatabaseTls* tls = static_cast<DatabaseTls*>(malloc(sizeof(DatabaseTls)));
        failure = "out of memory";
        if (!tls) return 0;
        br_ssl_client_init_full(&tls->client, &tls->validator, anchors, anchorCount);
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
        if (!seedError && br_ssl_client_reset(&tls->client, host, 0))
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

    int GetDatabase(const char* host, const char* request, char* response, int capacity, HANDLE stop, HANDLE cancel,
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
        if (event != WSA_INVALID_EVENT && !XNetDnsLookup(host, event, &dns))
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
                                  "%s TCP %s -> %s:443: %s (%d), XNet flags=0x%08X, TCP override=%d",
                                  host, localIp, remoteIp, failure, socketError, unsigned(addressStatus), plainTcpError);
                        LogCdDiagnostic(message);
                    }
                    if (socketFd != INVALID_SOCKET) closesocket(socketFd);
                    socketFd = INVALID_SOCKET;
                }
                if (socketFd != INVALID_SOCKET && !Cancelled(stop, cancel))
                {
                    TlsSocket io = {socketFd, stop, cancel, GetTickCount(), socketError, 0, 0};
                    size = ReadDatabaseHttps(io, host, request, response, capacity, failure);
                    if (!size && !Cancelled(stop, cancel))
                    {
                        char message[192];
                        sprintf_s(message, sizeof(message),
                                  "%s %u.%u.%u.%u:443: %s (%d), TCP override=%d, TLS sent=%u received=%u",
                                  host, unsigned(address.sin_addr.S_un.S_un_b.s_b1),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b2),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b3),
                                  unsigned(address.sin_addr.S_un.S_un_b.s_b4), failure, socketError,
                                  plainTcpError, io.sent, io.received);
                        LogCdDiagnostic(message);
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

    void CopyInquiryField(char* out, const BYTE* data, unsigned size)
    {
        while (size && (data[size - 1] == ' ' || !data[size - 1])) --size;
        for (unsigned i = 0; i < size; ++i)
            out[i] = data[i] >= 32 && data[i] <= 126 ? char(data[i]) : '?';
        out[size] = '\0';
    }

    bool CalibrateOffset(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                         const AccurateRipLookup& lookup, HANDLE stop, HANDLE cancel, int& offset)
    {
        if (!lookup.size || disc.audioTracks < 2) return false;
        const unsigned candidates = 2 * offsetSearchFrames + 1;
        const unsigned maxTracks = 8;
        const unsigned probeSectors = 2 * offsetSearchSectors + 1;
        unsigned tracks[maxTracks];
        uint32_t (*checksums)[candidates] =
            static_cast<uint32_t (*)[candidates]>(malloc(maxTracks * candidates * sizeof(uint32_t)));
        if (!checksums) return false;
        unsigned used = 0;
        BYTE* pcm = static_cast<BYTE*>(av_malloc(probeSectors * 2352));
        if (!pcm)
        {
            free(checksums);
            return false;
        }
        for (unsigned track = 0; track < disc.audioTracks && used < maxTracks; ++track)
        {
            if (Cancelled(stop, cancel)) break;
            const auto& range = disc.tracks[track];
            if (range.end - range.start < 450 + offsetSearchSectors + 1) continue;
            unsigned bytes;
            const HRESULT status = stream.ReadCdAudio(range.start + 450 - offsetSearchSectors,
                                                      probeSectors, pcm, bytes);
            char line[96];
            sprintf_s(line, sizeof(line), "AccurateRip frame450 probe: track %02u, bytes=%u",
                      range.number, bytes);
            LogCdDiagnostic(line, status);
            if (status == S_OK)
            {
                const unsigned frames = framesPerSector;
                uint32_t sum = 0, weighted = 0;
                for (unsigned i = 0; i < frames; ++i)
                {
                    const uint32_t sample = AV_RL32(pcm + i * 4);
                    sum += sample;
                    weighted += sample * (i + 1);
                }
                checksums[used][0] = weighted;
                for (unsigned i = 0; i + 1 < candidates; ++i)
                {
                    const uint32_t incoming = AV_RL32(pcm + (i + frames) * 4);
                    weighted += incoming * frames - sum;
                    sum += incoming - AV_RL32(pcm + i * 4);
                    checksums[used][i + 1] = weighted;
                }
                tracks[used++] = track;
            }
        }
        av_free(pcm);
        const DiscId id = Identify(disc);
        const unsigned recordSize = 13 + 9 * disc.audioTracks;
        const char* body = strstr(lookup.response, "\r\n\r\n") + 4;
        int match = -1;
        bool ambiguous = false;
        for (; used >= 2 && body < lookup.response + lookup.size && !ambiguous &&
               !Cancelled(stop, cancel); body += recordSize)
        {
            const BYTE* record = reinterpret_cast<const BYTE*>(body);
            if (record[0] != disc.audioTracks || AV_RL32(record + 1) != id.first ||
                AV_RL32(record + 5) != id.second || AV_RL32(record + 9) != id.cddb)
                continue;
            for (unsigned candidate = 0; candidate < candidates; ++candidate)
            {
                unsigned matches = 0;
                for (unsigned t = 0; t < used; ++t)
                {
                    const BYTE* entry = record + 13 + 9 * tracks[t];
                    if (entry[0] && checksums[t][candidate] && AV_RL32(entry + 5) == checksums[t][candidate])
                        ++matches;
                }
                if (matches < 2) continue;
                char line[128];
                sprintf_s(line, sizeof(line), "AccurateRip frame450 candidate: %+d samples, %u tracks",
                          int(candidate) - int(offsetSearchFrames), matches);
                LogCdDiagnostic(line);
                if (match >= 0 && match != int(candidate))
                {
                    ambiguous = true;
                    break;
                }
                match = int(candidate);
            }
        }
        free(checksums);
        if (match < 0 || ambiguous || Cancelled(stop, cancel))
        {
            LogCdDiagnostic(ambiguous ? "AccurateRip frame450: ambiguous correction" :
                            "AccurateRip frame450: no unique multi-track correction");
            return false;
        }
        offset = match - int(offsetSearchFrames);
        return true;
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

void InstallCdOffsetTable()
{
    if (GetFileAttributesA(offsetTablePath) != static_cast<DWORD>(-1)) return;
    const char staging[] = "game:\\drive-offsets.json.part";
    HANDLE file = CreateFileA(staging, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = sizeof(defaultOffsetTable) - 1;
    DWORD written = 0;
    const bool complete = WriteFile(file, defaultOffsetTable, size, &written, NULL) &&
                          written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (!complete || !MoveFile(staging, offsetTablePath)) DeleteFile(staging);
}

void ProbeCdDrive(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc, CdDriveInfo& drive)
{
    enum { Inquiry, Capabilities, Configuration };
    const struct
    {
        unsigned kind;
        const char* name;
        unsigned capacity;
        unsigned cdbSize;
        BYTE cdb[10];
    } probes[] = {
        // The Xbox kernel's optical initialization uses 36 bytes and control 0xC0.
        {Inquiry, "INQUIRY", 36, 6, {0x12, 0, 0, 0, 36, 0xc0}},
        {Capabilities, "MODE SENSE(10) 2A", 64, 10, {0x5a, 0, 0x2a, 0, 0, 0, 0, 0, 64, 0}},
        {Capabilities, "MODE SENSE(10) 2A DBD", 64, 10, {0x5a, 8, 0x2a, 0, 0, 0, 0, 0, 64, 0}},
        // Xbox pass-through requires even lengths, so use 254 instead of 255.
        {Capabilities, "MODE SENSE(10) all pages", 254, 10, {0x5a, 0, 0x3f, 0, 0, 0, 0, 0, 254, 0}},
        {Capabilities, "MODE SENSE(6) 2A", 64, 6, {0x1a, 0, 0x2a, 0, 64, 0}},
        {Configuration, "GET CONFIGURATION (C0)", 4096, 10, {0x46, 0, 0, 0, 0, 0, 0, 16, 0, 0xc0}},
        // If the full list is rejected, ask only for the CD Read feature.
        {Configuration, "GET CONFIGURATION CD Read (C0)", 16, 10, {0x46, 2, 0, 0x1e, 0, 0, 0, 0, 16, 0xc0}},
        {Configuration, "GET CONFIGURATION CD Read (standard)", 16, 10, {0x46, 2, 0, 0x1e, 0, 0, 0, 0, 16, 0}},
    };
    __declspec(align(16)) BYTE data[4096];
    char line[192];
    bool received[3] = {};
    for (const auto& probe : probes)
    {
#ifndef DVD360_LOGGING
        if (probe.kind != Inquiry) break;
#endif
        if (received[probe.kind]) continue;
        // This firmware rejects the capability pages. Test the read path directly.
        if (probe.kind != Inquiry && !_stricmp(drive.vendor, "PLDS") &&
            !_stricmp(drive.model, "DG-16D4S") && !_stricmp(drive.revision, "0401"))
            continue;
        memset(data, 0, probe.capacity);
        unsigned bytes;
        BYTE sense[18];
        const HRESULT status = stream.ReadOpticalInfo(probe.cdb, probe.cdbSize,
                                                      data, probe.capacity, bytes, sense);
        const unsigned format = sense[0] & 0x7f;
        const bool descriptor = format == 0x72 || format == 0x73;
        const unsigned key = sense[descriptor ? 1 : 2] & 0x0f;
        sprintf_s(line, sizeof(line), "CD drive %s: buffer bytes %u, sense %02x/%02x/%02x (format %02x)",
                  probe.name, bytes, key, unsigned(sense[descriptor ? 2 : 12]),
                  unsigned(sense[descriptor ? 3 : 13]), format);
        LogCdDiagnostic(line, status);
        if (format)
        {
            int pos = sprintf_s(line, sizeof(line), "CD drive %s raw sense:", probe.name);
            for (unsigned i = 0; i < sizeof(sense); ++i)
                pos += sprintf_s(line + pos, sizeof(line) - pos, " %02x", unsigned(sense[i]));
            LogCdDiagnostic(line);
        }
        if (status != S_OK) continue;
        if (probe.kind == Inquiry && bytes >= 5) bytes = min(bytes, 5u + data[4]);
        if (probe.kind == Capabilities && bytes >= 2)
            bytes = min(bytes, probe.cdbSize == 6 ? 1u + data[0] : 2u + AV_RB16(data));
        if (probe.kind == Configuration && bytes >= 4)
            bytes = min(bytes - 4, AV_RB32(data)) + 4;
        for (unsigned i = 0; i < bytes; i += 32)
        {
            int pos = sprintf_s(line, sizeof(line), "CD drive %s %04x:", probe.name, i);
            for (unsigned j = i; j < min(i + 32, bytes); ++j)
                pos += sprintf_s(line + pos, sizeof(line) - pos, " %02x", unsigned(data[j]));
            LogCdDiagnostic(line);
        }
        if (probe.kind == Inquiry && bytes >= 36)
        {
            CopyInquiryField(drive.vendor, data + 8, 8);
            CopyInquiryField(drive.model, data + 16, 16);
            CopyInquiryField(drive.revision, data + 32, 4);
            sprintf_s(line, sizeof(line), "CD drive: %s / %s / %s", drive.vendor, drive.model, drive.revision);
            LogCdDiagnostic(line);
        }
        const bool shortModeSense = probe.cdbSize == 6;
        const unsigned modeHeader = shortModeSense ? 4 : 8;
        if (probe.kind == Capabilities && bytes >= modeHeader)
        {
            unsigned page = modeHeader + (shortModeSense ? data[3] : AV_RB16(data + 6));
            while (page + 2 <= bytes)
            {
                const bool subpage = (data[page] & 0x40) != 0;
                const unsigned header = subpage ? 4 : 2;
                if (page + header > bytes) break;
                const unsigned length = subpage ? AV_RB16(data + page + 2) : data[page + 1];
                if (page + header + length > bytes) break;
                if (!subpage && (data[page] & 0x3f) == 0x2a && length >= 4)
                {
                    received[Capabilities] = true;
                    const unsigned flags = data[page + 5];
                    sprintf_s(line, sizeof(line), "CD drive advertised: CDDA=%u Accurate Stream=%u C2=%u",
                              flags & 1, (flags >> 1) & 1, (flags >> 4) & 1);
                    LogCdDiagnostic(line);
                    break;
                }
                page += header + length;
            }
        }
        if (probe.kind == Configuration && bytes >= 8)
        {
            received[Configuration] = true;
            sprintf_s(line, sizeof(line), "CD drive current media profile: %04x", AV_RB16(data + 6));
            LogCdDiagnostic(line);
            for (unsigned i = 8; i + 4 <= bytes && i + 4 + data[i + 3] <= bytes; i += 4 + data[i + 3])
            {
                sprintf_s(line, sizeof(line), "CD drive advertised feature: %04x (current=%u)",
                          AV_RB16(data + i), unsigned(data[i + 2] & 1));
                LogCdDiagnostic(line);
            }
        }
    }
#ifdef DVD360_LOGGING
    if (disc.endLba - disc.startLba >= 12)
    {
        __declspec(align(16)) BYTE first[3 * 2352], second[3 * 2352];
        const unsigned lba = disc.startLba + 8;
        unsigned bytes;
        const HRESULT a = stream.ReadCdAudio(lba, 3, first, bytes);
        const HRESULT b = stream.ReadCdAudio(lba + 1, 3, second, bytes);
        const char* result = "CD drive overlapping CDDA reads: unknown";
        if (a == S_OK && b == S_OK)
        {
            bool nonzero = false;
            for (unsigned i = 2352; i < sizeof(first) && !nonzero; ++i) nonzero = first[i] != 0;
            if (nonzero)
                result = memcmp(first + 2352, second, 2 * 2352) == 0
                    ? "CD drive overlapping CDDA reads: identical on this disc"
                    : "CD drive overlapping CDDA reads: mismatch";
        }
        LogCdDiagnostic(result);
        BYTE cdb[12] = {0xbe, 0x04, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
        AV_WB32(cdb + 2, lba);
        BYTE sense[18];
        const unsigned sizes[] = {2352, 2646, 2648};
        for (unsigned format = 0; format < ARRAYSIZE(sizes); ++format)
        {
            const unsigned size = sizes[format];
            cdb[9] = BYTE(0x10 + 2 * format);
            memset(data, 0xa5, size);
            const HRESULT status = stream.ReadOpticalInfo(cdb, sizeof(cdb), data, size, bytes, sense);
            unsigned changed = 0, flagged = 0;
            for (unsigned i = 2352; i < size; ++i)
            {
                if (data[i] != 0xa5) ++changed;
                if (data[i]) ++flagged;
            }
            const bool audioMatch = status == S_OK && a == S_OK && !memcmp(data, first, 2352);
            sprintf_s(line, sizeof(line),
                      "CD drive READ CD %02x: audio match=%u, tail overwritten=%u/%u, nonzero tail=%u, sense %02x/%02x/%02x",
                      unsigned(cdb[9]), unsigned(audioMatch), changed, size - 2352, flagged,
                      unsigned(sense[2] & 15), unsigned(sense[12]), unsigned(sense[13]));
            LogCdDiagnostic(line, status);
        }

        if (!disc.dataTracks)
        {
            const int leadout = int(disc.tracks[disc.audioTracks - 1].end);
            const int boundaries[] = {-3, -2, -1, 0, 1, leadout - 1, leadout, leadout + 1, leadout + 2};
            cdb[9] = 0x10;
            for (int sector : boundaries)
            {
                AV_WB32(cdb + 2, uint32_t(sector));
                memset(data, 0xa5, 2352);
                const HRESULT status = stream.ReadOpticalInfo(cdb, sizeof(cdb), data, 2352, bytes, sense);
                unsigned overwritten = 0;
                bool nonzero = false;
                if (status == S_OK)
                    for (unsigned i = 0; i < 2352; ++i)
                        if (data[i] != 0xa5)
                        {
                            ++overwritten;
                            nonzero |= data[i] != 0;
                        }
                sprintf_s(line, sizeof(line),
                          "CD drive boundary LBA %d: changed=%u/2352, nonzero=%u, sense %02x/%02x/%02x",
                          sector, overwritten, unsigned(nonzero),
                          unsigned(sense[2] & 15), unsigned(sense[12]), unsigned(sense[13]));
                LogCdDiagnostic(line, status);
            }
        }
    }
#endif
}

#ifdef DVD360_LOGGING
static void ProbeCdCache(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                         HANDLE stop, HANDLE cancel)
{
    if (disc.dataTracks) return;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart < 1000) return;
    const unsigned start = disc.tracks[0].start;
    const unsigned end = disc.tracks[disc.audioTracks - 1].end;
    if (end - start < 2048) return;
    const unsigned anchor = start + 32;
    const unsigned distant = start + (end - start) / 2;
    const unsigned batchSize = 32;
    BYTE* storage = static_cast<BYTE*>(malloc(2 * batchSize * 2352 + 15));
    if (!storage) return;
    BYTE* reference = reinterpret_cast<BYTE*>((reinterpret_cast<uintptr_t>(storage) + 15) &
                                              ~static_cast<uintptr_t>(15));
    BYTE* scratch = reference + batchSize * 2352;
    unsigned bytes;
    const unsigned evictionSizes[] = {0, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    if (stream.ReadCdAudio(anchor, batchSize, reference, bytes) == S_OK)
        for (unsigned sectors : evictionSizes)
        {
            if (sectors > end - distant || Cancelled(stop, cancel)) break;
            unsigned done = 0;
            while (done < sectors && !Cancelled(stop, cancel))
            {
                const unsigned count = min(batchSize, sectors - done);
                if (stream.ReadCdAudio(distant + done, count, scratch, bytes) != S_OK) break;
                done += count;
            }
            if (done != sectors || Cancelled(stop, cancel)) break;
            LARGE_INTEGER before, after;
            QueryPerformanceCounter(&before);
            const HRESULT status = stream.ReadCdAudio(anchor, batchSize, scratch, bytes);
            QueryPerformanceCounter(&after);
            const unsigned micros = unsigned((after.QuadPart - before.QuadPart) * 1000000 / frequency.QuadPart);
            char line[128];
            sprintf_s(line, sizeof(line), "CD drive cache timing: distant sectors=%u, reread=%u us, match=%u",
                      sectors, micros, unsigned(status == S_OK && !memcmp(reference, scratch, bytes)));
            LogCdDiagnostic(line, status);
        }
    free(storage);
}
#endif

namespace
{
    struct MetadataJson
    {
        struct Token { std::string text; unsigned end; char kind; };
        std::vector<Token> tokens;

        static void Space(const char*& p) { p += strspn(p, " \r\n\t"); }

        static bool Hex(const char*& p, unsigned& value)
        {
            value = 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                const char digit = *p;
                if (digit >= '0' && digit <= '9') value = value * 16 + digit - '0';
                else if (digit >= 'a' && digit <= 'f') value = value * 16 + digit - 'a' + 10;
                else if (digit >= 'A' && digit <= 'F') value = value * 16 + digit - 'A' + 10;
                else return false;
                ++p;
            }
            return true;
        }

        static bool String(const char*& p, std::string& text)
        {
            ++p;
            while (*p && *p != '"')
            {
                if (*p != '\\')
                {
                    text += *p++;
                    continue;
                }
                ++p;
                if (!*p) return false;
                const char escape = *p++;
                if (escape == '"' || escape == '\\' || escape == '/') text += escape;
                else if (escape == 'n' || escape == 'r' || escape == 't' || escape == 'b' || escape == 'f')
                    text += ' ';
                else if (escape == 'u')
                {
                    unsigned cp;
                    if (!Hex(p, cp) || !cp) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff)
                    {
                        if (p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        unsigned low;
                        if (!Hex(p, low) || low < 0xdc00 || low > 0xdfff) return false;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    }
                    else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                    if (cp >= 0x10000) text += char(0xf0 | (cp >> 18));
                    if (cp >= 0x800) text += char((cp >= 0x10000 ? 0x80 : 0xe0) | ((cp >> 12) & 63));
                    if (cp >= 0x80) text += char((cp >= 0x800 ? 0x80 : 0xc0) | ((cp >> 6) & 63));
                    text += char(cp >= 0x80 ? 0x80 | (cp & 63) : cp);
                }
                else return false;
            }
            if (*p != '"') return false;
            ++p;
            return true;
        }

        bool Read(const char*& p, unsigned depth)
        {
            Space(p);
            if (!*p || depth > 32 || tokens.size() >= 60000) return false;
            const unsigned index = unsigned(tokens.size());
            const char kind = *p;
            tokens.push_back(Token{std::string(), 0, kind});
            if (kind == '{' || kind == '[')
            {
                ++p;
                Space(p);
                const char close = kind == '{' ? '}' : ']';
                if (*p != close)
                    for (;;)
                    {
                        if (kind == '{')
                        {
                            if (*p != '"' || !Read(p, depth + 1)) return false;
                            Space(p);
                            if (*p != ':') return false;
                            ++p;
                        }
                        if (!Read(p, depth + 1)) return false;
                        Space(p);
                        if (*p != ',') break;
                        ++p;
                        Space(p);
                    }
                if (*p != close) return false;
                ++p;
            }
            else if (kind == '"')
            {
                if (!String(p, tokens[index].text)) return false;
            }
            else
            {
                const size_t length = strcspn(p, ",]} \r\n\t");
                if (!length) return false;
                tokens[index].text.assign(p, length);
                p += length;
            }
            tokens[index].end = unsigned(tokens.size());
            return true;
        }

        unsigned Field(unsigned object, const char* key) const
        {
            if (object >= tokens.size() || tokens[object].kind != '{') return unsigned(tokens.size());
            for (unsigned i = object + 1; i + 1 < tokens[object].end; i = tokens[i + 1].end)
                if (tokens[i].text == key) return i + 1;
            return unsigned(tokens.size());
        }

        const char* Text(unsigned object, const char* key) const
        {
            const unsigned field = Field(object, key);
            return field < tokens.size() && tokens[field].kind == '"' ? tokens[field].text.c_str() : "";
        }

        std::string Artist(unsigned object) const
        {
            std::string artist;
            const unsigned credits = Field(object, "artist-credit");
            if (credits < tokens.size() && tokens[credits].kind == '[')
                for (unsigned i = credits + 1; i < tokens[credits].end; i = tokens[i].end)
                {
                    artist += Text(i, "name");
                    artist += Text(i, "joinphrase");
                }
            return artist;
        }

        unsigned Number(unsigned object, const char* key) const
        {
            const unsigned field = Field(object, key);
            return field < tokens.size() ? unsigned(strtoul(tokens[field].text.c_str(), NULL, 10)) : 0;
        }
    };

    void CopyTag(char* output, size_t size, const char* text)
    {
        size_t length = min(strlen(text), size - 1);
        while (length && (BYTE(text[length]) & 0xc0) == 0x80) --length;
        memcpy(output, text, length);
        output[length] = '\0';
    }

    bool ReleaseId(const char* id)
    {
        if (strlen(id) != 36) return false;
        for (unsigned i = 0; i < 36; ++i)
            if ((i == 8 || i == 13 || i == 18 || i == 23) ? id[i] != '-' :
                !strchr("0123456789abcdef", id[i]))
                return false;
        return true;
    }

    void DownloadCover(const char* release, HANDLE stop, CdMetadata& metadata)
    {
        if (!ReleaseId(release)) return;
        char host[128], path[512];
        strcpy_s(host, sizeof(host), coverArtHost);
        sprintf_s(path, sizeof(path), "/release/%s/front-500", release);
        const int capacity = 768 * 1024;
        BYTE* buffer = static_cast<BYTE*>(malloc(capacity + 1));
        if (!buffer) return;
        char* response = reinterpret_cast<char*>(buffer);
        for (unsigned redirect = 0; redirect < 4 && !Cancelled(stop, stop); ++redirect)
        {
            char request[768];
            sprintf_s(request, sizeof(request),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: 360DVD/1.6.1\r\n"
                      "Accept: image/jpeg, image/png\r\nConnection: close\r\n\r\n", path, host);
            const char* failure = "network startup failed";
            int error = 0;
            const int size = GetDatabase(host, request, response, capacity, stop, stop, failure, error);
            response[size] = '\0';
            const char* code = strchr(response, ' ');
            const char* body = strstr(response, "\r\n\r\n");
            if (!code || !body) break;
            const int status = atoi(code + 1);
            if (status == 200)
            {
                body += 4;
                const unsigned length = unsigned(response + size - body);
                const bool jpeg = length >= 3 && BYTE(body[0]) == 0xff && BYTE(body[1]) == 0xd8 &&
                                  BYTE(body[2]) == 0xff;
                const bool png = length >= 8 && !memcmp(body, "\x89PNG\r\n\x1a\n", 8);
                if (!jpeg && !png) break;
                memmove(buffer, body, length);
                metadata.cover = buffer;
                metadata.coverSize = length;
                metadata.coverPng = png;
                LogCdDiagnostic("Cover Art Archive front cover cached in RAM");
                return;
            }
            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
                break;
            const char* location = strstr(response, "\r\n") + 2;
            while (location < body && _strnicmp(location, "Location:", 9))
                location = strstr(location, "\r\n") + 2;
            if (location >= body) break;
            location += 9;
            while (*location == ' ' || *location == '\t') ++location;
            if (strncmp(location, "https://", 8)) break;
            location += 8;
            const char* end = strstr(location, "\r\n");
            const char* slash = strchr(location, '/');
            if (!end || !slash || slash >= end || slash - location >= sizeof(host) ||
                end - slash >= sizeof(path)) break;
            const std::string destination(location, slash);
            const size_t length = destination.size();
            if (destination != coverArtHost && destination != "archive.org" &&
                (length <= 12 || destination.compare(length - 12, 12, ".archive.org")))
                break;
            if (destination.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos)
                break;
            bool validPath = true;
            for (const char* p = slash; p < end; ++p)
                if (*p <= ' ' || BYTE(*p) >= 127) validPath = false;
            if (!validPath) break;
            strcpy_s(host, sizeof(host), destination.c_str());
            memcpy(path, slash, end - slash);
            path[end - slash] = '\0';
        }
        free(buffer);
        LogCdDiagnostic("Cover Art Archive front cover unavailable");
    }
}

void LookupCdMetadata(const XboxDvdStream::CdAudioProbe& disc, HANDLE stop, CdMetadata& metadata,
                      CdEditionPicker& picker)
{
    metadata = {};
    if (disc.dataTracks || !disc.audioTracks || Cancelled(stop, stop)) return;
    char toc[805];
    int pos = sprintf_s(toc, sizeof(toc), "%02X%02X%08X", disc.tocFirst, disc.tocLast,
                        disc.tracks[disc.audioTracks - 1].end + 150);
    for (unsigned i = 0; i < 99; ++i)
        pos += sprintf_s(toc + pos, sizeof(toc) - pos, "%08X",
                        i < disc.audioTracks ? disc.tracks[i].start + 150 : 0);
    AVSHA* sha = av_sha_alloc();
    if (!sha) return;
    BYTE hash[20];
    av_sha_init(sha, 160);
    av_sha_update(sha, reinterpret_cast<const BYTE*>(toc), pos);
    av_sha_final(sha, hash);
    av_free(sha);
    char discId[32];
    av_base64_encode(discId, sizeof(discId), hash, sizeof(hash));
    for (char* p = discId; *p; ++p)
    {
        if (*p == '+') *p = '.';
        else if (*p == '/') *p = '_';
        else if (*p == '=') *p = '-';
    }
    char request[512];
    sprintf_s(request, sizeof(request),
              "GET /ws/2/discid/%s?fmt=json&inc=recordings+artist-credits+labels+isrcs HTTP/1.0\r\n"
              "Host: musicbrainz.org\r\nUser-Agent: 360DVD/1.6.1 (https://github.com/GOffrier-DEV/360DVD)\r\n"
              "Accept: application/json\r\nConnection: close\r\n\r\n", discId);
    const int capacity = 512 * 1024;
    std::vector<char> response(capacity + 1);
    const char* failure = "network startup failed";
    int error = 0;
    const int size = GetDatabase(musicBrainzHost, request, response.data(), capacity, stop, stop, failure, error);
    response[size] = '\0';
    const char* code = strchr(response.data(), ' ');
    const char* body = strstr(response.data(), "\r\n\r\n");
    if (!code || atoi(code + 1) != 200 || !body)
    {
        LogCdDiagnostic("MusicBrainz disc lookup unavailable");
        return;
    }
    body += 4;
    MetadataJson json;
    if (!json.Read(body, 0)) return;
    const unsigned releases = json.Field(0, "releases");
    if (releases >= json.tokens.size() || json.tokens[releases].kind != '[')
        return;
    char selectedRelease[37] = {};
    char selectionPath[96];
    sprintf_s(selectionPath, sizeof(selectionPath), "game:\\release-%s.txt", discId);
    FILE* selection = fopen(selectionPath, "r");
    if (selection)
    {
        char line[64] = {};
        if (fgets(line, sizeof(line), selection))
        {
            line[strcspn(line, " \t\r\n")] = '\0';
            if (ReleaseId(line)) strcpy_s(selectedRelease, sizeof(selectedRelease), line);
        }
        fclose(selection);
    }
    struct Edition { unsigned release, medium; };
    std::vector<Edition> editions;
    for (unsigned release = releases + 1; release < json.tokens[releases].end; release = json.tokens[release].end)
    {
        const unsigned media = json.Field(release, "media");
        if (media >= json.tokens.size() || json.tokens[media].kind != '[') continue;
        for (unsigned i = media + 1; i < json.tokens[media].end; i = json.tokens[i].end)
        {
            const unsigned discs = json.Field(i, "discs");
            const unsigned tracks = json.Field(i, "tracks");
            if (discs >= json.tokens.size() || tracks >= json.tokens.size() ||
                json.Number(i, "track-count") != disc.audioTracks) continue;
            bool exact = false;
            for (unsigned d = discs + 1; d < json.tokens[discs].end; d = json.tokens[d].end)
                if (!strcmp(json.Text(d, "id"), discId)) exact = true;
            if (!exact) continue;
            char message[280];
            const char* id = json.Text(release, "id");
            sprintf_s(message, sizeof(message), "MusicBrainz edition %.36s: %.150s (%.16s, %.8s)",
                      id, json.Text(release, "title"), json.Text(release, "date"), json.Text(release, "country"));
            LogCdDiagnostic(message);
            if (selectedRelease[0] && strcmp(selectedRelease, id)) continue;
            editions.push_back(Edition{release, i});
        }
    }
    if (editions.empty()) return;
    unsigned choice = 0;
    if (editions.size() > 1)
    {
        if (!picker.ready || !picker.decided) return;
        picker.count = min(unsigned(editions.size()), unsigned(ARRAYSIZE(picker.labels)));
        for (unsigned j = 0; j < picker.count; ++j)
        {
            const unsigned release = editions[j].release;
            const unsigned labels = json.Field(release, "label-info");
            const char* catalog = labels < json.tokens.size() && labels + 1 < json.tokens[labels].end
                ? json.Text(labels + 1, "catalog-number") : "";
            const char* barcode = json.Text(release, "barcode");
            sprintf_s(picker.labels[j], sizeof(picker.labels[j]), "%.48s | %.8s %.10s | %.22s | %.16s",
                      json.Text(release, "title"), json.Text(release, "country"),
                      json.Text(release, "date"), catalog, barcode);
        }
        SetEvent(picker.ready);
        HANDLE events[] = {stop, picker.decided};
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1 ||
            picker.selected < 0 || unsigned(picker.selected) >= picker.count)
            return;
        choice = unsigned(picker.selected);
    }
    const unsigned chosen = editions[choice].release, medium = editions[choice].medium;
    if (!chosen || !*json.Text(chosen, "title")) return;
    CopyTag(metadata.album, sizeof(metadata.album), json.Text(chosen, "title"));
    CopyTag(metadata.artist, sizeof(metadata.artist), json.Artist(chosen).c_str());
    CopyTag(metadata.date, sizeof(metadata.date), json.Text(chosen, "date"));
    metadata.discNumber = json.Number(medium, "position");
    const unsigned labels = json.Field(chosen, "label-info");
    if (labels < json.tokens.size() && labels + 1 < json.tokens[labels].end &&
        json.tokens[labels + 1].end == json.tokens[labels].end)
    {
        CopyTag(metadata.label, sizeof(metadata.label), json.Text(json.Field(labels + 1, "label"), "name"));
        CopyTag(metadata.catalog, sizeof(metadata.catalog), json.Text(labels + 1, "catalog-number"));
    }
    const unsigned tracks = json.Field(medium, "tracks");
    for (unsigned i = tracks + 1; i < json.tokens[tracks].end; i = json.tokens[i].end)
    {
        const unsigned position = json.Number(i, "position");
        if (!position || position > disc.audioTracks) continue;
        CdTrackMetadata& track = metadata.tracks[position - 1];
        const unsigned recording = json.Field(i, "recording");
        const char* title = json.Text(i, "title");
        CopyTag(track.title, sizeof(track.title), *title ? title : json.Text(recording, "title"));
        std::string artist = json.Artist(i);
        if (artist.empty()) artist = json.Artist(recording);
        CopyTag(track.artist, sizeof(track.artist), artist.empty() ? metadata.artist : artist.c_str());
        const unsigned isrc = json.Field(recording, "isrcs");
        if (isrc < json.tokens.size() && isrc + 2 == json.tokens[isrc].end &&
            json.tokens[isrc + 1].kind == '"' && json.tokens[isrc + 1].text.size() == 12)
            CopyTag(track.isrc, sizeof(track.isrc), json.tokens[isrc + 1].text.c_str());
    }
    LogCdDiagnostic("MusicBrainz album and track metadata downloaded");
    DownloadCover(json.Text(chosen, "id"), stop, metadata);
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
    const int size = response ? GetDatabase(accurateRipHost, request, response, capacity,
                                           stop, stop, failure, socketError) : 0;
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
                   const CdDriveInfo& driveInfo, HANDLE metadataDone, const CdMetadata& metadata,
                   volatile LONG& progress, char (&summary)[96])
{
    strcpy_s(summary, sizeof(summary), "CD rip failed: HDD unavailable");
    RipDrive drive;
    if (!drive.Mount()) return E_FAIL;
    const char ripRoot[] = "DvdRip:\\360DVD-Rips";
    if (!CreateDirectory(ripRoot, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return E_FAIL;
    int offset = 0;
    const char* offsetSource = "uncalibrated";
    FILE* config = fopen("DvdRip:\\360DVD-Rips\\drive-offset.txt", "r");
    const bool manualOffset = config != NULL;
    if (config)
    {
        char text[32] = {};
        const bool read = fgets(text, sizeof(text), config) != NULL;
        fclose(config);
        char* end;
        const long value = strtol(text, &end, 10);
        if (!read || end == text || value < -long(offsetSearchFrames) || value > long(offsetSearchFrames) ||
            end[strspn(end, " \t\r\n")])
        {
            strcpy_s(summary, sizeof(summary), "CD rip failed: invalid drive-offset.txt");
            return E_INVALIDARG;
        }
        offset = int(value);
        offsetSource = "manual";
    }

    const DiscId id = Identify(disc);
    bool calibrated = false;
    bool haveProfile = false;
    unsigned previousDisc = 0;
    int previousOffset = 0, confirmed = 0;
    char profile[MAX_PATH] = {};
    if (!manualOffset && driveInfo.vendor[0] && driveInfo.model[0] && driveInfo.revision[0])
    {
        int pos = sprintf_s(profile, sizeof(profile), "%s\\offset-", ripRoot);
        const char* identity[] = {driveInfo.vendor, driveInfo.model, driveInfo.revision};
        for (unsigned i = 0; i < ARRAYSIZE(identity); ++i)
        {
            for (const char* p = identity[i]; *p; ++p)
                pos += sprintf_s(profile + pos, sizeof(profile) - pos, "%02x", unsigned(BYTE(*p)));
            profile[pos++] = '-';
        }
        strcpy_s(profile + pos, sizeof(profile) - pos, "v1.txt");
        FILE* saved = fopen(profile, "r");
        if (saved)
        {
            haveProfile = fscanf(saved, "%d %x %d", &previousOffset, &previousDisc, &confirmed) == 3 &&
                          previousOffset >= -int(offsetSearchFrames) && previousOffset <= int(offsetSearchFrames) &&
                          (confirmed == 0 || confirmed == 1);
            fclose(saved);
        }
    }
    const bool tableOffset = !manualOffset && ReadCdOffset(driveInfo, offset);
    if (tableOffset)
        offsetSource = "drive-offsets.json";
    else if (!manualOffset && haveProfile && confirmed)
    {
        offset = previousOffset;
        offsetSource = "confirmed on two discs";
    }
    else if (!manualOffset && !disc.dataTracks && lookupDone)
    {
        PublishProgress(progress, CdRipCalibrating);
        HANDLE events[] = {stop, cancel, lookupDone};
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) == WAIT_OBJECT_0 + 2)
            calibrated = CalibrateOffset(stream, disc, lookup, stop, cancel, offset);
        if (calibrated) offsetSource = "matched frame450 references on this disc";
    }
    if (Cancelled(stop, cancel))
    {
        strcpy_s(summary, sizeof(summary), "CD rip cancelled");
        return S_FALSE;
    }
#ifdef DVD360_LOGGING
    if (!tableOffset)
        ProbeCdCache(stream, disc, stop, cancel);
#endif
    if (Cancelled(stop, cancel))
    {
        strcpy_s(summary, sizeof(summary), "CD rip cancelled");
        return S_FALSE;
    }
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
    static const CdMetadata empty = {};
    const CdMetadata* tags = &empty;
    if (metadataDone)
    {
        HANDLE events[] = {stop, cancel, metadataDone};
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 3000) == WAIT_OBJECT_0 + 2)
            tags = &metadata;
    }
    for (; completed < disc.audioTracks && status == S_OK;)
    {
        char path[MAX_PATH];
        sprintf_s(path, sizeof(path), "%s\\Track %02u.wav", directory, disc.tracks[completed].number);
        status = RipTrack(stream, disc, completed, offset, stop, cancel, path, crc[completed],
                          framesCompleted, discFrames, progress, *tags);
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
        else if (calibrated && profile[0] && (!haveProfile || previousDisc != id.cddb || previousOffset != offset))
        {
            unsigned matched = 0;
            for (unsigned i = 0; i < completed; ++i) if (confidence[i]) ++matched;
            if (matched >= 2)
            {
                char temporary[MAX_PATH];
                sprintf_s(temporary, sizeof(temporary), "%s.part", profile);
                FILE* saved = fopen(temporary, "w");
                if (saved)
                {
                    const bool verified = haveProfile && previousDisc != id.cddb && previousOffset == offset;
                    const bool written = fprintf(saved, "%d %08x %d\n", offset, id.cddb, int(verified)) > 0;
                    const bool closed = fclose(saved) == 0;
                    if (!written || !closed || !MoveFileEx(temporary, profile, MOVEFILE_REPLACE_EXISTING))
                        DeleteFile(temporary);
                }
            }
        }
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
            fprintf(report, "Offset source: %s\nCD drive: %s / %s / %s\n", offsetSource,
                    driveInfo.vendor, driveInfo.model, driveInfo.revision);
            fprintf(report, "AccurateRip disc ID: %03u-%08x-%08x-%08x\n",
                    disc.audioTracks, id.first, id.second, id.cddb);
            for (unsigned i = 0; i < completed; ++i)
                fprintf(report, "Track %02u: ARv1 %08x ARv2 %08x confidence %u\n",
                        disc.tracks[i].number, crc[i].v1, crc[i].v2, confidence[i]);
            if (lookupDone && WaitForSingleObject(lookupDone, 0) == WAIT_OBJECT_0 && lookup.size)
            {
                const char* body = strstr(lookup.response, "\r\n\r\n") + 4;
                const char* end = lookup.response + lookup.size;
                const unsigned recordSize = 13 + 9 * disc.audioTracks;
                unsigned index = 0;
                for (; body < end; body += recordSize)
                {
                    const BYTE* record = reinterpret_cast<const BYTE*>(body);
                    fprintf(report, "Database record %u: %03u-%08x-%08x-%08x\n",
                            ++index, unsigned(record[0]), AV_RL32(record + 1),
                            AV_RL32(record + 5), AV_RL32(record + 9));
                    for (unsigned i = 0; i < disc.audioTracks; ++i)
                    {
                        const BYTE* entry = record + 13 + 9 * i;
                        fprintf(report, "  Track %02u: CRC %08x frame450 %08x confidence %u\n",
                                disc.tracks[i].number, AV_RL32(entry + 1),
                                AV_RL32(entry + 5), unsigned(entry[0]));
                    }
                }
            }
            fclose(report);
        }
    }
    return status;
}
