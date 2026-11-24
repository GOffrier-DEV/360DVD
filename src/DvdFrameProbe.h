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
    void Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const;

private:
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
