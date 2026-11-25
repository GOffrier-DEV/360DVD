#include "CdDiscScene.h"
#include "DvdEmbeddedResources.h"
#include "PlaybackOsd.h"
#include <d3dx9.h>
#include <math.h>
#include <string.h>

namespace
{
    const UINT kSceneSize = 512;
    struct Vertex { float x, y, z, u, v; };
    const char kVs[] =
        "float4x4 transform:register(c0);"
        "void main(float3 p:POSITION,float2 t:TEXCOORD0,"
        "out float4 clip:POSITION,out float2 uv:TEXCOORD0)"
        "{clip=mul(float4(p,1),transform);uv=t;}";
    const char kPs[] =
        "sampler2D image:register(s0);"
        "float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(image,uv);}";
    const char kBlurPs[] =
        "sampler2D image:register(s0);"
        "float4 rotation:register(c0);"
        "float2 turn(float2 p,float s,float c){"
        "return float2(c*p.x-s*p.y,s*p.x+c*p.y)+0.5;}"
        "float4 main(float2 uv:TEXCOORD0):COLOR0{"
        "float2 p=uv-0.5;"
        "return (tex2D(image,turn(p,-rotation.z,rotation.w))"
        "+2*tex2D(image,turn(p,-rotation.x,rotation.y))"
        "+3*tex2D(image,uv)"
        "+2*tex2D(image,turn(p,rotation.x,rotation.y))"
        "+tex2D(image,turn(p,rotation.z,rotation.w)))/9;}";
}

CdDiscScene::~CdDiscScene()
{
    if (transportResolved) transportResolved->Release();
    if (transportTarget) transportTarget->Release();
    if (blurPs) blurPs->Release();
    if (ps) ps->Release();
    if (vs) vs->Release();
    if (declaration) declaration->Release();
    if (depth) depth->Release();
    if (target) target->Release();
    if (resolved) resolved->Release();
    if (artwork) artwork->Release();
    if (mesh) mesh->Release();
}

bool CdDiscScene::DrawTransport(IDirect3DDevice9* device, PlaybackOsd& osd,
                                bool paused, double elapsed)
{
    if (!ready) return false;
    const UINT width = PlaybackOsd::SceneWidth, height = PlaybackOsd::SceneHeight;
    if (!transportAttempted)
    {
        transportAttempted = true;
        D3DSURFACE_PARAMETERS params = {};
        // Keep this target above the single-sample backbuffer in EDRAM.
        // The disc pass has already resolved and no longer needs its depth tiles.
        params.Base = 512;
        transportReady = SUCCEEDED(device->CreateRenderTarget(
            width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
            &transportTarget, &params)) && SUCCEEDED(device->CreateTexture(
            width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &transportResolved, NULL));
    }
    if (!transportReady) return false;
    IDirect3DSurface9* back = NULL;
    IDirect3DSurface9* oldDepth = NULL;
    if (FAILED(device->GetRenderTarget(0, &back)) || !back) return false;
    device->GetDepthStencilSurface(&oldDepth);
    D3DVIEWPORT9 previous;
    device->GetViewport(&previous);
    DWORD msaa;
    device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &msaa);
    device->SetDepthStencilSurface(NULL);
    device->SetRenderTarget(0, transportTarget);
    const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.f, 1.f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.f, 0);
    osd.DrawCd(paused, elapsed);
    device->Resolve(D3DRESOLVE_RENDERTARGET0 | D3DRESOLVE_ALLFRAGMENTS,
                    NULL, transportResolved, NULL, 0, 0, NULL, 1.f, 0, NULL);

    device->SetRenderTarget(0, back);
    device->SetDepthStencilSurface(oldDepth);
    device->SetViewport(&previous);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, msaa);
    if (oldDepth) oldDepth->Release();
    back->Release();
    if (FAILED(device->BeginScene())) return false;
    DWORD z, cull, blend, src, dst;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_CULLMODE, &cull);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_SRCBLEND, &src);
    device->GetRenderState(D3DRS_DESTBLEND, &dst);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetVertexDeclaration(declaration);
    device->SetVertexShader(vs);
    device->SetPixelShader(ps);
    D3DXMATRIX matrix;
    D3DXMatrixIdentity(&matrix);
    device->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&matrix), 4);
    const Vertex quad[] = {
        {-1,1,.5f,0,0}, {1,1,.5f,1,0}, {-1,-1,.5f,0,1}, {1,-1,.5f,1,1}
    };
    device->SetTexture(0, transportResolved);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    device->SetTexture(0, NULL);
    device->SetRenderState(D3DRS_ZENABLE, z);
    device->SetRenderState(D3DRS_CULLMODE, cull);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
    device->SetRenderState(D3DRS_SRCBLEND, src);
    device->SetRenderState(D3DRS_DESTBLEND, dst);
    device->EndScene();
    return true;
}

