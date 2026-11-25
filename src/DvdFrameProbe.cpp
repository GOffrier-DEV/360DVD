#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "DvdFrameProbe.h"

#include "FfmpegFileIo.h"
#include "FfmpegAudio.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace
{
    const unsigned kWidth = 720;
    const unsigned kHeight = 576;
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

    bool DecodeCpuTime(ULONGLONG& ticks)
    {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            return false;
        ticks = ((ULONGLONG)kernel.dwHighDateTime << 32) + kernel.dwLowDateTime
              + ((ULONGLONG)user.dwHighDateTime << 32) + user.dwLowDateTime;
        return true;
    }
}

struct DvdFrameProbe::QueuedFrame
{
    BYTE pixels[kWidth * kHeight * 3 / 2];
    unsigned width, height, calls;
    unsigned motion, dc, idct;
    double duration;
    LONGLONG parseTicks, readTicks, inputWaitTicks;
    LONGLONG decodeTicks, copyTicks;
    LONGLONG audioWorkTicks;
    unsigned videoPacketHighWater;
    double decodeCpuMs;
    LONGLONG motionTicks, idctTicks;
    LONGLONG worstReadTicks;
    unsigned readCalls, readBytes;
    HRESULT status;
};

struct DvdFrameProbe::Decoder
{
    CRITICAL_SECTION queueStats;
    unsigned queuedFrames;
    HANDLE file;
    FfmpegReadStats ioStats;
    AVIOContext* io;
    AVFormatContext* format;
    AVCodecContext* codec;
    AVFrame* decoded;
    AVPacket* packet;
    int video;
    double frameRate;
    FfmpegAudio* audio;
    int audioStream;
    bool audioFailed;
    bool videoEnded;
    bool flushed;
    int demuxEnd;
    enum { PacketCapacity = 96, PacketByteBudget = 4 * 1024 * 1024 };
    AVPacket pendingVideo[PacketCapacity];
    unsigned packetHead, packetCount, packetBytes, packetHighWater;
    unsigned packetBudget, startupVideoBytes, primedVideoBytes;
    void PumpPacket();
    HANDLE thread, stop, freeSlots, ready, primed;
    unsigned writeIndex, readIndex, calls;
    LONGLONG parseTicks, inputWaitTicks;
    LONGLONG audioWorkTicks;
    enum { QueueCapacity = 6 };
    QueuedFrame queue[QueueCapacity];
};

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
    if (audio && !audioFailed &&
        (result == AVERROR_EOF || (result == 0 && packet->stream_index == audioStream)))
    {
        QueryPerformanceCounter(&start);
        audioFailed = FAILED(audio->Decode(result == AVERROR_EOF ? NULL : packet, stop));
        QueryPerformanceCounter(&end);
        audioWorkTicks += end.QuadPart - start.QuadPart;
    }
    if (audio && (demuxEnd || audioFailed))
        audio->Finish();
    av_packet_unref(packet);
}

DvdFrameProbe::DvdFrameProbe()
    : m_decoder(NULL), m_width(0), m_height(0), m_untilNext(0),
      m_framesReady(0), m_lateByFrame(0), m_audioSyncDrops(0), m_videoPacketHighWater(0),
      m_audioWorkTicks(0), m_maxLate(0),
      m_parseCalls(0), m_parseTicks(0), m_parseFrequency(0),
      m_readTicks(0), m_uploadTicks(0), m_gpuWaitTicks(0),
      m_inputWaitTicks(0), m_motionTicks(0), m_idctTicks(0),
      m_lastDecodeTicks(0), m_lastCopyTicks(0),
      m_lastDecodeCpuMs(-1.0),
      m_worstReadTicks(0), m_readCalls(0), m_readBytes(0),
      m_underruns(0), m_textureSet(0),
#ifdef DVD360_FFMPEG_ALTIVEC
      m_decodeMode("FFmpeg MPEG-2 (experimental AltiVec, scalar IDCT)"),
#else
      m_decodeMode("FFmpeg MPEG-2 (scalar)"),
#endif
      m_motionBlocks(0), m_dcBlocks(0), m_idctBlocks(0),
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
    if (d.freeSlots) CloseHandle(d.freeSlots);
    if (d.stop) CloseHandle(d.stop);
    delete d.audio;
    for (unsigned i = 0; i < Decoder::PacketCapacity; ++i)
        av_packet_unref(&d.pendingVideo[i]);
    av_packet_free(&d.packet);
    av_frame_free(&d.decoded);
    avcodec_free_context(&d.codec);
    avformat_close_input(&d.format);
    DestroyFfmpegFileIo(&d.io);
    DeleteCriticalSection(&d.queueStats);

    CloseHandle(d.file);
    delete m_decoder;
    m_decoder = NULL;
}

