#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "DvdFrameProbe.h"
#include "PlaybackAffinity.h"

#include "FfmpegFileIo.h"
#include "FfmpegAudio.h"
#include "PlaybackOsd.h"
#include <stdlib.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace
{
    const unsigned kWidth = 720;
    const unsigned kHeight = 576;
    HRESULT VideoPipelineError(const char* stage, int error)
    {
#ifdef DVD360_LOGGING
        char message[160], detail[64];
        av_strerror(error, detail, sizeof(detail));
        sprintf_s(message, sizeof(message), "Video %s: %s (%d)", stage, detail, error);
        LogDvdNavStartup(message);
#endif
        return E_FAIL;
    }
    const char* kVs =
        "struct V { float4 p : POSITION; float2 uv : TEXCOORD0; };"
        "V main(V v) { return v; }";
    const char* kPs =
        "sampler2D ty : register(s0); sampler2D tu : register(s1);"
        "sampler2D tv : register(s2);"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {"
        "float y = 1.16438 * (tex2D(ty, uv).r - 16.0/255.0);"
        "float u = tex2D(tu, uv).r - 128.0/255.0;"
        "float v = tex2D(tv, uv).r - 128.0/255.0;"
        "return float4(saturate(float3(y+1.59603*v,"
        "y-0.39176*u-0.81297*v,y+2.01723*u)),1); }";

    HRESULT UploadPlane(IDirect3DDevice9* device, const BYTE* data, unsigned stride,
                        unsigned width, unsigned height, IDirect3DTexture9** texture)
    {
        if (!*texture)
        {
            HRESULT hr = device->CreateTexture(width, height, 1, 0, D3DFMT_LIN_L8,
                                               D3DPOOL_DEFAULT, texture, NULL);
            if (FAILED(hr))
                return hr;
        }
        D3DLOCKED_RECT lock;
        HRESULT hr = (*texture)->LockRect(0, &lock, NULL, 0);
        if (FAILED(hr))
            return hr;
        bool ok = lock.Pitch >= (int)width;
        for (unsigned y = 0; ok && y < height; ++y)
            memcpy((BYTE*)lock.pBits + y * lock.Pitch, data + y * stride, width);
        hr = (*texture)->UnlockRect(0);
        return ok ? hr : E_FAIL;
    }

#ifdef DVD360_LOGGING
    bool DecodeCpuTime(ULONGLONG& ticks)
    {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            return false;
        ticks = ((ULONGLONG)kernel.dwHighDateTime << 32) + kernel.dwLowDateTime
              + ((ULONGLONG)user.dwHighDateTime << 32) + user.dwLowDateTime;
        return true;
    }
#endif
}

struct DvdFrameProbe::QueuedFrame
{
    BYTE pixels[kWidth * kHeight * 3 / 2];
    unsigned width, height, calls;
    double duration;
    bool filtered;
    bool deinterlaced;
    LONGLONG parseTicks, readTicks, inputWaitTicks;
    LONGLONG decodeTicks, copyTicks, bwdifTicks;
    LONGLONG audioWorkTicks;
    unsigned videoPacketHighWater;
    double decodeCpuMs;
    LONGLONG worstReadTicks;
    unsigned readCalls, readBytes;
    HRESULT status;
    int64_t dvdPts;
    double subtitleTime;
};

struct DvdFrameProbe::Decoder
{
    CRITICAL_SECTION queueStats;
    unsigned queuedFrames;
    FfmpegReadStats ioStats;
    AVIOContext* io;
    AVFormatContext* format;
    AVCodecContext* codec;
    AVFrame* decoded;
    AVFilterGraph* videoFilter;
    AVFilterContext *videoSource, *videoSink;
    bool videoFilterEnded, disableDeinterlace;
    int OpenVideoFilter(const AVFrame& frame, AVRational timeBase);
    AVPacket* packet;
    int video;
    double frameRate;
    double duration;
    float displayAspect;
    FfmpegAudio* audio;
    int audioStream;
    const char* audioLanguage;
    enum { SubtitleTracks = 32, SubtitleCues = 24 };
    struct Cue { AVSubtitle bitmap; double start, end; unsigned track, streamId; };
    AVCodecContext* subtitleCodec[SubtitleTracks];
    int subtitleStream[SubtitleTracks];
    const char* subtitleLanguage[SubtitleTracks];
    unsigned subtitleCount, cueCount;
    Cue cues[SubtitleCues];
    Cue heldMenuCue;
    bool audioFailed;
    bool videoEnded;
    bool stillPresented, stillContinueQueued;
    double stillElapsed;
    int64_t presentedDvdPts;
    double presentedSubtitleTime;
    bool discResumeRequest, discResuming;
    bool emptyDiscBoundary;
    bool discAudioHandoffArmed, discAudioHandedOff;
    bool needsFirstDiscPicture;
    struct DeferredAudio { AVPacket packet; DeferredAudio* next; };
    DeferredAudio *deferredHead, *deferredTail;
    unsigned deferredBytes, deferredCount;
    bool deferStartupAudio;
    void DrainDeferredAudio(bool play);
    void DecodeAudioPacket(AVPacket* packet);
    bool audioSwitchPending;
    int audioSwitchRequest;
    HRESULT audioSwitchStatus;
    bool flushed;
    int demuxEnd;
    enum { PacketCapacity = 96, PacketByteBudget = 4 * 1024 * 1024 };
    AVPacket pendingVideo[PacketCapacity];
    unsigned packetHead, packetCount, packetBytes, packetHighWater;
    unsigned packetBudget, startupVideoBytes, primedVideoBytes;
    void PumpPacket();
    bool AddSubtitleStream(unsigned index);
    const char* ProbeTrackLanguage(bool subtitle, int index) const;
    HANDLE thread, stop, freeSlots, ready, primed;
    HANDLE seekRequest, seekComplete;
    HRESULT seekStatus;
    unsigned writeIndex, readIndex, calls;
    LONGLONG parseTicks, inputWaitTicks;
    LONGLONG audioWorkTicks;
    enum { QueueCapacity = 6 };
    QueuedFrame queue[QueueCapacity];
};

int DvdFrameProbe::Decoder::OpenVideoFilter(const AVFrame& frame, AVRational timeBase)
{
    videoFilter = avfilter_graph_alloc();
    if (!videoFilter) return AVERROR(ENOMEM);
    videoFilter->nb_threads = 2;
    AVFilterContext* bwdif = NULL;
    const AVRational sar = frame.sample_aspect_ratio.den > 0
        ? frame.sample_aspect_ratio : av_make_q(0, 1);
    char args[160];
    sprintf_s(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        frame.width, frame.height, frame.format, timeBase.num, timeBase.den,
        sar.num, sar.den);
    int status = avfilter_graph_create_filter(&videoSource, avfilter_get_by_name("buffer"),
                                             "dvd_in", args, NULL, videoFilter);
    if (status >= 0)
        status = avfilter_graph_create_filter(&bwdif, avfilter_get_by_name("bwdif"),
            "dvd_bwdif", "mode=send_field:parity=auto:deint=interlaced", NULL, videoFilter);
    if (status >= 0)
        status = avfilter_graph_create_filter(&videoSink, avfilter_get_by_name("buffersink"),
                                             "dvd_out", NULL, NULL, videoFilter);
    if (status >= 0) status = avfilter_link(videoSource, 0, bwdif, 0);
    if (status >= 0) status = avfilter_link(bwdif, 0, videoSink, 0);
    if (status >= 0) status = avfilter_graph_config(videoFilter, NULL);
    return status;
}

void DvdFrameProbe::Decoder::DecodeAudioPacket(AVPacket* packet)
{
    if (!audio || audioFailed) return;
    audioFailed = FAILED(audio->Decode(packet, stop));
    if (audioFailed)
        LogDvdNavStartup("DVD audio decode or PCM submission failed");
    if (discAudioHandoffArmed && !audioFailed && audio->QueuedBuffers() >= 24)
    {
        discAudioHandoffArmed = false;
        discAudioHandedOff = true;
        SetEvent(seekComplete);
    }
}

void DvdFrameProbe::Decoder::DrainDeferredAudio(bool play)
{
    while (deferredHead)
    {
        DeferredAudio* item = deferredHead;
        deferredHead = item->next;
        if (play && WaitForSingleObject(seekRequest, 0) == WAIT_TIMEOUT)
            DecodeAudioPacket(&item->packet);
        av_packet_unref(&item->packet);
        delete item;
    }
    deferredTail = NULL;
    deferredBytes = deferredCount = 0;
}

bool DvdFrameProbe::Decoder::AddSubtitleStream(unsigned index)
{
    if (subtitleCount == SubtitleTracks) return false;
    const AVCodecParameters* par = format->streams[index]->codecpar;
    if (par->codec_id != AV_CODEC_ID_DVD_SUBTITLE) return false;
    for (unsigned i = 0; i < subtitleCount; ++i)
        if (subtitleStream[i] == static_cast<int>(index)) return false;
    const AVCodec* sub = avcodec_find_decoder(AV_CODEC_ID_DVD_SUBTITLE);
    AVCodecContext* context = sub ? avcodec_alloc_context3(sub) : NULL;
    if (!context) return false;
    AVDictionary* options = NULL;
    uint32_t clut[16];
    if (FfmpegDiscSubtitlePalette(io, clut))
    {
        char palette[16 * 8 + 1];
        for (unsigned i = 0; i < 16; ++i)
        {
            const int y = int((clut[i] >> 16) & 255) - 16;
            const int cr = int((clut[i] >> 8) & 255) - 128;
            const int cb = int(clut[i] & 255) - 128;
            const unsigned r = av_clip_uint8((298 * y + 409 * cr + 128) >> 8);
            const unsigned g = av_clip_uint8((298 * y - 100 * cb - 208 * cr + 128) >> 8);
            const unsigned b = av_clip_uint8((298 * y + 516 * cb + 128) >> 8);
            sprintf_s(palette + i * 8, sizeof(palette) - i * 8, "%06x%s",
                      (r << 16) | (g << 8) | b, i == 15 ? "" : ", ");
        }
        av_dict_set(&options, "palette", palette, 0);
    }
    int opened = avcodec_parameters_to_context(context, par);
    if (opened >= 0) opened = avcodec_open2(context, sub, &options);
    av_dict_free(&options);
    if (opened < 0)
    {
        avcodec_free_context(&context);
        return false;
    }
    const char* language = ProbeTrackLanguage(true, index);
    EnterCriticalSection(&queueStats);
    subtitleStream[subtitleCount] = index;
    subtitleLanguage[subtitleCount] = language;
    subtitleCodec[subtitleCount++] = context;
    LeaveCriticalSection(&queueStats);
    return true;
}

void DvdFrameProbe::Decoder::PumpPacket()
{
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    int result = av_read_frame(format, packet);
    QueryPerformanceCounter(&end);
    inputWaitTicks += end.QuadPart - start.QuadPart;
    if (result == AVERROR_EOF && io->error < 0)
        result = io->error;
    if (result < 0)
        demuxEnd = result;

    if (result == 0 && packet->stream_index == video)
    {
        packetBytes += packet->size;
        av_packet_move_ref(&pendingVideo[(packetHead + packetCount) % PacketCapacity], packet);
        ++packetCount;
        if (packetCount > packetHighWater)
            packetHighWater = packetCount;
        return;
    }
    if (result == 0)
    {
        // MPEG-PS can expose a subtitle stream only when its first packet arrives.
        if (AddSubtitleStream(packet->stream_index))
        {
            char message[128];
            sprintf_s(message, sizeof(message), "Late subtitle %u: stream=%d language=%s",
                subtitleCount, packet->stream_index, subtitleLanguage[subtitleCount - 1]);
            LogDvdNavStartup(message);
        }
        for (unsigned i = 0; i < subtitleCount; ++i)
        {
            if (packet->stream_index != subtitleStream[i]) continue;
            AVSubtitle subtitle = {};
            int got = 0;
            avcodec_decode_subtitle2(subtitleCodec[i], &subtitle, &got, packet);
            if (got && subtitle.num_rects && packet->pts != AV_NOPTS_VALUE)
            {
                const double pts = packet->pts * av_q2d(format->streams[packet->stream_index]->time_base);
                const double origin = format->start_time == AV_NOPTS_VALUE
                    ? 0 : format->start_time / double(AV_TIME_BASE);
                EnterCriticalSection(&queueStats);
                if (cueCount < SubtitleCues)
                {
                    Cue& cue = cues[cueCount++];
                    cue.bitmap = subtitle;
                    cue.track = i + 1;
                    cue.streamId = format->streams[packet->stream_index]->id;
                    cue.start = pts - origin + subtitle.start_display_time / 1000.0;
                    cue.end = pts - origin + subtitle.end_display_time / 1000.0;
                    if (cue.end <= cue.start) cue.end = cue.start + 5;
                    subtitle = AVSubtitle();
                }
                LeaveCriticalSection(&queueStats);
            }
            avsubtitle_free(&subtitle);
            break;
        }
    }
    if (audio && !audioFailed &&
        (result == AVERROR_EOF || (result == 0 && packet->stream_index == audioStream)))
    {
        QueryPerformanceCounter(&start);
        {
            // A menu can have a lone MPEG reference picture that isn't
            // output until decoder flush. Don't let intervening audio
            // block demux before that picture becomes available.
            DeferredAudio* item = NULL;
            if (deferStartupAudio && result == 0 && deferredCount < 4096 &&
                static_cast<unsigned>(packet->size) <= 4 * 1024 * 1024 - deferredBytes)
                item = new DeferredAudio();
            if (item)
            {
                deferredBytes += packet->size;
                ++deferredCount;
                av_packet_move_ref(&item->packet, packet);
                if (deferredTail) deferredTail->next = item;
                else deferredHead = item;
                deferredTail = item;
            }
            else if (!deferStartupAudio || result == 0)
            {
                deferStartupAudio = false;
                DrainDeferredAudio(true);
                DecodeAudioPacket(result == AVERROR_EOF ? NULL : packet);
            }
        }
        QueryPerformanceCounter(&end);
        audioWorkTicks += end.QuadPart - start.QuadPart;
    }
    if (audio && (audioFailed || (demuxEnd && !deferStartupAudio)))
        audio->Finish();
    av_packet_unref(packet);
}

DvdFrameProbe::DvdFrameProbe()
    : m_decoder(NULL), m_width(0), m_height(0), m_discSegment(0),
      m_deinterlacingActive(false), m_untilNext(0),
      m_framesReady(0), m_lateByFrame(0), m_audioSyncDrops(0), m_videoPacketHighWater(0),
      m_audioWorkTicks(0), m_maxLate(0),
      m_parseCalls(0), m_parseTicks(0), m_parseFrequency(0),
      m_readTicks(0), m_uploadTicks(0), m_gpuWaitTicks(0),
      m_inputWaitTicks(0),
      m_lastDecodeTicks(0), m_lastCopyTicks(0), m_lastBwdifTicks(0),
      m_lastDecodeCpuMs(-1.0),
      m_worstReadTicks(0), m_readCalls(0), m_readBytes(0),
      m_underruns(0), m_textureSet(0),
#ifdef DVD360_FFMPEG_ALTIVEC
      m_decodeMode("FFmpeg MPEG-2 (experimental AltiVec, scalar IDCT)"),
#else
      m_decodeMode("FFmpeg MPEG-2 (scalar)"),
#endif
      m_vs(NULL), m_ps(NULL), m_decl(NULL)
{
    memset(m_planes, 0, sizeof(m_planes));
}

DvdFrameProbe::~DvdFrameProbe()
{
    CloseDecoder();
    for (unsigned set = 0; set < 3; ++set)
        for (unsigned i = 0; i < 3; ++i)
            if (m_planes[set][i]) m_planes[set][i]->Release();
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
}

void DvdFrameProbe::CloseDecoder()
{
    if (!m_decoder)
        return;
    Decoder& d = *m_decoder;
    if (d.stop)
        SetEvent(d.stop);
    if (d.thread)
    {
        WaitForSingleObject(d.thread, INFINITE);
        CloseHandle(d.thread);
    }
    if (d.ready) CloseHandle(d.ready);
    if (d.primed) CloseHandle(d.primed);
    if (d.seekRequest) CloseHandle(d.seekRequest);
    if (d.seekComplete) CloseHandle(d.seekComplete);
    if (d.freeSlots) CloseHandle(d.freeSlots);
    if (d.stop) CloseHandle(d.stop);
    delete d.audio;
    d.DrainDeferredAudio(false);
    for (unsigned i = 0; i < d.cueCount; ++i)
        avsubtitle_free(&d.cues[i].bitmap);
    avsubtitle_free(&d.heldMenuCue.bitmap);
    for (unsigned i = 0; i < d.subtitleCount; ++i)
        avcodec_free_context(&d.subtitleCodec[i]);
    for (unsigned i = 0; i < Decoder::PacketCapacity; ++i)
        av_packet_unref(&d.pendingVideo[i]);
    av_packet_free(&d.packet);
    avfilter_graph_free(&d.videoFilter);
    av_frame_free(&d.decoded);
    avcodec_free_context(&d.codec);
    avformat_close_input(&d.format);
    DestroyFfmpegFileIo(&d.io);
    DeleteCriticalSection(&d.queueStats);

    delete m_decoder;
    m_decoder = NULL;
}

DWORD WINAPI DvdFrameProbe::DecodeWorker(void* context)
{
    DvdFrameProbe& player = *static_cast<DvdFrameProbe*>(context);
    if (PlaybackAffinity::Enabled())
        PlaybackAffinity::Pin(PlaybackAffinity::Decoder);
    Decoder& d = *player.m_decoder;
    HANDLE wait[] = {d.stop, d.seekRequest, d.freeSlots};
    unsigned initialFrames = 0;
    bool primed = false;
    bool refillAudio = false;
    bool urgentAudio = false;
    bool seeking = false;
    bool ended = false;
    const unsigned audioLow = 8, audioTarget = 16, videoPacketTarget = 12;
    const unsigned audioCritical = 3, audioResumeVideo = 6;
    for (;;)
    {
        if (WaitForSingleObject(d.stop, 0) == WAIT_OBJECT_0) break;
        if (seeking && d.discAudioHandedOff)
            seeking = false;
        if (WaitForSingleObject(d.seekRequest, 0) == WAIT_OBJECT_0)
        {
            ResetEvent(d.seekRequest);
            if (d.audioSwitchRequest)
            {
                d.audioSwitchRequest = 0;
                d.audioSwitchStatus = S_FALSE;
                const unsigned count = d.format->nb_streams;
                const unsigned current = static_cast<unsigned>(d.audioStream);
                for (unsigned step = 1; !ended && step < count; ++step)
                {
                    const unsigned index = (current + step) % count;
                    AVStream& stream = *d.format->streams[index];
                    if (stream.codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
                        !avcodec_find_decoder(stream.codecpar->codec_id)) continue;
                    FfmpegAudio* replacement = new FfmpegAudio();
                    if (!replacement) continue;
                    if (FAILED(replacement->Open(stream)))
                    {
                        delete replacement;
                        continue;
                    }
                    delete d.audio;
                    d.audio = replacement;
                    d.audioStream = index;
                    d.audioLanguage = d.ProbeTrackLanguage(false, index);
                    d.audioFailed = false;
                    d.audioSwitchStatus = S_OK;
                    break;
                }
                SetEvent(d.seekComplete);
                continue;
            }
            if (!ResumeFfmpegDiscWait(d.io, true))
            {
                d.seekStatus = E_FAIL;
                SetEvent(d.seekComplete);
                continue;
            }
            // Disc resumes free the old objects below. A NAV-only still
            // may never have allocated a frame, packet, or codec at all.
            for (unsigned i = 0; i < Decoder::PacketCapacity; ++i)
                av_packet_unref(&d.pendingVideo[i]);
            EnterCriticalSection(&d.queueStats);
            for (unsigned i = 0; i < d.cueCount; ++i) avsubtitle_free(&d.cues[i].bitmap);
            d.cueCount = d.queuedFrames = 0;
            LeaveCriticalSection(&d.queueStats);
            for (unsigned i = 0; i < d.subtitleCount; ++i)
            {
                avcodec_free_context(&d.subtitleCodec[i]);
            }
            {
                delete d.audio;
                d.audio = NULL;
                d.audioFailed = false;
                d.subtitleCount = 0;
                av_packet_free(&d.packet);
                avfilter_graph_free(&d.videoFilter);
                d.videoSource = d.videoSink = NULL;
                d.videoFilterEnded = false;
                av_frame_free(&d.decoded);
                avcodec_free_context(&d.codec);
                avformat_close_input(&d.format);
            }
            while (WaitForSingleObject(d.ready, 0) == WAIT_OBJECT_0) {}
            while (WaitForSingleObject(d.freeSlots, 0) == WAIT_OBJECT_0) {}
            ReleaseSemaphore(d.freeSlots, Decoder::QueueCapacity, NULL);
            d.readIndex = d.writeIndex = d.packetHead = d.packetCount = d.packetBytes = 0;
            d.demuxEnd = 0;
            d.emptyDiscBoundary = false;
            d.DrainDeferredAudio(false);
            d.deferStartupAudio = true;
            d.discAudioHandoffArmed = true;
            d.discAudioHandedOff = false;
            d.flushed = false;
            d.seekStatus = S_OK;
            initialFrames = 0;
            primed = refillAudio = urgentAudio = ended = false;
            seeking = true;
        }
        if (ended)
        {
            WaitForMultipleObjects(2, wait, FALSE, INFINITE);
            continue;
        }
        if (!d.deferStartupAudio && d.deferredHead)
            d.DrainDeferredAudio(true);
        const bool haveAudio = d.audio && !d.audioFailed;
        const unsigned audioBuffers = haveAudio ? d.audio->QueuedBuffers() : 0;
        const bool canReadAhead = d.format && !d.demuxEnd &&
            d.packetCount < Decoder::PacketCapacity && d.packetBytes < d.packetBudget;
        const bool audioReady = !haveAudio || audioBuffers >= audioTarget;
        const bool videoReady = d.packetBytes >= d.startupVideoBytes;
        if (!primed && initialFrames == Decoder::QueueCapacity &&
            ((audioReady && videoReady) || !canReadAhead || audioBuffers >= 24))
        {
            d.primedVideoBytes = d.packetBytes;
            SetEvent(d.primed);
            primed = true;
            if (seeking)
            {
                seeking = false;
                d.discAudioHandoffArmed = false;
                SetEvent(d.seekComplete);
            }
        }
        // Normally keep decoded video ahead. If audio is at risk of
        // starving, demux for it even with a shallow decoded-video queue.
        // The renderer drops expired frames against the audio clock.
        refillAudio = haveAudio && audioBuffers < (refillAudio ? audioTarget : audioLow);
        urgentAudio = primed && haveAudio &&
            audioBuffers < (urgentAudio ? audioResumeVideo : audioCritical);
        const bool needAudio = haveAudio && (refillAudio || !primed);
        const bool wantPackets = d.packetCount < videoPacketTarget &&
            (!haveAudio || audioBuffers < audioTarget);
        const bool readAhead = canReadAhead &&
            (needAudio || wantPackets || (!primed && !videoReady));
        const unsigned depth = player.QueueDepth();
        if (readAhead && primed &&
            (urgentAudio || (refillAudio && depth >= Decoder::QueueCapacity / 2)))
        {
            if (WaitForSingleObject(d.stop, 0) != WAIT_TIMEOUT)
                break;
            d.PumpPacket();
            continue;
        }
        const DWORD available = WaitForMultipleObjects(3, wait, FALSE,
            readAhead ? 0 : canReadAhead && haveAudio ? 10 : INFINITE);
        if (available == WAIT_TIMEOUT)
        {
            if (readAhead)
                d.PumpPacket();
            continue;
        }
        if (available == WAIT_OBJECT_0 + 1) continue;
        if (available != WAIT_OBJECT_0 + 2)
            break;
        QueuedFrame& frame = d.queue[d.writeIndex];
        LARGE_INTEGER start, finish;
#ifdef DVD360_LOGGING
        ULONGLONG cpuStart = 0, cpuFinish = 0;
        const bool haveCpuStart = DecodeCpuTime(cpuStart);
#endif
        QueryPerformanceCounter(&start);
        const HRESULT status = player.NextFrame(frame);
        if (status == S_FALSE && seeking && d.discResumeRequest && !initialFrames &&
            FfmpegDiscBoundaryPending(d.io))
            d.emptyDiscBoundary = true;
        QueryPerformanceCounter(&finish);
#ifdef DVD360_LOGGING
        frame.decodeCpuMs = haveCpuStart && DecodeCpuTime(cpuFinish) && cpuFinish >= cpuStart
            ? (cpuFinish - cpuStart) / 10000.0 : -1.0;
#else
        frame.decodeCpuMs = -1.0;
#endif
        frame.decodeTicks = finish.QuadPart - start.QuadPart;
        frame.status = status;
        frame.calls = d.calls;
        frame.parseTicks = d.parseTicks;
        frame.readTicks = d.ioStats.ticks;
        frame.inputWaitTicks = d.inputWaitTicks;
        frame.audioWorkTicks = d.audioWorkTicks;
        frame.videoPacketHighWater = d.packetHighWater;
        frame.worstReadTicks = d.ioStats.worstTicks;
        frame.readCalls = d.ioStats.calls;
        frame.readBytes = d.ioStats.bytes;
        d.writeIndex = (d.writeIndex + 1) % Decoder::QueueCapacity;
        if (status == S_OK)
        {
            EnterCriticalSection(&d.queueStats);
            ++d.queuedFrames;
            LeaveCriticalSection(&d.queueStats);
        }
        ReleaseSemaphore(d.ready, 1, NULL);
        if (!primed)
            ++initialFrames;
        if (status != S_OK)
            SetEvent(d.primed);
        if (status != S_OK)
        {
            if (d.audio)
                d.audio->Finish();
            ended = true;
            if (seeking)
            {
                seeking = false;
                d.discAudioHandoffArmed = false;
                if (!d.discAudioHandedOff)
                {
                    d.seekStatus = initialFrames > 1 || d.emptyDiscBoundary ? S_OK : E_FAIL;
                    SetEvent(d.seekComplete);
                }
            }
        }
    }
    return 0;
}

HRESULT DvdFrameProbe::Consume(IDirect3DDevice9* device, DWORD timeout, bool audioMaster)
{
    Decoder& d = *m_decoder;
    const DWORD wait = WaitForSingleObject(d.ready, timeout);
    if (wait == WAIT_TIMEOUT)
        return E_PENDING;
    if (wait != WAIT_OBJECT_0)
        return E_FAIL;
    QueuedFrame& frame = d.queue[d.readIndex];
    m_parseCalls = frame.calls;
    m_parseTicks = frame.parseTicks;
    m_readTicks = frame.readTicks;
    m_inputWaitTicks = frame.inputWaitTicks;
    m_audioWorkTicks = frame.audioWorkTicks;
    m_videoPacketHighWater = frame.videoPacketHighWater;
    m_worstReadTicks = frame.worstReadTicks;
    m_readCalls = frame.readCalls;
    m_readBytes = frame.readBytes;
    HRESULT hr = frame.status;
    if (hr == S_OK)
    {
        EnterCriticalSection(&d.queueStats);
        --d.queuedFrames;
        LeaveCriticalSection(&d.queueStats);
        m_lastDecodeTicks = frame.decodeTicks;
        m_lastDecodeCpuMs = frame.decodeCpuMs;
        m_lastCopyTicks = frame.copyTicks;
        m_lastBwdifTicks = frame.bwdifTicks;
        const unsigned luma = frame.width * frame.height;
        const unsigned chroma = ((frame.width + 1) / 2) * ((frame.height + 1) / 2);
        const BYTE* planes[] = {frame.pixels, frame.pixels + luma, frame.pixels + luma + chroma};
        const double late = -m_untilNext;
        const bool firstDiscPicture = d.needsFirstDiscPicture;
        const bool drop = !firstDiscPicture && audioMaster && late >= frame.duration;
        if (!drop)
            hr = LoadPlanes(device, frame.width, frame.height, planes, frame.width);
        if (SUCCEEDED(hr))
        {
            if (!drop)
            {
                if (firstDiscPicture)
                {
                    d.needsFirstDiscPicture = false;
                    m_untilNext = 0;
                }
                d.presentedDvdPts = frame.dvdPts;
                d.presentedSubtitleTime = frame.subtitleTime;
                m_deinterlacingActive = frame.deinterlaced;
                ++m_framesReady;
            }
            else
                ++m_audioSyncDrops;
            if (late > m_maxLate) m_maxLate = late;
            if (late >= frame.duration) ++m_lateByFrame;
            m_untilNext += frame.duration;
            // Keep sub-refresh timing error for field-rate video even when
            // there is no audio clock, but don't accumulate excess debt.
            if (!audioMaster && m_untilNext < (frame.filtered ? -frame.duration : 0))
                m_untilNext = frame.filtered ? -frame.duration : 0;
        }
    }
    d.readIndex = (d.readIndex + 1) % Decoder::QueueCapacity;
    ReleaseSemaphore(d.freeSlots, 1, NULL);
    return hr;
}

HRESULT DvdFrameProbe::DecodeMpeg2(IDirect3DDevice9* device)
{
    for (unsigned set = 0; set < 3; ++set)
        for (unsigned i = 0; i < 3; ++i)
            if (m_planes[set][i])
            {
                m_planes[set][i]->Release();
                m_planes[set][i] = NULL;
            }
    m_width = m_height = m_textureSet = m_discSegment = 0;
    m_untilNext = 0;
    m_deinterlacingActive = false;
#ifndef DVD360_LOGGING
    av_log_set_level(AV_LOG_QUIET);
#endif
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency))
        m_parseFrequency = frequency.QuadPart;
    m_decoder = new Decoder();
    if (!m_decoder)
    {
        return E_OUTOFMEMORY;
    }
    InitializeCriticalSection(&m_decoder->queueStats);
    m_decoder->disableDeinterlace =
        GetFileAttributesA("game:\\bwdif-disable.txt") != static_cast<DWORD>(-1);
    m_decoder->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    const LONG capacity = Decoder::QueueCapacity;
    m_decoder->freeSlots = CreateSemaphore(NULL, capacity, capacity, NULL);
    m_decoder->ready = CreateSemaphore(NULL, 0, capacity, NULL);
    m_decoder->primed = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_decoder->seekRequest = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_decoder->seekComplete = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (m_decoder->stop && m_decoder->freeSlots && m_decoder->ready && m_decoder->primed &&
        m_decoder->seekRequest && m_decoder->seekComplete)
        m_decoder->thread = CreateThread(NULL, 0, DecodeWorker, this, 0, NULL);
    HRESULT hr = E_FAIL;
    if (m_decoder->thread)
    {
        HANDLE wait[] = {m_decoder->primed, m_decoder->thread};
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0)
            hr = Consume(device, 0);
    }
    if (hr != S_OK)
        CloseDecoder();
    return hr == S_FALSE ? E_FAIL : hr;
}

