#include <xtl.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "DvdLogoBounce.h"

namespace
{
    void BootLog(const char* message, HRESULT result = S_OK)
    {
        char line[160];
        sprintf_s(line, sizeof(line), "%s (hr=0x%08X)\r\n",
                  message, (unsigned int)result);
        OutputDebugStringA(line);

        static bool firstLine = true;
        HANDLE file = CreateFile("game:\\360DVD.log", GENERIC_WRITE,
                                 FILE_SHARE_READ, NULL,
                                 firstLine ? CREATE_ALWAYS : OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        firstLine = false;
        if (file != INVALID_HANDLE_VALUE)
        {
            SetFilePointer(file, 0, NULL, FILE_END);
            DWORD written = 0;
            WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
            CloseHandle(file);
        }
    }

    int Run()
    {
        BootLog("360DVD boot");
        XVIDEO_MODE videoMode;
        XGetVideoMode(&videoMode);

        D3DPRESENT_PARAMETERS present;
        ZeroMemory(&present, sizeof(present));
        present.BackBufferWidth = videoMode.fIsWideScreen ? 1280 : 960;
        present.BackBufferHeight = 720;
        present.BackBufferFormat = D3DFMT_A8R8G8B8;
        present.BackBufferCount = 1;
        present.MultiSampleType = D3DMULTISAMPLE_NONE;
        present.SwapEffect = D3DSWAPEFFECT_DISCARD;
        present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d)
        {
            BootLog("Direct3DCreate9 failed");
            return 1;
        }

        IDirect3DDevice9* device = NULL;
        HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0,
                                        &present, &device);
        d3d->Release();
        if (FAILED(hr) || !device)
        {
            BootLog("CreateDevice failed", hr);
            return 2;
        }

        D3DVIEWPORT9 viewport;
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = present.BackBufferWidth;
        viewport.Height = present.BackBufferHeight;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        if (FAILED(device->SetViewport(&viewport)))
        {
            BootLog("SetViewport failed");
            device->Release();
            return 2;
        }

        DvdLogoBounce animation;
        if (!animation.Startup(device, (int)present.BackBufferWidth,
                               (int)present.BackBufferHeight))
        {
            BootLog("DVD logo startup failed");
            device->Release();
            return 3;
        }

        BootLog("DVD logo rendering");
        LARGE_INTEGER frequency;
        LARGE_INTEGER previous;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);

        for (;;)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const float elapsed = (float)(now.QuadPart - previous.QuadPart) /
                                  (float)frequency.QuadPart;
            previous = now;

            animation.Update(elapsed);
            device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            if (SUCCEEDED(device->BeginScene()))
            {
                animation.Render();
                device->EndScene();
            }
            device->Present(NULL, NULL, NULL, NULL);
        }

        animation.Shutdown();
        device->Release();
        return 0;
    }
}

int main()
{
    return Run();
}
