#include "PlaybackOsd.h"
#include "DvdEmbeddedResources.h"

#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace
{
    HRESULT PrepareStockTypeface()
    {
        const char path[] = "game:\\Media\\Blades\\XenonCLatin.ttf";
        UINT size;
        const BYTE* bytes = FindDvdResource(L"XenonCLatin.ttf", size);
        if (!bytes) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) &&
            !attributes.nFileSizeHigh && attributes.nFileSizeLow == size)
            return S_OK;
        CreateDirectoryA("game:\\Media", NULL);
        CreateDirectoryA("game:\\Media\\Blades", NULL);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        DWORD written = 0;
        const BOOL ok = WriteFile(file, bytes, size, &written, NULL);
        CloseHandle(file);
        return ok && written == size ? S_OK : E_FAIL;
    }

    HRESULT APIENTRY LoadStockTexture(IXuiDevice* device, LPCWSTR locator,
                                      XUIImageInfo* info, IDirect3DTexture9** texture)
    {
        if (!locator) return E_INVALIDARG;
        // The dashboard uses its own resource locators. Read the bundled
        // DVD icons directly and let XUI decode the original PNG bytes.
        const wchar_t* name = locator;
        for (const wchar_t* p = locator; *p; ++p)
            if (*p == L'/' || *p == L'\\') name = p + 1;
        if (wcsncmp(name, L"ico_32x_", 8) != 0 && wcsncmp(name, L"wmp11-", 6) != 0)
            return XuiPNGTextureLoader(device, locator, info, texture);
        UINT size;
        const BYTE* png = FindDvdResource(name, size);
        return png ? XuiCreateTextureFromPNG(const_cast<BYTE*>(png), size, device, texture, info)
                   : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const char kVs[] = "void main(float4 p:POSITION,float4 c:COLOR0,"
        "out float4 pos:POSITION,out float4 col:COLOR0){pos=p;col=c;}";
    const char kPs[] = "float4 main(float4 c:COLOR0):COLOR0{return c;}";
    void Clock(char* text, size_t size, double seconds)
    {
        const unsigned s = seconds > 0 ? static_cast<unsigned>(seconds) : 0;
        sprintf_s(text, size, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    }

}

PlaybackOsd::PlaybackOsd() : m_vs(NULL), m_ps(NULL), m_decl(NULL), m_selected(0),
    m_trackPopupTime(0), m_count(0), m_visible(0),
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
    const wchar_t root[] = L"dvd360:///";
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: register embedded resources";
        hr = RegisterDvdResourceTransport();
    }
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: register DVD typeface";
        // The font loader needs a filesystem locator even though XUI
        // scenes and textures can use the embedded resource transport.
        const bool fileReady = SUCCEEDED(PrepareStockTypeface());
        TypefaceDescriptor typeface = { L"Xenon", fileReady
            ? L"file://game:/Media/Blades/XenonCLatin.ttf"
            : L"dvd360:///XenonCLatin.ttf" };
        hr = XuiRegisterTypeface(&typeface, TRUE);
    }
    if (SUCCEEDED(hr))
    {
        stage = "Stock XUI: load DVD skin";
        hr = XuiLoadVisualFromBinary(L"dvd360:///dvd_skin.xur", NULL);
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
        XuiSceneCreate(root, L"CdPadPlayback.xur", NULL, &m_cdScene);
        stage = SUCCEEDED(XuiSceneCreate(root, L"DvdTrackPopup.xur", NULL, &m_stockPopup))
            ? "Stock XUI: ready" : "Stock XUI: ready (track popup asset unavailable)";
    }
    return hr;
}

void PlaybackOsd::CloseStock()
{
    if (m_cdScene) XuiDestroyObject(m_cdScene);
    m_cdScene = NULL;
    ResetCdFocus();
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
                            const char* audioLanguage, const char* subtitleLanguage,
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
                swprintf_s(text, ARRAYSIZE(text), L"Audio: %hs",
                           audioLanguage ? audioLanguage : "Unavailable");
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
            L"dvd360:///ico_32x_play_black.png" :
            L"dvd360:///ico_32x_pause_black.png")))
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
    ID3DXBuffer* code = NULL;
    HRESULT hr = D3DXCompileShader(kVs, sizeof(kVs) - 1, NULL, NULL, "main",
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
        D3DDECL_END()
    };
    return device->CreateVertexDeclaration(elements, &m_decl);
}

