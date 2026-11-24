#include "PlaybackOsd.h"

#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace
{
    HRESULT APIENTRY LoadStockTexture(IXuiDevice* device, LPCWSTR locator,
                                      XUIImageInfo* info, IDirect3DTexture9** texture)
    {
        if (!locator) return E_INVALIDARG;
        // The dashboard uses its own resource locators. Read the bundled
        // DVD icons directly and let XUI decode the original PNG bytes.
        const wchar_t* name = locator;
        for (const wchar_t* p = locator; *p; ++p)
            if (*p == L'/' || *p == L'\\') name = p + 1;
        if (wcsncmp(name, L"ico_32x_", 8) != 0)
            return XuiPNGTextureLoader(device, locator, info, texture);
        char path[160];
        sprintf_s(path, sizeof(path), "game:\\Media\\Blades\\%ls", name);
        FILE* file = fopen(path, "rb");
        if (!file) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        BYTE png[4096];
        const size_t size = fread(png, 1, sizeof(png), file);
        fclose(file);
        if (!size || size == sizeof(png)) return E_FAIL;
        return XuiCreateTextureFromPNG(png, static_cast<DWORD>(size), device, texture, info);
    }
    enum { AtlasWidth = 512, AtlasHeight = 256, CellSize = 32 };
    const char kVs[] = "void main(float4 p:POSITION,float4 c:COLOR0,float2 uv:TEXCOORD0,float3 box:TEXCOORD1,"
        "out float4 pos:POSITION,out float4 col:COLOR0,out float2 tc:TEXCOORD0,out float3 b:TEXCOORD1)"
        "{pos=p;col=c;tc=uv;b=box;}";
    const char kPs[] = "sampler2D atlas:register(s0);"
        "float4 main(float4 c:COLOR0,float2 uv:TEXCOORD0,float3 box:TEXCOORD1):COLOR0"
        "{float2 q=abs(uv)-box.xy+box.z;"
        "float d=length(max(q,0))+min(max(q.x,q.y),0)-box.z;"
        "float edge=saturate(0.5-d/max(fwidth(d),0.001));"
        "if(box.x>0)return float4(c.rgb,c.a*edge);"
        "float a=tex2D(atlas,uv).r;float slot=floor(uv.x*16);"
        "if(uv.y<0.125 && slot>=1 && slot<=6){"
        "float button=slot<=4?slot-1:slot-5;"
        "float2 p=uv+(slot<=2?float2((7+2*slot)/16,0):float2((3*button-slot)/16,0.125));"
        "return float4(tex2D(atlas,p).r,tex2D(atlas,p+float2(0.0625,0)).r,"
        "tex2D(atlas,p+float2(0.125,0)).r,c.a*a);}"
        "return float4(c.rgb,c.a*a);}";

    void Clock(char* text, size_t size, double seconds)
    {
        const unsigned s = seconds > 0 ? static_cast<unsigned>(seconds) : 0;
        sprintf_s(text, size, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    }

}

PlaybackOsd::PlaybackOsd() : m_vs(NULL), m_ps(NULL), m_decl(NULL), m_atlas(NULL), m_selected(0),
    m_trackPopupTime(0), m_count(0), m_visible(0), m_openAmount(0), m_focus(0), m_panelOffset(0),
    m_stockPopup(NULL), m_stockDc(NULL), m_stockRender(false), m_stockRuntime(false),
    m_stockPage(0), m_stockInfo(false)
{
    memset(m_buttons, 0, sizeof(m_buttons));
    memset(m_stockScenes, 0, sizeof(m_stockScenes));
    m_stockPlayState[0] = m_stockPlayState[1] = -1;
}

PlaybackOsd::~PlaybackOsd()
{
    CloseStock();
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
    if (m_atlas) m_atlas->Release();
}

HRESULT PlaybackOsd::OpenStock(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* present,
                               const char*& stage)
{
    stage = "Stock XUI: renderer initialization";
    HRESULT hr = XuiRenderInitShared(device, present, LoadStockTexture);
    if (FAILED(hr)) return hr;
    m_stockRender = true;
    stage = "Stock XUI: create device context";
    hr = XuiRenderCreateDC(&m_stockDc);
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: runtime initialization";
        XUIInitParams params = {};
        params.cbSize = sizeof(params);
        params.dwFlags = XUI_INIT_PARAMS_FLAGS_NOKEYBOARD;
        hr = XuiInit(&params);
        m_stockRuntime = SUCCEEDED(hr);
    }
    const wchar_t root[] = L"file://game:/Media/Blades/";
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: register DVD typeface";
        TypefaceDescriptor typeface = { L"Xenon", L"file://game:/Media/Blades/XenonCLatin.ttf" };
        hr = XuiRegisterTypeface(&typeface, TRUE);
    }
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: load DVD skin";
        hr = XuiLoadVisualFromBinary(L"file://game:/Media/Blades/dvd_skin.xur", NULL);
    }
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: create Playback scene";
        hr = XuiSceneCreate(root, L"DvdPadPlayback.xur", NULL, &m_stockScenes[0]);
    }
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: create Display scene";
        hr = XuiSceneCreate(root, L"DvdPadDisplay.xur", NULL, &m_stockScenes[1]);
    }
    if (FAILED(hr)) CloseStock();
    else
    {
        stage = SUCCEEDED(XuiSceneCreate(root, L"DvdTrackPopup.xur", NULL, &m_stockPopup))
            ? "Stock XUI: ready" : "Stock XUI: ready (track popup asset unavailable)";
    }
    return hr;
}

