#include "FfmpegFileIo.h"
#include "PlaybackAffinity.h"
#include "XboxDvdStream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <dvdnav/dvdnav.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
int dvdnav_xbox_menu_transition(dvdnav_t* nav, int wasStill);
int dvdnav_xbox_selected_button(dvdnav_t* nav);
void dvdnav_xbox_cell_location(dvdnav_t* nav, int* domain, int* vts, int* pgc);
}

namespace
{
    bool navVmTrace;
    struct NavLogLock
    {
        CRITICAL_SECTION lock;
        NavLogLock() { InitializeCriticalSection(&lock); }
        ~NavLogLock() { DeleteCriticalSection(&lock); }
    } navLogLock;
}

void LogDvdNavStartup(const char* message, bool reset)
{
    EnterCriticalSection(&navLogLock.lock);
    HANDLE log = CreateFileA("game:\\360DVD-nav.log", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, reset ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&navLogLock.lock);
        return;
    }
    if (!reset) SetFilePointer(log, 0, NULL, FILE_END);
    char line[256];
    const int size = sprintf_s(line, sizeof(line), "%s\r\n", message);
    DWORD written;
    if (size > 0) WriteFile(log, line, size, &written, NULL);
    CloseHandle(log);
    LeaveCriticalSection(&navLogLock.lock);
}

extern "C" int dvd360_vm_trace_enabled(void) { return navVmTrace; }
extern "C" void dvd360_vm_trace_line(const char* message) { LogDvdNavStartup(message); }

namespace
{
    enum { ChunkSize = 128 * 1024, ChunkCount = 4 };

    struct FileInput
    {
        HANDLE file;
        XboxDvdStream* disc;
        dvdnav_t* nav;
        int navTerminal;
        bool navWaiting, navResume;
        bool navNeedsAck, navSegmentData, navMenuTransition, userJump;
        double navDuration;
        uint16_t audioLanguages[8], subtitleLanguages[32];
        int segmentAudioPhysical;
        uint32_t navClut[16], segmentClut[16];
        bool navClutValid, segmentClutValid;
        DvdMenuCommand menuCommand;
        bool menuCommandRunning;
        DvdMenuState menu;
        pci_t menuPci, menuCommandPci, menuHistory[64];
        unsigned menuHistoryHead, menuHistoryCount;
        int32_t menuButton;
        unsigned menuSubpictureId;
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
        bool navFirstBlock;
    };

    bool HasMenuButtons(const pci_t* pci)
    {
        return pci && (pci->hli.hl_gi.hli_ss & 3) && pci->hli.hl_gi.btn_ns;
    }

    int VisibleMenuButton(const pci_t& pci, int button)
    {
        if (button > 0 && button <= pci.hli.hl_gi.btn_ns) return button;
        const int forced = pci.hli.hl_gi.fosl_btnn;
        return forced > 0 && forced <= pci.hli.hl_gi.btn_ns ? forced : 1;
    }

    DvdMenuState DescribeMenu(pci_t* pci, unsigned still, int32_t button)
    {
        DvdMenuState state = {};
        state.stillSeconds = still;
        state.buttons = HasMenuButtons(pci);
        dvdnav_highlight_area_t area;
        if (state.buttons &&
            dvdnav_get_highlight_area(pci, VisibleMenuButton(*pci, button), 0, &area) == DVDNAV_STATUS_OK)
        {
            state.highlight.left = area.sx;
            state.highlight.top = area.sy;
            state.highlight.right = area.ex + 1;
            state.highlight.bottom = area.ey + 1;
        }
        return state;
    }

    // Caller holds the input lock. Match the PCI to the actual displayed
    // video frame, not the optical reader's position several VOBUs ahead.
    pci_t* PresentedMenuPci(FileInput& input, int64_t videoPts)
    {
        if (input.menu.stillSeconds) return &input.menuPci;
        if (videoPts < 0) return NULL;
        const uint32_t pts = static_cast<uint32_t>(videoPts);
        const unsigned capacity = ARRAYSIZE(input.menuHistory);
        for (unsigned i = 1; i <= input.menuHistoryCount; ++i)
        {
            pci_t& pci = input.menuHistory[(input.menuHistoryHead + capacity - i) % capacity];
            const uint32_t start = pci.pci_gi.vobu_s_ptm;
            if (pts - start < pci.pci_gi.vobu_e_ptm - start) return &pci;
        }
        return NULL;
    }

