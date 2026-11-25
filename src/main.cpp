#include <xtl.h>
#include <d3d9.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>

#include "DvdLogoBounce.h"
#include "DvdFrameProbe.h"
#include "PlaybackAffinity.h"
#include "PlaybackOsd.h"
#include "XboxDvdStream.h"
#include <dvdread/dvd_udf.h>
#include <limits.h>

extern "C" unsigned dvd360_css_verified_sessions();
extern "C" int32_t dvd360_css_last_error();

namespace
{
    // Optional fixture: native-endian signed 16-bit stereo PCM, 48 kHz, PTS zero.
    class ProbeAudio
    {
    public:
        enum { Rate = 48000, BlockBytes = 4, MaxBytes = Rate * BlockBytes * 60 };
        ProbeAudio() : engine(NULL), master(NULL), voice(NULL), pcm(NULL), last(0), total(0) {}
        ~ProbeAudio() { Close(); }

        HRESULT Open()
        {
            HANDLE file = CreateFileA("game:\\mpeg2-probe.pcm", GENERIC_READ,
                                      FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE)
            {
                const DWORD error = GetLastError();
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? S_FALSE : HRESULT_FROM_WIN32(error);
            }
            DWORD high = 0, received = 0;
            const DWORD bytes = GetFileSize(file, &high);
            HRESULT hr = E_INVALIDARG;
            if (!high && bytes && bytes <= MaxBytes && bytes % BlockBytes == 0)
            {
                pcm = new BYTE[bytes];
                hr = !pcm ? E_OUTOFMEMORY :
                    (ReadFile(file, pcm, bytes, &received, NULL) && received == bytes
                        ? S_OK : E_FAIL);
            }
            CloseHandle(file);
            if (SUCCEEDED(hr))
                hr = XAudio2Create(&engine, 0, XboxThread5);
            if (SUCCEEDED(hr))
                hr = engine->CreateMasteringVoice(&master);
            WAVEFORMATEX format = {};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = Rate;
            format.nAvgBytesPerSec = Rate * BlockBytes;
            format.nBlockAlign = BlockBytes;
            format.wBitsPerSample = 16;
            if (SUCCEEDED(hr))
                hr = engine->CreateSourceVoice(&voice, &format);
            XAUDIO2_BUFFER buffer = {};
            buffer.Flags = XAUDIO2_END_OF_STREAM;
            buffer.AudioBytes = bytes;
            buffer.pAudioData = pcm;
            if (SUCCEEDED(hr))
                hr = voice->SubmitSourceBuffer(&buffer);
            if (SUCCEEDED(hr))
                total = bytes / BlockBytes;
            if (FAILED(hr))
                Close();
            return hr;
        }

        HRESULT Start() { return voice->Start(0); }
        HRESULT SetPaused(bool paused) { return paused ? voice->Stop(0) : voice->Start(0); }
        double Advance(bool& drained)
        {
            XAUDIO2_VOICE_STATE state;
            voice->GetState(&state);
            drained = state.BuffersQueued == 0;
            const UINT64 samples = drained ? total : state.SamplesPlayed;
            const double elapsed = samples >= last ? (double)(samples - last) / Rate : 0.0;
            last = samples;
            return elapsed;
        }
        double Milliseconds() const { return (double)last * 1000.0 / Rate; }
        double PresentedMilliseconds() const
        {
            XAUDIO2_VOICE_STATE state;
            voice->GetState(&state);
            return (double)(state.BuffersQueued ? state.SamplesPlayed : total) * 1000.0 / Rate;
        }
        void Close()
        {
            if (voice) voice->DestroyVoice();
            if (master) master->DestroyVoice();
            if (engine) engine->Release();
            delete[] pcm;
            voice = NULL;
            master = NULL;
            engine = NULL;
            pcm = NULL;
        }

    private:
        IXAudio2* engine;
        IXAudio2MasteringVoice* master;
        IXAudio2SourceVoice* voice;
        BYTE* pcm;
        UINT64 last, total;
    };

