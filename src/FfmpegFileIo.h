#pragma once

#include <xtl.h>

struct AVIOContext;
struct FfmpegReadStats
{
    unsigned calls, bytes;
    LONGLONG ticks, worstTicks;
};

// Borrows a synchronous, seekable file handle. Keep it open and do not share
// its file position until the format context and this AVIO context are closed.
AVIOContext* CreateFfmpegFileIo(HANDLE file, FfmpegReadStats* stats = NULL);
void DestroyFfmpegFileIo(AVIOContext** io);