void PlaybackOsd::CloseStock()
{
    if (m_stockPopup) XuiDestroyObject(m_stockPopup);
    m_stockPopup = NULL;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (m_stockScenes[i]) XuiDestroyObject(m_stockScenes[i]);
        m_stockScenes[i] = NULL;
    }
    if (m_stockDc) XuiRenderDestroyDC(m_stockDc);
    if (m_stockRuntime) XuiUninit();
    if (m_stockRender) XuiRenderUninit();
    m_stockDc = NULL;
    m_stockRuntime = m_stockRender = false;
}

void PlaybackOsd::DrawStock(bool paused, double elapsed, double position, double duration,
                            const char* audioLanguage, bool muted, const char* subtitleLanguage,
                            unsigned subtitle)
{
    if (!UsesStock()) return;
    const bool popup = m_stockPopup && m_trackPopupTime > 0 &&
        (m_trackPopupTime -= elapsed) > 0;
    if (m_visible <= 0 && !popup) return;
    XuiAnimRun(static_cast<float>(elapsed * 1000));
    XuiTimersRun();
    HXUIOBJ scene = m_stockScenes[m_stockPage], play;
    if (m_stockPage && (m_selected == 1 || m_selected == 3))
    {
        HXUIOBJ label;
        if (SUCCEEDED(XuiElementGetChildById(scene, L"txt_Action", &label)))
        {
            wchar_t text[96];
            if (m_selected == 1 && subtitle)
                swprintf_s(text, ARRAYSIZE(text), L"Subtitles %u: %hs", subtitle,
                           subtitleLanguage ? subtitleLanguage : "Unknown language");
            else if (m_selected == 1)
                swprintf_s(text, ARRAYSIZE(text), L"Subtitles: Off");
            else
                swprintf_s(text, ARRAYSIZE(text), L"Audio: %hs%ls",
                           audioLanguage ? audioLanguage : "Unavailable", muted ? L" (Muted)" : L"");
            XuiControlSetText(label, text);
        }
    }
    else if (m_stockInfo)
    {
        HXUIOBJ label;
        if (SUCCEEDED(XuiElementGetChildById(scene, L"txt_Action", &label)))
        {
            char current[16], total[16];
            wchar_t text[64];
            Clock(current, sizeof(current), position);
            Clock(total, sizeof(total), duration - position);
            if (duration > 0)
                swprintf_s(text, ARRAYSIZE(text), L"%hs / -%hs", current, total);
            else
                swprintf_s(text, ARRAYSIZE(text), L"%hs / --:--", current);
            XuiControlSetText(label, text);
        }
    }
    if (m_stockPlayState[m_stockPage] != int(paused) &&
        SUCCEEDED(XuiElementGetChildById(scene, L"btn_Play", &play)) &&
        SUCCEEDED(XuiControlSetImagePath(play, paused ?
            L"file://game:/Media/Blades/ico_32x_play_black.png" :
            L"file://game:/Media/Blades/ico_32x_pause_black.png")))
        m_stockPlayState[m_stockPage] = int(paused);
    if (SUCCEEDED(XuiRenderBegin(m_stockDc, 0)))
    {
        XUIMessage msg;
        XUIMessageRender render;
        XuiMessageRender(&msg, &render, m_stockDc, 0xffffffff, XUI_BLEND_NORMAL);
        if (m_visible > 0) XuiSendMessage(scene, &msg);
        if (popup)
        {
            const double fade = m_trackPopupTime < 0.25 ? m_trackPopupTime :
                m_trackPopupTime > 2.35 ? 2.5 - m_trackPopupTime : 0.25;
            const float opacity = static_cast<float>(fade *
                (m_trackPopupTime > 2.35 ? 1.0 / 0.15 : 4.0));
            XuiElementSetOpacity(m_stockPopup, opacity);
            XuiMessageRender(&msg, &render, m_stockDc, 0xffffffff, XUI_BLEND_NORMAL);
            XuiSendMessage(m_stockPopup, &msg);
        }
        XuiRenderEnd(m_stockDc);
    }
}