DWORD PlaybackOsd::ReadPressedButtons()
{
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
    return pressed;
}

PlaybackOsd::Action PlaybackOsd::UpdateCd()
{
    const DWORD pressed = ReadPressedButtons();
    const DWORD horizontal = pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    if (horizontal == XINPUT_GAMEPAD_DPAD_LEFT && m_cdSelected > 0) --m_cdSelected;
    if (horizontal == XINPUT_GAMEPAD_DPAD_RIGHT && m_cdSelected < 2) ++m_cdSelected;
    if (pressed & XINPUT_GAMEPAD_START) return TogglePause;
    if (!(pressed & XINPUT_GAMEPAD_A)) return None;
    return m_cdSelected == 0 ? PreviousChapter :
           m_cdSelected == 1 ? TogglePause : NextChapter;
}

void PlaybackOsd::ResetCdFocus()
{
    m_cdSelected = 1;
    m_cdFocused = -1;
    m_cdPaused = -1;
}

void PlaybackOsd::DrawCd(bool paused, double elapsed)
{
    if (!m_cdScene) return;
    if (m_cdFocused != int(m_cdSelected))
    {
        const wchar_t* ids[] = {L"focus_Previous", L"focus_Play", L"focus_Next"};
        for (unsigned i = 0; i < ARRAYSIZE(ids); ++i)
        {
            HXUIOBJ ring;
            if (SUCCEEDED(XuiElementGetChildById(m_cdScene, ids[i], &ring)))
                XuiElementSetOpacity(ring, i == m_cdSelected ? 1.0f : 0.0f);
        }
        m_cdFocused = int(m_cdSelected);
    }
    if (m_cdPaused != int(paused))
    {
        HXUIOBJ icon;
        if (SUCCEEDED(XuiElementGetChildById(m_cdScene, L"ico_Play", &icon)))
            XuiElementSetOpacity(icon, paused ? 1.0f : 0.0f);
        if (SUCCEEDED(XuiElementGetChildById(m_cdScene, L"ico_Pause", &icon)))
            XuiElementSetOpacity(icon, paused ? 0.0f : 1.0f);
        m_cdPaused = int(paused);
    }
    XuiAnimRun(static_cast<float>(elapsed * 1000));
    XuiTimersRun();
    if (SUCCEEDED(XuiRenderBegin(m_stockDc, 0)))
    {
        XUIMessage msg;
        XUIMessageRender render;
        XuiMessageRender(&msg, &render, m_stockDc, 0xffffffff, XUI_BLEND_NORMAL);
        XuiSendMessage(m_cdScene, &msg);
        XuiRenderEnd(m_stockDc);
    }
}

PlaybackOsd::Action PlaybackOsd::Update(double elapsed, bool paused, bool discButtons, bool discStill)
{
    const DWORD pressed = ReadPressedButtons();
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
            else if (!m_stockPage && m_selected == 2) action = SeekBackward;
            else if (!m_stockPage && m_selected == 3) action = Stop;
            else if (!m_stockPage && m_selected == 4) action = SeekForward;
            else if (!m_stockPage && m_selected == 5) action = NextChapter;
            else if (!m_stockPage && m_selected == 6) action = DiscRoot;
            else if (!m_stockPage && m_selected == 7) action = DiscTitle;
            else if (m_selected == 8) { m_stockPage ^= 1; m_selected = 0; m_stockInfo = false; }
            else if (m_stockPage && m_selected == 1) action = SubtitleTrack;
            else if (m_stockPage && m_selected == 3) action = NextAudioTrack;
            else if (m_stockPage && m_selected == 4) action = DisplayMode;
            else if (m_stockPage && m_selected == 5) action = ToggleZoom;
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
    return None;
}

