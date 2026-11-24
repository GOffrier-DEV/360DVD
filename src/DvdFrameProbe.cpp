#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "DvdFrameProbe.h"

extern "C" {
#include "../deps/libmpeg2/include/mpeg2.h"
int dvd360_mpeg2_enable_vmx(void);
int dvd360_mpeg2_enable_vmx_dc(void);
int dvd360_mpeg2_enable_vmx_idct(void);
unsigned dvd360_mpeg2_vmx_blocks(void);
unsigned dvd360_mpeg2_vmx_dc_blocks(void);
unsigned dvd360_mpeg2_vmx_idct_blocks(void);
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

struct DvdFrameProbe::QueuedFrame
{
    BYTE pixels[kWidth * kHeight * 3 / 2];
    unsigned width, height, calls;
    unsigned motion, dc, idct;
    double duration;
    LONGLONG parseTicks, readTicks, inputWaitTicks;
    LONGLONG decodeTicks, copyTicks;
    LONGLONG worstReadTicks;
    unsigned readCalls, readBytes;
    HRESULT status;
};

struct DvdFrameProbe::Decoder
{
    CRITICAL_SECTION queueStats;
    unsigned queuedFrames;
    HANDLE file;
    unsigned readCalls, readBytes;
    LONGLONG worstReadTicks;
    mpeg2dec_t* handle;
    enum { InputSlots = 16 };
    struct InputChunk
    {
        BYTE bytes[128 * 1024];
        size_t size;
        bool failed;
        LONGLONG ticks;
    };
    InputChunk input[InputSlots];
    HANDLE reader, inputFree, inputReady;
    unsigned inputIndex;
    bool inputHeld;
    BYTE* preload;
    size_t preloadSize;
    bool preloadFed;
    BYTE end[8];
    bool flushed;
    HANDLE thread, stop, freeSlots, ready, primed;
    unsigned writeIndex, readIndex, calls;
    LONGLONG parseTicks, readTicks, inputWaitTicks;
    enum { QueueCapacity = 6 };
    QueuedFrame queue[QueueCapacity];
};

DvdFrameProbe::DvdFrameProbe()
    : m_decoder(NULL), m_width(0), m_height(0), m_untilNext(0),
      m_framesReady(0), m_lateByFrame(0), m_maxLate(0),
      m_parseCalls(0), m_parseTicks(0), m_parseFrequency(0),
      m_readTicks(0), m_uploadTicks(0), m_gpuWaitTicks(0),
      m_inputWaitTicks(0), m_lastDecodeTicks(0), m_lastCopyTicks(0),
      m_worstReadTicks(0), m_readCalls(0), m_readBytes(0),
      m_underruns(0), m_textureSet(0),
      m_decodeMode("MPEG-2 VMX startup not reached"),
      m_motionBlocks(0), m_dcBlocks(0), m_idctBlocks(0),
      m_vs(NULL), m_ps(NULL), m_decl(NULL)
{
    memset(m_planes, 0, sizeof(m_planes));
}

DvdFrameProbe::~DvdFrameProbe()
{
    CloseDecoder();
    for (unsigned set = 0; set < 3; ++set)
        for (unsigned i = 0; i < 3; ++i)
            if (m_planes[set][i]) m_planes[set][i]->Release();
    if (m_decl) m_decl->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
}

void DvdFrameProbe::CloseDecoder()
{
    if (!m_decoder)
        return;
    Decoder& d = *m_decoder;
    if (d.stop)
        SetEvent(d.stop);
    if (d.thread)
    {
        WaitForSingleObject(d.thread, INFINITE);
        CloseHandle(d.thread);
    }
    if (d.reader)
    {
        WaitForSingleObject(d.reader, INFINITE);
        CloseHandle(d.reader);
    }
    if (d.inputReady) CloseHandle(d.inputReady);
    if (d.inputFree) CloseHandle(d.inputFree);
    if (d.ready) CloseHandle(d.ready);
    if (d.primed) CloseHandle(d.primed);
    if (d.freeSlots) CloseHandle(d.freeSlots);
    if (d.stop) CloseHandle(d.stop);
    if (d.handle) mpeg2_close(d.handle);
    DeleteCriticalSection(&d.queueStats);
    free(d.preload);
    CloseHandle(d.file);
    delete m_decoder;
    m_decoder = NULL;
}

DWORD WINAPI DvdFrameProbe::ReadWorker(void* context)
{
    Decoder& d = *static_cast<Decoder*>(context);
    HANDLE wait[] = {d.stop, d.inputFree};
    unsigned index = 0;
    while (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        Decoder::InputChunk& chunk = d.input[index];
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        DWORD bytes = 0;
        chunk.failed = !ReadFile(d.file, chunk.bytes, sizeof(chunk.bytes), &bytes, NULL);
        chunk.size = bytes;
        QueryPerformanceCounter(&end);
        chunk.ticks = end.QuadPart - start.QuadPart;
        const bool done = chunk.failed || chunk.size == 0;
        index = (index + 1) % Decoder::InputSlots;
        ReleaseSemaphore(d.inputReady, 1, NULL);
        if (done)
            break;
    }
    return 0;
}

DWORD WINAPI DvdFrameProbe::DecodeWorker(void* context)
{
    DvdFrameProbe& player = *static_cast<DvdFrameProbe*>(context);
    Decoder& d = *player.m_decoder;
    HANDLE wait[] = {d.stop, d.freeSlots};
    unsigned initialFrames = 0;
    while (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        QueuedFrame& frame = d.queue[d.writeIndex];
        LARGE_INTEGER start, finish;
        QueryPerformanceCounter(&start);
        const HRESULT status = player.NextFrame(frame);
        QueryPerformanceCounter(&finish);
        frame.decodeTicks = finish.QuadPart - start.QuadPart;
        frame.status = status;
        frame.calls = d.calls;
        frame.parseTicks = d.parseTicks;
        frame.readTicks = d.readTicks;
        frame.inputWaitTicks = d.inputWaitTicks;
        frame.worstReadTicks = d.worstReadTicks;
        frame.readCalls = d.readCalls;
        frame.readBytes = d.readBytes;
        frame.motion = dvd360_mpeg2_vmx_blocks();
        frame.dc = dvd360_mpeg2_vmx_dc_blocks();
        frame.idct = dvd360_mpeg2_vmx_idct_blocks();
        d.writeIndex = (d.writeIndex + 1) % Decoder::QueueCapacity;
        if (status == S_OK)
        {
            EnterCriticalSection(&d.queueStats);
            ++d.queuedFrames;
            LeaveCriticalSection(&d.queueStats);
        }
        ReleaseSemaphore(d.ready, 1, NULL);
        // No consumer runs until the initial queue is full or the stream ends.
        if (++initialFrames == Decoder::QueueCapacity || status != S_OK)
            SetEvent(d.primed);
        if (status != S_OK)
            break;
    }
    return 0;
}

HRESULT DvdFrameProbe::Consume(IDirect3DDevice9* device, DWORD timeout)
{
    Decoder& d = *m_decoder;
    const DWORD wait = WaitForSingleObject(d.ready, timeout);
    if (wait == WAIT_TIMEOUT)
        return E_PENDING;
    if (wait != WAIT_OBJECT_0)
        return E_FAIL;
    QueuedFrame& frame = d.queue[d.readIndex];
    m_parseCalls = frame.calls;
    m_parseTicks = frame.parseTicks;
    m_readTicks = frame.readTicks;
    m_inputWaitTicks = frame.inputWaitTicks;
    m_worstReadTicks = frame.worstReadTicks;
    m_readCalls = frame.readCalls;
    m_readBytes = frame.readBytes;
    m_motionBlocks = frame.motion;
    m_dcBlocks = frame.dc;
    m_idctBlocks = frame.idct;
    HRESULT hr = frame.status;
    if (hr == S_OK)
    {
        EnterCriticalSection(&d.queueStats);
        --d.queuedFrames;
        LeaveCriticalSection(&d.queueStats);
        m_lastDecodeTicks = frame.decodeTicks;
        m_lastCopyTicks = frame.copyTicks;
        const unsigned luma = frame.width * frame.height;
        const unsigned chroma = ((frame.width + 1) / 2) * ((frame.height + 1) / 2);
        const BYTE* planes[] = {frame.pixels, frame.pixels + luma, frame.pixels + luma + chroma};
        hr = LoadPlanes(device, frame.width, frame.height, planes, frame.width);
        if (SUCCEEDED(hr))
        {
            ++m_framesReady;
            const double late = -m_untilNext;
            if (late > m_maxLate) m_maxLate = late;
            if (late >= frame.duration) ++m_lateByFrame;
            m_untilNext += frame.duration;
            if (m_untilNext < 0) m_untilNext = 0;
        }
    }
    d.readIndex = (d.readIndex + 1) % Decoder::QueueCapacity;
    ReleaseSemaphore(d.freeSlots, 1, NULL);
    return hr;
}

HRESULT DvdFrameProbe::Load(IDirect3DDevice9* device, HANDLE file)
{
    const unsigned luma = kWidth * kHeight;
    const unsigned bytes = luma * 3 / 2;
    BYTE* data = (BYTE*)malloc(bytes);
    if (!data)
        return E_OUTOFMEMORY;
    HRESULT hr = E_INVALIDARG;
    DWORD sizeHigh = 0, received = 0;
    if (GetFileSize(file, &sizeHigh) == bytes && !sizeHigh &&
        ReadFile(file, data, bytes, &received, NULL) && received == bytes)
    {
        const BYTE* planes[] = {data, data + luma, data + luma + luma / 4};
        hr = LoadPlanes(device, kWidth, kHeight, planes, kWidth);
    }
    free(data);
    return hr;
}

HRESULT DvdFrameProbe::DecodeMpeg2(IDirect3DDevice9* device, HANDLE file)
{
    LARGE_INTEGER frequency;
    if (QueryPerformanceFrequency(&frequency))
        m_parseFrequency = frequency.QuadPart;
    // Takes ownership of the input file, even when startup fails.
    m_decoder = new Decoder();
    if (!m_decoder)
    {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
    m_decoder->file = file;
    InitializeCriticalSection(&m_decoder->queueStats);
    FILE* ram = fopen("game:\\mpeg2-ram.enable", "rb");
    if (ram)
    {
        fclose(ram);
        const int limit = 64 * 1024 * 1024;
        DWORD sizeHigh = 0;
        const DWORD bytes = GetFileSize(file, &sizeHigh);
        if (!bytes || sizeHigh || bytes > limit)
        {
            CloseDecoder();
            return E_INVALIDARG;
        }
        m_decoder->preload = (BYTE*)malloc(bytes);
        if (!m_decoder->preload)
        {
            CloseDecoder();
            return E_OUTOFMEMORY;
        }
        DWORD received = 0;
        if (!ReadFile(file, m_decoder->preload, bytes, &received, NULL) || received != bytes)
        {
            CloseDecoder();
            return E_FAIL;
        }
        m_decoder->preloadSize = bytes;
    }
    mpeg2_accel(0);
    m_decoder->handle = mpeg2_init();
    if (!m_decoder->handle)
    {
        CloseDecoder();
        return E_OUTOFMEMORY;
    }
    // Default to the console-tested VMX combination. Keep a scalar escape hatch.
    FILE* scalar = fopen("game:\\mpeg2-scalar.enable", "rb");
    if (scalar)
    {
        fclose(scalar);
        m_decodeMode = "MPEG-2 scalar override detected";
    }
    else
    {
        const unsigned motion = dvd360_mpeg2_enable_vmx() ? 1 : 0;
        const unsigned dc = dvd360_mpeg2_enable_vmx_dc() ? 2 : 0;
        const unsigned idct = dvd360_mpeg2_enable_vmx_idct() ? 4 : 0;
        static const char* const modes[] = {
            "MPEG-2 VMX default: no self-tests passed",
            "MPEG-2 VMX default: motion",
            "MPEG-2 VMX default: DC",
            "MPEG-2 VMX default: motion + DC",
            "MPEG-2 VMX default: IDCT",
            "MPEG-2 VMX default: motion + IDCT",
            "MPEG-2 VMX default: DC + IDCT",
            "MPEG-2 VMX default: motion + DC + IDCT"
        };
        m_decodeMode = modes[motion | dc | idct];
    }
    const BYTE end[] = {0, 0, 1, 0xB7, 0, 0, 1, 0xB7};
    memcpy(m_decoder->end, end, sizeof(end));
    m_decoder->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    const LONG capacity = Decoder::QueueCapacity;
    m_decoder->freeSlots = CreateSemaphore(NULL, capacity, capacity, NULL);
    m_decoder->ready = CreateSemaphore(NULL, 0, capacity, NULL);
    m_decoder->primed = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_decoder->inputFree = CreateSemaphore(NULL, Decoder::InputSlots, Decoder::InputSlots, NULL);
    m_decoder->inputReady = CreateSemaphore(NULL, 0, Decoder::InputSlots, NULL);
    if (m_decoder->stop && m_decoder->freeSlots && m_decoder->ready && m_decoder->primed)
    {
        if (!m_decoder->preload && m_decoder->inputFree && m_decoder->inputReady)
            m_decoder->reader = CreateThread(NULL, 0, ReadWorker, m_decoder, 0, NULL);
        if (m_decoder->preload || m_decoder->reader)
            m_decoder->thread = CreateThread(NULL, 0, DecodeWorker, this, 0, NULL);
    }
    HRESULT hr = E_FAIL;
    if (m_decoder->thread)
    {
        HANDLE wait[] = {m_decoder->primed, m_decoder->thread};
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0)
            hr = Consume(device, 0);
    }
    if (hr != S_OK)
        CloseDecoder();
    return hr == S_FALSE ? E_FAIL : hr;
}

HRESULT DvdFrameProbe::Advance(IDirect3DDevice9* device, double elapsed)
{
    if (!m_decoder)
        return S_OK;
    m_untilNext -= elapsed;
    if (m_untilNext > 0)
        return S_OK;
    const HRESULT hr = Consume(device, 0);
    if (hr == E_PENDING)
    {
        ++m_underruns;
        return S_OK; // Hold the previous frame and retry at the next refresh.
    }
    if (hr != S_OK)
        CloseDecoder();
    return hr;
}

double DvdFrameProbe::ParseMilliseconds() const
{
    return m_parseFrequency ? m_parseTicks * 1000.0 / m_parseFrequency : 0.0;
}

double DvdFrameProbe::ReadMilliseconds() const
{ return m_parseFrequency ? m_readTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::InputWaitMilliseconds() const
{ return m_parseFrequency ? m_inputWaitTicks * 1000.0 / m_parseFrequency : 0.0; }
unsigned DvdFrameProbe::QueueDepth() const
{
    if (!m_decoder)
        return 0;
    EnterCriticalSection(&m_decoder->queueStats);
    const unsigned depth = m_decoder->queuedFrames;
    LeaveCriticalSection(&m_decoder->queueStats);
    return depth;
}
double DvdFrameProbe::LastDecodeMilliseconds() const
{ return m_parseFrequency ? m_lastDecodeTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::LastCopyMilliseconds() const
{ return m_parseFrequency ? m_lastCopyTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::WorstReadMilliseconds() const
{ return m_parseFrequency ? m_worstReadTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::UploadMilliseconds() const
{ return m_parseFrequency ? m_uploadTicks * 1000.0 / m_parseFrequency : 0.0; }
double DvdFrameProbe::GpuWaitMilliseconds() const
{ return m_parseFrequency ? m_gpuWaitTicks * 1000.0 / m_parseFrequency : 0.0; }

HRESULT DvdFrameProbe::NextFrame(QueuedFrame& frame)
{
    Decoder& stream = *m_decoder;
    mpeg2dec_t* decoder = stream.handle;
    const mpeg2_info_t* info = mpeg2_info(decoder);
    for (unsigned steps = 0; steps < 8192; ++steps)
    {
        LARGE_INTEGER start, finish;
        QueryPerformanceCounter(&start);
        const mpeg2_state_t state = mpeg2_parse(decoder);
        QueryPerformanceCounter(&finish);
        stream.parseTicks += finish.QuadPart - start.QuadPart;
        ++stream.calls;
        if (state == STATE_BUFFER)
        {
            if (stream.flushed)
                return S_FALSE;
            if (stream.preload)
            {
                if (!stream.preloadFed)
                {
                    stream.preloadFed = true;
                    mpeg2_buffer(decoder, stream.preload, stream.preload + stream.preloadSize);
                }
                else
                {
                    stream.flushed = true;
                    mpeg2_buffer(decoder, stream.end, stream.end + sizeof(stream.end));
                }
                continue;
            }
            // libmpeg2 owns the previous input pointer until STATE_BUFFER.
            if (stream.inputHeld)
            {
                stream.inputHeld = false;
                stream.inputIndex = (stream.inputIndex + 1) % Decoder::InputSlots;
                ReleaseSemaphore(stream.inputFree, 1, NULL);
            }
            HANDLE wait[] = {stream.stop, stream.inputReady};
            DWORD available = WaitForMultipleObjects(2, wait, FALSE, 0);
            if (available == WAIT_TIMEOUT)
            {
                QueryPerformanceCounter(&start);
                available = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
                QueryPerformanceCounter(&finish);
                stream.inputWaitTicks += finish.QuadPart - start.QuadPart;
            }
            if (available != WAIT_OBJECT_0 + 1)
                return E_FAIL;
            const Decoder::InputChunk& chunk = stream.input[stream.inputIndex];
            stream.readTicks += chunk.ticks;
            ++stream.readCalls;
            stream.readBytes += (unsigned)chunk.size;
            if (chunk.ticks > stream.worstReadTicks)
                stream.worstReadTicks = chunk.ticks;
            stream.inputHeld = true;
            if (chunk.failed)
                return E_FAIL;
            if (chunk.size)
            {
                BYTE* bytes = stream.input[stream.inputIndex].bytes;
                mpeg2_buffer(decoder, bytes, bytes + chunk.size);
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
            frame.duration = seq->frame_period && info->display_picture->nb_fields
                ? seq->frame_period * (double)info->display_picture->nb_fields / 54000000.0
                : 1.0 / 25.0;
            frame.width = seq->picture_width;
            frame.height = seq->picture_height;
            LARGE_INTEGER copyStart, copyEnd;
            QueryPerformanceCounter(&copyStart);
            BYTE* dest = frame.pixels;
            for (unsigned i = 0; i < 3; ++i)
            {
                const unsigned width = i ? (frame.width + 1) / 2 : frame.width;
                const unsigned height = i ? (frame.height + 1) / 2 : frame.height;
                const unsigned stride = i ? seq->chroma_width : seq->width;
                if (stride == width)
                {
                    const unsigned planeBytes = width * height;
                    memcpy(dest, planes[i], planeBytes);
                    dest += planeBytes;
                }
                else
                {
                    for (unsigned row = 0; row < height; ++row, dest += width)
                        memcpy(dest, planes[i] + row * stride, width);
                }
            }
            QueryPerformanceCounter(&copyEnd);
            frame.copyTicks = copyEnd.QuadPart - copyStart.QuadPart;
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT DvdFrameProbe::LoadPlanes(IDirect3DDevice9* device, unsigned width, unsigned height,
                                  const BYTE* const planes[3], unsigned stride)
{
    if (m_width && (width != m_width || height != m_height))
        return E_INVALIDARG;
    const unsigned next = m_decl ? (m_textureSet + 1) % 3 : 0;
    LARGE_INTEGER start, waited, finish;
    QueryPerformanceCounter(&start);
    for (unsigned i = 0; i < 3; ++i)
        if (m_planes[next][i])
            m_planes[next][i]->BlockUntilNotBusy();
    QueryPerformanceCounter(&waited);
    m_gpuWaitTicks += waited.QuadPart - start.QuadPart;
    for (unsigned i = 0; i < 3; ++i)
    {
        HRESULT hr = UploadPlane(device, planes[i], i ? (stride + 1) / 2 : stride,
                                  i ? (width + 1) / 2 : width,
                                  i ? (height + 1) / 2 : height, &m_planes[next][i]);
        if (FAILED(hr))
            return hr;
    }
    QueryPerformanceCounter(&finish);
    m_uploadTicks += finish.QuadPart - waited.QuadPart;
    m_textureSet = next;
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
        device->SetTexture(i, m_planes[m_textureSet][i]);
        device->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
}
