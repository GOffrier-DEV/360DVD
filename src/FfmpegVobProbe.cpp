#include "FfmpegFileIo.h"
#include <stdio.h>

extern "C" {
#include <libavformat/avformat.h>
}

static int InspectVob(AVFormatContext* format, HANDLE log)
{
    int status = avformat_find_stream_info(format, NULL);
    if (status < 0)
        return status;

    DWORD written;
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVStream* stream = format->streams[i];
        char line[160];
        int length = sprintf_s(line, sizeof(line), "stream=%u id=0x%x codec=%s\r\n",
                               i, unsigned(stream->id), avcodec_get_name(stream->codecpar->codec_id));
        if (length > 0)
            WriteFile(log, line, DWORD(length), &written, NULL);
    }

    const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video < 0)
        return video;
    status = av_seek_frame(format, video,
                           format->streams[video]->start_time == AV_NOPTS_VALUE
                               ? 0 : format->streams[video]->start_time,
                           AVSEEK_FLAG_BACKWARD);
    if (status < 0)
        return status;

    AVPacket* packet = av_packet_alloc();
    if (!packet)
        return AVERROR(ENOMEM);
    unsigned audioPackets = 0, videoPackets = 0;
    for (unsigned i = 0; i < 256; ++i)
    {
        status = av_read_frame(format, packet);
        if (status < 0)
            break;
        const AVMediaType type = format->streams[packet->stream_index]->codecpar->codec_type;
        audioPackets += type == AVMEDIA_TYPE_AUDIO;
        videoPackets += type == AVMEDIA_TYPE_VIDEO;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    char line[160];
    int length = sprintf_s(line, sizeof(line), "after seek: video=%u audio=%u read_status=%d\r\n",
                           videoPackets, audioPackets, status);
    if (length > 0)
        WriteFile(log, line, DWORD(length), &written, NULL);
    return status == AVERROR_EOF || status == 0 ? (videoPackets ? 0 : AVERROR_INVALIDDATA) : status;
}

void RunFfmpegVobProbe()
{
    HANDLE file = CreateFileA("game:\\ffmpeg-probe.vob", GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    HANDLE log = CreateFileA("game:\\ffmpeg-vob.log", GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log != INVALID_HANDLE_VALUE)
    {
        AVIOContext* io = CreateFfmpegFileIo(file);
        AVFormatContext* format = io ? avformat_alloc_context() : NULL;
        int status = AVERROR(ENOMEM);
        if (format)
        {
            format->pb = io;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
            status = avformat_open_input(&format, NULL, av_find_input_format("mpeg"), NULL);
            if (status >= 0)
                status = InspectVob(format, log);
        }
        avformat_close_input(&format);
        DestroyFfmpegFileIo(&io);
        char line[96];
        DWORD written;
        int length = sprintf_s(line, sizeof(line), "VOB probe status=%d\r\n", status);
        if (length > 0)
            WriteFile(log, line, DWORD(length), &written, NULL);
        CloseHandle(log);
    }
    CloseHandle(file);
}
