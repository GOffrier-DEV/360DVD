#include <xtl.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "DvdLogoBounce.h"
#include "DvdFrameProbe.h"
#include "XboxDvdStream.h"
#include <dvdread/dvd_udf.h>
#include <limits.h>

extern "C" unsigned dvd360_css_verified_sessions();
extern "C" int32_t dvd360_css_last_error();

namespace
{
    void BootLog(const char* message, HRESULT result = S_OK)
    {
        char line[160];
        sprintf_s(line, sizeof(line), "%s (status=0x%08X)\r\n",
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
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }

    void ProbeLog(const char* stage, LONG status = 0,
                  const char* path = "game:\\360DVD-probe.log")
    {
        // Replace a short checkpoint rather than appending to the boot log.
        char line[64];
        const int length = sprintf_s(line, sizeof(line), "%s 0x%08X\r\n",
                                     stage, (unsigned int)status);
        if (length <= 0)
            return;
        OutputDebugStringA(line);
        HANDLE file = CreateFile(path, GENERIC_WRITE,
                                 FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            OutputDebugStringA("Could not open DVD probe log\r\n");
            return;
        }
        DWORD written = 0;
        if (!WriteFile(file, line, (DWORD)length, &written, NULL) ||
            written != (DWORD)length || !FlushFileBuffers(file))
            OutputDebugStringA("Could not persist DVD probe checkpoint\r\n");
        CloseHandle(file);
    }

    void ProbeCss(XboxDvdStream& stream, uint32_t start, uint32_t size)
    {
        const char* authLog = "game:\\360DVD-css-auth.log";
        ProbeLog("CSS auth probe starting", 0, authLog);
        if (!start || start > INT_MAX - 128 || size < DVDCSS_BLOCK_SIZE)
        {
            ProbeLog("CSS probe VOB not found");
            return;
        }

        const unsigned verified = dvd360_css_verified_sessions();
        stream.Seek(0);
        ProbeLog("opening CSS auth stream", 0, authLog);
        ProbeLog("opening CSS auth stream");
        dvdcss_t css = dvdcss_open_stream(&stream, stream.CssAuthCallbacks());
        if (!css)
        {
            ProbeLog("CSS auth stream open failed", dvd360_css_last_error(), authLog);
            ProbeLog("CSS auth stream open failed", dvd360_css_last_error());
            return;
        }
        const bool openAuthenticated = dvd360_css_verified_sessions() != verified;
        ProbeLog(openAuthenticated ? "drive auth confirmed; seeking title" :
                 "drive auth not confirmed; seeking title",
                 openAuthenticated ? 0 : dvd360_css_last_error(), authLog);
        ProbeLog("requesting CSS title key");
        const int titleSeek = dvdcss_seek(css, (int)start, DVDCSS_SEEK_KEY);
        const bool authenticated = dvd360_css_verified_sessions() != verified;
        ProbeLog(authenticated ? "drive auth confirmed by ASF" : "drive auth not confirmed",
                 authenticated ? 0 : dvd360_css_last_error(), authLog);
        if (titleSeek != (int)start)
        {
            ProbeLog("CSS title seek failed", dvd360_css_last_error());
            dvdcss_close(css);
            return;
        }

        unsigned char raw[DVDCSS_BLOCK_SIZE], clear[DVDCSS_BLOCK_SIZE];
        const unsigned sectors = size / DVDCSS_BLOCK_SIZE;
        const unsigned limit = sectors < 128 ? sectors : 128;
        int block = (int)start;
        unsigned i = 0;
        ProbeLog("finding scrambled VOB sector");
        for (; i < limit; ++i, ++block)
        {
            if (dvdcss_read(css, raw, 1, DVDCSS_NOFLAGS) != 1)
            {
                ProbeLog("raw VOB read after CSS seek failed", stream.LastStatus());
                dvdcss_close(css);
                return;
            }
            if ((raw[0x14] & 0x30) && !memcmp(raw, "\x00\x00\x01\xBA", 4))
                break;
        }
        if (i == limit)
        {
            ProbeLog("no scrambled VOB sector in sample");
            dvdcss_close(css);
            return;
        }

        ProbeLog("reading VOB sector with decryption");
        if (dvdcss_seek(css, block, DVDCSS_NOFLAGS) != block)
            ProbeLog("CSS VOB reread seek failed", stream.LastStatus());
        else if (dvdcss_read(css, clear, 1, DVDCSS_READ_DECRYPT) != 1)
            ProbeLog("CSS decrypt read failed", stream.LastStatus());
        else if ((clear[0x14] & 0x30) || memcmp(raw + 0x80, clear + 0x80, sizeof(raw) - 0x80) == 0)
            ProbeLog("VOB sector still scrambled", dvd360_css_last_error());
        else if (!authenticated)
            ProbeLog("VOB decrypted; drive auth not confirmed", dvd360_css_last_error());
        else
            ProbeLog("CSS drive auth and VOB decryption OK");
        dvdcss_close(css);
    }

    DWORD WINAPI ProbeDvd(void*)
    {
        ProbeLog("opening");
        XboxDvdStream dvdStream;
        if (dvdStream.Open())
        {
            unsigned char sector[2048];
            ProbeLog("opened; reading sector 0");
            const int bytesRead = dvdStream.Read(sector, sizeof(sector));
            if (bytesRead == sizeof(sector))
            {
                ProbeLog("read 2048 bytes");
                // A newly opened library stream assumes byte position zero.
                dvdStream.Seek(0);
                ProbeLog("opening DVD reader");
                dvd_reader_t* reader = DVDOpenStream(&dvdStream, dvdStream.ReaderCallbacks());
                if (!reader)
                {
                    ProbeLog("DVD reader open failed", dvdStream.LastStatus());
                    return 0;
                }
                ProbeLog("opening VIDEO_TS.IFO");
                dvd_file_t* ifo = DVDOpenFile(reader, 0, DVD_READ_INFO_FILE);
                if (!ifo)
                    ProbeLog("VIDEO_TS.IFO open failed", dvdStream.LastStatus());
                else
                {
                    char signature[12];
                    const int bytes = DVDReadBytes(ifo, signature, sizeof(signature));
                    if (bytes == sizeof(signature) &&
                        memcmp(signature, "DVDVIDEO-VMG", sizeof(signature)) == 0)
                        ProbeLog("VIDEO_TS.IFO signature verified");
                    else
                        ProbeLog("VIDEO_TS.IFO signature check failed", dvdStream.LastStatus());
                    DVDCloseFile(ifo);
                }
                uint32_t vobSize = 0;
                const uint32_t vobStart = UDFFindFile(reader, "/VIDEO_TS/VTS_01_1.VOB", &vobSize);
                DVDClose(reader);
                // Read raw sectors beyond 2 GiB without requesting CSS decryption.
                const unsigned long long offset = 0x80008000ULL;
                unsigned char first[8 * 2048];
                unsigned char second[sizeof(first)];
                ProbeLog("reading raw sectors past 2 GiB");
                if (!dvdStream.Seek(offset) ||
                    dvdStream.Read(first, sizeof(first)) != sizeof(first))
                    ProbeLog("large-offset raw read failed", dvdStream.LastStatus());
                else if (!dvdStream.Seek(offset) ||
                         dvdStream.Read(second, sizeof(second)) != sizeof(second))
                    ProbeLog("large-offset raw reread failed", dvdStream.LastStatus());
                else if (memcmp(first, second, sizeof(first)) != 0)
                    ProbeLog("large-offset raw data mismatch");
                else
                {
                    dvdStream.Seek(0);
                    ProbeLog("opening libdvdcss stream");
                    dvdcss_t css = dvdcss_open_stream(&dvdStream, dvdStream.CssCallbacks());
                    if (!css)
                        ProbeLog("libdvdcss stream open failed");
                    else
                    {
                        const int block = (int)(offset / DVDCSS_BLOCK_SIZE);
                        ProbeLog("libdvdcss seek past 2 GiB");
                        if (dvdcss_seek(css, block, DVDCSS_NOFLAGS) != block)
                            ProbeLog("libdvdcss large seek failed", dvdStream.LastStatus());
                        else if (dvdcss_read(css, second, 8, DVDCSS_NOFLAGS) != 8)
                            ProbeLog("libdvdcss large read failed", dvdStream.LastStatus());
                        else if (memcmp(first, second, sizeof(first)) != 0)
                            ProbeLog("libdvdcss large read data mismatch");
                        else
                        {
                            ProbeLog("libdvdcss read matched past 2 GiB");
                            dvdcss_close(css);
                            ProbeCss(dvdStream, vobStart, vobSize);
                            return 0;
                        }
                        dvdcss_close(css);
                    }
                }
            }
            else if (bytesRead == 0)
                ProbeLog("read returned no data");
            else
                ProbeLog("read failed", dvdStream.LastStatus());
            dvdStream.Close();
        }
        else
        {
            ProbeLog("open failed", dvdStream.LastStatus());
        }
        return 0;
    }

    // Capture numeric samples only during playback. Format and write at EOF.
    struct PacingSample
    {
        unsigned frame, presented, missed, underruns;
        float interval, advance, draw, present;
        double parse, read, upload, gpuWait;
        unsigned int advanceStatus, presentStatus;
        bool drewVideo;
    };

    PacingSample pacingTrace[16384];

    void WritePacingTrace(unsigned count, unsigned omitted)
    {
        FILE* file = fopen("game:\\360DVD-pacing.csv", "wb");
        if (!file)
        {
            BootLog("Could not open 360DVD-pacing.csv", E_FAIL);
            return;
        }
        static char buffer[16384];
        setvbuf(file, buffer, _IOFBF, sizeof(buffer));
        fprintf(file, "refresh,ready,presented,missed,underruns,interval_ms,advance_ms,"
                      "draw_ms,present_ms,parse_ms,read_ms,upload_ms,gpu_wait_ms,"
                      "advance_status,present_status,drew_video\n");
        double priorParse = 0, priorRead = 0, priorUpload = 0, priorWait = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            const PacingSample& s = pacingTrace[i];
            fprintf(file, "%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,"
                          "%.3f,%.3f,%.3f,%.3f,%08X,%08X,%u\n",
                    i + 1, s.frame, s.presented, s.missed, s.underruns,
                    s.interval, s.advance, s.draw, s.present,
                    s.parse - priorParse, s.read - priorRead,
                    s.upload - priorUpload, s.gpuWait - priorWait,
                    s.advanceStatus, s.presentStatus, (unsigned)s.drewVideo);
            priorParse = s.parse;
            priorRead = s.read;
            priorUpload = s.upload;
            priorWait = s.gpuWait;
        }
        const bool writeFailed = ferror(file) != 0;
        const int closeStatus = fclose(file);
        char result[96];
        sprintf_s(result, sizeof(result), "MPEG-2 trace: refreshes=%u omitted=%u", count, omitted);
        BootLog(result, writeFailed || closeStatus ? E_FAIL : S_OK);
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
        DvdFrameProbe frame;
        bool showFrame = false;
        FILE* fixture = fopen("game:\\mpeg2-probe.m2v", "rb");
        const bool compressed = fixture != NULL;
        if (!fixture)
        {
            const DWORD attrs = GetFileAttributes("game:\\mpeg2-probe.m2v");
            const DWORD error = attrs == (DWORD)-1 ? GetLastError() : ERROR_OPEN_FAILED;
            BootLog("Could not open game:\\mpeg2-probe.m2v", HRESULT_FROM_WIN32(error));
            fixture = fopen("game:\\dvd-frame.yuv", "rb");
        }
        if (fixture)
        {
            const HRESULT frameStatus = compressed ? frame.DecodeMpeg2(device, fixture)
                                                   : frame.Load(device, fixture);
            if (!compressed)
                fclose(fixture);
            showFrame = SUCCEEDED(frameStatus);
            BootLog(compressed ? (showFrame ? "CPU MPEG-2 frame ready" : "CPU MPEG-2 probe failed")
                               : (showFrame ? "YUV test frame ready" : "YUV test frame failed"),
                    frameStatus);
        }
        // Keep synchronous optical I/O off the render thread.
        ProbeLog("starting probe thread");
        HANDLE probeThread = CreateThread(NULL, 0, ProbeDvd, NULL, 0, NULL);
        if (probeThread)
            CloseHandle(probeThread);
        else
            ProbeLog("thread creation failed", GetLastError());
        LARGE_INTEGER frequency;
        LARGE_INTEGER previous;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&previous);
        unsigned presentedFrames = 0;
        unsigned lastPresented = 0;
        unsigned missedFrames = 0;
        LONGLONG presentTicks = 0;
        unsigned traceCount = 0, traceOmitted = 0;
        bool capture = showFrame && compressed;
        bool playbackClockStarted = false;
        const double ticksToMs = 1000.0 / frequency.QuadPart;

