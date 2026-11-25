#ifndef DVD_FRAME_PROBE_H
#define DVD_FRAME_PROBE_H

#include <d3d9.h>
#include <stdio.h>

class DvdFrameProbe
{
public:
    DvdFrameProbe();
    ~DvdFrameProbe();
    HRESULT Load(IDirect3DDevice9* device, HANDLE file);
    HRESULT DecodeMpeg2(IDirect3DDevice9* device, HANDLE file);
    HRESULT StartEmbeddedAudio();
    double AdvanceEmbeddedAudio(double fallback);
    bool EmbeddedAudioState(double& milliseconds, unsigned& buffers) const;
    void StartupVideoBuffer(unsigned& queued, unsigned& target, unsigned& budget) const;
    unsigned AudioRecoveryTicks() const;
    double AudioWorkMilliseconds() const;
    unsigned VideoPacketHighWater() const { return m_videoPacketHighWater; }
    HRESULT Advance(IDirect3DDevice9* device, double elapsed, bool audioMaster = false);
    unsigned FramesReady() const { return m_framesReady; }
    unsigned LateByFrame() const { return m_lateByFrame; }
    unsigned AudioSyncDrops() const { return m_audioSyncDrops; }
    double MaxLateSeconds() const { return m_maxLate; }
    unsigned ParseCalls() const { return m_parseCalls; }
    double ParseMilliseconds() const;
    unsigned Underruns() const { return m_underruns; }
    const char* DecodeMode() const { return m_decodeMode; }
    unsigned MotionBlocks() const { return m_motionBlocks; }
    unsigned DcBlocks() const { return m_dcBlocks; }
    unsigned IdctBlocks() const { return m_idctBlocks; }
    double ReadMilliseconds() const;
    double InputWaitMilliseconds() const;
    double MotionEstimateMilliseconds() const;
    double IdctEstimateMilliseconds() const;
    unsigned QueueDepth() const;
    double LastDecodeMilliseconds() const;
    double LastDecodeCpuMilliseconds() const { return m_lastDecodeCpuMs; }
    double LastCopyMilliseconds() const;
    double WorstReadMilliseconds() const;
    unsigned ReadCalls() const { return m_readCalls; }
    unsigned ReadBytes() const { return m_readBytes; }
    double UploadMilliseconds() const;
    double GpuWaitMilliseconds() const;
    HRESULT Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const;

private:
    struct QueuedFrame;
    HRESULT NextFrame(QueuedFrame& frame);
    HRESULT Consume(IDirect3DDevice9* device, DWORD timeout, bool audioMaster = false);
    void CloseDecoder();
    static DWORD WINAPI DecodeWorker(void* context);
    struct Decoder;
    Decoder* m_decoder;
    unsigned m_width, m_height;
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
    LONGLONG m_motionTicks, m_idctTicks;
    LONGLONG m_lastDecodeTicks, m_lastCopyTicks;
    double m_lastDecodeCpuMs;
    LONGLONG m_worstReadTicks;
    unsigned m_readCalls, m_readBytes;
    unsigned m_underruns, m_textureSet;
    const char* m_decodeMode;
    unsigned m_motionBlocks, m_dcBlocks, m_idctBlocks;
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