void PlaybackOsd::Rect(float x, float y, float w, float h, DWORD color)
{
    if (m_count + 6 > ARRAYSIZE(m_vertices)) return;
    const float l = 2 * x / SceneWidth - 1, r = 2 * (x + w) / SceneWidth - 1;
    const float t = 1 - 2 * y / SceneHeight, b = 1 - 2 * (y + h) / SceneHeight;
    const Vertex quad[] = {{l,t,0,1,color},{r,t,0,1,color},{l,b,0,1,color},
                           {l,b,0,1,color},{r,t,0,1,color},{r,b,0,1,color}};
    memcpy(m_vertices + m_count, quad, sizeof(quad));
    m_count += 6;
}

void PlaybackOsd::DrawMenuFocus(IDirect3DDevice9* device, float x, float y, float width, float height)
{
    m_count = 0;
    const DWORD color = 0xD0E9F8BD;
    Rect(x, y, width, 2, color);
    Rect(x, y + height - 2, width, 2, color);
    Rect(x, y, 2, height, color);
    Rect(x + width - 2, y, 2, height, color);
    Submit(device);
}

void PlaybackOsd::ShowTrackPopup(bool subtitle, unsigned track, const char* language)
{
    if (subtitle && track)
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Subtitles %u: %s", track,
                  language ? language : "Unknown language");
    else if (subtitle)
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Subtitles: %s", language ? language : "Off");
    else
        sprintf_s(m_trackPopup, sizeof(m_trackPopup), "Audio: %s",
                  language ? language : "Unavailable");
    StartPopup();
}

void PlaybackOsd::ShowDeinterlacePopup()
{
    strcpy_s(m_trackPopup, sizeof(m_trackPopup), "Deinterlacing active (bwdif)");
    StartPopup();
}

void PlaybackOsd::StartPopup()
{
    m_trackPopupTime = 2.5;
    HXUIOBJ label;
    if (m_stockPopup && SUCCEEDED(XuiElementGetChildById(m_stockPopup, L"txt_Action", &label)))
    {
        wchar_t text[96];
        swprintf_s(text, ARRAYSIZE(text), L"%hs", m_trackPopup);
        XuiControlSetText(label, text);
    }
}

void PlaybackOsd::ShowNoDisc()
{
    strcpy_s(m_trackPopup, sizeof(m_trackPopup), "NO DISC");
    StartPopup();
    m_trackPopupTime = 0;
}

void PlaybackOsd::DrawNoDisc()
{
    if (!m_stockPopup || FAILED(XuiRenderBegin(m_stockDc, 0))) return;
    XuiElementSetOpacity(m_stockPopup, 1.0f);
    XUIMessage msg;
    XUIMessageRender render;
    XuiMessageRender(&msg, &render, m_stockDc, 0xffffffff, XUI_BLEND_NORMAL);
    XuiSendMessage(m_stockPopup, &msg);
    XuiRenderEnd(m_stockDc);
}

void PlaybackOsd::DrawBitmap(IDirect3DDevice9* device, float x, float y, float sx, float sy,
                              unsigned width, unsigned height, const BYTE* pixels, int pitch,
                              const BYTE* palette, unsigned colors)
{
    m_count = 0;
    if (!width || !height || !pixels || !palette || pitch == 0)
        return;

    // Decode the palette once per bitmap. Keep the original row-local run
    // geometry: vertical coalescing can change edge rasterization on D3D9.
    DWORD paletteColors[256] = {};
    const unsigned paletteCount = colors < ARRAYSIZE(paletteColors)
        ? colors : ARRAYSIZE(paletteColors);
    for (unsigned i = 0; i < paletteCount; ++i)
        memcpy(&paletteColors[i], palette + i * sizeof(DWORD), sizeof(DWORD));

    for (unsigned row = 0; row < height; ++row)
    {
        const BYTE* line = pixels + static_cast<int>(row) * pitch;
        for (unsigned col = 0; col < width;)
        {
            const unsigned index = line[col];
            unsigned end = col + 1;
            while (end < width && line[end] == index)
                ++end;
            const DWORD color = index < paletteCount ? paletteColors[index] : 0;
            if (color >> 24)
            {
                if (m_count + 6 > ARRAYSIZE(m_vertices))
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
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, m_count / 3, m_vertices, sizeof(Vertex));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_count = 0;
}