HRESULT PlaybackOsd::Open(IDirect3DDevice9* device)
{
    FILE* file = fopen("game:\\Media\\playback-osd.atlas", "rb");
    if (!file) return E_FAIL;
    HRESULT hr = device->CreateTexture(AtlasWidth, AtlasHeight, 1, 0, D3DFMT_LIN_L8,
                                       D3DPOOL_DEFAULT, &m_atlas, NULL);
    if (SUCCEEDED(hr))
    {
        D3DLOCKED_RECT locked;
        hr = m_atlas->LockRect(0, &locked, NULL, 0);
        if (SUCCEEDED(hr))
        {
            bool ok = locked.Pitch >= AtlasWidth &&
                fread(m_widths, 1, sizeof(m_widths), file) == sizeof(m_widths);
            for (unsigned y = 0; ok && y < AtlasHeight; ++y)
                ok = fread(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch,
                           1, AtlasWidth, file) == AtlasWidth;
            hr = m_atlas->UnlockRect(0);
            if (!ok) hr = E_FAIL;
        }
    }
    fclose(file);
    if (FAILED(hr)) return hr;
    ID3DXBuffer* code = NULL;
    hr = D3DXCompileShader(kVs, sizeof(kVs) - 1, NULL, NULL, "main",
                                   "vs_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr)) return hr;
    hr = device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &m_vs);
    code->Release();
    if (FAILED(hr)) return hr;
    hr = D3DXCompileShader(kPs, sizeof(kPs) - 1, NULL, NULL, "main",
                           "ps_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &m_ps);
    code->Release();
    if (FAILED(hr)) return hr;
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    return device->CreateVertexDeclaration(elements, &m_decl);
}

PlaybackOsd::Action PlaybackOsd::Update(double elapsed, bool paused, bool discButtons, bool discStill)
{
    const float step = elapsed < 0.05 ? static_cast<float>(elapsed) : 0.05f;
    if (m_visible > 0)
    {
        m_openAmount += step / 0.18f;
        if (m_openAmount > 1) m_openAmount = 1;
    }
    else
    {
        m_openAmount -= step / 0.14f;
        if (m_openAmount < 0) m_openAmount = 0;
    }
    const float blend = step * 16;
    m_focus += (static_cast<float>(m_selected) - m_focus) * blend;
    DWORD pressed = 0;
    for (DWORD pad = 0; pad < 4; ++pad)
    {
        XINPUT_STATE input = {};
        DWORD buttons = 0;
        if (XInputGetState(pad, &input) == ERROR_SUCCESS)
        {
            buttons = input.Gamepad.wButtons & 0xF3FF; // exclude Guide and reserved bits
            if (input.Gamepad.bLeftTrigger > 30) buttons |= 0x10000;
            if (input.Gamepad.bRightTrigger > 30) buttons |= 0x20000;
        }
        pressed |= buttons & ~m_buttons[pad];
        m_buttons[pad] = buttons;
    }
    if (!paused)
        m_visible = m_visible > elapsed ? m_visible - elapsed : 0;
    if (!pressed)
        return None;
    const bool wasVisible = m_visible > 0;
    if (!wasVisible && (discButtons || discStill))
    {
        if (pressed & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK))
        {
            m_visible = 4;
            return None;
        }
        if (discButtons)
        {
            if (pressed & XINPUT_GAMEPAD_DPAD_UP) return DiscUp;
            if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) return DiscDown;
            if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) return DiscLeft;
            if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) return DiscRight;
        }
        if (pressed & XINPUT_GAMEPAD_A) return discButtons ? DiscActivate : DiscContinue;
        return None;
    }
    if (wasVisible && (pressed & XINPUT_GAMEPAD_B))
    {
        if (UsesStock() && m_stockPage)
        {
            m_stockPage = 0;
            m_stockInfo = false;
            m_selected = 0;
            HXUIOBJ play;
            if (SUCCEEDED(XuiElementGetChildById(m_stockScenes[0], L"btn_Play", &play)))
                XuiElementSetFocus(play);
            return None;
        }
        m_stockInfo = false;
        m_visible = 0;
        return None;
    }
    m_visible = 4;
    if (pressed & XINPUT_GAMEPAD_START)
    {
        if (!wasVisible && UsesStock()) m_selected = 0;
        return TogglePause;
    }
    if (UsesStock())
    {
        static const wchar_t* const ids[2][8] = {
            {L"btn_Play", L"btn_Last", L"btn_RR", L"btn_Stop", L"btn_FF", L"btn_Next", L"btn_DVDMenu", L"btn_Title"},
            {L"btn_Play", L"btn_SubTitles", L"btn_Camera", L"btn_Sound", L"btn_Aspect", L"btn_Zoom", L"btn_AB", L"btn_Info"}
        };
        if (!wasVisible) m_selected = 0;
        if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) m_stockInfo = false;
        if (wasVisible && (pressed & XINPUT_GAMEPAD_DPAD_LEFT)) m_selected = (m_selected + 8) % 9;
        if (wasVisible && (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)) m_selected = (m_selected + 1) % 9;
        Action action = None;
        if (wasVisible && (pressed & XINPUT_GAMEPAD_A))
        {
            if (!m_selected) action = TogglePause;
            else if (!m_stockPage && m_selected == 1) action = PreviousChapter;
            else if (!m_stockPage && m_selected == 2) action = Rewind;
            else if (!m_stockPage && m_selected == 3) action = Stop;
            else if (!m_stockPage && m_selected == 4) action = FastForward;
            else if (!m_stockPage && m_selected == 5) action = NextChapter;
            else if (!m_stockPage && m_selected == 6) action = DiscRoot;
            else if (!m_stockPage && m_selected == 7) action = DiscTitle;
            else if (m_selected == 8) { m_stockPage ^= 1; m_selected = 0; m_stockInfo = false; }
            else if (m_stockPage && m_selected == 1) action = SubtitleTrack;
            else if (m_stockPage && m_selected == 3) action = NextAudioTrack;
            else if (m_stockPage && m_selected == 4) action = DisplayMode;
            else if (m_stockPage && m_selected == 5) action = ToggleZoom;
            else if (m_stockPage && m_selected == 6) action = RepeatAB;
            else if (m_stockPage && m_selected == 7) m_stockInfo = !m_stockInfo;
        }
        HXUIOBJ focused;
        const wchar_t* id = m_selected < 8 ? ids[m_stockPage][m_selected] :
            (m_stockPage ? L"btn_Playback" : L"btn_Display");
        if (SUCCEEDED(XuiElementGetChildById(m_stockScenes[m_stockPage], id, &focused)))
        {
            XuiElementSetFocus(focused);
            HXUIOBJ label;
            const wchar_t* text = XuiControlGetText(focused);
            if (text && SUCCEEDED(XuiElementGetChildById(m_stockScenes[m_stockPage], L"txt_Action", &label)))
                XuiControlSetText(label, text);
        }
        return action;
    }
    if (wasVisible && (pressed & XINPUT_GAMEPAD_DPAD_LEFT))
        m_selected = (m_selected + 2) % 3;
    if (wasVisible && (pressed & XINPUT_GAMEPAD_DPAD_RIGHT))
        m_selected = (m_selected + 1) % 3;
    if (wasVisible && (pressed & XINPUT_GAMEPAD_A))
        return m_selected == 2 ? SubtitleTrack : m_selected == 1 ? DisplayMode : TogglePause;
    return None;
}

