#ifndef DVD_FRAME_PROBE_H
#define DVD_FRAME_PROBE_H

#include <d3d9.h>
#include <stdio.h>
#include "FfmpegFileIo.h"

class PlaybackOsd;

class DvdFrameProbe
{
public:
    DvdFrameProbe();
    ~DvdFrameProbe();
    HRESULT DecodeMpeg2(IDirect3DDevice9* device);
    void Stop();
    HRESULT StartEmbeddedAudio();
    bool RequestNextAudioTrack();
    HRESULT PollAudioTrack(bool paused);
    HRESULT SetEmbeddedAudioPaused(bool paused);
    double DurationSeconds() const;
    float DisplayAspect() const;
    unsigned SubtitleCount() const;
    const char* TrackLanguage(bool subtitle, unsigned track = 0) const;
    unsigned DiscSegment() const { return m_discSegment; }
    bool DeinterlacingActive() const { return m_deinterlacingActive; }
    DvdMenuState DiscMenuState() const;
    bool DiscTransitionPending() const;
    bool DiscMenuInput(DvdMenuCommand command);
    void DrawDiscHighlight(IDirect3DDevice9* device, PlaybackOsd& osd, unsigned width,
                           unsigned height, unsigned mode) const;
    void DrawSubtitles(IDirect3DDevice9* device, PlaybackOsd& osd, unsigned width,
                       unsigned height, unsigned mode, unsigned track, double seconds);
    double AdvanceEmbeddedAudio(double fallback);
    bool EmbeddedAudioState(double& milliseconds, unsigned& buffers) const;
    void StartupVideoBuffer(unsigned& queued, unsigned& target, unsigned& budget) const;
    unsigned AudioRecoveryTicks() const;
    double AudioWorkMilliseconds() const;
    unsigned VideoPacketHighWater() const { return m_videoPacketHighWater; }
    HRESULT Advance(IDirect3DDevice9* device, double elapsed, bool audioMaster = false,
                    double wallElapsed = 0);
    unsigned FramesReady() const { return m_framesReady; }
    unsigned LateByFrame() const { return m_lateByFrame; }
    unsigned AudioSyncDrops() const { return m_audioSyncDrops; }
    double MaxLateSeconds() const { return m_maxLate; }
    unsigned ParseCalls() const { return m_parseCalls; }
    double ParseMilliseconds() const;
    unsigned Underruns() const { return m_underruns; }
    const char* DecodeMode() const { return m_decodeMode; }
    unsigned ActiveSliceThreads() const;
    double ReadMilliseconds() const;
    double InputWaitMilliseconds() const;
    unsigned QueueDepth() const;
    double LastDecodeMilliseconds() const;
    double LastDecodeCpuMilliseconds() const { return m_lastDecodeCpuMs; }
    double LastCopyMilliseconds() const;
    double LastBwdifMilliseconds() const;
    double WorstReadMilliseconds() const;
    unsigned ReadCalls() const { return m_readCalls; }
    unsigned ReadBytes() const { return m_readBytes; }
    double UploadMilliseconds() const;
    double GpuWaitMilliseconds() const;
    HRESULT Draw(IDirect3DDevice9* device, unsigned width, unsigned height,
                 unsigned displayMode = 0) const;

private:
    void DisplayGeometry(unsigned width, unsigned height, unsigned mode,
                         float& x, float& y, float& u, float& v) const;
    struct QueuedFrame;
    HRESULT NextFrame(QueuedFrame& frame);
    HRESULT Consume(IDirect3DDevice9* device, DWORD timeout, bool audioMaster = false);
    void CloseDecoder();
    static DWORD WINAPI DecodeWorker(void* context);
    struct Decoder;
    Decoder* m_decoder;
    unsigned m_width, m_height;
    unsigned m_discSegment;
    bool m_deinterlacingActive;
    double m_untilNext;
    unsigned m_framesReady, m_lateByFrame;
    unsigned m_audioSyncDrops;
    unsigned m_videoPacketHighWater;
    LONGLONG m_audioWorkTicks;
    double m_maxLate;
    unsigned m_parseCalls;
    LONGLONG m_parseTicks, m_parseFrequency;
    LONGLONG m_readTicks, m_uploadTicks, m_gpuWaitTicks;
    LONGLONG m_inputWaitTicks;
    LONGLONG m_lastDecodeTicks, m_lastCopyTicks;
    LONGLONG m_lastBwdifTicks;
    double m_lastDecodeCpuMs;
    LONGLONG m_worstReadTicks;
    unsigned m_readCalls, m_readBytes;
    unsigned m_underruns, m_textureSet;
    const char* m_decodeMode;
    HRESULT LoadPlanes(IDirect3DDevice9* device, unsigned width, unsigned height,
                       const unsigned char* const planes[3], unsigned stride);
    DvdFrameProbe(const DvdFrameProbe&);
    DvdFrameProbe& operator=(const DvdFrameProbe&);
    IDirect3DTexture9* m_planes[3][3];
    IDirect3DVertexShader9* m_vs;
    IDirect3DPixelShader9* m_ps;
    IDirect3DVertexDeclaration9* m_decl;
};

#endif