HRESULT DvdFrameProbe::Advance(IDirect3DDevice9* device, double elapsed, bool audioMaster, double wallElapsed)
{
    if (!m_decoder)
        return S_OK;
    Decoder& d = *m_decoder;
    if (!d.discResuming && FfmpegDiscUserJumpPending(d.io))
    {
        // An explicit VM jump replaces the old menu. Unlike an authored
        // WAIT, it must not play the remaining menu audio before switching.
        ResetEvent(d.seekComplete);
        d.discResuming = d.discResumeRequest = true;
        SetEvent(d.seekRequest);
        return S_OK;
    }
    if (d.discResuming)
    {
        if (WaitForSingleObject(d.seekComplete, 0) != WAIT_OBJECT_0) return S_OK;
        d.discResuming = d.discResumeRequest = false;
        if (FAILED(d.seekStatus))
        {
            const HRESULT status = d.seekStatus;
            CloseDecoder();
            return status;
        }
        d.videoEnded = false;
        d.stillPresented = d.stillContinueQueued = false;
        d.stillElapsed = 0;
        if (!d.emptyDiscBoundary)
        {
            avsubtitle_free(&d.heldMenuCue.bitmap);
            d.needsFirstDiscPicture = true;
            m_deinterlacingActive = false;
            ++m_discSegment;
        }
        m_untilNext = 0;
        elapsed = 0;
    }
    HRESULT hr = S_FALSE;
    if (!d.videoEnded)
    {
        m_untilNext -= elapsed;
        if (m_untilNext > 0) return S_OK;
        unsigned remaining = Decoder::QueueCapacity;
        do
        {
            hr = Consume(device, 0, audioMaster);
        } while (audioMaster && hr == S_OK && m_untilNext <= 0 && --remaining);
    }
    if (hr == E_PENDING)
    {
        ++m_underruns;
        return S_OK; // Hold the previous frame and retry at the next refresh.
    }
    if (hr == S_FALSE)
    {
        d.videoEnded = true;
        if (d.audio && d.audio->IsPlaying()) return S_OK;
        if (ResumeFfmpegDiscWait(d.io, false))
        {
            ResetEvent(d.seekComplete);
            d.discResuming = d.discResumeRequest = true;
            SetEvent(d.seekRequest);
            return S_OK;
        }
        if (FfmpegDiscBoundaryPending(d.io) && !FfmpegDiscMenuState(d.io).stillSeconds)
            return S_OK; // A queued menu command takes precedence over acknowledging WAIT.
        const DvdMenuState menu = FfmpegDiscMenuState(d.io);
        if (menu.stillSeconds || d.stillPresented)
        {
            if (d.stillPresented) d.stillElapsed += wallElapsed;
            d.stillPresented = true;
            if (menu.stillSeconds && menu.stillSeconds < 255 &&
                d.stillElapsed >= menu.stillSeconds && !d.stillContinueQueued)
                d.stillContinueQueued = QueueFfmpegDiscMenu(d.io, DvdMenuContinue);
            return S_OK;
        }
    }
    if (hr != S_OK)
        CloseDecoder();
    return hr;
}

