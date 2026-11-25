#include "PlaybackOsd.h"

#include <d3dx9.h>
#include <stdio.h>
#include <string.h>

namespace
{
    const char kVs[] = "void main(float4 p:POSITION,float4 c:COLOR0,"
                       "out float4 pos:POSITION,out float4 col:COLOR0){pos=p;col=c;}";
    const char kPs[] = "float4 main(float4 c:COLOR0):COLOR0{return c;}";
    const char kGlyphs[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-";
    const BYTE kRows[][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,2,12},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
        {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
        {16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},
        {30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
        {17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
        {0,4,4,0,4,4,0},{0,0,0,31,0,0,0}
    };

    void Clock(char* text, size_t size, double seconds)
    {
        const unsigned s = seconds > 0 ? static_cast<unsigned>(seconds) : 0;
        sprintf_s(text, size, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    }
}

PlaybackOsd::PlaybackOsd() : m_vs(NULL), m_ps(NULL), m_decl(NULL), m_selected(0),
    m_count(0), m_visible(0)
{
    memset(m_buttons, 0, sizeof(m_buttons));
}

PlaybackOsd::~PlaybackOsd()
{
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
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

PlaybackOsd::Action PlaybackOsd::Update(double elapsed, bool paused)
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
    if (!paused)
        m_visible = m_visible > elapsed ? m_visible - elapsed : 0;
    if (!pressed)
        return None;
    const bool wasVisible = m_visible > 0;
    if (wasVisible && (pressed & XINPUT_GAMEPAD_B))
    {
        m_visible = 0;
        return None;
    }
    m_visible = 4;
    if (pressed & XINPUT_GAMEPAD_START)
        return TogglePause;
    if (wasVisible && (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)))
        m_selected ^= 1;
    if (wasVisible && (pressed & XINPUT_GAMEPAD_A))
        return m_selected ? DisplayMode : TogglePause;
    return None;
}

void PlaybackOsd::Rect(float x, float y, float w, float h, DWORD color)
{
    if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices))
        return;
    const float l = x / 640 - 1, r = (x + w) / 640 - 1;
    const float t = 1 - y / 360, b = 1 - (y + h) / 360;
    const Vertex quad[] = {{l,t,0,1,color},{r,t,0,1,color},{l,b,0,1,color},
                           {l,b,0,1,color},{r,t,0,1,color},{r,b,0,1,color}};
    memcpy(m_vertices + m_count, quad, sizeof(quad));
    m_count += 6;
}

void PlaybackOsd::Text(float x, float y, const char* text, DWORD color)
{
    for (; *text; ++text, x += 12)
    {
        const char* glyph = strchr(kGlyphs, *text);
        if (!glyph) continue;
        const BYTE* rows = kRows[glyph - kGlyphs];
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (rows[row] & (16 >> col))
                    Rect(x + 2 * col, y + 2 * row, 2, 2, color);
    }
}

void PlaybackOsd::Draw(IDirect3DDevice9* device, unsigned, unsigned,
                       bool paused, unsigned mode, double seconds, double duration)
{
    if (!m_decl || m_visible <= 0) return;
    static const char* const modes[DisplayModeCount] = {
        "AUTO", "LETTERBOX", "FULLSCREEN", "STRETCH", "NATIVE", "LETTERBOX ZOOM"
    };
    char time[16], remaining[16], display[32];
    Clock(time, sizeof(time), seconds);
    Clock(remaining, sizeof(remaining), duration - seconds);
    sprintf_s(display, sizeof(display), "DISPLAY %s", modes[mode % DisplayModeCount]);
    m_count = 0;
    Rect(64, 540, 1152, 120, 0xDF152020);
    Rect(88, 555, 1104, 4, 0xFF667373);
    if (duration > 0)
    {
        const double fraction = seconds > duration ? 1.0 : seconds / duration;
        if (fraction > 0) Rect(88, 555, static_cast<float>(1104 * fraction), 4, 0xFF95CC45);
    }
    Text(88, 569, time, 0xFFFFFFFF);
    if (duration > 0)
    {
        Text(1084, 569, "-", 0xFFFFFFFF);
        Text(1096, 569, remaining, 0xFFFFFFFF);
    }
    Rect(m_selected ? 248 : 84, 594, m_selected ? 272 : 144, 26, 0xFF628E29);
    Text(92, 600, paused ? "PLAY" : "PAUSE", 0xFFFFFFFF);
    Text(256, 600, display, 0xFFFFFFFF);
    Text(88, 634, "A SELECT   B BACK   START PLAY PAUSE", 0xFFD5DEDE);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_ps);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, m_count / 3, m_vertices, sizeof(Vertex));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}
