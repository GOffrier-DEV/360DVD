#pragma once

#include <xtl.h>
#include <xaudio2.h>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwrContext;

class FfmpegAudio
{
public:
    FfmpegAudio();
    ~FfmpegAudio();
    HRESULT Open(const AVStream& stream);
    HRESULT Decode(const AVPacket* packet, HANDLE stop);
    HRESULT Start();
    void AllowPrimedStart() { m_allowPrimedStart = true; }
    HRESULT SetPaused(bool paused);
    void Finish();
    double Advance(double fallback, bool videoBackpressure);
    unsigned RecoveryTicks() const { return m_recoveryTicks; }
    bool IsPlaying() const;
    unsigned QueuedBuffers() const;
    void GetPlaybackState(UINT64& samples, unsigned& buffers) const;

private:
    HRESULT Convert(const AVFrame* input, HANDLE stop);
    enum { Rate = 48000, Slots = 32, SlotSamples = 4096, Channels = 2 };
    IXAudio2* m_engine;
    IXAudio2MasteringVoice* m_master;
    IXAudio2SourceVoice* m_voice;
    AVCodecContext* m_codec;
    AVFrame* m_frame;
    SwrContext* m_resample;
    volatile LONG m_started;
    volatile LONG m_finished;
    UINT64 m_lastSamples;
    double m_recoveryDebt;
    double m_voiceStall;
    unsigned m_voiceRestartAttempts;
    bool m_allowPrimedStart;
    unsigned m_recoveryTicks;
    unsigned m_next;
    SHORT m_pcm[Slots][SlotSamples * Channels];
};