void DvdFrameProbe::Stop()
{
    if (m_decoder)
    {
        SetEmbeddedAudioPaused(true);
        CloseDecoder();
    }
}

double DvdFrameProbe::ParseMilliseconds() const
{
    return m_parseFrequency ? m_parseTicks * 1000.0 / m_parseFrequency : 0.0;
}

double DvdFrameProbe::ReadMilliseconds() const
{ return m_parseFrequency ? m_readTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::InputWaitMilliseconds() const
{ return m_parseFrequency ? m_inputWaitTicks * 1000.0 / m_parseFrequency : 0.0; }

unsigned DvdFrameProbe::QueueDepth() const
{
    if (!m_decoder)
        return 0;
    EnterCriticalSection(&m_decoder->queueStats);
    const unsigned depth = m_decoder->queuedFrames;
    LeaveCriticalSection(&m_decoder->queueStats);
    return depth;
}
double DvdFrameProbe::LastDecodeMilliseconds() const
{ return m_parseFrequency ? m_lastDecodeTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::LastCopyMilliseconds() const
{ return m_parseFrequency ? m_lastCopyTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::LastBwdifMilliseconds() const
{ return m_parseFrequency ? m_lastBwdifTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::WorstReadMilliseconds() const
{ return m_parseFrequency ? m_worstReadTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::UploadMilliseconds() const
{ return m_parseFrequency ? m_uploadTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::GpuWaitMilliseconds() const
{ return m_parseFrequency ? m_gpuWaitTicks * 1000.0 / m_parseFrequency : 0.0; }

HRESULT DvdFrameProbe::NextFrame(QueuedFrame& frame)
{
    Decoder& d = *m_decoder;
    if (!d.codec)
    {
        HRESULT inputStatus = E_FAIL;
        if (!d.io)
            d.io = CreateFfmpegDiscIo(&d.ioStats, inputStatus);
        if (!d.io) return inputStatus;
        d.format = d.io ? avformat_alloc_context() : NULL;
        if (!d.format)
            return E_OUTOFMEMORY;
        d.format->pb = d.io;
        d.format->flags |= AVFMT_FLAG_CUSTOM_IO;
         LogDvdNavStartup("Opening FFmpeg MPEG program stream");
        const AVInputFormat* inputFormat = av_find_input_format("mpeg");
        const int opened = avformat_open_input(&d.format, NULL, inputFormat, NULL);
        if (opened >= 0) LogDvdNavStartup("Probing FFmpeg streams");
        const int probed = opened < 0 ? opened : avformat_find_stream_info(d.format, NULL);
        const AVCodec* codec = NULL;
        d.video = probed < 0 ? -1 :
            av_find_best_stream(d.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (d.video < 0 || !codec || codec->id != AV_CODEC_ID_MPEG2VIDEO)
        {
            if (d.discResumeRequest && FfmpegDiscBoundaryPending(d.io))
            {
                d.emptyDiscBoundary = true;
                LogDvdNavStartup("DVD boundary without new video; retaining last frame");
                return S_FALSE;
            }
            return probed < 0 ? E_FAIL : E_INVALIDARG;
        }
         LogDvdNavStartup("FFmpeg stream probe complete");
        d.codec = avcodec_alloc_context3(codec);
        d.decoded = av_frame_alloc();
        d.packet = av_packet_alloc();
        if (!d.codec || !d.decoded || !d.packet)
            return E_OUTOFMEMORY;
        AVStream* video = d.format->streams[d.video];
        d.duration = d.format->duration > 0 ? d.format->duration / double(AV_TIME_BASE) : 0;

        {
            const double duration = FfmpegDiscDuration(d.io);
            if (duration > 0) d.duration = duration;
        }
        const AVRational sar = av_guess_sample_aspect_ratio(d.format, video, NULL);
        if (sar.num > 0 && sar.den > 0 && video->codecpar->width > 0 && video->codecpar->height > 0)
            d.displayAspect = static_cast<float>(av_q2d(sar) * video->codecpar->width /
                                                  video->codecpar->height);
        d.frameRate = av_q2d(av_guess_frame_rate(d.format, video, NULL));
        // Target 750 ms of compressed video. Use the mux rate as an upper
        // estimate if the video rate is unknown, then fall back to 6 Mb/s.
        const int64_t bitrate = video->codecpar->bit_rate > 0 ? video->codecpar->bit_rate :
            d.format->bit_rate > 0 ? d.format->bit_rate : 6000000;
        MEMORYSTATUS memory = {};
        memory.dwLength = sizeof(memory);
        GlobalMemoryStatus(&memory);
        const unsigned ramBudget = memory.dwAvailPhys / 32;
        d.packetBudget = ramBudget < Decoder::PacketByteBudget ? ramBudget : Decoder::PacketByteBudget;
        const double target = bitrate * (0.75 / 8.0);
        d.startupVideoBytes = target < d.packetBudget ? static_cast<unsigned>(target) : d.packetBudget;
        if (avcodec_parameters_to_context(d.codec, video->codecpar) < 0)
            return E_FAIL;
        d.codec->pkt_timebase = video->time_base;
        d.codec->thread_count = 1;
#ifdef DVD360_FFMPEG_ALTIVEC
#ifdef DVD360_LOGGING
        FILE* singleThread = fopen("game:\\ffmpeg-single-thread.enable", "rb");
        if (singleThread)
            fclose(singleThread);
        else
#endif
        {
            PlaybackAffinity::Pin(PlaybackAffinity::Decoder);
            d.codec->thread_count = 2;
            d.codec->thread_type = FF_THREAD_SLICE;
        }
        d.codec->idct_algo = FF_IDCT_ALTIVEC;
        m_decodeMode = "FFmpeg MPEG-2 (AltiVec and Xenon IDCT)";
#ifdef DVD360_LOGGING
        FILE* scalarIdct = fopen("game:\\ffmpeg-scalar-idct.enable", "rb");
        if (scalarIdct)
        {
            fclose(scalarIdct);
            d.codec->idct_algo = FF_IDCT_SIMPLE;
            m_decodeMode = "FFmpeg MPEG-2 (AltiVec, scalar IDCT)";
        }
#endif
#endif
        if (avcodec_open2(d.codec, codec, NULL) < 0)
            return E_FAIL;
        const AVCodec* audioCodec = NULL;
        d.audioStream = av_find_best_stream(d.format, AVMEDIA_TYPE_AUDIO,
                                            -1, d.video, &audioCodec, 0);

        {
            const int physical = FfmpegDiscAudioStream(d.io);
            for (unsigned i = 0; physical >= 0 && i < d.format->nb_streams; ++i)
            {
                const AVStream& stream = *d.format->streams[i];
                const unsigned id = stream.id;
                if (stream.codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
                    !((id >= 0x80 && id <= 0x8f) || (id >= 0xa0 && id <= 0xa7) ||
                      (id >= 0x1c0 && id <= 0x1c7)) ||
                    (id & 7) != static_cast<unsigned>(physical)) continue;
                const AVCodec* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
                if (!decoder) continue;
                d.audioStream = i;
                audioCodec = decoder;
                break;
            }
        }
#ifdef DVD360_LOGGING
        FILE* trackFile = fopen("game:\\audio-track.txt", "r");
        if (trackFile)
        {
            char text[24] = {};
            unsigned wanted = fgets(text, sizeof(text), trackFile) ? strtoul(text, NULL, 10) : 0;
            fclose(trackFile);
            for (unsigned i = 0; wanted && i < d.format->nb_streams; ++i)
            {
                const AVCodecParameters* par = d.format->streams[i]->codecpar;
                const AVCodec* candidate = par->codec_type == AVMEDIA_TYPE_AUDIO
                    ? avcodec_find_decoder(par->codec_id) : NULL;
                if (candidate && --wanted == 0)
                {
                    d.audioStream = i;
                    audioCodec = candidate;
                    break;
                }
            }
        }
#endif
        for (unsigned i = 0; i < d.format->nb_streams && d.subtitleCount < Decoder::SubtitleTracks; ++i)
            d.AddSubtitleStream(i);
        d.audioLanguage = d.ProbeTrackLanguage(false, d.audioStream);
        if (d.audioStream >= 0 && audioCodec)
        {
            d.audio = new FfmpegAudio();
            if (d.audio && FAILED(d.audio->Open(*d.format->streams[d.audioStream])))
            {
                delete d.audio;
                d.audio = NULL;
            }
            else if (d.audio && d.discAudioHandoffArmed)
                d.audio->AllowPrimedStart();
        }

        {
            char message[192];
            sprintf_s(message, sizeof(message),
                "Track probe: audio_stream=%d decoder=%s opened=%u language=%s subtitle_tracks=%u",
                d.audioStream, audioCodec ? audioCodec->name : "none", d.audio ? 1u : 0u,
                d.audioLanguage ? d.audioLanguage : "unavailable", d.subtitleCount);
            LogDvdNavStartup(message);
            for (unsigned i = 0; i < d.subtitleCount; ++i)
            {
                sprintf_s(message, sizeof(message), "Subtitle %u: stream=%d language=%s",
                    i + 1, d.subtitleStream[i],
                    d.subtitleLanguage[i] ? d.subtitleLanguage[i] : "unavailable");
                LogDvdNavStartup(message);
            }
        }
    }

    const AVRational inputTimeBase = d.format->streams[d.video]->time_base;
    LONGLONG bwdifTicks = 0;
    for (;;)
    {
        if (WaitForSingleObject(d.stop, 0) == WAIT_OBJECT_0)
            return E_ABORT;
        if (WaitForSingleObject(d.seekRequest, 0) == WAIT_OBJECT_0 &&
            FfmpegDiscUserJumpPending(d.io))
            return S_FALSE;
        if (d.videoFilter)
        {
            LARGE_INTEGER filterStart, filterEnd;
            QueryPerformanceCounter(&filterStart);
            const int filtered = av_buffersink_get_frame(d.videoSink, d.decoded);
            QueryPerformanceCounter(&filterEnd);
            bwdifTicks += filterEnd.QuadPart - filterStart.QuadPart;
            if (filtered == 0) break;
            if (filtered == AVERROR_EOF) return S_FALSE;
            if (filtered != AVERROR(EAGAIN) || d.videoFilterEnded)
                return VideoPipelineError("bwdif output", filtered);
        }
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        const int status = avcodec_receive_frame(d.codec, d.decoded);
        QueryPerformanceCounter(&end);
        d.parseTicks += end.QuadPart - start.QuadPart;
        ++d.calls;
        if (status == AVERROR_EOF)
        {
            if (d.deferStartupAudio || d.deferredHead)
            {
                d.deferStartupAudio = false;
                d.DrainDeferredAudio(true);
                d.DecodeAudioPacket(NULL);
            }
            if (!d.videoFilter) return S_FALSE;
            const int drain = av_buffersrc_add_frame(d.videoSource, NULL);
            if (drain < 0) return VideoPipelineError("bwdif drain", drain);
            d.videoFilterEnded = true;
            continue;
        }
        if (status == 0)
        {
            d.deferStartupAudio = false;
            AVFrame& picture = *d.decoded;
            if (picture.format != AV_PIX_FMT_YUV420P ||
                picture.width <= 0 || picture.width > int(kWidth) ||
                picture.height <= 0 || picture.height > int(kHeight))
                return E_INVALIDARG;
            // Pure progressive streams avoid filter allocation and lookahead.
            if (!d.videoFilter &&
                (d.disableDeinterlace || !(picture.flags & AV_FRAME_FLAG_INTERLACED)))
                break;
            if (!d.videoFilter)
            {
                const int opened = d.OpenVideoFilter(picture, inputTimeBase);
                if (opened < 0) return VideoPipelineError("bwdif setup", opened);
            }
            picture.pts = picture.best_effort_timestamp;
            // bwdif copies opaque to its output and clears the interlaced
            // flag only when it actually reconstructs the picture.
            picture.opaque = (picture.flags & AV_FRAME_FLAG_INTERLACED) ? &d : NULL;
            if (picture.duration <= 0)
                picture.duration = static_cast<int64_t>(
                    (1.0 + picture.repeat_pict * 0.5) /
                    ((d.frameRate > 0 ? d.frameRate : 25.0) * av_q2d(inputTimeBase)) + 0.5);
            LARGE_INTEGER pushStart, pushEnd;
            QueryPerformanceCounter(&pushStart);
            const int pushed = av_buffersrc_add_frame(d.videoSource, &picture);
            QueryPerformanceCounter(&pushEnd);
            bwdifTicks += pushEnd.QuadPart - pushStart.QuadPart;
            if (pushed < 0) return VideoPipelineError("bwdif input", pushed);
            continue;
        }
        if (status != AVERROR(EAGAIN) || d.flushed)
            return VideoPipelineError("decoder receive", status);

        if (d.packetCount)
        {
            AVPacket* packet = &d.pendingVideo[d.packetHead];
            QueryPerformanceCounter(&start);
            const int sent = avcodec_send_packet(d.codec, packet);
            QueryPerformanceCounter(&end);
            d.parseTicks += end.QuadPart - start.QuadPart;
            d.packetBytes -= packet->size;
            av_packet_unref(packet);
            d.packetHead = (d.packetHead + 1) % Decoder::PacketCapacity;
            --d.packetCount;
            if (sent < 0)
                return E_FAIL;
        }
        else if (!d.demuxEnd)
            d.PumpPacket();
        else if (d.demuxEnd == AVERROR_EOF)
        {
            d.flushed = true;
            const int flush = avcodec_send_packet(d.codec, NULL);
            if (flush < 0)
                if (flush != AVERROR_EOF) return E_FAIL;
        }
        else
            return E_FAIL;
    }

    const AVFrame& decoded = *d.decoded;
    frame.bwdifTicks = bwdifTicks;
    if (decoded.format != AV_PIX_FMT_YUV420P ||
        decoded.width <= 0 || decoded.width > int(kWidth) ||
        decoded.height <= 0 || decoded.height > int(kHeight))
        return E_INVALIDARG;
    frame.width = decoded.width;
    frame.height = decoded.height;
    frame.filtered = d.videoFilter != NULL;
    frame.deinterlaced = frame.filtered && !(decoded.flags & AV_FRAME_FLAG_INTERLACED);
    const AVRational timeBase = d.videoFilter ? av_buffersink_get_time_base(d.videoSink) : inputTimeBase;
    const int64_t pts = d.videoFilter ? decoded.pts : decoded.best_effort_timestamp;
    frame.dvdPts = pts == AV_NOPTS_VALUE ? -1 :
        av_rescale_q(pts, timeBase, av_make_q(1, 90000));
    frame.subtitleTime = frame.dvdPts < 0 ? -1.0 : frame.dvdPts / 90000.0 -
        (d.format->start_time == AV_NOPTS_VALUE ? 0 : d.format->start_time / double(AV_TIME_BASE));
    const double rate = d.frameRate;
    frame.duration = decoded.duration > 0
        ? decoded.duration * av_q2d(timeBase)
        : (1.0 + decoded.repeat_pict * 0.5) / (rate > 0 ? rate : 25.0);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    BYTE* dest = frame.pixels;
    for (unsigned i = 0; i < 3; ++i)
    {
        const unsigned width = i ? (frame.width + 1) / 2 : frame.width;
        const unsigned height = i ? (frame.height + 1) / 2 : frame.height;
        const BYTE* source = decoded.data[i];
        if (decoded.linesize[i] == int(width))
        {
            memcpy(dest, source, width * height);
            dest += width * height;
        }
        else
            for (unsigned row = 0; row < height; ++row, dest += width)
                memcpy(dest, source + int(row) * decoded.linesize[i], width);
    }
    av_frame_unref(d.decoded);
    QueryPerformanceCounter(&end);
    frame.copyTicks = end.QuadPart - start.QuadPart;
    return S_OK;
}

HRESULT DvdFrameProbe::StartEmbeddedAudio()
{
    return m_decoder && m_decoder->audio ? m_decoder->audio->Start() : S_FALSE;
}

bool DvdFrameProbe::RequestNextAudioTrack()
{
    if (!m_decoder || m_decoder->discResuming || m_decoder->audioSwitchPending ||
        !m_decoder->audio) return false;
    Decoder& d = *m_decoder;
    ResetEvent(d.seekComplete);
    d.audioSwitchPending = true;
    d.audioSwitchRequest = 1;
    if (SetEvent(d.seekRequest)) return true;
    d.audioSwitchPending = false;
    d.audioSwitchRequest = 0;
    return false;
}

HRESULT DvdFrameProbe::PollAudioTrack(bool paused)
{
    Decoder& d = *m_decoder;
    if (WaitForSingleObject(d.seekComplete, 0) != WAIT_OBJECT_0) return E_PENDING;
    d.audioSwitchPending = false;
    if (d.audioSwitchStatus != S_OK) return d.audioSwitchStatus;
    return paused ? S_OK : d.audio->Start();
}

HRESULT DvdFrameProbe::SetEmbeddedAudioPaused(bool paused)
{
    return m_decoder && !m_decoder->discResuming && m_decoder->audio
        ? m_decoder->audio->SetPaused(paused) : E_FAIL;
}

double DvdFrameProbe::DurationSeconds() const
{
    return m_decoder && !m_decoder->discResuming ? m_decoder->duration : 0;
}

float DvdFrameProbe::DisplayAspect() const
{
    return m_decoder && !m_decoder->discResuming && m_decoder->displayAspect > 0
        ? m_decoder->displayAspect : 4.0f / 3.0f;
}

double DvdFrameProbe::AdvanceEmbeddedAudio(double fallback)
{
    if (m_decoder && m_decoder->discResuming) return 0;
    return m_decoder && m_decoder->audio
        ? m_decoder->audio->Advance(fallback, QueueDepth() == Decoder::QueueCapacity)
        : fallback;
}

bool DvdFrameProbe::DiscTransitionPending() const
{
    return m_decoder && (m_decoder->discResuming || FfmpegDiscUserJumpPending(m_decoder->io));
}

DvdMenuState DvdFrameProbe::DiscMenuState() const
{
    DvdMenuState state = {};
    if (!m_decoder || m_decoder->discResuming) return state;
    if (m_decoder->needsFirstDiscPicture && !m_decoder->stillPresented) return state;
    state = FfmpegDiscMenuState(m_decoder->io, m_decoder->presentedDvdPts);
    if (state.stillSeconds && !m_decoder->stillPresented)
        return DvdMenuState();
    return state;
}

bool DvdFrameProbe::DiscMenuInput(DvdMenuCommand command)
{
    if (command >= DvdMenuUp && command <= DvdMenuActivate && !DiscMenuState().buttons)
        return false;
    return m_decoder && !m_decoder->discResuming &&
        QueueFfmpegDiscMenu(m_decoder->io, command, m_decoder->presentedDvdPts);
}

void DvdFrameProbe::DrawDiscHighlight(IDirect3DDevice9* device, PlaybackOsd& osd,
                                       unsigned width, unsigned height, unsigned mode) const
{
    const RECT area = DiscMenuState().highlight;
    if (!m_width || !m_height || area.right <= area.left || area.bottom <= area.top) return;
    float x, y, u, v;
    DisplayGeometry(width, height, mode, x, y, u, v);
    const float sw = 720 * x / (1 - 2 * u), sh = 480 * y / (1 - 2 * v);
    const float sx = sw / m_width, sy = sh / m_height;
    osd.DrawMenuFocus(device, (720 - sw) / 2 + area.left * sx,
                      (480 - sh) / 2 + area.top * sy,
                      (area.right - area.left) * sx, (area.bottom - area.top) * sy);
}

unsigned DvdFrameProbe::AudioRecoveryTicks() const
{
    return m_decoder && !m_decoder->discResuming && !m_decoder->audioSwitchPending && m_decoder->audio
        ? m_decoder->audio->RecoveryTicks() : 0;
}

double DvdFrameProbe::AudioWorkMilliseconds() const
{
    return m_parseFrequency ? m_audioWorkTicks * 1000.0 / m_parseFrequency : 0.0;
}

unsigned DvdFrameProbe::ActiveSliceThreads() const
{
    const AVCodecContext* codec = m_decoder ? m_decoder->codec : NULL;
    return codec && (codec->active_thread_type & FF_THREAD_SLICE) ? codec->thread_count : 0;
}

void DvdFrameProbe::StartupVideoBuffer(unsigned& queued, unsigned& target, unsigned& budget) const
{
    queued = m_decoder ? m_decoder->primedVideoBytes : 0;
    target = m_decoder ? m_decoder->startupVideoBytes : 0;
    budget = m_decoder ? m_decoder->packetBudget : 0;
}

bool DvdFrameProbe::EmbeddedAudioState(double& milliseconds, unsigned& buffers) const
{
    if (!m_decoder || m_decoder->discResuming || m_decoder->audioSwitchPending || !m_decoder->audio)
        return false;
    UINT64 samples;
    m_decoder->audio->GetPlaybackState(samples, buffers);
    milliseconds = samples * (1000.0 / 48000.0);
    return true;
}

HRESULT DvdFrameProbe::LoadPlanes(IDirect3DDevice9* device, unsigned width, unsigned height,
                                  const BYTE* const planes[3], unsigned stride)
{
    if (m_width && (width != m_width || height != m_height))
        return E_INVALIDARG;
    const unsigned next = m_decl ? (m_textureSet + 1) % 3 : 0;
    LARGE_INTEGER start, waited, finish;
    QueryPerformanceCounter(&start);
    for (unsigned i = 0; i < 3; ++i)
        if (m_planes[next][i])
            m_planes[next][i]->BlockUntilNotBusy();
    QueryPerformanceCounter(&waited);
    m_gpuWaitTicks += waited.QuadPart - start.QuadPart;
    for (unsigned i = 0; i < 3; ++i)
    {
        HRESULT hr = UploadPlane(device, planes[i], i ? (stride + 1) / 2 : stride,
                                  i ? (width + 1) / 2 : width,
                                  i ? (height + 1) / 2 : height, &m_planes[next][i]);
        if (FAILED(hr))
            return hr;
    }
    QueryPerformanceCounter(&finish);
    m_uploadTicks += finish.QuadPart - waited.QuadPart;
    m_textureSet = next;
    m_width = width;
    m_height = height;
    if (m_decl)
        return S_OK;
    ID3DXBuffer* code = NULL;
    HRESULT hr = D3DXCompileShader(kVs, (UINT)strlen(kVs), NULL, NULL, "main",
                                   "vs_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr))
        return hr;
    hr = device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &m_vs);
    code->Release();
    if (FAILED(hr))
        return hr;
    hr = D3DXCompileShader(kPs, (UINT)strlen(kPs), NULL, NULL, "main",
                           "ps_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr))
        return hr;
    hr = device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &m_ps);
    code->Release();
    if (FAILED(hr))
        return hr;
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    return device->CreateVertexDeclaration(elements, &m_decl);
}

void DvdFrameProbe::DisplayGeometry(unsigned width, unsigned height, unsigned displayMode,
                                    float& x, float& y, float& u, float& v) const
{
    const float aspect = (float)width / height;
    const float sourceAspect = DisplayAspect();
    x = aspect > sourceAspect ? sourceAspect / aspect : 1.0f;
    y = aspect < sourceAspect ? aspect / sourceAspect : 1.0f;
    u = v = 0;
    if (displayMode == 2) // Fullscreen: fill without distortion, cropping the excess.
    {
        u = (1 - y) / 2;
        v = (1 - x) / 2;
        x = y = 1;
    }
    else if (displayMode == 3)
        x = y = 1;
    else if (displayMode == 4)
    {
        x = (float)m_width / width;
        y = (float)m_height / height;
        const float scale = x > y ? x : y;
        if (scale > 1) { x /= scale; y /= scale; }
    }
    else if (displayMode == 5 || displayMode == PlaybackOsd::StockZoomMode)
    {
        // Enlarge a 4:3 letterboxed transfer uniformly to remove baked-in
        // 16:9 mattes; crop the sides too if the output is narrower.
        const float zoom = 4.0f / 3.0f;
        if (displayMode == PlaybackOsd::StockZoomMode || sourceAspect < 1.4f)
        {
            x *= zoom;
            y *= zoom;
            if (x > 1) { u = (1 - 1 / x) / 2; x = 1; }
            if (y > 1) { v = (1 - 1 / y) / 2; y = 1; }
        }
    }
}

unsigned DvdFrameProbe::SubtitleCount() const
{
    if (!m_decoder || m_decoder->discResuming) return 0;
    Decoder& d = *m_decoder;
    EnterCriticalSection(&d.queueStats);
    const unsigned count = d.subtitleCount;
    LeaveCriticalSection(&d.queueStats);
    return count;
}

const char* DvdFrameProbe::TrackLanguage(bool subtitle, unsigned track) const
{
    if (!m_decoder || m_decoder->discResuming || m_decoder->audioSwitchPending) return NULL;
    const Decoder& d = *m_decoder;
    return subtitle ? (track && track <= SubtitleCount() ? d.subtitleLanguage[track - 1] : NULL)
                    : (d.audio ? d.audioLanguage : NULL);
}

const char* DvdFrameProbe::Decoder::ProbeTrackLanguage(bool subtitle, int index) const
{
    if (index < 0 || static_cast<unsigned>(index) >= format->nb_streams) return NULL;
    const AVStream* stream = format->streams[index];
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", NULL, 0);
    // Return only immutable literals: the demuxer owns its metadata.
    unsigned code = 0;
    if (language && language->value[0] && language->value[1] && !language->value[2])
        code = (static_cast<unsigned char>(language->value[0]) << 8) |
                static_cast<unsigned char>(language->value[1]);
    if (!code)
    {
        const unsigned id = stream->id;
        const bool dvdId = subtitle ? id >= 0x20 && id <= 0x3f :
            (id >= 0x80 && id <= 0x8f) || (id >= 0xa0 && id <= 0xa7) ||
            (id >= 0x1c0 && id <= 0x1c7);
        if (dvdId) code = FfmpegDiscLanguage(io, subtitle, id & (subtitle ? 31 : 7));
    }
    struct Language { const char* two; const char* three; const char* name; };
    static const Language names[] = {
        {"en","eng","English"}, {"fr","fra","French"}, {"fr","fre","French"},
        {"es","spa","Spanish"}, {"de","deu","German"}, {"de","ger","German"},
        {"it","ita","Italian"}, {"pt","por","Portuguese"}, {"ja","jpn","Japanese"},
        {"zh","zho","Chinese"}, {"zh","chi","Chinese"}, {"ko","kor","Korean"},
        {"nl","nld","Dutch"}, {"ru","rus","Russian"}, {"pl","pol","Polish"},
        {"sv","swe","Swedish"}, {"da","dan","Danish"}, {"fi","fin","Finnish"},
        {"no","nor","Norwegian"}, {"ar","ara","Arabic"}, {"cs","ces","Czech"},
        {"el","ell","Greek"}, {"he","heb","Hebrew"}, {"tr","tur","Turkish"}
    };
    for (unsigned i = 0; i < ARRAYSIZE(names); ++i)
        if (code == ((unsigned(names[i].two[0]) << 8) | unsigned(names[i].two[1])) ||
            (language && !strcmp(language->value, names[i].three)))
            return names[i].name;
    return "Unknown language";
}

void DvdFrameProbe::DrawSubtitles(IDirect3DDevice9* device, PlaybackOsd& osd, unsigned width,
                                 unsigned height, unsigned mode, unsigned track, double seconds)
{
    if (!m_decoder || m_decoder->discResuming || !m_width || !m_height) return;
    Decoder& d = *m_decoder;
    const DvdMenuState menu = DiscMenuState();
    if (d.presentedSubtitleTime >= 0 &&
        !d.needsFirstDiscPicture && !menu.buttons)
        seconds = d.presentedSubtitleTime;
    const Decoder::Cue* selected = NULL;
    // Only the render thread removes cues. The decoder only appends.
    EnterCriticalSection(&d.queueStats);
    unsigned kept = 0;
    for (unsigned i = 0; i < d.cueCount; ++i)
    {
        Decoder::Cue& incoming = d.cues[i];
        if (menu.buttons && incoming.streamId == menu.subpictureId &&
            incoming.start <= seconds && (!d.heldMenuCue.bitmap.num_rects ||
            incoming.start >= d.heldMenuCue.start))
        {
            avsubtitle_free(&d.heldMenuCue.bitmap);
            d.heldMenuCue = incoming;
            continue;
        }
        if (d.cues[i].end <= seconds)
            avsubtitle_free(&d.cues[i].bitmap);
        else
        {
            d.cues[kept] = d.cues[i];
            const Decoder::Cue& cue = d.cues[kept++];
            if (cue.track == track && cue.start <= seconds &&
                (!selected || cue.start >= selected->start))
                selected = &cue;
        }
    }
    d.cueCount = kept;
    if (menu.buttons && d.heldMenuCue.streamId == menu.subpictureId &&
        d.heldMenuCue.start <= seconds &&
        (menu.stillSeconds || seconds < d.heldMenuCue.end))
        selected = &d.heldMenuCue;
    LeaveCriticalSection(&d.queueStats);
    if (!selected || !osd.Ready()) return;
    float x, y, u, v;
    DisplayGeometry(width, height, mode, x, y, u, v);
    const float sw = width * x / (1 - 2 * u), sh = height * y / (1 - 2 * v);
    const float sx = sw / m_width, sy = sh / m_height;
    for (unsigned i = 0; i < selected->bitmap.num_rects; ++i)
    {
        const AVSubtitleRect& rect = *selected->bitmap.rects[i];
        if (rect.type == SUBTITLE_BITMAP && rect.data[0] && rect.data[1])
            osd.DrawBitmap(device, ((width - sw) / 2 + rect.x * sx) * 720.0f / width,
                           ((height - sh) / 2 + rect.y * sy) * 480.0f / height,
                           sx * 720.0f / width, sy * 480.0f / height, rect.w, rect.h,
                           rect.data[0], rect.linesize[0], rect.data[1], rect.nb_colors);
    }
}

HRESULT DvdFrameProbe::Draw(IDirect3DDevice9* device, unsigned width, unsigned height,
                            unsigned displayMode) const
{
    float x, y, u, v;
    DisplayGeometry(width, height, displayMode, x, y, u, v);
    const float vertices[][6] = {
        {-x,  y, 0, 1, u, v}, {x,  y, 0, 1, 1-u, v},
        {-x, -y, 0, 1, u, 1-v}, {x, -y, 0, 1, 1-u, 1-v}
    };
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_ps);
    for (unsigned i = 0; i < 3; ++i)
    {
        device->SetTexture(i, m_planes[m_textureSet][i]);
        device->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
}
