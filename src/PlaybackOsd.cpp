#include "PlaybackOsd.h"

#include <d3dx9.h>
#include <stdio.h>
#include <string.h>

namespace
{
    enum { AtlasWidth = 512, AtlasHeight = 256, CellSize = 32 };
    const char kVs[] = "void main(float4 p:POSITION,float4 c:COLOR0,float2 uv:TEXCOORD0,"
        "out float4 pos:POSITION,out float4 col:COLOR0,out float2 tc:TEXCOORD0){pos=p;col=c;tc=uv;}";
    const char kPs[] = "sampler2D atlas:register(s0);"
        "float4 main(float4 c:COLOR0,float2 uv:TEXCOORD0):COLOR0"
        "{return float4(c.rgb,c.a*tex2D(atlas,uv).r);}";

    void Clock(char* text, size_t size, double seconds)
    {
        const unsigned s = seconds > 0 ? static_cast<unsigned>(seconds) : 0;
        sprintf_s(text, size, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    }

    DWORD MixColor(DWORD a, DWORD b, float fraction)
    {
        DWORD result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            const float from = float((a >> shift) & 255);
            const float to = float((b >> shift) & 255);
            result |= static_cast<DWORD>(from + (to - from) * fraction + 0.5f) << shift;
        }
        return result;
    }
}

PlaybackOsd::PlaybackOsd() : m_vs(NULL), m_ps(NULL), m_decl(NULL), m_atlas(NULL), m_selected(0),
    m_count(0), m_visible(0), m_openAmount(0), m_focus(0), m_panelOffset(0)
{
    memset(m_buttons, 0, sizeof(m_buttons));
}

PlaybackOsd::~PlaybackOsd()
{
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
    if (m_atlas) m_atlas->Release();
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
        D3DDECL_END()
    };
    return device->CreateVertexDeclaration(elements, &m_decl);
}

PlaybackOsd::Action PlaybackOsd::Update(double elapsed, bool paused)
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
    if (wasVisible && (pressed & XINPUT_GAMEPAD_B))
    {
        m_visible = 0;
        return None;
    }
    m_visible = 4;
    if (pressed & XINPUT_GAMEPAD_START)
        return TogglePause;
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

void PlaybackOsd::Panel(float x, float y, float w, float h, float radius, DWORD top, DWORD bottom)
{
    // Sample quarter-circle corners; interpolate the body as one gradient.
    const float offsets[] = {1.0f, 0.339f, 0.134f, 0.032f, 0.0f, 0.0f, 0.032f, 0.134f, 0.339f, 1.0f};
    const float rows[] = {0, radius*.25f, radius*.5f, radius*.75f, radius,
                          h-radius, h-radius*.75f, h-radius*.5f, h-radius*.25f, h};
    const float u = 0.5f / AtlasWidth, v = 0.5f / AtlasHeight;
    for (unsigned i = 0; i + 1 < sizeof(rows) / sizeof(*rows); ++i)
    {
        if (rows[i] == rows[i+1]) continue;
        if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices)) return;
        const float l = (x + radius * offsets[i]) / 640 - 1;
        const float r = (x + w - radius * offsets[i]) / 640 - 1;
        const float nl = (x + radius * offsets[i+1]) / 640 - 1;
        const float nr = (x + w - radius * offsets[i+1]) / 640 - 1;
        const float t = 1 - (y + m_panelOffset + rows[i]) / 360;
        const float b = 1 - (y + m_panelOffset + rows[i+1]) / 360;
        const DWORD a = MixColor(top, bottom, rows[i] / h);
        const DWORD z = MixColor(top, bottom, rows[i+1] / h);
        const Vertex strip[] = {{l,t,0,1,a,u,v},{r,t,0,1,a,u,v},{nl,b,0,1,z,u,v},
                                {nl,b,0,1,z,u,v},{r,t,0,1,a,u,v},{nr,b,0,1,z,u,v}};
        memcpy(m_vertices + m_count, strip, sizeof(strip));
        m_count += 6;
    }
}

void PlaybackOsd::Quad(float x, float y, float w, float h, DWORD color,
                       float u0, float v0, float u1, float v1)
{
    if (m_count + 6 > sizeof(m_vertices) / sizeof(*m_vertices))
        return;
    y += m_panelOffset;
    const float l = x / 640 - 1, r = (x + w) / 640 - 1;
    const float t = 1 - y / 360, b = 1 - (y + h) / 360;
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
            Quad(x, y, CellSize * scale, CellSize * scale, color,
                 u, v, u + float(CellSize) / AtlasWidth, v + float(CellSize) / AtlasHeight);
        x += m_widths[glyph] * scale;
    }
}

