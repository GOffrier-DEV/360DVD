#ifndef DVD_FRAME_PROBE_H
#define DVD_FRAME_PROBE_H

#include <d3d9.h>
#include <stdio.h>

class DvdFrameProbe
{
public:
    DvdFrameProbe();
    ~DvdFrameProbe();
    HRESULT Load(IDirect3DDevice9* device, FILE* file);
    HRESULT DecodeMpeg2(IDirect3DDevice9* device, FILE* file);
    HRESULT Advance(IDirect3DDevice9* device, double elapsed);
    unsigned FramesReady() const { return m_framesReady; }
    unsigned LateByFrame() const { return m_lateByFrame; }
    double MaxLateSeconds() const { return m_maxLate; }
    unsigned ParseCalls() const { return m_parseCalls; }
    double ParseMilliseconds() const;
    unsigned Underruns() const { return m_underruns; }
    unsigned MotionBlocks() const { return m_motionBlocks; }
    unsigned DcBlocks() const { return m_dcBlocks; }
    unsigned IdctBlocks() const { return m_idctBlocks; }
    double ReadMilliseconds() const;
    double UploadMilliseconds() const;
    double GpuWaitMilliseconds() const;
    HRESULT Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const;

private:
    struct QueuedFrame;
    HRESULT NextFrame(QueuedFrame& frame);
    HRESULT Consume(IDirect3DDevice9* device, DWORD timeout);
    void CloseDecoder();
    static DWORD WINAPI DecodeWorker(void* context);
    struct Decoder;
    Decoder* m_decoder;
    unsigned m_width, m_height;
    double m_untilNext;
    unsigned m_framesReady, m_lateByFrame;
    double m_maxLate;
    unsigned m_parseCalls;
    LONGLONG m_parseTicks, m_parseFrequency;
    LONGLONG m_readTicks, m_uploadTicks, m_gpuWaitTicks;
    unsigned m_underruns, m_textureSet;
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
