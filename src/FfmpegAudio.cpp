#include "FfmpegAudio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

FfmpegAudio::FfmpegAudio()
    : m_engine(NULL), m_master(NULL), m_voice(NULL), m_codec(NULL),
      m_frame(NULL), m_resample(NULL), m_started(0), m_finished(0),
      m_lastSamples(0), m_recoveryDebt(0), m_voiceStall(0), m_voiceRestartAttempts(0),
      m_allowPrimedStart(false), m_recoveryTicks(0), m_next(0)
{
}

FfmpegAudio::~FfmpegAudio()
{
    if (m_voice) m_voice->DestroyVoice();
    if (m_master) m_master->DestroyVoice();
    if (m_engine) m_engine->Release();
    swr_free(&m_resample);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_codec);
}

HRESULT FfmpegAudio::Open(const AVStream& stream)
{
    const AVCodec* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
    if (!decoder)
        return E_INVALIDARG;
    m_codec = avcodec_alloc_context3(decoder);
    m_frame = av_frame_alloc();
    if (!m_codec || !m_frame)
        return E_OUTOFMEMORY;
    if (avcodec_parameters_to_context(m_codec, stream.codecpar) < 0)
        return E_FAIL;
    m_codec->pkt_timebase = stream.time_base;
    m_codec->thread_count = 1;
    if (avcodec_open2(m_codec, decoder, NULL) < 0)
        return E_FAIL;

    HRESULT hr = XAudio2Create(&m_engine, 0, XboxThread5);
    if (SUCCEEDED(hr))
        hr = m_engine->CreateMasteringVoice(&m_master);
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = Channels;
    format.nSamplesPerSec = Rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = Channels * sizeof(SHORT);
    format.nAvgBytesPerSec = Rate * format.nBlockAlign;
    if (SUCCEEDED(hr))
        hr = m_engine->CreateSourceVoice(&m_voice, &format);
    return hr;
}

HRESULT FfmpegAudio::Start()
{
    if (!m_voice)
        return E_FAIL;
    const HRESULT hr = m_voice->Start(0);
    if (SUCCEEDED(hr))
        InterlockedExchange(&m_started, 1);
    return hr;
}

void FfmpegAudio::Finish()
{
    InterlockedExchange(&m_finished, 1);
}

HRESULT FfmpegAudio::SetPaused(bool paused)
{
    if (!m_voice)
        return E_FAIL;
    m_voiceStall = 0;
    m_voiceRestartAttempts = 0;
    return paused ? m_voice->Stop(0) : Start();
}

double FfmpegAudio::Advance(double fallback, bool videoBackpressure)
{
    if (!m_voice)
        return fallback;
    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state);
    const UINT64 samples = state.SamplesPlayed;
    const UINT64 delta = samples >= m_lastSamples ? samples - m_lastSamples : 0;
    m_lastSamples = samples;
    if (delta)
    {
        m_voiceStall = 0;
        m_voiceRestartAttempts = 0;
    }
    else if (!state.BuffersQueued)
        m_voiceStall = 0;
    else if (m_voiceRestartAttempts < 3)
    {
        // The render thread only advances this clock while unpaused. If
        // queued PCM isn't moving, retry the same stop/start as pause/play.
        // Don't flush the voice or advance video against a synthetic clock.
        m_voiceStall += fallback < 0.05 ? fallback : 0.05;
        if (m_voiceStall >= (m_voiceRestartAttempts ? 0.75 : 0.25))
        {
            ++m_voiceRestartAttempts;
            m_voiceStall = 0;
            ++m_recoveryTicks;
            m_voice->Stop(0);
            Start();
        }
    }
    if (!state.BuffersQueued)
    {
        if (!samples || (!delta && InterlockedCompareExchange(&m_finished, 0, 0)))
            return fallback;
        // A full video queue can block the shared producer from finding
        // the next audio packet. Borrow just enough time to free a slot.
        if (!delta && videoBackpressure)
        {
            ++m_recoveryTicks;
            m_recoveryDebt += fallback;
            return fallback;
        }
    }
    const double audioElapsed = double(delta) / Rate;
    const double repaid = audioElapsed < m_recoveryDebt ? audioElapsed : m_recoveryDebt;
    m_recoveryDebt -= repaid;
    return audioElapsed - repaid;
}

