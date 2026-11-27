#pragma once

#include "XboxDvdStream.h"

enum { CdRipPreparing = -1, CdRipVerifying = -2 };

struct AccurateRipLookup
{
    char* response;
    int size;
    char status[72];
};

void LookupAccurateRip(const XboxDvdStream::CdAudioProbe& disc, HANDLE stop, AccurateRipLookup& lookup);
void LogAccurateRipFailure(const char* message);

HRESULT RipAudioCd(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                   HANDLE stop, HANDLE cancel, HANDLE lookupDone, const AccurateRipLookup& lookup,
                   volatile LONG& progress, char (&summary)[96]);
