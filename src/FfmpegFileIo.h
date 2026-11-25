#pragma once

#include <xtl.h>

struct AVIOContext;
enum DvdMenuCommand { DvdMenuNone, DvdMenuUp, DvdMenuDown, DvdMenuLeft, DvdMenuRight,
                      DvdMenuActivate, DvdMenuRoot, DvdMenuTitle, DvdMenuContinue,
                      DvdMenuPreviousChapter, DvdMenuNextChapter,
                      DvdMenuSeekBackward, DvdMenuSeekForward };
struct DvdMenuState
{
    unsigned stillSeconds;
    bool buttons;
    RECT highlight;
    unsigned subpictureId;
};
bool QueueFfmpegDiscMenu(AVIOContext* io, DvdMenuCommand command, int64_t videoPts = -1);
DvdMenuState FfmpegDiscMenuState(AVIOContext* io, int64_t videoPts = -1);
struct FfmpegReadStats
{
    unsigned calls, bytes;
    LONGLONG ticks, worstTicks;
};

AVIOContext* CreateFfmpegDiscIo(FfmpegReadStats* stats, HRESULT& status);
void LogDvdNavStartup(const char* message, bool reset = false);
bool ResumeFfmpegDiscWait(AVIOContext* io, bool acknowledge);
bool FfmpegDiscBoundaryPending(AVIOContext* io);
bool FfmpegDiscUserJumpPending(AVIOContext* io);
double FfmpegDiscDuration(AVIOContext* io);
unsigned FfmpegDiscLanguage(AVIOContext* io, bool subtitle, unsigned physical);
int FfmpegDiscAudioStream(AVIOContext* io);
bool FfmpegDiscSubtitlePalette(AVIOContext* io, uint32_t (&palette)[16]);
void DestroyFfmpegFileIo(AVIOContext** io);