    void UpdateMenu(FileInput& input, pci_t* pci, unsigned still, bool record = false)
    {
        int32_t button = 0;
        if (pci) button = dvdnav_xbox_selected_button(input.nav);
        const int spu = HasMenuButtons(pci) ? dvdnav_get_active_spu_stream(input.nav) : -1;
        DvdMenuState state = DescribeMenu(pci, still, button);
        EnterCriticalSection(&input.lock);
        if (pci) input.menuPci = *pci;
        if (!pci) input.menuHistoryHead = input.menuHistoryCount = 0;
        if (record && state.buttons)
        {
            input.menuHistory[input.menuHistoryHead] = *pci;
            const unsigned capacity = ARRAYSIZE(input.menuHistory);
            input.menuHistoryHead = (input.menuHistoryHead + 1) % capacity;
            if (input.menuHistoryCount < capacity) ++input.menuHistoryCount;
        }
        input.menuButton = button;
        if (spu >= 0) input.menuSubpictureId = 0x20 | (spu & 31);
        else if (!pci) input.menuSubpictureId = 0;
        input.menu = state;
        LeaveCriticalSection(&input.lock);
    }

    HRESULT OpenDisc(FileInput& input)
    {
        navVmTrace = GetFileAttributesA("game:\\dvd-vm-trace.enable") != DWORD(-1);
        LogDvdNavStartup("Opening Xbox optical transport", true);
        input.disc = new XboxDvdStream();
        if (!input.disc) return E_OUTOFMEMORY;
        if (!input.disc->Open())
            return HRESULT_FROM_NT(input.disc->LastStatus());
        LogDvdNavStartup("Opening libdvdnav VM");
        if (dvdnav_open_stream(&input.nav, input.disc, input.disc->ReaderAuthCallbacks()) != DVDNAV_STATUS_OK)
        {
            LogDvdNavStartup("libdvdnav open failed");
            return E_FAIL;
        }
        dvdnav_set_readahead_flag(input.nav, 1);
        LogDvdNavStartup("libdvdnav VM open");
        return S_OK;
    }

