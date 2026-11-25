#include <xtl.h>
#include <d3d9.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DvdLogoBounce.h"
#include "CdDiscScene.h"
#include "DvdFrameProbe.h"
#include "PlaybackAffinity.h"
#include "PlaybackOsd.h"
#include "XboxDvdStream.h"
#include <dvdread/dvd_udf.h>
#include <limits.h>

extern "C" HRESULT __stdcall XexGetModuleHandle(char* moduleName, HANDLE* module);
extern "C" HRESULT __stdcall XexGetProcedureAddress(HANDLE module, DWORD ordinal,
                                                      void* procedure);

extern "C" unsigned dvd360_css_verified_sessions();
extern "C" int32_t dvd360_css_last_error();

namespace
{
    void BootLog(const char* message, HRESULT result = S_OK)
    {
#if defined(DVD360_LOGGING) || defined(DVD360_PERF_LOGGING)
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
#endif
    }

    // XAM normally responds to an optical-tray removal by launching its
    // dashboard fallback, which terminates the current title. We retain
    // XAM's media-change broadcast (360DVD consumes that to enter No Disc)
    // and suppress only that fallback launch call.
    //
    // This is deliberately build-locked.  Never write a guessed address:
    // XAM's image base comes from its loaded-module entry and the instruction
    // must still be the call verified in the 17489-dev XAM image.
    typedef HRESULT (*DmSetMemoryFn)(void* destination, DWORD size,
                                     const void* source, DWORD* written);

    struct LdrDataTableEntryPrefix
    {
        DWORD ignored[7];
        void* imageBase;
        DWORD imageSize;
    };

    bool InstallEjectTitleExitPatch()
    {
        static const DWORD kXamEjectLaunchOffset = 0x1DCDCu;
        static const DWORD kExpectedCall = 0x4BFF11F5u;
        static const DWORD kPowerPcNop = 0x60000000u;
        static const DWORD kDmSetMemoryOrdinal = 40;

        HANDLE xam = NULL;
        char xamName[] = "xam.xex";
        if (XexGetModuleHandle(xamName, &xam) != 0 || !xam)
        {
            BootLog("Eject patch: xam.xex module unavailable", E_FAIL);
            return false;
        }

        const auto* entry = static_cast<const LdrDataTableEntryPrefix*>(xam);
        if (!entry->imageBase || entry->imageSize < kXamEjectLaunchOffset + 8)
        {
            BootLog("Eject patch: XAM image does not contain the patch site", E_FAIL);
            return false;
        }
        volatile DWORD* const site =
            reinterpret_cast<volatile DWORD*>(
                static_cast<BYTE*>(entry->imageBase) + kXamEjectLaunchOffset);
        const DWORD instruction = *site;
        if (site[-1] != 0x38800000u || site[1] != 0x48000094u ||
            (instruction != kExpectedCall && instruction != kPowerPcNop))
        {
            BootLog("Eject patch: unrecognized XAM build", E_FAIL);
            return false;
        }
        if (instruction == kPowerPcNop)
        {
            BootLog("Eject patch: XAM tray fallback launch already disabled");
            return true;
        }

        HANDLE xbdm = NULL;
        void* procedure = NULL;
        char xbdmName[] = "xbdm.xex";
        if (XexGetModuleHandle(xbdmName, &xbdm) != 0 || !xbdm ||
            XexGetProcedureAddress(xbdm, kDmSetMemoryOrdinal, &procedure) != 0 ||
            !procedure)
        {
            BootLog("Eject patch: XBDM memory writer unavailable", E_FAIL);
            return false;
        }

        DWORD written = 0;
        const HRESULT result = reinterpret_cast<DmSetMemoryFn>(procedure)(
            const_cast<DWORD*>(site),
            sizeof(kPowerPcNop), &kPowerPcNop, &written);
        if (FAILED(result) || written != sizeof(kPowerPcNop) || *site != kPowerPcNop)
        {
            BootLog("Eject patch: XAM write verification failed",
                    FAILED(result) ? result : E_FAIL);
            return false;
        }

        BootLog("Eject patch: XAM tray fallback launch disabled");
        return true;
    }

