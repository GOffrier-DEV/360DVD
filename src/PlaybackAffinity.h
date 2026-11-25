#pragma once

#include <xtl.h>
#include <stdio.h>

namespace PlaybackAffinity
{
    // Use different physical cores (logical pairs 0/1, 2/3, 4/5).
    enum { Reader = 0, Decoder = 2, Renderer = 5 };

    inline bool Enabled()
    {
        FILE* marker = fopen("game:\\playback-affinity.enable", "rb");
        if (!marker)
            return false;
        fclose(marker);
        return true;
    }

    inline void Pin(DWORD processor)
    {
        XSetThreadProcessor(GetCurrentThread(), processor);
    }
}