    void ReadDisc(FileInput& input, uint8_t* buffer, DWORD& received)
    {
        unsigned events = 0;
        while (!input.navTerminal && received < ChunkSize && events++ < 512)
        {
            uint8_t* sector = buffer + received;
            int32_t event = 0, length = 0;
            if (dvdnav_get_next_block(input.nav, sector, &event, &length) != DVDNAV_STATUS_OK)
            {
                LogDvdNavStartup(dvdnav_err_to_string(input.nav));
                input.navTerminal = AVERROR(EIO);
            }
            else if (event == DVDNAV_SPU_CLUT_CHANGE && length >= sizeof(input.navClut))
            {
                memcpy(input.navClut, sector, sizeof(input.navClut));
                input.navClutValid = true;
            }
            else if (event == DVDNAV_CELL_CHANGE && length >= sizeof(dvdnav_cell_change_event_t))
            {
                dvdnav_cell_change_event_t cell;
                memcpy(&cell, sector, sizeof(cell));
                int32_t title = 0, part = 0;
                dvdnav_current_title_info(input.nav, &title, &part);
                int domain, vts, pgc;
                dvdnav_xbox_cell_location(input.nav, &domain, &vts, &pgc);
                char message[176];
                sprintf_s(message, sizeof(message),
                    "DVD cell: domain=%d vts=%d pgc=%d title=%d part=%d cell=%d program=%d duration=%.3fs",
                    domain, vts, pgc, title, part, cell.cellN, cell.pgN, cell.pgc_length / 90000.0);
                LogDvdNavStartup(message);
                EnterCriticalSection(&input.lock);
                if (!input.navSegmentData && cell.pgc_length > 0)
                    input.navDuration = cell.pgc_length / 90000.0;
                LeaveCriticalSection(&input.lock);
            }
            else if (event == DVDNAV_VTS_CHANGE || event == DVDNAV_HOP_CHANNEL)
            {
                UpdateMenu(input, NULL, 0);
                if (input.navSegmentData)
                {
                    LogDvdNavStartup(event == DVDNAV_HOP_CHANNEL
                        ? "DVDNAV_HOP_CHANNEL: draining before navigation discontinuity"
                        : "DVDNAV_VTS_CHANGE: draining before stream change");
                    input.navWaiting = true;
                    input.navTerminal = AVERROR_EOF;
                }
            }
            else if (event == DVDNAV_STOP)
            {
                LogDvdNavStartup("DVDNAV_STOP: authored playback end");
                input.navTerminal = AVERROR_EOF;
            }
            else if (event == DVDNAV_STILL_FRAME || event == DVDNAV_WAIT)
            {
                if (event == DVDNAV_WAIT) LogDvdNavStartup("Reached DVDNAV_WAIT");
                input.navWaiting = event == DVDNAV_WAIT;
                input.navNeedsAck = input.navWaiting;
                if (event == DVDNAV_STILL_FRAME)
                {
                    dvdnav_still_event_t still;
                    memcpy(&still, sector, sizeof(still));
                    char message[96];
                    sprintf_s(message, sizeof(message), "Reached DVDNAV_STILL_FRAME: %u seconds (255=indefinite)",
                              static_cast<unsigned>(still.length));
                    LogDvdNavStartup(message);
                    UpdateMenu(input, dvdnav_get_current_nav_pci(input.nav), still.length);
                }
                input.navTerminal = AVERROR_EOF;
            }
            else if (event == DVDNAV_BLOCK_OK || event == DVDNAV_NAV_PACKET)
            {
                if (event == DVDNAV_NAV_PACKET)
                    UpdateMenu(input, dvdnav_get_current_nav_pci(input.nav), 0, true);
                if (length != DVD_VIDEO_LB_LEN ||
                    ((sector[0x14] & 0x30) && !memcmp(sector, "\x00\x00\x01\xBA", 4)
                     && event == DVDNAV_BLOCK_OK))
                    input.navTerminal = AVERROR(EIO);
                else
                {
                    if (!input.navFirstBlock)
                    {
                        LogDvdNavStartup("First DVD block received");
                        input.navFirstBlock = true;
                    }
                    received += length;
                    if (event == DVDNAV_BLOCK_OK && !input.navSegmentData)
                    {
                        EnterCriticalSection(&input.lock);
                        input.segmentAudioPhysical = dvdnav_get_active_audio_stream(input.nav);
                        input.segmentClutValid = input.navClutValid;
                        memcpy(input.segmentClut, input.navClut, sizeof(input.segmentClut));
                        memset(input.audioLanguages, 0, sizeof(input.audioLanguages));
                        memset(input.subtitleLanguages, 0, sizeof(input.subtitleLanguages));
                        for (unsigned i = 0; i < 8; ++i)
                        {
                            const int physical = dvdnav_get_audio_logical_stream(input.nav, i);
                            if (physical >= 0 && physical < 8)
                                input.audioLanguages[physical] = dvdnav_audio_stream_to_lang(input.nav, i);
                        }
                        for (unsigned i = 0; i < 32; ++i)
                        {
                            const int physical = dvdnav_get_spu_logical_stream(input.nav, i);
                            if (physical >= 0 && physical < 32)
                                input.subtitleLanguages[physical] = dvdnav_spu_stream_to_lang(input.nav, i);
                        }
                        LeaveCriticalSection(&input.lock);
                        input.navSegmentData = true;
                    }
                }
            }
        }
        if (!received && !input.navTerminal) input.navTerminal = AVERROR(EIO);
    }