DWORD WINAPI DvdFrameProbe::DecodeWorker(void* context)
{
    DvdFrameProbe& player = *static_cast<DvdFrameProbe*>(context);
    Decoder& d = *player.m_decoder;
    HANDLE wait[] = {d.stop, d.freeSlots};
    unsigned initialFrames = 0;
    bool primed = false;
    bool refillAudio = false;
    const unsigned audioLow = 8, audioTarget = 16, videoPacketTarget = 12;
    for (;;)
    {
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
        }
        // Refill in bursts; don't spend a render deadline reading ahead when
        // the decoder has fewer than three ready frames to cover the work.
        refillAudio = haveAudio && audioBuffers < (refillAudio ? audioTarget : audioLow);
        const bool needAudio = haveAudio && (refillAudio || !primed);
        const bool wantPackets = d.packetCount < videoPacketTarget &&
            (!haveAudio || audioBuffers < audioTarget);
        const bool readAhead = canReadAhead &&
            (needAudio || wantPackets || (!primed && !videoReady));
        const unsigned depth = player.QueueDepth();
        if (readAhead && primed && refillAudio && depth >= Decoder::QueueCapacity / 2)
        {
            if (WaitForSingleObject(d.stop, 0) != WAIT_TIMEOUT)
                break;
            d.PumpPacket();
            continue;
        }
        const DWORD available = WaitForMultipleObjects(2, wait, FALSE,
            readAhead ? 0 : canReadAhead && haveAudio ? 10 : INFINITE);
        if (available == WAIT_TIMEOUT)
        {
            if (readAhead)
                d.PumpPacket();
            continue;
        }
        if (available != WAIT_OBJECT_0 + 1)
            break;
        QueuedFrame& frame = d.queue[d.writeIndex];
        LARGE_INTEGER start, finish;
        ULONGLONG cpuStart = 0, cpuFinish = 0;
        const bool haveCpuStart = DecodeCpuTime(cpuStart);
        QueryPerformanceCounter(&start);
        const HRESULT status = player.NextFrame(frame);
        QueryPerformanceCounter(&finish);
        frame.decodeCpuMs = haveCpuStart && DecodeCpuTime(cpuFinish) && cpuFinish >= cpuStart
            ? (cpuFinish - cpuStart) / 10000.0 : -1.0;
        frame.decodeTicks = finish.QuadPart - start.QuadPart;
        frame.status = status;
        frame.calls = d.calls;
        frame.parseTicks = d.parseTicks;
        frame.readTicks = d.ioStats.ticks;
        frame.inputWaitTicks = d.inputWaitTicks;
        frame.audioWorkTicks = d.audioWorkTicks;
        frame.videoPacketHighWater = d.packetHighWater;
        frame.motionTicks = frame.idctTicks = 0;
        frame.worstReadTicks = d.ioStats.worstTicks;
        frame.readCalls = d.ioStats.calls;
        frame.readBytes = d.ioStats.bytes;
        frame.motion = 0;
        frame.dc = 0;
        frame.idct = 0;
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
            break;
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
    m_motionTicks = frame.motionTicks;
    m_idctTicks = frame.idctTicks;
    m_worstReadTicks = frame.worstReadTicks;
    m_readCalls = frame.readCalls;
    m_readBytes = frame.readBytes;
    m_motionBlocks = frame.motion;
    m_dcBlocks = frame.dc;
    m_idctBlocks = frame.idct;
    HRESULT hr = frame.status;
    if (hr == S_OK)
    {
        EnterCriticalSection(&d.queueStats);
        --d.queuedFrames;
        LeaveCriticalSection(&d.queueStats);
        m_lastDecodeTicks = frame.decodeTicks;
        m_lastDecodeCpuMs = frame.decodeCpuMs;
        m_lastCopyTicks = frame.copyTicks;
        const unsigned luma = frame.width * frame.height;
        const unsigned chroma = ((frame.width + 1) / 2) * ((frame.height + 1) / 2);
        const BYTE* planes[] = {frame.pixels, frame.pixels + luma, frame.pixels + luma + chroma};
        const double late = -m_untilNext;
        const bool drop = audioMaster && late >= frame.duration;
        if (!drop)
            hr = LoadPlanes(device, frame.width, frame.height, planes, frame.width);
        if (SUCCEEDED(hr))
        {
            if (!drop)
                ++m_framesReady;
            else
                ++m_audioSyncDrops;
            if (late > m_maxLate) m_maxLate = late;
            if (late >= frame.duration) ++m_lateByFrame;
            m_untilNext += frame.duration;
            if (!audioMaster && m_untilNext < 0) m_untilNext = 0;
        }
    }
    d.readIndex = (d.readIndex + 1) % Decoder::QueueCapacity;
    ReleaseSemaphore(d.freeSlots, 1, NULL);
    return hr;
}

