#include "DvdEmbeddedResources.h"
#include <xui.h>
#include <xuiresource.h>
#include <string.h>
#include <wchar.h>

namespace
{
    struct EmbeddedResource { const wchar_t* name; const BYTE* bytes; UINT size; };
#include "DvdEmbeddedData.inc"

    class Reader : public IXuiResourceReader
    {
    public:
        Reader(const BYTE* bytes, UINT size, IXuiResourceManager* manager)
            : bytes(bytes), size(size), position(0), refs(1), manager(manager) {}
        VOID STDMETHODCALLTYPE AddRef() { ++refs; }
        UINT STDMETHODCALLTYPE Release() { const UINT n = --refs; if (!n) delete this; return n; }
        IXuiResourceManager* STDMETHODCALLTYPE GetManager() { return manager; }
        HRESULT STDMETHODCALLTYPE SetManager(IXuiResourceManager* value) { manager = value; return S_OK; }
        UINT STDMETHODCALLTYPE GetTotalSize() { return size; }
        UINT STDMETHODCALLTYPE GetRemainingBytes() { return size - position; }
        HRESULT STDMETHODCALLTYPE Read(BYTE* dst, UINT count, UINT* read)
        {
            if (read) *read = 0;
            if (count && !dst) return E_INVALIDARG;
            if (position > size) return E_FAIL;
            const UINT available = GetRemainingBytes();
            const UINT used = count < available ? count : available;
            memcpy(dst, bytes + position, used);
            position += used;
            if (read) *read = used;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Seek(UINT flags, INT offset)
        {
            if (flags != FILE_BEGIN && flags != FILE_CURRENT && flags != FILE_END)
                return E_INVALIDARG;
            const LONGLONG base = flags == FILE_BEGIN ? 0 : flags == FILE_CURRENT ? position : size;
            if ((offset < 0 && static_cast<LONGLONG>(offset) < -base) ||
                (offset > 0 && static_cast<LONGLONG>(offset) > static_cast<LONGLONG>(size) - base))
                return E_INVALIDARG;
            position = static_cast<UINT>(base + offset);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetBuffer(const BYTE** buffer)
        {
            if (!buffer) return E_POINTER;
            *buffer = bytes;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE ReleaseBuffer() { return S_OK; }
        HRESULT STDMETHODCALLTYPE Duplicate(IXuiResourceReader** reader)
        {
            if (!reader) return E_POINTER;
            *reader = NULL;
            Reader* copy = new Reader(bytes, size, manager);
            if (!copy) return E_OUTOFMEMORY;
            copy->position = position;
            *reader = copy;
            return S_OK;
        }
    private:
        const BYTE* bytes;
        UINT size, position, refs;
        IXuiResourceManager* manager;
    };

    class Transport : public IXuiResourceManager
    {
    public:
        VOID STDMETHODCALLTYPE AddRef() {}
        UINT STDMETHODCALLTYPE Release() { return 1; }
        LPCWSTR STDMETHODCALLTYPE GetName() { return L"dvd360"; }
        BOOL STDMETHODCALLTYPE IsTransport() { return TRUE; }
        HRESULT STDMETHODCALLTYPE Init(IXuiResourceReader*) { return S_OK; }
        HRESULT STDMETHODCALLTYPE Open(LPCWSTR path, IXuiResourceReader** reader)
        {
            if (!reader) return E_POINTER;
            *reader = NULL;
            UINT size;
            const BYTE* bytes = FindDvdResource(path, size);
            *reader = bytes ? new Reader(bytes, size, this) : NULL;
            return *reader ? S_OK : bytes ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        HRESULT STDMETHODCALLTYPE GetFileAttributes(LPCWSTR path, WIN32_FILE_ATTRIBUTE_DATA* attributes)
        {
            if (!attributes) return E_POINTER;
            UINT size;
            if (!FindDvdResource(path, size)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            memset(attributes, 0, sizeof(*attributes));
            attributes->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
            attributes->nFileSizeLow = size;
            return S_OK;
        }
    } transport;
}

const BYTE* FindDvdResource(const wchar_t* path, UINT& size)
{
    size = 0;
    if (!path) return NULL;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'/' || *p == L'\\') name = p + 1;
    for (unsigned i = 0; i < ARRAYSIZE(resources); ++i)
        if (!_wcsicmp(name, resources[i].name))
        {
            size = resources[i].size;
            return resources[i].bytes;
        }
    return NULL;
}

HRESULT RegisterDvdResourceTransport()
{
    return XuiRegisterResourceTransport(&transport, NULL);
}
