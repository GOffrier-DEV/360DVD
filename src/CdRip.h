#pragma once

#include "XboxDvdStream.h"

enum { CdRipPreparing = -1, CdRipVerifying = -2 };

void ProbeAccurateRipConnection(HANDLE stop, char (&summary)[96]);

HRESULT RipAudioCd(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                   HANDLE stop, HANDLE cancel, volatile LONG& progress, char (&summary)[96]);
