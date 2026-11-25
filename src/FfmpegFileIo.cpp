#include "FfmpegFileIo.h"
#include "PlaybackAffinity.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace
{
    enum { ChunkSize = 128 * 1024, ChunkCount = 4 };

    struct FileInput
    {
        HANDLE file;
        FfmpegReadStats* stats;
        FfmpegReadStats totals;
        HANDLE thread, dataReady, spaceReady;
        CRITICAL_SECTION lock;
        uint8_t* bytes;
        DWORD lengths[ChunkCount];
        unsigned head, count, consumed, generation;
        int64_t position, nextRead, fileSize;
        int terminal;
        bool stopping;
    };

    DWORD WINAPI ReadWorker(void* opaque)
    {
        if (PlaybackAffinity::Enabled())
            PlaybackAffinity::Pin(PlaybackAffinity::Reader);
        FileInput& input = *static_cast<FileInput*>(opaque);
        int64_t physical = -1;
        for (;;)
        {
            EnterCriticalSection(&input.lock);
            if (input.stopping)
            {
                LeaveCriticalSection(&input.lock);
                return 0;
            }
            if (input.count == ChunkCount || input.terminal)
            {
                LeaveCriticalSection(&input.lock);
                WaitForSingleObject(input.spaceReady, INFINITE);
                continue;
            }
            const unsigned slot = (input.head + input.count) % ChunkCount;
            const unsigned generation = input.generation;
            const int64_t offset = input.nextRead;
            LeaveCriticalSection(&input.lock);

            LARGE_INTEGER start, end, distance;
            distance.QuadPart = offset;
            DWORD received = 0;
            const bool positioned = physical == offset ||
                SetFilePointerEx(input.file, distance, NULL, FILE_BEGIN);
            QueryPerformanceCounter(&start);
            const BOOL ok = positioned &&
                ReadFile(input.file, input.bytes + slot * ChunkSize, ChunkSize, &received, NULL);
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            QueryPerformanceCounter(&end);
            physical = ok ? offset + received : -1;

            EnterCriticalSection(&input.lock);
            const LONGLONG ticks = end.QuadPart - start.QuadPart;
            if (positioned)
            {
                ++input.totals.calls;
                input.totals.bytes += received;
                input.totals.ticks += ticks;
                if (ticks > input.totals.worstTicks)
                    input.totals.worstTicks = ticks;
            }
            if (generation == input.generation)
            {
                if (ok && received)
                {
                    input.lengths[slot] = received;
                    ++input.count;
                    input.nextRead += received;
                }
                else
                    input.terminal = ok || error == ERROR_HANDLE_EOF ? AVERROR_EOF : AVERROR(EIO);
            }
            LeaveCriticalSection(&input.lock);
            SetEvent(input.dataReady);
        }
    }

    int ReadPacket(void* opaque, uint8_t* buffer, int size)
    {
        FileInput& input = *static_cast<FileInput*>(opaque);
        for (;;)
        {
            EnterCriticalSection(&input.lock);
            // Only the AVIO consumer publishes stats to the decoder.
            if (input.stats)
                *input.stats = input.totals;
            if (input.count)
            {
                const DWORD available = input.lengths[input.head] - input.consumed;
                const DWORD copied = available < static_cast<DWORD>(size) ? available : size;
                memcpy(buffer, input.bytes + input.head * ChunkSize + input.consumed, copied);
                input.position += copied;
                input.consumed += copied;
                if (input.consumed == input.lengths[input.head])
                {
                    input.consumed = 0;
                    input.head = (input.head + 1) % ChunkCount;
                    --input.count;
                    SetEvent(input.spaceReady);
                }
                LeaveCriticalSection(&input.lock);
                return copied;
            }
            const int terminal = input.terminal;
            LeaveCriticalSection(&input.lock);
            if (terminal)
                return terminal;
            if (WaitForSingleObject(input.dataReady, INFINITE) != WAIT_OBJECT_0)
                return AVERROR(EIO);
        }
    }

    int64_t Seek(void* opaque, int64_t offset, int whence)
    {
        FileInput& input = *static_cast<FileInput*>(opaque);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE)
            return input.fileSize;
        // FFmpeg is the sole consumer, so only it advances position.
        int64_t base;
        switch (whence)
        {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = input.position; break;
        case SEEK_END: base = input.fileSize; break;
        default: return AVERROR(EINVAL);
        }
        if (offset < -base || offset > INT64_MAX - base)
            return AVERROR(EINVAL);
        const int64_t position = base + offset;
        EnterCriticalSection(&input.lock);
        if (input.position != position)
        {
            ++input.generation;
            input.head = input.count = input.consumed = 0;
            input.position = input.nextRead = position;
            input.terminal = 0;
        }
        LeaveCriticalSection(&input.lock);
        SetEvent(input.spaceReady);
        return position;
    }

    void DestroyInput(FileInput* input)
    {
        if (input->thread)
        {
            EnterCriticalSection(&input->lock);
            input->stopping = true;
            LeaveCriticalSection(&input->lock);
            SetEvent(input->spaceReady);
            WaitForSingleObject(input->thread, INFINITE);
            CloseHandle(input->thread);
        }
        if (input->spaceReady) CloseHandle(input->spaceReady);
        if (input->dataReady) CloseHandle(input->dataReady);
        DeleteCriticalSection(&input->lock);
        av_free(input->bytes);
        delete input;
    }
}

AVIOContext* CreateFfmpegFileIo(HANDLE file, FfmpegReadStats* stats)
{
    if (file == NULL || file == INVALID_HANDLE_VALUE)
        return NULL;

    const int bufferSize = ChunkSize;
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(bufferSize));
    if (!buffer)
        return NULL;

    FileInput* input = new FileInput();
    if (!input)
    {
        av_free(buffer);
        return NULL;
    }
    input->file = file;
    input->stats = stats;
    InitializeCriticalSection(&input->lock);
    LARGE_INTEGER size, zero = {}, position;
    input->bytes = static_cast<uint8_t*>(av_malloc(ChunkCount * ChunkSize));
    input->dataReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    input->spaceReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (input->bytes && input->dataReady && input->spaceReady &&
        GetFileSizeEx(file, &size) && SetFilePointerEx(file, zero, &position, FILE_CURRENT))
    {
        input->fileSize = size.QuadPart;
        input->position = input->nextRead = position.QuadPart;
        input->thread = CreateThread(NULL, 0, ReadWorker, input, 0, NULL);
    }
    if (!input->thread)
    {
        DestroyInput(input);
        av_free(buffer);
        return NULL;
    }
    AVIOContext* io = avio_alloc_context(buffer, bufferSize, 0, input,
                                         ReadPacket, NULL, Seek);
    if (!io)
    {
        DestroyInput(input);
        av_free(buffer);
    }
    return io;
}

void DestroyFfmpegFileIo(AVIOContext** io)
{
    if (!io || !*io)
        return;

    // FFmpeg may have replaced the original buffer during probing.
    DestroyInput(static_cast<FileInput*>((*io)->opaque));
    av_freep(&(*io)->buffer);
    avio_context_free(io);
}