void PlaybackOsd::Rect(float x, float y, float w, float h, DWORD color)
{
    Quad(x, y, w, h, color, 0.5f / AtlasWidth, 0.5f / AtlasHeight,
         0.5f / AtlasWidth, 0.5f / AtlasHeight);
}

void PlaybackOsd::DrawMenuFocus(IDirect3DDevice9* device, float x, float y, float width, float height)
{
    m_count = 0;
    m_panelOffset = 0;
    const DWORD color = 0xD0E9F8BD;
    Rect(x, y, width, 2, color);
    Rect(x, y + height - 2, width, 2, color);
    Rect(x, y, 2, height, color);
    Rect(x + width - 2, y, 2, height, color);
    Submit(device);
}

void PlaybackOsd::Panel(float x, float y, float w, float h, float radius, DWORD top, DWORD bottom)
{
    if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices)) return;
    const float halfW = w * 0.5f, halfH = h * 0.5f;
    if (radius > halfW) radius = halfW;
    if (radius > halfH) radius = halfH;
    // Leave room outside the shape for the shader's coverage ramp.
    const float u = halfW + 2, v = halfH + 2;
    const float l = 2 * (x - 2) / SceneWidth - 1, r = 2 * (x + w + 2) / SceneWidth - 1;
    const float t = 1 - 2 * (y + m_panelOffset - 2) / SceneHeight;
    const float b = 1 - 2 * (y + m_panelOffset + h + 2) / SceneHeight;
    const Vertex tl = {l,t,0,1,top,-u,-v,halfW,halfH,radius};
    const Vertex tr = {r,t,0,1,top, u,-v,halfW,halfH,radius};
    const Vertex bl = {l,b,0,1,bottom,-u,v,halfW,halfH,radius};
    const Vertex br = {r,b,0,1,bottom, u,v,halfW,halfH,radius};
    const Vertex quad[] = {tl,tr,bl,bl,tr,br};
    memcpy(m_vertices + m_count, quad, sizeof(quad));
    m_count += 6;
}

