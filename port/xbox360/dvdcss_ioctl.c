#include <stddef.h>
#include <string.h>
#include "ioctl.h"

enum {
    StartSession = 0x335000, ReadKey = 0x335004,
    SendKey = 0x335008, EndSession = 0x33500C,
    ReadStructure = 0x335142
};

typedef struct {
    int32_t status;
    uintptr_t information;
} DvdIoStatus;

typedef struct {
    uint32_t length, agid, format, flags;
    uint64_t offset;
    uint8_t data[12];
} DvdKeyRequest;

typedef struct {
    uint64_t offset;
    uint32_t format, agid;
    uint8_t layer;
    uint8_t reserved[7];
} DvdStructureRequest;

_Static_assert(sizeof(DvdIoStatus) == 8, "Xbox I/O status layout");
_Static_assert(offsetof(DvdKeyRequest, data) == 24, "Xbox DVD key layout");
_Static_assert(sizeof(DvdStructureRequest) == 24, "Xbox DVD structure layout");

int32_t NtDeviceIoControlFile(void *, void *, void *, void *, DvdIoStatus *,
                              uint32_t, void *, uint32_t, void *, uint32_t);

/* Diagnostic for the single optical probe. No key material is logged. */
static unsigned verified_sessions;
static int32_t last_error;

unsigned dvd360_css_verified_sessions(void) { return verified_sessions; }
int32_t dvd360_css_last_error(void) { return last_error; }

static int control(int fd, uint32_t code, void *in, uint32_t inlen,
                   void *out, uint32_t outlen, uint32_t required)
{
    DvdIoStatus io = {0};
    int32_t status = NtDeviceIoControlFile((void *)(uintptr_t)(uint32_t)fd,
        NULL, NULL, NULL, &io, code, in, inlen, out, outlen);
    if (status == 0)
        status = io.status;
    if (status == 0 && (io.information < required || io.information > outlen))
        status = (int32_t)0xC000009C;
    if (status != 0)
        last_error = status;
    return status == 0 ? 0 : -1;
}

static int read_key(int fd, unsigned agid, unsigned format, uint64_t offset,
                    void *data, unsigned size, unsigned capacity)
{
    DvdKeyRequest in = {0}, out = {0};
    in.length = 24 + capacity;
    in.agid = agid;
    in.format = format;
    in.offset = offset;
    if (control(fd, ReadKey, &in, 24, &out, 24 + capacity, 24 + size))
        return -1;
    memcpy(data, out.data, size);
    return 0;
}

static int send_key(int fd, unsigned agid, unsigned format,
                    const void *data, unsigned size, unsigned capacity)
{
    DvdKeyRequest in = {0};
    in.length = 24 + capacity;
    in.agid = agid;
    in.format = format;
    memcpy(in.data, data, size);
    return control(fd, SendKey, &in, in.length, NULL, 0, 0);
}

static int structure(int fd, unsigned format, unsigned agid, unsigned layer,
                     void *out, uint32_t length, uint32_t required)
{
    DvdStructureRequest in = {0};
    in.format = format;
    in.agid = agid;
    in.layer = layer;
    return control(fd, ReadStructure, &in, sizeof(in), out, length, required);
}

int ioctl_ReadCopyright(int fd, int layer, int *copyright)
{
    uint32_t aligned[2] = {0};
    const uint8_t *data = (const uint8_t *)aligned;
    if (structure(fd, DVD_STRUCT_COPYRIGHT, 0, layer, aligned, 8, 5))
        return -1;
    *copyright = data[4];
    return 0;
}

int ioctl_ReadDiscKey(int fd, const int *agid, uint8_t *key)
{
    uint32_t aligned[(DVD_DISCKEY_SIZE + 4) / 4] = {0};
    const uint8_t *data = (const uint8_t *)aligned;
    if (structure(fd, DVD_STRUCT_DISCKEY, *agid, 0, aligned,
                  sizeof(aligned), sizeof(aligned)))
        return -1;
    memcpy(key, data + 4, DVD_DISCKEY_SIZE);
    return 0;
}

int ioctl_ReadTitleKey(int fd, const int *agid, int lba, uint8_t *key)
{
    if (lba < 0)
        return -1;
    /* The kernel extracts the five key bytes after the title flags. */
    return read_key(fd, *agid, DVD_REPORT_TITLE_KEY, (uint64_t)lba * 2048,
                    key, 5, 8);
}

int ioctl_ReportAgid(int fd, int *agid)
{
    uint32_t session = 0;
    if (control(fd, StartSession, NULL, 0, &session, 4, 4) || session > 3)
        return -1;
    *agid = session;
    return 0;
}

int ioctl_ReportChallenge(int fd, const int *agid, uint8_t *challenge)
{
    return read_key(fd, *agid, DVD_REPORT_CHALLENGE, 0, challenge, 10, 12);
}

int ioctl_ReportKey1(int fd, const int *agid, uint8_t *key)
{
    return read_key(fd, *agid, DVD_REPORT_KEY1, 0, key, 5, 8);
}

int ioctl_ReportASF(int fd, int *success)
{
    uint8_t data[4];
    if (read_key(fd, 0, DVD_REPORT_ASF, 0, data, sizeof(data), sizeof(data)))
        return -1;
    *success = data[3] & 1;
    if (*success)
        ++verified_sessions;
    return 0;
}

int ioctl_InvalidateAgid(int fd, int *agid)
{
    uint32_t in = *agid, out = 0;
    return control(fd, EndSession, &in, 4, &out, 4, 0);
}

int ioctl_SendChallenge(int fd, const int *agid, const uint8_t *challenge)
{
    return send_key(fd, *agid, DVD_SEND_CHALLENGE, challenge, 10, 12);
}

int ioctl_SendKey2(int fd, const int *agid, const uint8_t *key)
{
    return send_key(fd, *agid, DVD_SEND_KEY2, key, 5, 8);
}

int ioctl_ReportRPC(int fd, int *type, int *mask, int *scheme)
{
    uint8_t data[4];
    if (read_key(fd, 0, DVD_REPORT_RPC, 0, data, sizeof(data), sizeof(data)))
        return -1;
    *type = data[0] >> 6;
    *mask = data[1];
    *scheme = data[2];
    return 0;
}

/* CPRM is not part of the DVD-Video CSS transport. */
int ioctl_ReadCPRMMKBPack(int fd, int *agid, int pack, uint8_t *buffer, int *size)
{
    (void)fd; (void)agid; (void)pack; (void)buffer; (void)size;
    return -1;
}

int ioctl_ReadCPRMMediaId(int fd, int *agid, uint8_t *mediaId)
{
    (void)fd; (void)agid; (void)mediaId;
    return -1;
}
