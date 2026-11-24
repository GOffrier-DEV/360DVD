#pragma once
#include <xtl.h>

const BYTE* FindDvdResource(const wchar_t* path, UINT& size);
HRESULT RegisterDvdResourceTransport();
