#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "DvdFrameProbe.h"

extern "C" {
#include "../deps/libmpeg2/include/mpeg2.h"
}

namespace
{
    const unsigned kWidth = 720;
    const unsigned kHeight = 576;
    const char* kVs =
        "struct V { float4 p : POSITION; float2 uv : TEXCOORD0; };"
        "V main(V v) { return v; }";
    const char* kPs =
        "sampler2D ty : register(s0); sampler2D tu : register(s1);"
        "sampler2D tv : register(s2);"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {"
        "float y = 1.16438 * (tex2D(ty, uv).r - 16.0/255.0);"
        "float u = tex2D(tu, uv).r - 128.0/255.0;"
        "float v = tex2D(tv, uv).r - 128.0/255.0;"
        "return float4(saturate(float3(y+1.59603*v,"
        "y-0.39176*u-0.81297*v,y+2.01723*u)),1); }";

    HRESULT UploadPlane(IDirect3DDevice9* device, const BYTE* data, unsigned stride,
                        unsigned width, unsigned height, IDirect3DTexture9** texture)
    {
        if (!*texture)
        {
            HRESULT hr = device->CreateTexture(width, height, 1, 0, D3DFMT_LIN_L8,
                                               D3DPOOL_DEFAULT, texture, NULL);
            if (FAILED(hr))
                return hr;
        }
        D3DLOCKED_RECT lock;
        HRESULT hr = (*texture)->LockRect(0, &lock, NULL, 0);
        if (FAILED(hr))
            return hr;
        bool ok = lock.Pitch >= (int)width;
        for (unsigned y = 0; ok && y < height; ++y)
            memcpy((BYTE*)lock.pBits + y * lock.Pitch, data + y * stride, width);
        hr = (*texture)->UnlockRect(0);
        return ok ? hr : E_FAIL;
    }
}

struct DvdFrameProbe::Decoder
{
    FILE* file;
    mpeg2dec_t* handle;
    BYTE input[8192];
    BYTE end[8];
    bool flushed;
    double duration;
};

DvdFrameProbe::DvdFrameProbe()
    : m_decoder(NULL), m_width(0), m_height(0), m_untilNext(0),
      m_framesReady(0), m_lateByFrame(0), m_maxLate(0),
      m_vs(NULL), m_ps(NULL), m_decl(NULL)
{
    for (unsigned i = 0; i < 3; ++i)
        m_planes[i] = NULL;
}

DvdFrameProbe::~DvdFrameProbe()
{
    if (m_decoder)
    {
        if (m_decoder->handle) mpeg2_close(m_decoder->handle);
        fclose(m_decoder->file);
        delete m_decoder;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (m_planes[i]) m_planes[i]->Release();
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
}

HRESULT DvdFrameProbe::Load(IDirect3DDevice9* device, FILE* file)
{
    const unsigned luma = kWidth * kHeight;
    const unsigned bytes = luma * 3 / 2;
    BYTE* data = (BYTE*)malloc(bytes);
    if (!data)
        return E_OUTOFMEMORY;
    HRESULT hr = E_INVALIDARG;
    if (fread(data, 1, bytes, file) == bytes && fgetc(file) == EOF && !ferror(file))
    {
        const BYTE* planes[] = {data, data + luma, data + luma + luma / 4};
        hr = LoadPlanes(device, kWidth, kHeight, planes, kWidth);
    }
    free(data);
    return hr;
}

HRESULT DvdFrameProbe::DecodeMpeg2(IDirect3DDevice9* device, FILE* file)
{
    // Takes ownership of the input file, even when startup fails.
    m_decoder = new Decoder();
    if (!m_decoder)
    {
        fclose(file);
        return E_OUTOFMEMORY;
    }
    m_decoder->file = file;
    mpeg2_accel(0);
    m_decoder->handle = mpeg2_init();
    if (!m_decoder->handle)
        return E_OUTOFMEMORY;
    const BYTE end[] = {0, 0, 1, 0xB7, 0, 0, 1, 0xB7};
    memcpy(m_decoder->end, end, sizeof(end));
    HRESULT hr = NextFrame(device);
    m_untilNext = m_decoder->duration;
    if (hr == S_OK)
        ++m_framesReady;
    return hr == S_FALSE ? E_FAIL : hr;
}

HRESULT DvdFrameProbe::Advance(IDirect3DDevice9* device, double elapsed)
{
    if (!m_decoder)
        return S_OK;
    m_untilNext -= elapsed;
    if (m_untilNext > 0)
        return S_OK;
    HRESULT hr = NextFrame(device);
    if (hr == S_OK)
    {
        ++m_framesReady;
        const double late = -m_untilNext;
        if (late > m_maxLate)
            m_maxLate = late;
        if (late >= m_decoder->duration)
            ++m_lateByFrame;
        m_untilNext += m_decoder->duration;
        if (m_untilNext < 0)
            m_untilNext = 0;
    }
    else
    {
        mpeg2_close(m_decoder->handle);
        fclose(m_decoder->file);
        delete m_decoder;
        m_decoder = NULL;
    }
    return hr;
}

HRESULT DvdFrameProbe::NextFrame(IDirect3DDevice9* device)
{
    Decoder& stream = *m_decoder;
    mpeg2dec_t* decoder = stream.handle;
    const mpeg2_info_t* info = mpeg2_info(decoder);
    for (unsigned steps = 0; steps < 8192; ++steps)
    {
        const mpeg2_state_t state = mpeg2_parse(decoder);
        if (state == STATE_BUFFER)
        {
            if (stream.flushed)
                return S_FALSE;
            const size_t size = fread(stream.input, 1, sizeof(stream.input), stream.file);
            if (ferror(stream.file))
                return E_FAIL;
            if (size)
            {
                mpeg2_buffer(decoder, stream.input, stream.input + size);
            }
            else
            {
                stream.flushed = true;
                mpeg2_buffer(decoder, stream.end, stream.end + sizeof(stream.end));
            }
        }
        else if (state == STATE_SEQUENCE || state == STATE_SEQUENCE_REPEATED ||
                 state == STATE_SEQUENCE_MODIFIED)
        {
            const mpeg2_sequence_t* seq = info->sequence;
            if (!seq || !seq->picture_width || !seq->picture_height ||
                seq->width > 720 || seq->height > 576 ||
                seq->chroma_width * 2 != seq->width || seq->chroma_height * 2 != seq->height)
            {
                return E_INVALIDARG;
            }
        }
        else if ((state == STATE_SLICE || state == STATE_END) &&
                 info->display_fbuf && info->display_picture && info->sequence)
        {
            const mpeg2_sequence_t* seq = info->sequence;
            const BYTE* planes[] = {info->display_fbuf->buf[0],
                                    info->display_fbuf->buf[1],
                                    info->display_fbuf->buf[2]};
            // libmpeg2 frame periods use a 27 MHz clock; nb_fields includes repeats.
            stream.duration = seq->frame_period && info->display_picture->nb_fields
                ? seq->frame_period * (double)info->display_picture->nb_fields / 54000000.0
                : 1.0 / 25.0;
            return LoadPlanes(device, seq->picture_width, seq->picture_height, planes, seq->width);
        }
    }
    return E_FAIL;
}

HRESULT DvdFrameProbe::LoadPlanes(IDirect3DDevice9* device, unsigned width, unsigned height,
                                  const BYTE* const planes[3], unsigned stride)
{
    if (m_width && (width != m_width || height != m_height))
        return E_INVALIDARG;
    if (m_decl)
        device->BlockUntilIdle(); // The preceding draw may still sample these textures.
    for (unsigned i = 0; i < 3; ++i)
    {
        HRESULT hr = UploadPlane(device, planes[i], i ? stride / 2 : stride,
                                  i ? (width + 1) / 2 : width,
                                  i ? (height + 1) / 2 : height, &m_planes[i]);
        if (FAILED(hr))
            return hr;
    }
    m_width = width;
    m_height = height;
    if (m_decl)
        return S_OK;
    ID3DXBuffer* code = NULL;
    HRESULT hr = D3DXCompileShader(kVs, (UINT)strlen(kVs), NULL, NULL, "main",
                                   "vs_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr))
        return hr;
    hr = device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &m_vs);
    code->Release();
    if (FAILED(hr))
        return hr;
    hr = D3DXCompileShader(kPs, (UINT)strlen(kPs), NULL, NULL, "main",
                           "ps_3_0", 0, &code, NULL, NULL);
    if (FAILED(hr))
        return hr;
    hr = device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &m_ps);
    code->Release();
    if (FAILED(hr))
        return hr;
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    return device->CreateVertexDeclaration(elements, &m_decl);
}

HRESULT DvdFrameProbe::Draw(IDirect3DDevice9* device, unsigned width, unsigned height) const
{
    // The smoke-test fixture is displayed as 4:3, independent of coded size.
    const float aspect = (float)width / height;
    const float x = aspect > 4.0f / 3.0f ? (4.0f / 3.0f) / aspect : 1.0f;
    const float y = aspect < 4.0f / 3.0f ? aspect / (4.0f / 3.0f) : 1.0f;
    const float vertices[][6] = {
        {-x,  y, 0, 1, 0, 0}, {x,  y, 0, 1, 1, 0},
        {-x, -y, 0, 1, 0, 1}, {x, -y, 0, 1, 1, 1}
    };
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vs);
    device->SetPixelShader(m_ps);
    for (unsigned i = 0; i < 3; ++i)
    {
        device->SetTexture(i, m_planes[i]);
        device->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
}