void PlaybackOsd::Quad(float x, float y, float w, float h, DWORD color,
                       float u0, float v0, float u1, float v1)
{
    if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices))
        return;
    y += m_panelOffset;
    const float l = 2 * x / SceneWidth - 1, r = 2 * (x + w) / SceneWidth - 1;
    const float t = 1 - 2 * y / SceneHeight, b = 1 - 2 * (y + h) / SceneHeight;
    const Vertex quad[] = {{l,t,0,1,color,u0,v0},{r,t,0,1,color,u1,v0},{l,b,0,1,color,u0,v1},
                           {l,b,0,1,color,u0,v1},{r,t,0,1,color,u1,v0},{r,b,0,1,color,u1,v1}};
    memcpy(m_vertices + m_count, quad, sizeof(quad));
    m_count += 6;
}

void PlaybackOsd::Text(float x, float y, const char* text, DWORD color, float scale)
{
    for (; *text; ++text)
    {
        const unsigned glyph = static_cast<BYTE>(*text);
        if (glyph >= sizeof(m_widths)) continue;
        const float u = float((glyph % 16) * CellSize) / AtlasWidth;
        const float v = float((glyph / 16) * CellSize) / AtlasHeight;
        if (glyph != ' ')
        {
            Quad(x, y, CellSize * scale, CellSize * scale, color,
                 u, v, u + float(CellSize) / AtlasWidth, v + float(CellSize) / AtlasHeight);
        }
        x += m_widths[glyph] * scale;
    }
}

void PlaybackOsd::ShowTrackPopup(bool subtitle, unsigned track, const char* language, bool muted)
{
    if (subtitle && track)
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Subtitles %u: %s", track,
                  language ? language : "Unknown language");
    else if (subtitle)
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Subtitles: %s", language ? language : "Off");
    else
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Audio: %s%s",
                  language ? language : "Unavailable", muted ? " (Muted)" : "");
    m_trackPopupTime = 2.5;
    HXUIOBJ label;
    if (m_stockPopup && SUCCEEDED(XuiElementGetChildById(m_stockPopup, L"txt_Action", &label)))
    {
        wchar_t text[96];
        swprintf_s(text, ARRAYSIZE(text), L"%hs", m_trackPopup);
        XuiControlSetText(label, text);
    }
}

void PlaybackOsd::DrawTrackPopup(IDirect3DDevice9* device, double elapsed)
{
    if (m_stockPopup || !Ready() || m_trackPopupTime <= 0 || (m_trackPopupTime -= elapsed) <= 0) return;
    const float scale = 0.8f;
    float width = 0;
    for (const char* p = m_trackPopup; *p; ++p)
        width += m_widths[static_cast<unsigned char>(*p)] * scale;
    const float x = (SceneWidth - width) / 2;
    m_count = 0;
    m_panelOffset = 0;
    Panel(x - 12, 303, width + 24, 30, 14, 0xF0EBEBEB, 0xE0AEB0B0);
    Text(x, 309, m_trackPopup, 0xFF273633, scale);
    Submit(device);
}