bool FfmpegAudio::IsPlaying() const
{
    if (!m_voice)
        return false;
    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state);
    return state.BuffersQueued != 0;
}

unsigned FfmpegAudio::QueuedBuffers() const
{
    if (!m_voice)
        return 0;
    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state);
    return state.BuffersQueued;
}

void FfmpegAudio::GetPlaybackState(UINT64& samples, unsigned& buffers) const
{
    if (!m_voice)
    {
        samples = 0;
        buffers = 0;
        return;
    }
    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state);
    samples = state.SamplesPlayed;
    buffers = state.BuffersQueued;
}

HRESULT FfmpegAudio::Convert(const AVFrame* input, HANDLE stop)
{
    if (!m_voice)
        return E_FAIL;
    if (!m_resample)
    {
        if (!input)
            return S_FALSE;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m_resample, &stereo, AV_SAMPLE_FMT_S16, Rate,
                               &input->ch_layout, AVSampleFormat(input->format),
                               input->sample_rate, 0, NULL) < 0 ||
            swr_init(m_resample) < 0)
            return E_FAIL;
    }

    AVFrame output = {};
    output.format = AV_SAMPLE_FMT_S16;
    output.ch_layout = AVChannelLayout AV_CHANNEL_LAYOUT_STEREO;
    output.sample_rate = Rate;
    // swr_convert_frame checks format changes instead of reading stale layouts.
    const int capacity = swr_get_out_samples(m_resample, input ? input->nb_samples : 0);
    if (capacity < 0 || capacity > SlotSamples)
        return E_FAIL;
    for (;;)
    {
        XAUDIO2_VOICE_STATE state;
        m_voice->GetState(&state);
        if (state.BuffersQueued < Slots)
            break;
        // A DVD segment can notify the renderer before the voice fills,
        // independent of how many video frames the segment contains.
        if (!m_allowPrimedStart && !InterlockedCompareExchange(&m_started, 0, 0))
            return E_FAIL;
        if (WaitForSingleObject(stop, 2) != WAIT_TIMEOUT)
            return E_ABORT;
    }
    output.data[0] = reinterpret_cast<uint8_t*>(m_pcm[m_next]);
    output.extended_data = output.data;
    output.linesize[0] = sizeof(m_pcm[m_next]);
    output.nb_samples = SlotSamples;
    if (swr_convert_frame(m_resample, &output, input) < 0)
        return E_FAIL;
    if (!output.nb_samples)
        return S_FALSE;

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = output.nb_samples * Channels * sizeof(SHORT);
    buffer.pAudioData = output.data[0];
    const HRESULT hr = m_voice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(hr))
        m_next = (m_next + 1) % Slots;
    return hr;
}

HRESULT FfmpegAudio::Decode(const AVPacket* packet, HANDLE stop)
{
    const int sent = avcodec_send_packet(m_codec, packet);
    if (sent == AVERROR_INVALIDDATA && packet) return S_OK;
    if (sent < 0)
        return E_FAIL;
    for (;;)
    {
        const int status = avcodec_receive_frame(m_codec, m_frame);
        if (status == AVERROR(EAGAIN))
            return S_OK;
        if (status == AVERROR_EOF)
        {
            HRESULT hr;
            do { hr = Convert(NULL, stop); } while (hr == S_OK);
            return hr == S_FALSE ? S_OK : hr;
        }
        if (status == AVERROR_INVALIDDATA && packet) return S_OK;
        if (status < 0)
            return E_FAIL;
        const HRESULT hr = Convert(m_frame, stop);
        av_frame_unref(m_frame);
        if (FAILED(hr))
            return hr;
    }
}