    void RunMenuCommand(FileInput& input, DvdMenuCommand command, pci_t& pci)
    {
        const bool still = input.menu.stillSeconds != 0;
        int32_t button = 0;
        if (HasMenuButtons(&pci)) button = dvdnav_xbox_selected_button(input.nav);
        dvdnav_status_t result = DVDNAV_STATUS_ERR;
        if (command == DvdMenuRoot || command == DvdMenuTitle)
            result = dvdnav_menu_call(input.nav, command == DvdMenuRoot ? DVD_MENU_Root : DVD_MENU_Title);
        else if (command == DvdMenuContinue && still)
            result = dvdnav_still_skip(input.nav);
        else if (HasMenuButtons(&pci))
        {
            const int target = VisibleMenuButton(pci, button);
            if (target != button && dvdnav_button_select(input.nav, &pci, target) == DVDNAV_STATUS_OK)
                button = target;
            switch (command)
            {
            case DvdMenuUp: result = dvdnav_upper_button_select(input.nav, &pci); break;
            case DvdMenuDown: result = dvdnav_lower_button_select(input.nav, &pci); break;
            case DvdMenuLeft: result = dvdnav_left_button_select(input.nav, &pci); break;
            case DvdMenuRight: result = dvdnav_right_button_select(input.nav, &pci); break;
            case DvdMenuActivate: result = dvdnav_button_activate(input.nav, &pci); break;
            default: break;
            }
        }
        char message[128];
        sprintf_s(message, sizeof(message), "DVD menu command %u on button %d: %s",
                  static_cast<unsigned>(command), button,
                  result == DVDNAV_STATUS_OK ? "accepted" : "rejected");
        LogDvdNavStartup(message);
        if (result != DVDNAV_STATUS_OK)
        {
            LogDvdNavStartup(dvdnav_err_to_string(input.nav));
            return;
        }
        if (command == DvdMenuContinue || command == DvdMenuRoot || command == DvdMenuTitle ||
            dvdnav_xbox_menu_transition(input.nav, still))
        {
            UpdateMenu(input, NULL, 0);
            EnterCriticalSection(&input.lock);
            input.navWaiting = true;
            input.navNeedsAck = false;
            input.navMenuTransition = true;
            input.userJump = command != DvdMenuContinue;
            input.navTerminal = input.terminal = AVERROR_EOF;
            input.count = input.consumed = 0; // queued optical data belongs to the old VM position
            LeaveCriticalSection(&input.lock);
            SetEvent(input.dataReady);
        }
        else
            UpdateMenu(input, &input.menuPci, input.menu.stillSeconds);
    }

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
            if (input.navResume)
            {
                input.navResume = false;
                LeaveCriticalSection(&input.lock);
                const bool ack = input.navNeedsAck;
                const bool menuTransition = input.navMenuTransition;
                input.navMenuTransition = false;
                input.navNeedsAck = input.navSegmentData = false;
                LogDvdNavStartup(ack ? "Output drained; acknowledging DVDNAV_WAIT" :
                    menuTransition ? "Output drained; continuing after DVD menu command" :
                                     "Output drained; continuing after VTS change");
                input.navTerminal = !ack || dvdnav_wait_skip(input.nav) == DVDNAV_STATUS_OK
                    ? 0 : AVERROR(EIO);
                continue;
            }
            if (input.menuCommand != DvdMenuNone)
            {
                const DvdMenuCommand command = input.menuCommand;
                pci_t pci = input.menuCommandPci;
                input.menuCommand = DvdMenuNone;
                input.menuCommandRunning = true;
                LeaveCriticalSection(&input.lock);
                RunMenuCommand(input, command, pci);
                EnterCriticalSection(&input.lock);
                input.menuCommandRunning = false;
                LeaveCriticalSection(&input.lock);
                continue;
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
            const bool positioned = input.nav || physical == offset ||
                SetFilePointerEx(input.file, distance, NULL, FILE_BEGIN);
            QueryPerformanceCounter(&start);
            uint8_t* target = input.bytes + slot * ChunkSize;
            BOOL ok = TRUE;
            if (input.nav) ReadDisc(input, target, received);
            else ok = positioned && ReadFile(input.file, target, ChunkSize, &received, NULL);
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
                    input.terminal = input.nav ? input.navTerminal :
                        ok || error == ERROR_HANDLE_EOF ? AVERROR_EOF : AVERROR(EIO);
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
        if (input->nav) dvdnav_close(input->nav);
        delete input->disc;
        DeleteCriticalSection(&input->lock);
        av_free(input->bytes);
        delete input;
    }
}

