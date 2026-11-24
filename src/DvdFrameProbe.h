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
    HRESULT Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const;

private:
    HRESULT NextFrame(IDirect3DDevice9* device);
    struct Decoder;
    Decoder* m_decoder;
    unsigned m_width, m_height;
    double m_untilNext;
    unsigned m_framesReady, m_lateByFrame;
    double m_maxLate;
    HRESULT LoadPlanes(IDirect3DDevice9* device, unsigned width, unsigned height,
                       const unsigned char* const planes[3], unsigned stride);
    DvdFrameProbe(const DvdFrameProbe&);
    DvdFrameProbe& operator=(const DvdFrameProbe&);
    IDirect3DTexture9* m_planes[3];
    IDirect3DVertexShader9* m_vs;
    IDirect3DPixelShader9* m_ps;
    IDirect3DVertexDeclaration9* m_decl;
};

#endif
