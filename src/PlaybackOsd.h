#pragma once

#include <xtl.h>
#include <d3d9.h>
#include <xui.h>

class PlaybackOsd
{
public:
    enum { SceneWidth = 720, SceneHeight = 480 };
    enum Action { None, TogglePause, DisplayMode, SubtitleTrack, ToggleZoom, NextAudioTrack,
                  Stop, PreviousChapter, NextChapter, SeekBackward, SeekForward,
                  DiscUp, DiscDown, DiscLeft, DiscRight, DiscActivate, DiscRoot, DiscTitle, DiscContinue };
    enum { DisplayModeCount = 6 };
    enum { StockZoomMode = DisplayModeCount };
    PlaybackOsd();
    ~PlaybackOsd();
    HRESULT Open(IDirect3DDevice9* device);
    HRESULT OpenStock(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* present, const char*& stage);
    void CloseStock();
    bool UsesStock() const { return m_stockScenes[0] && m_stockScenes[1]; }
    void DrawStock(bool paused, double elapsed, double position, double duration,
                   const char* audioLanguage, const char* subtitleLanguage, unsigned subtitle);
    Action Update(double elapsed, bool paused, bool discButtons = false, bool discStill = false);
    void DrawMenuFocus(IDirect3DDevice9* device, float x, float y, float width, float height);
    void ShowTrackPopup(bool subtitle, unsigned track, const char* language);
    bool Ready() const { return m_decl != NULL; }
    void DrawBitmap(IDirect3DDevice9* device, float x, float y, float sx, float sy,
                    unsigned width, unsigned height, const BYTE* pixels, int pitch,
                    const BYTE* palette, unsigned colors);

private:
    struct Vertex { float x, y, z, w; DWORD color; };
    void Rect(float x, float y, float w, float h, DWORD color);
    void Submit(IDirect3DDevice9* device);
    char m_trackPopup[96];
    double m_trackPopupTime;
    IDirect3DVertexShader9* m_vs;
    IDirect3DPixelShader9* m_ps;
    IDirect3DVertexDeclaration9* m_decl;
    DWORD m_buttons[4];
    unsigned m_selected, m_count;
    double m_visible;
    HXUIDC m_stockDc;
    HXUIOBJ m_stockScenes[2], m_stockPopup;
    bool m_stockRender, m_stockRuntime;
    unsigned m_stockPage;
    bool m_stockInfo;
    int m_stockPlayState[2];
    Vertex m_vertices[12000];
};