HRESULT DvdFrameProbe::Load(IDirect3DDevice9* device, HANDLE file)
{
    const unsigned luma = kWidth * kHeight;
    const unsigned bytes = luma * 3 / 2;
    BYTE* data = (BYTE*)malloc(bytes);
    if (!data)
        return E_OUTOFMEMORY;
    HRESULT hr = E_INVALIDARG;
    DWORD sizeHigh = 0, received = 0;
    if (GetFileSize(file, &sizeHigh) == bytes && !sizeHigh &&
        ReadFile(file, data, bytes, &received, NULL) && received == bytes)
    {
        const BYTE* planes[] = {data, data + luma, data + luma + luma / 4};
        hr = LoadPlanes(device, kWidth, kHeight, planes, kWidth);
    }
    free(data);
    return hr;
}

HRESULT DvdFrameProbe::DecodeMpeg2(IDirect3DDevice9* device, HANDLE file)
{
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency))
        m_parseFrequency = frequency.QuadPart;
    // Takes ownership of the input file, even when startup fails.
    m_decoder = new Decoder();
    if (!m_decoder)
    {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
    m_decoder->file = file;
    InitializeCriticalSection(&m_decoder->queueStats);
    m_decoder->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    const LONG capacity = Decoder::QueueCapacity;
    m_decoder->freeSlots = CreateSemaphore(NULL, capacity, capacity, NULL);
    m_decoder->ready = CreateSemaphore(NULL, 0, capacity, NULL);
    m_decoder->primed = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (m_decoder->stop && m_decoder->freeSlots && m_decoder->ready && m_decoder->primed)
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

HRESULT DvdFrameProbe::Advance(IDirect3DDevice9* device, double elapsed, bool audioMaster)
{
    if (!m_decoder)
        return S_OK;
    if (m_decoder->videoEnded)
    {
        if (m_decoder->audio && m_decoder->audio->IsPlaying())
            return S_OK;
        CloseDecoder();
        return S_FALSE;
    }
    m_untilNext -= elapsed;
    if (m_untilNext > 0)
        return S_OK;
    HRESULT hr;
    unsigned remaining = Decoder::QueueCapacity;
    do
    {
        hr = Consume(device, 0, audioMaster);
    } while (audioMaster && hr == S_OK && m_untilNext <= 0 && --remaining);
    if (hr == E_PENDING)
    {
        ++m_underruns;
        return S_OK; // Hold the previous frame and retry at the next refresh.
    }
    if (hr == S_FALSE && m_decoder->audio && m_decoder->audio->IsPlaying())
    {
        m_decoder->videoEnded = true;
        return S_OK;
    }
    if (hr != S_OK)
        CloseDecoder();
    return hr;
}

double DvdFrameProbe::ParseMilliseconds() const
{
    return m_parseFrequency ? m_parseTicks * 1000.0 / m_parseFrequency : 0.0;
}

double DvdFrameProbe::ReadMilliseconds() const
{ return m_parseFrequency ? m_readTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::InputWaitMilliseconds() const
{ return m_parseFrequency ? m_inputWaitTicks * 1000.0 / m_parseFrequency : 0.0; }

double DvdFrameProbe::MotionEstimateMilliseconds() const
{ return m_parseFrequency ? m_motionTicks * 1000.0 / m_parseFrequency : 0.0; }

double DvdFrameProbe::IdctEstimateMilliseconds() const
{ return m_parseFrequency ? m_idctTicks * 1000.0 / m_parseFrequency : 0.0; }
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
        d.io = CreateFfmpegFileIo(d.file, &d.ioStats);
        d.format = d.io ? avformat_alloc_context() : NULL;
        if (!d.format)
            return E_OUTOFMEMORY;
        d.format->pb = d.io;
        d.format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&d.format, NULL, NULL, NULL) < 0 ||
            avformat_find_stream_info(d.format, NULL) < 0)
            return E_FAIL;
        const AVCodec* codec = NULL;
        d.video = av_find_best_stream(d.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (d.video < 0 || !codec || codec->id != AV_CODEC_ID_MPEG2VIDEO)
            return E_INVALIDARG;
        d.codec = avcodec_alloc_context3(codec);
        d.decoded = av_frame_alloc();
        d.packet = av_packet_alloc();
        if (!d.codec || !d.decoded || !d.packet)
            return E_OUTOFMEMORY;
        AVStream* video = d.format->streams[d.video];
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
        d.codec->idct_algo = FF_IDCT_ALTIVEC;
        m_decodeMode = "FFmpeg MPEG-2 (AltiVec and Xenon IDCT)";
        FILE* scalarIdct = fopen("game:\\ffmpeg-scalar-idct.enable", "rb");
        if (scalarIdct)
        {
            fclose(scalarIdct);
            d.codec->idct_algo = FF_IDCT_SIMPLE;
            m_decodeMode = "FFmpeg MPEG-2 (AltiVec, scalar IDCT)";
        }
#endif
        if (avcodec_open2(d.codec, codec, NULL) < 0)
            return E_FAIL;
        const AVCodec* audioCodec = NULL;
        d.audioStream = av_find_best_stream(d.format, AVMEDIA_TYPE_AUDIO,
                                            -1, d.video, &audioCodec, 0);
        if (d.audioStream >= 0 && audioCodec)
        {
            d.audio = new FfmpegAudio();
            if (d.audio && FAILED(d.audio->Open(*d.format->streams[d.audioStream])))
            {
                delete d.audio;
                d.audio = NULL;
            }
        }
    }

    for (;;)
    {
        if (WaitForSingleObject(d.stop, 0) == WAIT_OBJECT_0)
            return E_ABORT;
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        const int status = avcodec_receive_frame(d.codec, d.decoded);
        QueryPerformanceCounter(&end);
        d.parseTicks += end.QuadPart - start.QuadPart;
        ++d.calls;
        if (status == AVERROR_EOF)
            return S_FALSE;
        if (status == 0)
            break;
        if (status != AVERROR(EAGAIN) || d.flushed)
            return E_FAIL;

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
                return flush == AVERROR_EOF ? S_FALSE : E_FAIL;
        }
        else
            return E_FAIL;
    }

    const AVFrame& decoded = *d.decoded;
    if (decoded.format != AV_PIX_FMT_YUV420P ||
        decoded.width <= 0 || decoded.width > int(kWidth) ||
        decoded.height <= 0 || decoded.height > int(kHeight))
        return E_INVALIDARG;
    frame.width = decoded.width;
    frame.height = decoded.height;
    AVStream* video = d.format->streams[d.video];
    const double rate = d.frameRate;
    frame.duration = decoded.duration > 0
        ? decoded.duration * av_q2d(video->time_base)
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