    class CdAudioPlayer
    {
    public:
        ~CdAudioPlayer() { Stop(); }

        bool Start();
        bool HasDisc() const { return thread != NULL; }
        bool Paused() { return InterlockedCompareExchange(&paused, 0, 0) != 0; }
        void TogglePause() { InterlockedExchange(&paused, Paused() ? 0 : 1); }
        void Skip(LONG direction) { InterlockedExchange(&skip, direction); }
        void Stop()
        {
            if (!thread) return;
            SetEvent(stop);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            CloseHandle(stop);
            thread = stop = NULL;
        }

    private:
        static DWORD WINAPI Run(void* context)
        {
            CdAudioPlayer& player = *static_cast<CdAudioPlayer*>(context);
            const HRESULT status = player.Play();
            BootLog(status == S_OK ? "CD audio finished" :
                    status == S_FALSE ? "CD audio stopped" : "CD audio playback failed", status);
            return 0;
        }
        HRESULT Play();
        HANDLE stop = NULL;
        HANDLE thread = NULL;
        volatile LONG paused = 0;
        volatile LONG skip = 0;
        XboxDvdStream::CdAudioProbe probe = {};
    };

    bool CdAudioPlayer::Start()
    {
        XboxDvdStream stream;
        const HRESULT status = stream.ProbeCdAudio(probe);
        char line[240];
        sprintf_s(line, sizeof(line),
                  "CD audio probe (%s): TOC bytes=%u length=%u tracks=%u-%u audio=%u data=%u first track=%u LBA=[%u,%u) raw bytes=%u",
                  probe.stage, probe.tocBytes, probe.tocLength, probe.tocFirst, probe.tocLast,
                  probe.audioTracks, probe.dataTracks, probe.firstTrack,
                  probe.startLba, probe.endLba, probe.bytesRead);
        BootLog(line, status);
        if (status != S_OK) return false;
        InterlockedExchange(&paused, 0);
        InterlockedExchange(&skip, 0);
        stop = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (stop) thread = CreateThread(NULL, 0, Run, this, 0, NULL);
        if (thread) return true;
        if (stop) CloseHandle(stop);
        stop = NULL;
        BootLog("CD audio worker could not start", E_FAIL);
        return false;
    }

