#pragma once

#include <xtl.h>
#include <d3d9.h>

class DvdLogoBounce
{
public:
    DvdLogoBounce();
    ~DvdLogoBounce();

    bool Startup(IDirect3DDevice9* device, int screenWidth, int screenHeight);
    void Update(float elapsedSeconds);
    void Render(bool noDisc = false);
    void RenderNoDisc();
    void Center();
    void Shutdown();

private:
    bool LoadLogoTexture();
    bool CreateShaders();

    IDirect3DDevice9* m_device;
    IDirect3DTexture9* m_texture;
    IDirect3DTexture9* m_noDiscTexture = NULL;
    float m_noDiscAspect = 1.f;
    bool m_noDiscLoadAttempted = false;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    int m_screenWidth;
    int m_screenHeight;
    float m_x;
    float m_y;
    float m_width;
    float m_height;
    float m_velocityX;
    float m_velocityY;
    unsigned int m_colorIndex;
};