void PlaybackOsd::Draw(IDirect3DDevice9* device, unsigned, unsigned,
                       bool paused, unsigned mode, double seconds, double duration,
                       unsigned subtitle, unsigned subtitleCount)
{
    if (!m_decl || m_openAmount <= 0) return;
    const float closed = 1 - m_openAmount;
    m_panelOffset = 190 * closed * closed;
    static const char* const modes[DisplayModeCount] = {
        "Auto", "Letterbox", "Fullscreen", "Stretch", "Native", "Letterbox Zoom"
    };
    char time[16], remaining[16], display[32];
    Clock(time, sizeof(time), seconds);
    Clock(remaining, sizeof(remaining), duration - seconds);
    sprintf_s(display, sizeof(display), "\x03 %s", modes[mode % DisplayModeCount]);
    m_count = 0;
    const DWORD ink = 0xFF273633;
    Panel(68, 532, 1144, 130, 19, 0x28000000, 0x80000000);
    Panel(73, 527, 1134, 130, 16, 0xFFFDFEF8, 0xFF81908B);
    Panel(75, 529, 1130, 126, 14, 0xFFF6F7EF, 0xFFD0D9D0);
    Panel(86, 535, 1108, 45, 11, 0xFFC9D9E2, 0xFFE3ECED);
    Panel(94, 549, 1092, 6, 3, 0xFF8B9C9D, 0xFFBDC8C9);
    if (duration > 0)
    {
        const double fraction = seconds > duration ? 1.0 : seconds / duration;
        if (fraction > 0)
            Panel(94, 549, static_cast<float>(1092 * fraction), 6,
                  fraction < 0.006 ? 0 : 3, 0xFFC8E87C, 0xFF74A637);
    }
    Text(94, 556, time, ink, 0.85f);
    Text(578, 556, paused ? "DVD  |  Paused" : "DVD  |  Playing", 0xFF344F5D, 0.85f);
    if (duration > 0)
    {
        Text(1078, 556, "-", ink, 0.85f);
        Text(1090, 556, remaining, ink, 0.85f);
    }
    const float selection[] = {88, 249, 629};
    const float selectionWidth[] = {157, 376, 550};
    for (unsigned i = 0; i < 3; ++i)
    {
        Panel(selection[i], 585, selectionWidth[i], 39, 9, 0xFFFDFEF9, 0xFF9FABA3);
        Panel(selection[i]+1, 586, selectionWidth[i]-2, 36, 8, 0xFFF5F8F2, 0xFFD7E0D7);
    }
    const unsigned left = m_focus < 1 ? 0 : 1;
    const float fraction = m_focus - left;
    const float focusX = selection[left] + fraction * (selection[left+1] - selection[left]);
    const float focusW = selectionWidth[left] + fraction * (selectionWidth[left+1] - selectionWidth[left]);
    Panel(focusX, 584, focusW, 41, 10, 0xFFECF9C5, 0xFF759347);
    Panel(focusX+1, 585, focusW-2, 38, 9, 0xFFE9F8BD, 0xFFA8CD64);
    Panel(focusX+4, 587, focusW-8, 12, 5, 0xAEFFFFFF, 0x00FFFFFF);
    Text(105, 590, paused ? "\x01 Play" : "\x02 Pause", ink);
    Text(266, 590, display, ink);
    char subtitles[32];
    if (subtitle)
        sprintf_s(subtitles, sizeof(subtitles), "\x04 Subtitles %u", subtitle);
    else
        strcpy_s(subtitles, sizeof(subtitles), subtitleCount ? "\x04 Subtitles Off" : "\x04 No subtitles");
    Text(646, 590, subtitles, ink);
    Text(90, 628, "\x05", 0xFF386317, 0.85f);
    Text(120, 628, "Select", ink, 0.85f);
    Text(207, 628, "\x06", 0xFF973725, 0.85f);
    Text(237, 628, "Back", ink, 0.85f);
    Text(307, 628, "\x08 Navigate     \x07 Play / Pause", ink, 0.85f);
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
