#pragma once

#include <xtl.h>
#include <d3d9.h>

class PlaybackOsd
{
public:
    enum Action { None, TogglePause, DisplayMode, SubtitleTrack };
    enum { DisplayModeCount = 6 };
    PlaybackOsd();
    ~PlaybackOsd();
    HRESULT Open(IDirect3DDevice9* device);
    Action Update(double elapsed, bool paused);
    void Draw(IDirect3DDevice9* device, unsigned width, unsigned height,
              bool paused, unsigned mode, double seconds, double duration,
              unsigned subtitle, unsigned subtitleCount);
    bool Ready() const { return m_decl != NULL; }
    void DrawBitmap(IDirect3DDevice9* device, float x, float y, float sx, float sy,
                    unsigned width, unsigned height, const BYTE* pixels, int pitch,
                    const BYTE* palette, unsigned colors);

private:
    struct Vertex { float x, y, z, w; DWORD color; float u, v; };
    void Rect(float x, float y, float w, float h, DWORD color);
    void Panel(float x, float y, float w, float h, float radius, DWORD top, DWORD bottom);
    void Quad(float x, float y, float w, float h, DWORD color, float u0, float v0, float u1, float v1);
    void Text(float x, float y, const char* text, DWORD color, float scale = 1);
    void Submit(IDirect3DDevice9* device);
    IDirect3DVertexShader9* m_vs;
    IDirect3DPixelShader9* m_ps;
    IDirect3DVertexDeclaration9* m_decl;
    IDirect3DTexture9* m_atlas;
    BYTE m_widths[128];
    DWORD m_buttons[4];
    unsigned m_selected, m_count;
    double m_visible;
    float m_openAmount, m_focus, m_panelOffset;
    Vertex m_vertices[12000];
};
