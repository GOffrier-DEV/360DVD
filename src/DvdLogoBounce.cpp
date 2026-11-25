#include "DvdLogoBounce.h"
#include "DvdEmbeddedResources.h"

#include <d3dx9.h>
#include <xgraphics.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace
{
    struct Vertex
    {
        float x, y, z, w;
        float u, v;
    };

    WORD ReadLittleEndian16(const BYTE* bytes)
    {
        return (WORD)((WORD)bytes[0] | ((WORD)bytes[1] << 8));
    }

    DWORD ReadLittleEndian32(const BYTE* bytes)
    {
        return (DWORD)bytes[0] |
               ((DWORD)bytes[1] << 8) |
               ((DWORD)bytes[2] << 16) |
               ((DWORD)bytes[3] << 24);
    }
}

namespace
{
    const D3DVERTEXELEMENT9 kVertexElements[] =
    {
        { 0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    const DWORD kColors[] =
    {
        0xFFFFFFFF, // white
        0xFFFFD22E, // gold
        0xFF42D7FF, // cyan
        0xFF63E65C, // green
        0xFFFF5C91, // pink
        0xFFB785FF, // violet
        0xFFFF7048  // orange-red
    };

    const unsigned int kColorCount = sizeof(kColors) / sizeof(kColors[0]);

    void StartupLog(const char* message, HRESULT result = S_OK)
    {
#ifdef DVD360_LOGGING
        char line[256];
        sprintf_s(line, sizeof(line), "%s (hr=0x%08X)\r\n",
                  message, (unsigned int)result);
        OutputDebugStringA(line);

        HANDLE file = CreateFile("game:\\360DVD.log", GENERIC_WRITE,
                                 FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            SetFilePointer(file, 0, NULL, FILE_END);
            DWORD written = 0;
            WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
            CloseHandle(file);
        }
#endif
    }

    const char* kVertexShaderSource =
        "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "V main(V v) { return v; }\n";

    const char* kPixelShaderSource =
        "sampler2D logo : register(s0);\n"
        "float4 tint : register(c0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 texel = tex2D(logo, uv);\n"
        "  return float4(tint.rgb, texel.a * tint.a);\n"
        "}\n";

    bool CompileShader(const char* source, const char* profile, ID3DXBuffer** output)
    {
        ID3DXBuffer* errors = NULL;
        HRESULT hr = D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL,
                                       "main", profile, 0, output, &errors, NULL);
        if (FAILED(hr))
        {
            StartupLog(profile, hr);
            if (errors && errors->GetBufferPointer())
                StartupLog((const char*)errors->GetBufferPointer());
        }
        if (errors)
            errors->Release();
        return SUCCEEDED(hr);
    }

    float Clamp(float value, float low, float high)
    {
        if (value < low)
            return low;
        if (value > high)
            return high;
        return value;
    }
}

DvdLogoBounce::DvdLogoBounce()
    : m_device(NULL), m_texture(NULL), m_vertexShader(NULL), m_pixelShader(NULL),
      m_vertexDeclaration(NULL), m_screenWidth(0), m_screenHeight(0),
      m_x(0), m_y(0), m_width(0), m_height(0),
      m_velocityX(90.0f), m_velocityY(90.0f), m_colorIndex(0)
{
}

DvdLogoBounce::~DvdLogoBounce()
{
    Shutdown();
}

bool DvdLogoBounce::Startup(IDirect3DDevice9* device, int screenWidth, int screenHeight)
{
    if (!device || screenWidth <= 0 || screenHeight <= 0)
        return false;

    m_device = device;
    m_device->AddRef();
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_width = (float)screenWidth * 0.20f;
    m_height = m_width * (300.0f / 500.0f);
    m_x = ((float)screenWidth - m_width) * 0.5f;
    m_y = ((float)screenHeight - m_height) * 0.5f;

    if (!LoadLogoTexture())
    {
        StartupLog("Logo texture load failed");
        Shutdown();
        return false;
    }
    if (!CreateShaders())
    {
        StartupLog("Shader setup failed");
        Shutdown();
        return false;
    }
    return true;
}

bool DvdLogoBounce::LoadLogoTexture()
{
    UINT size;
    const BYTE* fileHeader = FindDvdResource(L"dvd-video-logo.bmp", size);
    if (!fileHeader || size < 54) return false;
    const BYTE* infoHeader = fileHeader + 14;
    bool ok = true;

    const WORD type = ok ? ReadLittleEndian16(fileHeader) : 0;
    const DWORD pixelOffset = ok ? ReadLittleEndian32(fileHeader + 10) : 0;
    const DWORD infoSize = ok ? ReadLittleEndian32(infoHeader) : 0;
    const LONG bitmapWidth = ok ? (LONG)ReadLittleEndian32(infoHeader + 4) : 0;
    const LONG bitmapHeight = ok ? (LONG)ReadLittleEndian32(infoHeader + 8) : 0;
    const WORD planes = ok ? ReadLittleEndian16(infoHeader + 12) : 0;
    const WORD bitCount = ok ? ReadLittleEndian16(infoHeader + 14) : 0;
    const DWORD compression = ok ? ReadLittleEndian32(infoHeader + 16) : 0;

    if (type != 0x4D42 || infoSize < 40 ||
        bitmapWidth <= 0 || bitmapWidth > 2048 ||
        bitmapHeight == 0 || bitmapHeight == LONG_MIN ||
        bitmapHeight > 2048 || bitmapHeight < -2048 || planes != 1 ||
        bitCount != 32 || compression != 0)
    {
        StartupLog("Unsupported or invalid BMP header");
        return false;
    }

    const int width = (int)bitmapWidth;
    const int height = (int)(bitmapHeight < 0 ? -bitmapHeight : bitmapHeight);
    const bool topDown = bitmapHeight < 0;
    const DWORD rowBytes = (DWORD)width * 4;
    if (rowBytes > UINT_MAX / (DWORD)height)
        return false;
    const DWORD imageBytes = rowBytes * (DWORD)height;
    if (pixelOffset > size || imageBytes > size - pixelOffset)
        return false;
    if (pixelOffset < 14 + infoSize)
        return false;

    HRESULT hr = m_device->CreateTexture(width, height, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &m_texture, NULL);
    if (FAILED(hr))
    {
        StartupLog("CreateTexture failed", hr);
        return false;
    }

    D3DLOCKED_RECT locked;
    if (FAILED(m_texture->LockRect(0, &locked, NULL, 0)))
    {
        StartupLog("Logo texture LockRect failed");
        return false;
    }

    if (locked.Pitch < 0 || (DWORD)locked.Pitch < rowBytes)
    {
        m_texture->UnlockRect(0);
        return false;
    }
    BYTE* pixels = new BYTE[imageBytes];
    if (!pixels)
        ok = false;

    for (int sourceY = 0; ok && sourceY < height; ++sourceY)
    {
        const int destinationY = topDown ? sourceY : height - 1 - sourceY;
        BYTE* row = pixels + destinationY * rowBytes;
        memcpy(row, fileHeader + pixelOffset + sourceY * rowBytes, rowBytes);
    }

    if (ok)
    {
        XGEndianSwapSurface(locked.pBits, (UINT)locked.Pitch, pixels,
                            rowBytes, width, height, D3DFMT_LIN_A8R8G8B8);
    }
    delete[] pixels;
    m_texture->UnlockRect(0);
    return ok;
}

bool DvdLogoBounce::CreateShaders()
{
    ID3DXBuffer* code = NULL;
    if (!CompileShader(kVertexShaderSource, "vs_3_0", &code))
        return false;
    HRESULT hr = m_device->CreateVertexShader((const DWORD*)code->GetBufferPointer(),
                                               &m_vertexShader);
    code->Release();
    if (FAILED(hr))
        return false;

    if (!CompileShader(kPixelShaderSource, "ps_3_0", &code))
        return false;
    hr = m_device->CreatePixelShader((const DWORD*)code->GetBufferPointer(),
                                     &m_pixelShader);
    code->Release();
    if (FAILED(hr))
        return false;

    return SUCCEEDED(m_device->CreateVertexDeclaration(kVertexElements,
                                                       &m_vertexDeclaration));
}

void DvdLogoBounce::Update(float elapsedSeconds)
{
    // Clamp long pauses (for example, when the debugger breaks) to avoid a
    // visible teleport while retaining smooth, frame-rate-independent motion.
    const float dt = Clamp(elapsedSeconds, 0.0f, 0.05f);
    m_x += m_velocityX * dt;
    m_y += m_velocityY * dt;

    const float maxX = (float)m_screenWidth - m_width;
    const float maxY = (float)m_screenHeight - m_height;
    bool hitX = false;
    bool hitY = false;

    while (m_x < 0.0f || m_x > maxX)
    {
        if (m_x < 0.0f)
        {
            m_x = -m_x;
            m_velocityX = (float)fabs(m_velocityX);
        }
        else
        {
            m_x = 2.0f * maxX - m_x;
            m_velocityX = -(float)fabs(m_velocityX);
        }
        hitX = true;
    }

    while (m_y < 0.0f || m_y > maxY)
    {
        if (m_y < 0.0f)
        {
            m_y = -m_y;
            m_velocityY = (float)fabs(m_velocityY);
        }
        else
        {
            m_y = 2.0f * maxY - m_y;
            m_velocityY = -(float)fabs(m_velocityY);
        }
        hitY = true;
    }

    if (hitX)
        m_colorIndex = (m_colorIndex + 1) % kColorCount;
    if (hitY)
        m_colorIndex = (m_colorIndex + 1) % kColorCount;
}

void DvdLogoBounce::Center()
{
    m_x = (float(m_screenWidth) - m_width) * 0.5f;
    m_y = (float(m_screenHeight) - m_height) * 0.5f;
}

void DvdLogoBounce::Render(bool noDisc)
{
    if (!m_device || !m_texture || !m_vertexShader || !m_pixelShader)
        return;

    if (noDisc && !m_noDiscLoadAttempted)
    {
        m_noDiscLoadAttempted = true;
        UINT size = 0;
        const BYTE* png = FindDvdResource(L"DVD-plus-CD-original.png", size);
        D3DXIMAGE_INFO info = {};
        if (png && SUCCEEDED(D3DXGetImageInfoFromFileInMemory(png, size, &info)) &&
            SUCCEEDED(D3DXCreateTextureFromFileInMemory(m_device, png, size, &m_noDiscTexture)))
            m_noDiscAspect = float(info.Width) / info.Height;
    }
    const bool combined = noDisc && m_noDiscTexture;
    const float halfWidth = .28f;
    const float halfHeight = halfWidth * (16.f / 9.f) / m_noDiscAspect;
    const float left = combined ? -halfWidth : m_x / m_screenWidth * 2.f - 1.f;
    const float right = combined ? halfWidth : (m_x + m_width) / m_screenWidth * 2.f - 1.f;
    const float top = combined ? halfHeight : 1.f - m_y / m_screenHeight * 2.f;
    const float bottom = combined ? -halfHeight : 1.f - (m_y + m_height) / m_screenHeight * 2.f;
    const float imageV = combined ? 1.f : 307.2f / 512.0f;

    const Vertex quad[] =
    {
        { left,  top,    0.0f, 1.0f, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0.0f, imageV },
        { right, bottom, 0.0f, 1.0f, 1.0f, imageV }
    };

    const DWORD color = combined ? 0xffffffff : kColors[m_colorIndex];
    const float tint[4] =
    {
        ((color >> 16) & 255) / 255.0f,
        ((color >> 8) & 255) / 255.0f,
        (color & 255) / 255.0f,
        1.0f
    };

    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_device->SetVertexDeclaration(m_vertexDeclaration);
    m_device->SetVertexShader(m_vertexShader);
    m_device->SetPixelShader(m_pixelShader);
    m_device->SetPixelShaderConstantF(0, tint, 1);
    m_device->SetTexture(0, combined ? m_noDiscTexture : m_texture);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    m_device->SetTexture(0, NULL);
    m_device->SetVertexShader(NULL);
    m_device->SetPixelShader(NULL);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void DvdLogoBounce::Shutdown()
{
    if (m_noDiscTexture) m_noDiscTexture->Release();
    m_noDiscTexture = NULL;
    m_noDiscLoadAttempted = false;
    if (m_vertexDeclaration)
    {
        m_vertexDeclaration->Release();
        m_vertexDeclaration = NULL;
    }
    if (m_pixelShader)
    {
        m_pixelShader->Release();
        m_pixelShader = NULL;
    }
    if (m_vertexShader)
    {
        m_vertexShader->Release();
        m_vertexShader = NULL;
    }
    if (m_texture)
    {
        m_texture->Release();
        m_texture = NULL;
    }
    if (m_device)
    {
        m_device->Release();
        m_device = NULL;
    }
}