void CdDiscScene::Reset()
{
    angle = speed = 0;
}

HRESULT CdDiscScene::Open(IDirect3DDevice9* device)
{
    UINT size = 0;
    const BYTE* bytes = FindDvdResource(L"compact-disc.bin", size);
    if (!bytes || !size || size % (3 * sizeof(Vertex))) return E_FAIL;
    vertexCount = size / sizeof(Vertex);
    HRESULT hr = device->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY, 0,
                                            D3DPOOL_DEFAULT, &mesh, NULL);
    if (FAILED(hr)) return hr;
    void* data = NULL;
    hr = mesh->Lock(0, size, &data, 0);
    if (FAILED(hr)) return hr;
    memcpy(data, bytes, size);
    mesh->Unlock();
    bytes = FindDvdResource(L"compact-disc.png", size);
    if (!bytes) return E_FAIL;
    hr = D3DXCreateTextureFromFileInMemory(device, bytes, size, &artwork);
    if (FAILED(hr)) return hr;
    hr = device->CreateTexture(kSceneSize, kSceneSize, 1, 0, D3DFMT_A8R8G8B8,
                               D3DPOOL_DEFAULT, &resolved, NULL);
    if (FAILED(hr)) return hr;

    // This pass runs after the backbuffer scene has been cleared to black.
    // Reuse EDRAM from tile 0 and clear the backbuffer again after resolving.
    D3DSURFACE_PARAMETERS colorParams = {};
    D3DSURFACE_PARAMETERS depthParams = {};
    // 512x512x2x4 bytes fits in 512 5-KiB tiles, including tile padding.
    depthParams.Base = 512;
    hr = device->CreateRenderTarget(kSceneSize, kSceneSize, D3DFMT_A8R8G8B8,
                                    D3DMULTISAMPLE_2_SAMPLES, 0, FALSE, &target, &colorParams);
    if (FAILED(hr)) return hr;
    hr = device->CreateDepthStencilSurface(kSceneSize, kSceneSize, D3DFMT_D24S8,
                                          D3DMULTISAMPLE_2_SAMPLES, 0, FALSE, &depth, &depthParams);
    if (FAILED(hr)) return hr;
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    hr = device->CreateVertexDeclaration(elements, &declaration);
    if (FAILED(hr)) return hr;
    ID3DXBuffer* code = NULL;
    hr = D3DXCompileShader(kVs, sizeof(kVs) - 1, NULL, NULL, "main", "vs_3_0", 0,
                            &code, NULL, NULL);
    if (FAILED(hr)) return hr;
    hr = device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &vs);
    code->Release();
    if (FAILED(hr)) return hr;
    hr = D3DXCompileShader(kPs, sizeof(kPs) - 1, NULL, NULL, "main", "ps_3_0", 0,
                            &code, NULL, NULL);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
    code->Release();
    if (FAILED(hr)) return hr;
    hr = D3DXCompileShader(kBlurPs, sizeof(kBlurPs) - 1, NULL, NULL, "main", "ps_3_0", 0,
                           &code, NULL, NULL);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &blurPs);
    code->Release();
    return hr;
}