void PlaybackOsd::Draw(IDirect3DDevice9* device, unsigned, unsigned,
                       bool paused, unsigned mode, double seconds, double duration,
                       unsigned subtitle, unsigned subtitleCount)
{
    if (!m_decl || m_openAmount <= 0) return;
    const float closed = 1 - m_openAmount;
    // Keep the legend inside the 480-line title-safe area.
    m_panelOffset = -30 + 138 * closed * closed;
    static const char* const modes[DisplayModeCount] = {
        "Auto", "Letterbox", "Fullscreen", "Stretch", "Native", "Letterbox Zoom"
    };
    char time[16], remaining[16], display[32];
    Clock(time, sizeof(time), seconds);
    Clock(remaining, sizeof(remaining), duration - seconds);
    sprintf_s(display, sizeof(display), "\x03 %s", modes[mode % DisplayModeCount]);
    m_count = 0;
    const DWORD ink = 0xFF273633;
    // DvdPadPlayback.xui anchors the circle at (139,376), the control
    // strip at (150,385), and the action strip at (185,417).
    Panel(139, 376, 80, 80, 40, 0xFFEBEBEB, 0xC8EBEBEB);
    Panel(185, 417, 373, 29, 14, 0xF0EBEBEB, 0xC8AEB0B0);
    Panel(150, 385, 485, 53, 24, 0xF0EBEBEB, 0xC8AEB0B0);
    Panel(229, 369, 322, 4, 2, 0xC8828282, 0xC8EBEBEB);
    if (duration > 0)
    {
        const double fraction = seconds > duration ? 1.0 : seconds / duration;
        if (fraction > 0)
            Panel(229, 369, static_cast<float>(322 * fraction), 4,
                  2, 0xFFC8E87C, 0xFF74A637);
    }
    Text(233, 425, time, ink, 0.7f);
    Text(330, 425, paused ? "DVD  |  Paused" : "DVD  |  Playing", ink, 0.7f);
    if (duration > 0)
    {
        Text(466, 425, "-", ink, 0.7f);
        Text(476, 425, remaining, ink, 0.7f);
    }
    const float selection[] = {155, 220, 427};
    const float selectionWidth[] = {62, 204, 202};
    for (unsigned i = 0; i < 3; ++i)
    {
        Panel(selection[i], 388, selectionWidth[i], 39, 18, 0xFFEBEBEB, 0xFFAEB0B0);
    }
    const unsigned left = m_focus < 1 ? 0 : 1;
    const float fraction = m_focus - left;
    const float focusX = selection[left] + fraction * (selection[left+1] - selection[left]);
    const float focusW = selectionWidth[left] + fraction * (selectionWidth[left+1] - selectionWidth[left]);
    Panel(focusX, 388, focusW, 39, 18, 0xFFECF9C5, 0xFF759347);
    Panel(focusX+2, 390, focusW-4, 34, 16, 0xFFE9F8BD, 0xFFA8CD64);
    Text(170, 391, paused ? "\x01" : "\x02", ink);
    Text(230, 395, display, ink, 0.8f);
    char subtitles[32];
    if (subtitle)
        sprintf_s(subtitles, sizeof(subtitles), "\x04 Subtitles %u", subtitle);
    else
        strcpy_s(subtitles, sizeof(subtitles), subtitleCount ? "\x04 Subtitles Off" : "\x04 No subtitles");
    Text(437, 395, subtitles, ink, 0.8f);
    Panel(213, 448, 416, 27, 12, 0xF0EBEBEB, 0xE0AEB0B0);
    Text(223, 451, "\x05 Select    \x06 Back    \x08 Navigate    \x07 Play / Pause", ink, 0.7f);
    Submit(device);
    m_panelOffset = 0;
}

void PlaybackOsd::DrawBitmap(IDirect3DDevice9* device, float x, float y, float sx, float sy,
                              unsigned width, unsigned height, const BYTE* pixels, int pitch,
                              const BYTE* palette, unsigned colors)
{
    m_count = 0;
    for (unsigned row = 0; row < height; ++row)
    {
        const BYTE* line = pixels + static_cast<int>(row) * pitch;
        for (unsigned col = 0; col < width;)
        {
            const unsigned index = line[col];
            unsigned end = col + 1;
            while (end < width && line[end] == index)
                ++end;
            DWORD color = 0;
            if (index < colors)
                memcpy(&color, palette + index * sizeof(color), sizeof(color));
            if (color >> 24)
            {
                if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices))
                    Submit(device);
                Rect(x + col * sx, y + row * sy, (end - col) * sx, sy, color);
            }
            col = end;
        }
    }
    Submit(device);
}

void PlaybackOsd::Submit(IDirect3DDevice9* device)
{
    if (!m_count) return;
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_ps);
    device->SetTexture(0, m_atlas);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, m_count / 3, m_vertices, sizeof(Vertex));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_count = 0;
}