    HRESULT CdAudioPlayer::Play()
    {
        XboxDvdStream stream;
        if (!stream.Open()) return HRESULT_FROM_NT(stream.LastStatus());
        IXAudio2* engine = NULL;
        IXAudio2MasteringVoice* master = NULL;
        IXAudio2SourceVoice* voice = NULL;
        HRESULT hr = XAudio2Create(&engine, 0, XboxThread5);
        if (SUCCEEDED(hr)) hr = engine->CreateMasteringVoice(&master);
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (SUCCEEDED(hr)) hr = engine->CreateSourceVoice(&voice, &format);
        const unsigned slots = 4, batch = 12, sectorBytes = 2352;
        BYTE* storage = static_cast<BYTE*>(malloc(slots * batch * sectorBytes + 15));
        if (!storage && SUCCEEDED(hr)) hr = E_OUTOFMEMORY;
        BYTE* buffers = reinterpret_cast<BYTE*>((reinterpret_cast<uintptr_t>(storage) + 15) &
                                                 ~static_cast<uintptr_t>(15));
        unsigned next = 0;
        bool started = false;
        bool loggedStart = false;
        unsigned track = 0;
        unsigned lba = probe.tracks[0].start;
        while (hr == S_OK)
        {
            if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            {
                hr = S_FALSE;
                break;
            }
            const LONG jump = InterlockedExchange(&skip, 0);
            if (jump)
            {
                hr = voice->Stop();
                if (SUCCEEDED(hr)) hr = voice->FlushSourceBuffers();
                if (FAILED(hr)) break;
                started = false;
                next = 0;
                const LONG target = static_cast<LONG>(track) + jump;
                track = target < 0 ? 0 : static_cast<unsigned>(target);
                if (track >= probe.audioTracks) track = probe.audioTracks - 1;
                lba = probe.tracks[track].start;
            }
            if (Paused())
            {
                if (started) hr = voice->Stop();
                started = false;
                WaitForSingleObject(stop, 5);
                continue;
            }
            if (!started)
            {
                hr = voice->Start();
                if (FAILED(hr)) break;
                started = true;
                if (!loggedStart)
                {
                    BootLog("CD audio: XAudio2 started (byte-swapped PCM)");
                    loggedStart = true;
                }
            }
            if (track < probe.audioTracks)
            {
                const auto& range = probe.tracks[track];
                XAUDIO2_VOICE_STATE state;
                voice->GetState(&state);
                if (lba == range.end)
                {
                    if (state.BuffersQueued)
                    {
                        WaitForSingleObject(stop, 5);
                        continue;
                    }
                    if (++track == probe.audioTracks)
                    {
                        track = 0;
                        InterlockedExchange(&paused, 1);
                    }
                    lba = probe.tracks[track].start;
                    continue;
                }
                if (WaitForSingleObject(stop, state.BuffersQueued == slots ? 5 : 0) == WAIT_OBJECT_0)
                {
                    hr = S_FALSE;
                    break;
                }
                if (state.BuffersQueued == slots) continue;
                const unsigned sectors = range.end - lba < batch ? range.end - lba : batch;
                unsigned bytes = 0;
                BYTE* pcm = buffers + next * batch * sectorBytes;
                hr = stream.ReadCdAudio(lba, sectors, pcm, bytes);
                if (FAILED(hr)) break;
                // The drive's CDDA samples need a byte swap for XAudio2 PCM.
                for (unsigned i = 0; i < bytes; i += 2)
                {
                    const BYTE first = pcm[i];
                    pcm[i] = pcm[i + 1];
                    pcm[i + 1] = first;
                }
                XAUDIO2_BUFFER buffer = {};
                buffer.AudioBytes = bytes;
                buffer.pAudioData = pcm;
                lba += sectors;
                if (track + 1 == probe.audioTracks && lba == range.end)
                    buffer.Flags = XAUDIO2_END_OF_STREAM;
                hr = voice->SubmitSourceBuffer(&buffer);
                next = (next + 1) % slots;
            }
            else
                WaitForSingleObject(stop, 5);
        }
        if (voice) voice->DestroyVoice();
        if (master) master->DestroyVoice();
        if (engine) engine->Release();
        free(storage);
        return hr;
    }

    class PlaybackLog
    {
    public:
        PlaybackLog() : wake(NULL), thread(NULL), pending(false), stopping(false) {}

