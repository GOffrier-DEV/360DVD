#pragma once

#include <xtl.h>
#include <d3d9.h>

class PlaybackOsd
{
public:
    enum Action { None, TogglePause, DisplayMode };
    enum { DisplayModeCount = 6 };
    PlaybackOsd();
    ~PlaybackOsd();
    HRESULT Open(IDirect3DDevice9* device);
    Action Update(double elapsed, bool paused);
    void Draw(IDirect3DDevice9* device, unsigned width, unsigned height,
              bool paused, unsigned mode, double seconds, double duration);

private:
    struct Vertex { float x, y, z, w; DWORD color; };
    void Rect(float x, float y, float w, float h, DWORD color);
    void Text(float x, float y, const char* text, DWORD color);
    IDirect3DVertexShader9* m_vs;
    IDirect3DPixelShader9* m_ps;
    IDirect3DVertexDeclaration9* m_decl;
    DWORD m_buttons[4];
    unsigned m_selected, m_count;
    double m_visible;
    Vertex m_vertices[12000];
};