    void BootLog(const char* message, HRESULT result = S_OK)
    {
        char line[320];
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

    class PlaybackLog
    {
    public:
        PlaybackLog() : wake(NULL), thread(NULL), pending(false), stopping(false) {}

        void Open()
        {
            InitializeCriticalSection(&lock);
            wake = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (wake)
                thread = CreateThread(NULL, 0, Run, this, 0, NULL);
            if (!thread)
            {
                if (wake) CloseHandle(wake);
                DeleteCriticalSection(&lock);
                BootLog("Live playback logger unavailable", E_FAIL);
            }
        }

        void Publish(const char* text, HRESULT status = S_OK)
        {
            // Keep only the latest snapshot rather than backing up playback.
            if (!thread || !TryEnterCriticalSection(&lock))
                return;
            strcpy_s(message, sizeof(message), text);
            result = status;
            pending = true;
            LeaveCriticalSection(&lock);
            SetEvent(wake);
        }

        void Close()
        {
            if (!thread) return;
            EnterCriticalSection(&lock);
            stopping = true;
            LeaveCriticalSection(&lock);
            SetEvent(wake);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            CloseHandle(wake);
            DeleteCriticalSection(&lock);
            thread = NULL;
        }

    private:
        static DWORD WINAPI Run(void* context)
        {
            PlaybackLog& log = *static_cast<PlaybackLog*>(context);
            while (WaitForSingleObject(log.wake, INFINITE) == WAIT_OBJECT_0)
            {
                char text[sizeof(log.message)];
                EnterCriticalSection(&log.lock);
                const bool write = log.pending;
                const bool stop = log.stopping;
                const HRESULT status = write ? log.result : S_OK;
                if (write)
                    strcpy_s(text, sizeof(text), log.message);
                log.pending = false;
                LeaveCriticalSection(&log.lock);
                if (write)
                    BootLog(text, status);
                if (stop)
                    break;
            }
            return 0;
        }

        CRITICAL_SECTION lock;
        HANDLE wake, thread;
        char message[256];
        HRESULT result;
        bool pending, stopping;
    };

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
        unsigned queueBefore, queueAfter, audioBuffers, videoDropped, audioRecovery;
        float interval, advance, draw, present;
        double decode, copy, audio, audioAtPresent;
        bool audioMaster;
        double parse, read, upload, gpuWait;
        double demux, audioWork;
        double decodeCpu;
        unsigned int advanceStatus, presentStatus;
        bool drewVideo;
    };

    PacingSample pacingTrace[16384];

    bool WriteTraceBytes(HANDLE file, const char* bytes, DWORD count)
    {
        while (count)
        {
            DWORD written = 0;
            if (!WriteFile(file, bytes, count, &written, NULL) || !written)
                return false;
            bytes += written;
            count -= written;
        }
        return true;
    }

    void WritePacingTrace(unsigned count, unsigned omitted)
    {
        HANDLE file = CreateFileA("game:\\360DVD-pacing.csv",
                                  GENERIC_WRITE, FILE_SHARE_READ,
                                  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            BootLog("Could not open 360DVD-pacing.csv", E_FAIL);
            return;
        }
        static char buffer[16384];
        const char header[] = "refresh,ready,presented,missed,underruns,interval_ms,advance_ms,"
                      "draw_ms,present_ms,video_codec_ms,readfile_ms,upload_ms,gpu_wait_ms,"
                      "advance_status,present_status,drew_video,queue_before,queue_after,"
                        "decode_frame_ms,frame_copy_ms,audio_ms,audio_master,audio_present_ms,"
                         "demux_ms_including_readfile,decode_cpu_ms,audio_buffers,video_expired,audio_recovery_ticks,audio_work_ms\n";
        bool ok = WriteTraceBytes(file, header, sizeof(header) - 1);
        DWORD used = 0;
        double priorParse = 0, priorRead = 0, priorUpload = 0, priorWait = 0;
        double priorDemux = 0, priorAudioWork = 0;
        for (unsigned i = 0; ok && i < count; ++i)
        {
            const PacingSample& s = pacingTrace[i];
            char row[512];
            const int length = sprintf_s(row, sizeof(row), "%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,"
                          "%.3f,%.3f,%.3f,%.3f,%08X,%08X,%u,%u,%u,%.3f,%.3f,%.3f,%u,%.3f,"
                           "%.3f,%.3f,%u,%u,%u,%.3f\n",
                    i + 1, s.frame, s.presented, s.missed, s.underruns,
                    s.interval, s.advance, s.draw, s.present,
                    s.parse - priorParse, s.read - priorRead,
                    s.upload - priorUpload, s.gpuWait - priorWait,
                    s.advanceStatus, s.presentStatus, (unsigned)s.drewVideo,
                     s.queueBefore, s.queueAfter, s.decode, s.copy, s.audio,
                       (unsigned)s.audioMaster, s.audioAtPresent, s.demux - priorDemux,
                       s.decodeCpu, s.audioBuffers, s.videoDropped, s.audioRecovery,
                       s.audioWork - priorAudioWork);
            priorDemux = s.demux;
            priorAudioWork = s.audioWork;
            if (length <= 0)
            {
                ok = false;
                break;
            }
            if (used + length > sizeof(buffer))
            {
                ok = WriteTraceBytes(file, buffer, used);
                used = 0;
            }
            memcpy(buffer + used, row, length);
            used += length;
            priorParse = s.parse;
            priorRead = s.read;
            priorUpload = s.upload;
            priorWait = s.gpuWait;
        }
        if (ok)
            ok = WriteTraceBytes(file, buffer, used);
        const BOOL closed = CloseHandle(file);
        char result[96];
        sprintf_s(result, sizeof(result), "MPEG-2 trace: refreshes=%u omitted=%u", count, omitted);
        BootLog(result, ok && closed ? S_OK : E_FAIL);
    }

