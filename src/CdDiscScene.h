#pragma once

#include <xtl.h>
#include <d3d9.h>

class CdDiscScene
{
public:
    ~CdDiscScene();
    bool Draw(IDirect3DDevice9* device, bool spinning, double elapsed);
    void Reset();

private:
    HRESULT Open(IDirect3DDevice9* device);
    bool attempted = false, ready = false;
    float angle = 0, speed = 0;
    UINT vertexCount = 0;
    IDirect3DVertexBuffer9* mesh = NULL;
    IDirect3DTexture9* artwork = NULL;
    IDirect3DTexture9* resolved = NULL;
    IDirect3DSurface9* target = NULL;
    IDirect3DSurface9* depth = NULL;
    IDirect3DVertexDeclaration9* declaration = NULL;
    IDirect3DVertexShader9* vs = NULL;
    IDirect3DPixelShader9* ps = NULL;
};