double DvdFrameProbe::AdvanceEmbeddedAudio(double fallback)
{
    return m_decoder && m_decoder->audio
        ? m_decoder->audio->Advance(fallback, QueueDepth() == Decoder::QueueCapacity)
        : fallback;
}

unsigned DvdFrameProbe::AudioRecoveryTicks() const
{
    return m_decoder && m_decoder->audio ? m_decoder->audio->RecoveryTicks() : 0;
}

double DvdFrameProbe::AudioWorkMilliseconds() const
{
    return m_parseFrequency ? m_audioWorkTicks * 1000.0 / m_parseFrequency : 0.0;
}

void DvdFrameProbe::StartupVideoBuffer(unsigned& queued, unsigned& target, unsigned& budget) const
{
    queued = m_decoder ? m_decoder->primedVideoBytes : 0;
    target = m_decoder ? m_decoder->startupVideoBytes : 0;
    budget = m_decoder ? m_decoder->packetBudget : 0;
}

bool DvdFrameProbe::EmbeddedAudioState(double& milliseconds, unsigned& buffers) const
{
    if (!m_decoder || !m_decoder->audio)
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

HRESULT DvdFrameProbe::Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const
{
    // The smoke-test fixture is displayed as 4:3, independent of coded size.
    const float aspect = (float)width / height;
    const float x = aspect > 4.0f / 3.0f ? (4.0f / 3.0f) / aspect : 1.0f;
    const float y = aspect < 4.0f / 3.0f ? aspect / (4.0f / 3.0f) : 1.0f;
    const float vertices[][6] = {
        {-x,  y, 0, 1, 0, 0}, {x,  y, 0, 1, 1, 0},
        {-x, -y, 0, 1, 0, 1}, {x, -y, 0, 1, 1, 1}
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