bool CdDiscScene::Draw(IDirect3DDevice9* device, bool spinning, double elapsed)
{
    if (!attempted)
    {
        attempted = true;
        const HRESULT hr = Open(device);
        ready = SUCCEEDED(hr);
        if (!ready) OutputDebugStringA("CD scene: initialization failed\r\n");
    }
    if (!ready) return false;
    const float dt = elapsed > .1 ? .1f : static_cast<float>(elapsed);
    // A 1x audio CD spins at roughly 200–500 RPM. Use a fixed
    // near-inner-track speed for the visualization, not the drive speed.
    const float playbackRpm = 450.f;
    const float playbackSpeed = playbackRpm * (6.2831853f / 60.f);
    const float goal = spinning ? playbackSpeed : 0.f;
    const float rampSeconds = spinning ? 1.1f : .8f;
    const float step = playbackSpeed * dt / rampSeconds;
    if (speed < goal) speed = speed + step < goal ? speed + step : goal;
    if (speed > goal) speed = speed - step > goal ? speed - step : goal;
    angle = fmodf(angle + speed * dt, 6.2831853f);

    IDirect3DSurface9* back = NULL;
    IDirect3DSurface9* oldDepth = NULL;
    if (FAILED(device->GetRenderTarget(0, &back)) || !back) return false;
    device->GetDepthStencilSurface(&oldDepth);
    D3DVIEWPORT9 previous;
    device->GetViewport(&previous);
    DWORD z, write, cull, blend, msaa;
    device->GetRenderState(D3DRS_ZENABLE, &z);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &write);
    device->GetRenderState(D3DRS_CULLMODE, &cull);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &msaa);
    device->SetDepthStencilSurface(NULL);
    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(depth);
    const D3DVIEWPORT9 viewport = {0, 0, kSceneSize, kSceneSize, 0.f, 1.f};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
    device->SetVertexDeclaration(declaration);
    device->SetVertexShader(vs);
    device->SetPixelShader(ps);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    D3DXMATRIX spin, shift, scale, matrix;
    D3DXMatrixRotationZ(&spin, angle);
    // Keep the label face toward the camera, but reflect the in-plane X
    // axis so the baked lettering reads left to right.
    D3DXMatrixScaling(&scale, -.88f, -.88f, -.22f);
    D3DXMatrixTranslation(&shift, 0, 0, .5f);
    matrix = spin * scale * shift;
    D3DXMatrixTranspose(&matrix, &matrix);
    device->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&matrix), 4);
    device->SetTexture(0, artwork);
    device->SetStreamSource(0, mesh, 0, sizeof(Vertex));
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, vertexCount / 3);
    device->SetTexture(0, NULL);
    device->Resolve(D3DRESOLVE_RENDERTARGET0 | D3DRESOLVE_ALLFRAGMENTS,
                    NULL, resolved, NULL, 0, 0, NULL, 1.f, 0, NULL);

    device->SetDepthStencilSurface(NULL);
    device->SetRenderTarget(0, back);
    device->SetDepthStencilSurface(oldDepth);
    device->SetViewport(&previous);
    device->Clear(0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.f, 0);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, msaa);
    D3DXMatrixIdentity(&matrix);
    device->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&matrix), 4);
    // The 720x480 scene is presented widescreen; keep the disc round.
    const float halfWidth = .60f * 9.f / 16.f;
    // Match the Play/Pause center in CdPadPlayback.xui (372.5 on a 720-wide canvas).
    const float centerX = 2.f * 372.5f / 720.f - 1.f;
    const Vertex quad[] = {
        {centerX - halfWidth, .65f, .5f, 0, 0}, {centerX + halfWidth, .65f, .5f, 1, 0},
        {centerX - halfWidth,-.55f, .5f, 0, 1}, {centerX + halfWidth,-.55f, .5f, 1, 1}
    };
    device->SetTexture(0, resolved);
    if (speed > 0.f)
    {
        // Simulate a 1/120-second exposure around the current disc angle.
        // Sampling the resolved texture keeps the blur out of the XUI pass.
        const float stepAngle = speed / 480.f;
        const float rotation[] = {sinf(stepAngle), cosf(stepAngle),
                                  sinf(2.f * stepAngle), cosf(2.f * stepAngle)};
        device->SetPixelShaderConstantF(0, rotation, 1);
        device->SetPixelShader(blurPs);
    }
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    device->SetTexture(0, NULL);
    device->SetRenderState(D3DRS_ZENABLE, z);
    device->SetRenderState(D3DRS_ZWRITEENABLE, write);
    device->SetRenderState(D3DRS_CULLMODE, cull);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blend);
    if (oldDepth) oldDepth->Release();
    back->Release();
    return true;
}