        void Open()
        {
#if defined(DVD360_LOGGING) || defined(DVD360_PERF_LOGGING)
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
#endif
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
            ProbeLog("CSS probe VOB not found", 0, authLog);
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
                uint32_t vobStart = UDFFindFile(reader, "/VIDEO_TS/VTS_01_1.VOB", &vobSize);
                for (unsigned titleSet = 2; (!vobStart || vobSize < DVDCSS_BLOCK_SIZE) &&
                     titleSet <= 99; ++titleSet)
                {
                    char path[32];
                    sprintf_s(path, sizeof(path), "/VIDEO_TS/VTS_%02u_1.VOB", titleSet);
                    vobStart = UDFFindFile(reader, path, &vobSize);
                }
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
                        }
                        dvdcss_close(css);
                    }
                }
                // A short disc can fail the 2 GiB diagnostic and still use CSS.
                ProbeCss(dvdStream, vobStart, vobSize);
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
        double bwdif;
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
#if defined(DVD360_LOGGING) || defined(DVD360_PERF_LOGGING)
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
                          "demux_ms_including_readfile,decode_cpu_ms,audio_buffers,video_expired,audio_recovery_ticks,audio_work_ms,bwdif_ms\n";
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
                           "%.3f,%.3f,%u,%u,%u,%.3f,%.3f\n",
                    i + 1, s.frame, s.presented, s.missed, s.underruns,
                    s.interval, s.advance, s.draw, s.present,
                    s.parse - priorParse, s.read - priorRead,
                    s.upload - priorUpload, s.gpuWait - priorWait,
                    s.advanceStatus, s.presentStatus, (unsigned)s.drewVideo,
                     s.queueBefore, s.queueAfter, s.decode, s.copy, s.audio,
                       (unsigned)s.audioMaster, s.audioAtPresent, s.demux - priorDemux,
                       s.decodeCpu, s.audioBuffers, s.videoDropped, s.audioRecovery,
                        s.audioWork - priorAudioWork, s.bwdif);
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
#endif
    }

    int Run()
    {
        BootLog("360DVD boot");
        InstallEjectTitleExitPatch();
        if (PlaybackAffinity::Enabled())
        {
            PlaybackAffinity::Pin(PlaybackAffinity::Renderer);
            BootLog("Playback affinity: render=5 decode=2 reader=0");
        }
        D3DPRESENT_PARAMETERS present;
        ZeroMemory(&present, sizeof(present));
        present.BackBufferWidth = PlaybackOsd::SceneWidth;
        present.BackBufferHeight = PlaybackOsd::SceneHeight;
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
        const DWORD discMediaChanged = 0x8000000D;
        const HANDLE discListener = XNotifyCreateListener(XNOTIFY_SYSTEM);
        BootLog("Opening optical DVD at First Play");
        device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (SUCCEEDED(device->BeginScene()))
        {
            animation.Render();
            device->EndScene();
            device->Present(NULL, NULL, NULL, NULL);
        }
        const HRESULT discStatus = frame.DecodeMpeg2(device);
        const bool discPlayback = discStatus == S_OK;
        CdAudioPlayer cdAudio;
        CdDiscScene cdScene;
        const bool cdPlayback = !discPlayback && cdAudio.Start();
        bool showFrame = discPlayback;
        bool compressed = discPlayback;
        BootLog(discPlayback ? "DVD First Play stream primed" : "DVD First Play startup failed", discStatus);
        if (compressed && showFrame)
        {
            BootLog(frame.DecodeMode());
            unsigned queued, target, budget;
            frame.StartupVideoBuffer(queued, target, budget);
            char startup[128];
            sprintf_s(startup, sizeof(startup),
                      "Startup video bytes: queued=%u target=%u budget=%u slice_threads=%u",
                      queued, target, budget, frame.ActiveSliceThreads());
            BootLog(startup);
        }
        bool embeddedAudio = false;
#ifdef DVD360_LOGGING
        // Do not run a second authentication session during playback.
        if (!discPlayback)
        {
            ProbeLog("starting probe thread");
            HANDLE probeThread = CreateThread(NULL, 0, ProbeDvd, NULL, 0, NULL);
            if (probeThread)
                CloseHandle(probeThread);
            else
                ProbeLog("thread creation failed", GetLastError());
        }
#endif
        static PlaybackOsd osd;
        osd.ResetCdFocus();
        unsigned subtitleCount = frame.SubtitleCount();
        const HRESULT osdStatus = osd.Open(device);
        bool osdReady = false;
        if (osdStatus == S_OK)
        {
            const char* stage;
            const HRESULT stockStatus = osd.OpenStock(device, &present, stage);
            BootLog(stage, stockStatus);
            osdReady = stockStatus == S_OK;
        }
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
        unsigned unzoomedMode = 0;
        unsigned subtitle = 0;
        double playbackSeconds = 0;
        double playbackDuration = frame.DurationSeconds();
        unsigned discSegment = frame.DiscSegment();
        bool deinterlaceNotified = false;
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
        bool switchingAudio = false;
        // XAM's disc-change broadcast carries zero when media is removed.
        bool noDisc = !discPlayback && !cdPlayback;
        const LONGLONG discProbeInterval = 2 * frequency.QuadPart;
        LONGLONG nextDiscProbe = previous.QuadPart + discProbeInterval;
        // Keep removal detection responsive without opening the drive on
        // every rendered frame. Reinsertion probes remain less frequent.
        const LONGLONG removalCheckInterval = frequency.QuadPart / 4;
        LONGLONG nextRemovalCheck = previous.QuadPart + removalCheckInterval;
        if (noDisc)
        {
            animation.Center();
            if (osdReady) osd.ShowNoDisc();
        }

        for (;;)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const float elapsed = (float)(now.QuadPart - previous.QuadPart) /
                                  (float)frequency.QuadPart;
            previous = now;

            bool playbackEnded = false;
            bool userStopped = false;
            bool resumed = false;
            DWORD notification;
            ULONG_PTR media;
            bool mediaRemoved = false;
            while (discListener && discListener != INVALID_HANDLE_VALUE &&
                   XNotifyGetNext(discListener, discMediaChanged, &notification, &media))
            {
                if (media == 0) mediaRemoved = true;
            }
            // Still menus and paused playback may issue no sector reads.
            // Poll the drive independently of the decoder and XAM events.
            if (!noDisc && !mediaRemoved && now.QuadPart >= nextRemovalCheck)
            {
                mediaRemoved = XboxDvdStream::DriveReportsNoMedia();
                LARGE_INTEGER checked;
                QueryPerformanceCounter(&checked);
                nextRemovalCheck = checked.QuadPart + removalCheckInterval;
            }
            if (!noDisc && mediaRemoved)
            {
                noDisc = true;
                playbackEnded = capture;
                showFrame = switchingAudio = embeddedAudio = false;
                cdAudio.Stop();
                cdScene.Reset();
                frame.Stop();
                animation.Center();
                if (!osdReady)
                {
                    const char* stage;
                    osdReady = SUCCEEDED(osd.OpenStock(device, &present, stage));
                }
                if (osdReady) osd.ShowNoDisc();
                BootLog("DVD media removed");
                nextDiscProbe = now.QuadPart + discProbeInterval;
            }
            // A tray close does not always deliver a usable insertion event.
            // Retry only while idle, with a delay between completed attempts.
            if (noDisc && !playbackEnded && now.QuadPart >= nextDiscProbe)
            {
                const HRESULT status = frame.DecodeMpeg2(device);
                BootLog("DVD idle probe: opening First Play", status);
                if (status != S_OK && cdAudio.Start())
                {
                    noDisc = false;
                    osd.ResetCdFocus();
                }
                QueryPerformanceCounter(&previous);
                nextDiscProbe = previous.QuadPart + discProbeInterval;
                if (status == S_OK)
                {
                    noDisc = paused = embeddedAudio = playbackClockStarted = false;
                    showFrame = compressed = capture = true;
                    playbackSeconds = 0;
                    playbackDuration = frame.DurationSeconds();
                    discSegment = frame.DiscSegment();
                    subtitle = 0;
                    subtitleCount = frame.SubtitleCount();
                    deinterlaceNotified = false;
                    presentedFrames = lastPresented = missedFrames = 0;
                    traceCount = traceOmitted = 0;
                    presentTicks = 0;
                    nextProgress = previous.QuadPart + 2 * frequency.QuadPart;
                    nextRemovalCheck = previous.QuadPart + removalCheckInterval;
                    playbackLog.Open();
                }
            }
            if (cdAudio.HasDisc())
            {
                const PlaybackOsd::Action action = osd.UpdateCd();
                if (action == PlaybackOsd::TogglePause) cdAudio.TogglePause();
                if (action == PlaybackOsd::PreviousChapter) cdAudio.Skip(-1);
                if (action == PlaybackOsd::NextChapter) cdAudio.Skip(1);
            }
            if (switchingAudio)
            {
                const HRESULT status = frame.PollAudioTrack(paused);
                if (status != E_PENDING)
                {
                    switchingAudio = false;
                    resumed = status == S_OK;
                    osd.ShowTrackPopup(false, 0, frame.TrackLanguage(false));
                    playbackLog.Publish(status == S_OK ? "Audio track switched" :
                        status == S_FALSE ? "No alternate audio track opened" : "Audio track start failed");
                }
            }
            if (!switchingAudio && osdReady && capture && showFrame && playbackClockStarted)
            {
                const DvdMenuState menu = frame.DiscMenuState();
                const PlaybackOsd::Action action = osd.Update(elapsed, paused, menu.buttons, menu.stillSeconds != 0);
                if (action >= PlaybackOsd::DiscUp && action <= PlaybackOsd::DiscContinue)
                {
                    static const DvdMenuCommand commands[] = {
                        DvdMenuUp, DvdMenuDown, DvdMenuLeft, DvdMenuRight,
                        DvdMenuActivate, DvdMenuRoot, DvdMenuTitle, DvdMenuContinue
                    };
                    frame.DiscMenuInput(commands[action - PlaybackOsd::DiscUp]);
                }
                else if (action == PlaybackOsd::TogglePause)
                {
                    const HRESULT result = frame.DiscTransitionPending() ? S_OK :
                        embeddedAudio ? frame.SetEmbeddedAudioPaused(!paused) : S_OK;
                    if (SUCCEEDED(result))
                    {
                        paused = !paused;
                        resumed = !paused;
                    }
                }
                else if (action == PlaybackOsd::Stop)
                {
                    userStopped = playbackEnded = true;
                    showFrame = false;
                    frame.Stop();
                }
                else if (action == PlaybackOsd::PreviousChapter)
                    frame.DiscMenuInput(DvdMenuPreviousChapter);
                else if (action == PlaybackOsd::NextChapter)
                    frame.DiscMenuInput(DvdMenuNextChapter);
                else if (action == PlaybackOsd::SeekBackward)
                    frame.DiscMenuInput(DvdMenuSeekBackward);
                else if (action == PlaybackOsd::SeekForward)
                    frame.DiscMenuInput(DvdMenuSeekForward);
                else if (action == PlaybackOsd::DisplayMode)
                    displayMode = (displayMode + 1) % PlaybackOsd::DisplayModeCount;
                else if (action == PlaybackOsd::ToggleZoom)
                {
                    if (displayMode == PlaybackOsd::StockZoomMode)
                        displayMode = unzoomedMode;
                    else
                    {
                        unzoomedMode = displayMode;
                        displayMode = PlaybackOsd::StockZoomMode;
                    }
                }
                else if (action == PlaybackOsd::NextAudioTrack)
                {
                    switchingAudio = !paused && embeddedAudio && frame.RequestNextAudioTrack();
                    if (!switchingAudio)
                        osd.ShowTrackPopup(false, 0, frame.TrackLanguage(false));
                }
                else if (action == PlaybackOsd::SubtitleTrack)
                {
                    subtitleCount = frame.SubtitleCount();
                    subtitle = (subtitle + 1) % (subtitleCount + 1);
                    osd.ShowTrackPopup(true, subtitle,
                        subtitleCount ? frame.TrackLanguage(true, subtitle) : "No tracks detected");
                }
            }
            const unsigned queueBefore = capture ? frame.QueueDepth() : 0;
            const unsigned frameBefore = frame.FramesReady();
            HRESULT advanceStatus = S_OK;
            if (!switchingAudio && showFrame && compressed && playbackClockStarted &&
                (!paused || frame.DiscTransitionPending()))
            {
                const unsigned before = frame.FramesReady();
                const double fallback = resumed ? 0.0 : elapsed;
                const double videoElapsed = paused ? 0 : embeddedAudio ? frame.AdvanceEmbeddedAudio(fallback)
                    : fallback;
                if (!frame.DiscMenuState().stillSeconds) playbackSeconds += videoElapsed;
                const HRESULT status = frame.Advance(device, videoElapsed, embeddedAudio,
                                                     paused ? 0 : elapsed);
                if (discSegment != frame.DiscSegment())
                {
                    discSegment = frame.DiscSegment();
                    deinterlaceNotified = false;
                    playbackSeconds = 0;
                    playbackDuration = frame.DurationSeconds();
                    subtitleCount = frame.SubtitleCount();
                    subtitle = 0;
                    double audioMs;
                    unsigned audioBuffers;
                    embeddedAudio = frame.EmbeddedAudioState(audioMs, audioBuffers);
                    const HRESULT segmentAudioStart = embeddedAudio && !paused ? frame.StartEmbeddedAudio() : S_FALSE;
                    char segment[160];
                    sprintf_s(segment, sizeof(segment),
                        "DVD segment %u: audio=%u queued=%u subtitles=%u duration=%.3fs",
                        discSegment, embeddedAudio ? 1u : 0u, embeddedAudio ? audioBuffers : 0u,
                        subtitleCount, playbackDuration);
                    playbackLog.Publish(segment, segmentAudioStart);
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
                    // An eject can reach FFmpeg as a read failure before (or
                    // instead of) XAM's media-change notification. Keep the
                    // title alive and use the normal idle re-probe path.
                    noDisc = true;
                    playbackEnded = capture;
                    showFrame = false;
                    switchingAudio = embeddedAudio = false;
                    animation.Center();
                    frame.Stop();
                    if (osdReady) osd.ShowNoDisc();
                    BootLog("DVD stream lost; awaiting replacement media", status);
                    nextDiscProbe = now.QuadPart + discProbeInterval;
                }
            }
            if (osdReady && showFrame && capture && !playbackEnded &&
                !deinterlaceNotified && frame.DeinterlacingActive())
            {
                osd.ShowDeinterlacePopup();
                BootLog("Interlaced video detected; FFmpeg bwdif active");
                deinterlaceNotified = true;
            }
            LARGE_INTEGER advanceEnd;
            QueryPerformanceCounter(&advanceEnd);
            if (!noDisc) animation.Update(elapsed);
            device->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            bool drewVideo = false;
            if (SUCCEEDED(device->BeginScene()))
            {
                if (showFrame)
                {
                    drewVideo = SUCCEEDED(frame.Draw(device, present.BackBufferWidth,
                                                     present.BackBufferHeight, displayMode));
                     if (osd.Ready() && capture && !playbackEnded)
                    {
                         frame.DrawSubtitles(device, osd, present.BackBufferWidth,
                                             present.BackBufferHeight, displayMode, subtitle, playbackSeconds);
                         frame.DrawDiscHighlight(device, osd, present.BackBufferWidth,
                                             present.BackBufferHeight, displayMode);
                    }
                }
                else if (!cdAudio.HasDisc() ||
                         !cdScene.Draw(device, !cdAudio.Paused(), elapsed))
                    animation.Render();
                device->EndScene();
                if (osdReady && noDisc)
                    osd.DrawNoDisc();
                else if (osdReady && cdAudio.HasDisc())
                    osd.DrawCd(cdAudio.Paused(), elapsed);
                else if (osdReady && showFrame && capture && !playbackEnded)
                    osd.DrawStock(paused, elapsed, playbackSeconds, playbackDuration,
                                   frame.TrackLanguage(false),
                                  frame.TrackLanguage(true, subtitle), subtitle);
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
                ? embeddedMs
                : -1.0;
            presentTicks += presentEnd.QuadPart - presentStart.QuadPart;
            if (compressed && drewVideo && SUCCEEDED(presentStatus) && !playbackClockStarted)
            {
                // Keep startup Present in the trace, but out of the video timeline.
                previous = presentEnd;
                playbackClockStarted = true;
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
                if (audioStartLog[0])
                    playbackLog.Publish(audioStartLog, audioStartStatus);
            }
            if (compressed && drewVideo && SUCCEEDED(presentStatus) &&
                lastPresented != frame.FramesReady())
            {
                lastPresented = frame.FramesReady();
                ++presentedFrames;
            }
#if defined(DVD360_LOGGING) || defined(DVD360_PERF_LOGGING)
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
                    s.bwdif = newFrame ? frame.LastBwdifMilliseconds() : 0.0;
                     s.audio = embeddedMs;
                     s.audioMaster = embeddedAudio;
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
#endif
            if (playbackEnded)
            {
                capture = false;
                playbackLog.Close();
                WritePacingTrace(traceCount, traceOmitted);
                BootLog(userStopped ? "Playback stopped by controller" :
                        FAILED(advanceStatus) ? "CPU MPEG-2 playback failed" :
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

        if (discListener && discListener != INVALID_HANDLE_VALUE) CloseHandle(discListener);
        animation.Shutdown();
        osd.CloseStock();
        device->Release();
        return 0;
    }
}

int main()
{
    return Run();
}