static AVIOContext* CreateInput(HANDLE file, bool disc, FfmpegReadStats* stats, HRESULT& status)
{
    status = E_OUTOFMEMORY;
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
    LARGE_INTEGER size = {}, zero = {}, position = {};
    input->bytes = static_cast<uint8_t*>(av_malloc(ChunkCount * ChunkSize));
    input->dataReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    input->spaceReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (input->bytes && input->dataReady && input->spaceReady)
    {
        status = disc ? OpenDisc(*input) :
            GetFileSizeEx(file, &size) && SetFilePointerEx(file, zero, &position, FILE_CURRENT)
                ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(status))
        {
            if (!disc) input->fileSize = size.QuadPart;
            input->position = input->nextRead = position.QuadPart;
            input->thread = CreateThread(NULL, 0, ReadWorker, input, 0, NULL);
            if (!input->thread) status = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    if (!input->thread)
    {
        DestroyInput(input);
        av_free(buffer);
        return NULL;
    }
    AVIOContext* io = avio_alloc_context(buffer, bufferSize, 0, input,
                                         ReadPacket, NULL, disc ? NULL : Seek);
    if (!io)
    {
        status = E_OUTOFMEMORY;
        DestroyInput(input);
        av_free(buffer);
    }
    return io;
}

AVIOContext* CreateFfmpegFileIo(HANDLE file, FfmpegReadStats* stats)
{
    if (!file || file == INVALID_HANDLE_VALUE) return NULL;
    HRESULT status;
    return CreateInput(file, false, stats, status);
}

AVIOContext* CreateFfmpegDiscIo(FfmpegReadStats* stats, HRESULT& status)
{
    return CreateInput(INVALID_HANDLE_VALUE, true, stats, status);
}

bool ResumeFfmpegDiscWait(AVIOContext* io, bool acknowledge)
{
    if (!io) return false;
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const bool resume = input.nav && input.terminal == AVERROR_EOF && input.navWaiting &&
        (acknowledge || (!input.menuCommandRunning && input.menuCommand == DvdMenuNone));
    if (resume && acknowledge)
    {
        input.navWaiting = false;
        input.userJump = false;
        input.navResume = true;
        input.navDuration = 0;
        input.terminal = 0;
        avio_flush(io);
        io->eof_reached = io->error = 0;
    }
    LeaveCriticalSection(&input.lock);
    if (resume && acknowledge) SetEvent(input.spaceReady);
    return resume;
}

bool FfmpegDiscBoundaryPending(AVIOContext* io)
{
    if (!io) return false;
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const bool pending = input.nav && input.terminal == AVERROR_EOF &&
        (input.navWaiting || input.menu.stillSeconds);
    LeaveCriticalSection(&input.lock);
    return pending;
}

bool FfmpegDiscUserJumpPending(AVIOContext* io)
{
    if (!io) return false;
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const bool pending = input.userJump && !input.menuCommandRunning;
    LeaveCriticalSection(&input.lock);
    return pending;
}

DvdMenuState FfmpegDiscMenuState(AVIOContext* io, int64_t videoPts)
{
    DvdMenuState state = {};
    if (!io) return state;
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    state = DescribeMenu(PresentedMenuPci(input, videoPts), input.menu.stillSeconds, input.menuButton);
    state.subpictureId = input.menuSubpictureId;
    LeaveCriticalSection(&input.lock);
    return state;
}

bool QueueFfmpegDiscMenu(AVIOContext* io, DvdMenuCommand command, int64_t videoPts)
{
    if (!io || command == DvdMenuNone) return false;
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    pci_t* pci = PresentedMenuPci(input, videoPts);
    const bool requiresButton = command >= DvdMenuUp && command <= DvdMenuActivate;
    const bool queued = input.nav && input.menuCommand == DvdMenuNone &&
        (!requiresButton || HasMenuButtons(pci));
    if (queued)
    {
        if (requiresButton) input.menuCommandPci = *pci;
        input.menuCommand = command;
    }
    LeaveCriticalSection(&input.lock);
    if (queued) SetEvent(input.spaceReady);
    return queued;
}

double FfmpegDiscDuration(AVIOContext* io)
{
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const double duration = input.navDuration;
    LeaveCriticalSection(&input.lock);
    return duration;
}

unsigned FfmpegDiscLanguage(AVIOContext* io, bool subtitle, unsigned physical)
{
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const unsigned language = subtitle ?
        (physical < 32 ? input.subtitleLanguages[physical] : 0) :
        (physical < 8 ? input.audioLanguages[physical] : 0);
    LeaveCriticalSection(&input.lock);
    return language;
}

int FfmpegDiscAudioStream(AVIOContext* io)
{
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const int stream = input.segmentAudioPhysical;
    LeaveCriticalSection(&input.lock);
    return stream;
}

bool FfmpegDiscSubtitlePalette(AVIOContext* io, uint32_t (&palette)[16])
{
    FileInput& input = *static_cast<FileInput*>(io->opaque);
    EnterCriticalSection(&input.lock);
    const bool valid = input.segmentClutValid;
    if (valid) memcpy(palette, input.segmentClut, sizeof(palette));
    LeaveCriticalSection(&input.lock);
    return valid;
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
