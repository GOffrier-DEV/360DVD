#pragma once

#include "XboxDvdStream.h"

enum { CdRipPreparing = -1, CdRipVerifying = -2, CdRipCalibrating = -3 };

struct CdDriveInfo
{
    char vendor[9], model[17], revision[5];
};

void InstallCdOffsetTable();
void ProbeCdDrive(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc, CdDriveInfo& drive);

struct AccurateRipLookup
{
    char* response;
    int size;
    char status[72];
};

struct CdTrackMetadata
{
    char title[256], artist[256], isrc[13];
};

struct CdMetadata
{
    char album[256], artist[256], date[16], label[128], catalog[64];
    unsigned discNumber;
    BYTE* cover;
    unsigned coverSize;
    bool coverPng;
    CdTrackMetadata tracks[99];
};

struct CdEditionPicker
{
    HANDLE ready, decided;
    unsigned count;
    int selected;
    char labels[32][128];
};

void LookupCdMetadata(const XboxDvdStream::CdAudioProbe& disc, HANDLE stop, CdMetadata& metadata,
                      CdEditionPicker& picker);
void LookupAccurateRip(const XboxDvdStream::CdAudioProbe& disc, HANDLE stop, AccurateRipLookup& lookup);
void LogCdDiagnostic(const char* message, HRESULT status = S_OK);

HRESULT RipAudioCd(XboxDvdStream& stream, const XboxDvdStream::CdAudioProbe& disc,
                   HANDLE stop, HANDLE cancel, HANDLE lookupDone, const AccurateRipLookup& lookup,
                   const CdDriveInfo& driveInfo, HANDLE metadataDone, const CdMetadata& metadata,
                   volatile LONG& progress, char (&summary)[96]);