        for (;;)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const float elapsed = (float)(now.QuadPart - previous.QuadPart) /
                                  (float)frequency.QuadPart;
            previous = now;

            bool playbackEnded = false;
            HRESULT advanceStatus = S_OK;
            if (showFrame && compressed && playbackClockStarted)
            {
                const unsigned before = frame.FramesReady();
                const HRESULT status = frame.Advance(device, elapsed);
                advanceStatus = status;
                if (frame.FramesReady() != before && lastPresented != before)
                    ++missedFrames;
                if (status == S_FALSE)
                {
                    playbackEnded = true;
                }
                else if (FAILED(status))
                {
                    playbackEnded = true;
                    showFrame = false;
                }
            }
            LARGE_INTEGER advanceEnd;
            QueryPerformanceCounter(&advanceEnd);
            animation.Update(elapsed);
            device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            bool drewVideo = false;
            if (SUCCEEDED(device->BeginScene()))
            {
                if (showFrame)
                {
                    drewVideo = SUCCEEDED(frame.Draw(device, present.BackBufferWidth,
                                                    present.BackBufferHeight));
                }
                else
                    animation.Render();
                device->EndScene();
            }
            LARGE_INTEGER presentStart, presentEnd;
            QueryPerformanceCounter(&presentStart);
            const HRESULT presentStatus = device->Present(NULL, NULL, NULL, NULL);
            QueryPerformanceCounter(&presentEnd);
            presentTicks += presentEnd.QuadPart - presentStart.QuadPart;
            if (compressed && drewVideo && SUCCEEDED(presentStatus) && !playbackClockStarted)
            {
                // Keep startup Present in the trace, but out of the video timeline.
                previous = presentEnd;
                playbackClockStarted = true;
            }
            if (compressed && drewVideo && SUCCEEDED(presentStatus) &&
                lastPresented != frame.FramesReady())
            {
                lastPresented = frame.FramesReady();
                ++presentedFrames;
            }
            if (capture)
            {
                if (traceCount < sizeof(pacingTrace) / sizeof(pacingTrace[0]))
                {
                    PacingSample& s = pacingTrace[traceCount++];
                    s.frame = frame.FramesReady();
                    s.presented = presentedFrames;
                    s.missed = missedFrames;
                    s.underruns = frame.Underruns();
                    s.interval = elapsed * 1000.0f;
                    s.advance = (float)((advanceEnd.QuadPart - now.QuadPart) * ticksToMs);
                    s.draw = (float)((presentStart.QuadPart - advanceEnd.QuadPart) * ticksToMs);
                    s.present = (float)((presentEnd.QuadPart - presentStart.QuadPart) * ticksToMs);
                    s.parse = frame.ParseMilliseconds();
                    s.read = frame.ReadMilliseconds();
                    s.upload = frame.UploadMilliseconds();
                    s.gpuWait = frame.GpuWaitMilliseconds();
                    s.advanceStatus = (unsigned)advanceStatus;
                    s.presentStatus = (unsigned)presentStatus;
                    s.drewVideo = drewVideo;
                }
                else
                    ++traceOmitted;
            }
            if (playbackEnded)
            {
                capture = false;
                WritePacingTrace(traceCount, traceOmitted);
                BootLog(FAILED(advanceStatus) ? "CPU MPEG-2 playback failed" :
                        "CPU MPEG-2 clip finished", FAILED(advanceStatus) ? advanceStatus : S_OK);
                char stats[160];
                sprintf_s(stats, sizeof(stats),
                          "MPEG-2 pacing: ready=%u presented=%u missed=%u late>=frame=%u maxLate=%.2fms vmx=%u dc=%u idct=%u",
                          frame.FramesReady(), presentedFrames, missedFrames,
                          frame.LateByFrame(), frame.MaxLateSeconds() * 1000.0,
                          frame.MotionBlocks(), frame.DcBlocks(), frame.IdctBlocks());
                BootLog(stats);
                sprintf_s(stats, sizeof(stats),
                          "MPEG-2 queue: underruns=%u read=%.2fms upload=%.2fms gpuWait=%.2fms present=%.2fms",
                          frame.Underruns(), frame.ReadMilliseconds(), frame.UploadMilliseconds(),
                          frame.GpuWaitMilliseconds(), presentTicks * 1000.0 / frequency.QuadPart);
                BootLog(stats);
                const double parseMs = frame.ParseMilliseconds();
                sprintf_s(stats, sizeof(stats),
                          "MPEG-2 parse: calls=%u total=%.2fms avg/frame=%.3fms",
                          frame.ParseCalls(), parseMs,
                          frame.FramesReady() ? parseMs / frame.FramesReady() : 0.0);
                BootLog(stats);
            }
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