    void BenchmarkMpegInput(HANDLE file)
    {
        static BYTE bytes[128 * 1024];
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        {
            unsigned calls = 0;
            unsigned long long total = 0;
            LONGLONG ticks = 0, worst = 0;
            bool ok;
            DWORD count;
            do
            {
                LARGE_INTEGER start, end;
                QueryPerformanceCounter(&start);
                count = 0;
                ok = ReadFile(file, bytes, sizeof(bytes), &count, NULL) != FALSE;
                QueryPerformanceCounter(&end);
                const LONGLONG elapsed = end.QuadPart - start.QuadPart;
                ticks += elapsed;
                if (elapsed > worst) worst = elapsed;
                total += count;
                ++calls;
            } while (ok && count);
            char line[144];
            sprintf_s(line, sizeof(line), "Read-only ReadFile: calls=%u bytes=%llu total=%.2fms worst=%.2fms",
                      calls, total,
                      ticks * 1000.0 / frequency.QuadPart, worst * 1000.0 / frequency.QuadPart);
            BootLog(line, ok ? S_OK : E_FAIL);
        }
    }

    int Run()
    {
        BootLog("360DVD boot");
        if (PlaybackAffinity::Enabled())
        {
            PlaybackAffinity::Pin(PlaybackAffinity::Renderer);
            BootLog("Playback affinity: render=5 decode=2 reader=0");
        }
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
        HANDLE fixture = CreateFileA("game:\\dvd-video.vob", GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        const bool vob = fixture != INVALID_HANDLE_VALUE;
        if (!vob)
            fixture = CreateFileA("game:\\mpeg2-probe.m2v", GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        const DWORD openError = fixture == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        const bool compressed = fixture != INVALID_HANDLE_VALUE;
        FILE* readOnly = compressed && !vob ? fopen("game:\\mpeg2-readbench.enable", "rb") : NULL;
        if (readOnly)
        {
            fclose(readOnly);
            BenchmarkMpegInput(fixture);
            CloseHandle(fixture);
            animation.Shutdown();
            device->Release();
            return 0;
        }
        if (!compressed)
        {
            BootLog("Could not open game:\\mpeg2-probe.m2v", HRESULT_FROM_WIN32(openError));
            fixture = CreateFileA("game:\\dvd-frame.yuv", GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (fixture != INVALID_HANDLE_VALUE)
        {
            const HRESULT frameStatus = compressed ? frame.DecodeMpeg2(device, fixture)
                                                   : frame.Load(device, fixture);
            if (!compressed)
                CloseHandle(fixture);
            showFrame = SUCCEEDED(frameStatus);
            if (compressed)
                BootLog(frame.DecodeMode());
                unsigned queued, target, budget;
                frame.StartupVideoBuffer(queued, target, budget);
                char startup[128];
                sprintf_s(startup, sizeof(startup),
                          "Startup video bytes: queued=%u target=%u budget=%u slice_threads=%u",
                          queued, target, budget, frame.ActiveSliceThreads());
                BootLog(startup);
            BootLog(compressed ? (showFrame ? "CPU MPEG-2 frame ready" : "CPU MPEG-2 probe failed")
                               : (showFrame ? "YUV test frame ready" : "YUV test frame failed"),
                    frameStatus);
        }
        ProbeAudio audio;
        const HRESULT audioOpen = showFrame && compressed && !vob ? audio.Open() : S_FALSE;
        if (FAILED(audioOpen))
            BootLog("PCM fixture unavailable; using performance counter", audioOpen);
        bool audioMaster = false;
        bool embeddedAudio = false;
        // Keep synchronous optical I/O off the render thread.
        ProbeLog("starting probe thread");
        HANDLE probeThread = CreateThread(NULL, 0, ProbeDvd, NULL, 0, NULL);
        if (probeThread)
            CloseHandle(probeThread);
        else
            ProbeLog("thread creation failed", GetLastError());
        static PlaybackOsd osd;
        const unsigned subtitleCount = frame.SubtitleCount();
        const HRESULT osdStatus = showFrame && compressed ? osd.Open(device) : S_FALSE;
        const bool osdReady = osdStatus == S_OK;
        if (FAILED(osdStatus))
            BootLog("Playback overlay unavailable", osdStatus);
        if (osdReady)
        {
            char display[128];
            sprintf_s(display, sizeof(display), "Playback display: output=%ux%u source_aspect=%.3f subtitle_tracks=%u",
                      present.BackBufferWidth, present.BackBufferHeight, frame.DisplayAspect(), subtitleCount);
            BootLog(display);
        }
        bool paused = false;
        unsigned displayMode = 0;
        unsigned subtitle = 0;
        double playbackSeconds = 0;
        const double playbackDuration = frame.DurationSeconds();
        LARGE_INTEGER frequency;
        LARGE_INTEGER previous;
        QueryPerformanceFrequency(&frequency);
        PlaybackLog playbackLog;
        if (showFrame && compressed)
            playbackLog.Open();
        QueryPerformanceCounter(&previous);
        LONGLONG nextProgress = previous.QuadPart + 2 * frequency.QuadPart;
        unsigned presentedFrames = 0;
        unsigned lastPresented = 0;
        unsigned missedFrames = 0;
        LONGLONG presentTicks = 0;
        unsigned traceCount = 0, traceOmitted = 0;
        bool capture = showFrame && compressed;
        bool playbackClockStarted = false;
        const double ticksToMs = 1000.0 / frequency.QuadPart;
        char audioStartLog[96] = {};
        HRESULT audioStartStatus = S_OK;
        bool pcmDrained = false;

        for (;;)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const float elapsed = (float)(now.QuadPart - previous.QuadPart) /
                                  (float)frequency.QuadPart;
            previous = now;

            bool playbackEnded = false;
            bool resumed = false;
            if (osdReady && capture && showFrame && playbackClockStarted)
            {
                const PlaybackOsd::Action action = osd.Update(elapsed, paused);
                if (action == PlaybackOsd::TogglePause)
                {
                    const HRESULT result = embeddedAudio ? frame.SetEmbeddedAudioPaused(!paused)
                        : audioMaster ? audio.SetPaused(!paused) : S_OK;
                    if (SUCCEEDED(result))
                    {
                        paused = !paused;
                        resumed = !paused;
                    }
                }
                else if (action == PlaybackOsd::DisplayMode)
                    displayMode = (displayMode + 1) % PlaybackOsd::DisplayModeCount;
                else if (action == PlaybackOsd::SubtitleTrack)
                    subtitle = (subtitle + 1) % (subtitleCount + 1);
            }
            const unsigned queueBefore = capture ? frame.QueueDepth() : 0;
            const unsigned frameBefore = frame.FramesReady();
            HRESULT advanceStatus = S_OK;
            if (showFrame && compressed && playbackClockStarted && !paused)
            {
                const unsigned before = frame.FramesReady();
                bool drained = false;
                const double fallback = resumed ? 0.0 : elapsed;
                const double videoElapsed = embeddedAudio ? frame.AdvanceEmbeddedAudio(fallback)
                    : audioMaster ? audio.Advance(drained) : fallback;
                playbackSeconds += videoElapsed;
                const HRESULT status = frame.Advance(device, videoElapsed, embeddedAudio || audioMaster);
                if (drained)
                {
                    audioMaster = false;
                    audio.Close();
                    pcmDrained = true;
                }
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
                                                     present.BackBufferHeight, displayMode));
                    if (osdReady && capture && !playbackEnded)
                    {
                        frame.DrawSubtitles(device, osd, present.BackBufferWidth,
                                            present.BackBufferHeight, displayMode, subtitle, playbackSeconds);
                        osd.Draw(device, present.BackBufferWidth, present.BackBufferHeight,
                                 paused, displayMode, playbackSeconds, playbackDuration,
                                 subtitle, subtitleCount);
                    }
                }
                else
                    animation.Render();
                device->EndScene();
            }
            LARGE_INTEGER presentStart, presentEnd;
            QueryPerformanceCounter(&presentStart);
            const HRESULT presentStatus = device->Present(NULL, NULL, NULL, NULL);
            QueryPerformanceCounter(&presentEnd);
            double embeddedMs = -1.0;
            unsigned audioBuffers = 0;
            if (capture && embeddedAudio)
                frame.EmbeddedAudioState(embeddedMs, audioBuffers);
            const double audioAtPresent = capture && drewVideo && SUCCEEDED(presentStatus)
                ? (embeddedAudio ? embeddedMs : audioMaster ? audio.PresentedMilliseconds() : -1.0)
                : -1.0;
            presentTicks += presentEnd.QuadPart - presentStart.QuadPart;
            if (compressed && drewVideo && SUCCEEDED(presentStatus) && !playbackClockStarted)
            {
                // Keep startup Present in the trace, but out of the video timeline.
                previous = presentEnd;
                playbackClockStarted = true;
                if (vob)
                {
                    const HRESULT started = frame.StartEmbeddedAudio();
                    audioStartStatus = started;
                    embeddedAudio = started == S_OK;
                    if (embeddedAudio)
                    {
                        double audioMs = 0;
                        unsigned buffers = 0;
                        frame.EmbeddedAudioState(audioMs, buffers);
                        sprintf_s(audioStartLog, sizeof(audioStartLog),
                                  "VOB audio: XAudio2 started (queued=%u)", buffers);
                    }
                    else
                        strcpy_s(audioStartLog, sizeof(audioStartLog),
                                 started == S_FALSE ? "VOB audio: no usable track" :
                                 "VOB audio: XAudio2 start failed");
                }
                else if (audioOpen == S_OK)
                {
                    const HRESULT started = audio.Start();
                    audioMaster = SUCCEEDED(started);
                    audioStartStatus = started;
                    strcpy_s(audioStartLog, sizeof(audioStartLog),
                             audioMaster ? "Playback clock: XAudio2 samples" :
                             "PCM start failed; using performance counter");
                    if (!audioMaster)
                        audio.Close();
                }
                if (audioStartLog[0])
                    playbackLog.Publish(audioStartLog, audioStartStatus);
            }
            if (compressed && drewVideo && SUCCEEDED(presentStatus) &&
                lastPresented != frame.FramesReady())
            {
                lastPresented = frame.FramesReady();
                ++presentedFrames;
            }
            if (capture && !paused)
            {
                if (traceCount < sizeof(pacingTrace) / sizeof(pacingTrace[0]))
                {
                    PacingSample& s = pacingTrace[traceCount++];
                    s.frame = frame.FramesReady();
                    s.presented = presentedFrames;
                    s.missed = missedFrames;
                    s.underruns = frame.Underruns();
                    s.queueBefore = queueBefore;
                    s.queueAfter = frame.QueueDepth();
                    const bool newFrame = traceCount == 1 || s.frame != frameBefore;
                    s.decode = newFrame ? frame.LastDecodeMilliseconds() : 0.0;
                    s.decodeCpu = newFrame ? frame.LastDecodeCpuMilliseconds() : 0.0;
                    s.copy = newFrame ? frame.LastCopyMilliseconds() : 0.0;
                     s.audio = embeddedAudio ? embeddedMs : audio.Milliseconds();
                     s.audioMaster = embeddedAudio || audioMaster;
                     s.audioBuffers = audioBuffers;
                     s.videoDropped = frame.AudioSyncDrops();
                     s.audioRecovery = frame.AudioRecoveryTicks();
                    s.audioAtPresent = audioAtPresent;
                    s.interval = elapsed * 1000.0f;
                    s.advance = (float)((advanceEnd.QuadPart - now.QuadPart) * ticksToMs);
                    s.draw = (float)((presentStart.QuadPart - advanceEnd.QuadPart) * ticksToMs);
                    s.present = (float)((presentEnd.QuadPart - presentStart.QuadPart) * ticksToMs);
                    s.parse = frame.ParseMilliseconds();
                    s.read = frame.ReadMilliseconds();
                    s.upload = frame.UploadMilliseconds();
                    s.gpuWait = frame.GpuWaitMilliseconds();
                     s.demux = frame.InputWaitMilliseconds();
                     s.audioWork = frame.AudioWorkMilliseconds();
                    s.advanceStatus = (unsigned)advanceStatus;
                    s.presentStatus = (unsigned)presentStatus;
                    s.drewVideo = drewVideo;
                }
                else
                    ++traceOmitted;
            }
            if (capture && !playbackEnded && now.QuadPart >= nextProgress)
            {
                nextProgress = now.QuadPart + 2 * frequency.QuadPart;
                char progress[256];
                sprintf_s(progress, sizeof(progress),
                          "Playback: %s time=%.3fs presented=%u queue=%u underruns=%u expired=%u audio_ms=%.1f audio_buffers=%u recovery=%u display=%u",
                          paused ? "paused" : "playing", playbackSeconds, presentedFrames,
                          frame.QueueDepth(), frame.Underruns(), frame.AudioSyncDrops(),
                          embeddedMs, audioBuffers, frame.AudioRecoveryTicks(), displayMode);
                playbackLog.Publish(progress);
            }
            if (playbackEnded)
            {
                capture = false;
                audio.Close();
                audioMaster = false;
                playbackLog.Close();
                if (pcmDrained)
                    BootLog("PCM drained; continuing on performance counter");
                WritePacingTrace(traceCount, traceOmitted);
                BootLog(FAILED(advanceStatus) ? "CPU MPEG-2 playback failed" :
                        "CPU MPEG-2 clip finished", FAILED(advanceStatus) ? advanceStatus : S_OK);
                char stats[160];
                sprintf_s(stats, sizeof(stats), "Playback summary: recovery=%u video_packet_peak=%u",
                          frame.AudioRecoveryTicks(), frame.VideoPacketHighWater());
                BootLog(stats);
                sprintf_s(stats, sizeof(stats),
                           "MPEG-2 pacing: ready=%u presented=%u missed=%u late>=frame=%u maxLate=%.2fms expired=%u",
                          frame.FramesReady(), presentedFrames, missedFrames,
                          frame.LateByFrame(), frame.MaxLateSeconds() * 1000.0,
                           frame.AudioSyncDrops());
                BootLog(stats);
                sprintf_s(stats, sizeof(stats),
                          "MPEG-2 queue: underruns=%u read=%.2fms upload=%.2fms gpuWait=%.2fms present=%.2fms",
                          frame.Underruns(), frame.ReadMilliseconds(), frame.UploadMilliseconds(),
                          frame.GpuWaitMilliseconds(), presentTicks * 1000.0 / frequency.QuadPart);
                BootLog(stats);
                sprintf_s(stats, sizeof(stats), "FFmpeg demux (including ReadFile): %.2fms",
                          frame.InputWaitMilliseconds());
                BootLog(stats);
                const double parseMs = frame.ParseMilliseconds();
                sprintf_s(stats, sizeof(stats),
                           "FFmpeg ReadFile: calls=%u bytes=%u worst=%.2fms",
                          frame.ReadCalls(), frame.ReadBytes(), frame.WorstReadMilliseconds());
                BootLog(stats);
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
#ifdef DVD360_FFMPEG_PROBE
    extern void RunFfmpegVobProbe();
    RunFfmpegVobProbe();
#endif
    return Run();
}
