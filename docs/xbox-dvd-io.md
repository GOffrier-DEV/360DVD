# Xbox 360 DVD device I/O findings

This note records DVD-device paths observed in the `xamd.dll`, `xboxkrnld.exe`,
`xboxkrnlc.exe`, and `xboxkrnlct.exe` IDA databases. The kernel builds examined
have matching control dispatch and packet-handler logic for the paths described
here. The C kernel database also exposed additional read-path details below.
This is a reverse-engineering reference, not a claim that every request is safe
or supported on every console/kernel revision. The XAM image was based at
`0x815F0000`; the kernel images were based at `0x80040000`.

## Summary

The kernel exposes DVD-specific device controls for DVD key and structure
operations, plus a SCSI pass-through path. These paths reach the kernel's
ATAPI-packet transport. XAM uses separate controls for its own authentication
and drive management; those should not be confused with CSS packet commands.

| Device control | Kernel handler | ATAPI CDB | Notes |
|---|---|---|---|
| `0x335000` | `SataCdRomStartStartSession` | `REPORT KEY` (`0xA4`) | Starts a DVD authentication session; completion reads the AGID from the response. |
| `0x335004` | `SataCdRomStartReadKey` | `REPORT KEY` (`0xA4`) | Reads a requested key format. |
| `0x335008` | `SataCdRomStartSendKey` | `SEND KEY` (`0xA3`) | Sends a key-format payload. |
| `0x33500C` | `SataCdRomStartEndSession` | `SEND KEY` (`0xA3`) | Ends the session using format `0x3F`; no data phase. |
| `0x335142` | `SataCdRomStartReadStructure` | `READ DVD STRUCTURE` (`0xAD`) | Reads DVD structure data. |
| `0x4D014` | `SataCdRomStartScsiPassThrough` | Caller-provided | Validated pass-through request, then dispatched through the same ATAPI transport. |
| `0x2403E` | `SataCdRomStartRawRead` | `READ CD` (`0xBE`) | Raw sector read; see limits below. |

The control-code-to-handler mapping is visible in
`SataCdRomDispatchIo` (`0x800C91C0`) and `SataCdRomDeviceControl`.
The C kernel build has the corresponding dispatch at `0x800C4990`; its control
code mapping for these paths matches the D build.

### Device-control gate versus packet dispatch

`SataCdRomDeviceControl` in `xboxkrnlc.exe` (`0x800C4558`) is the top-level
device-control handler, and is worth distinguishing from
`SataCdRomDispatchIo` (`0x800C4990`). It handles some controls directly
(authentication, media/drive queries, firmware challenge response, and drive
settings). For packet-backed controls, it validates/queues the request through
`SataChannelStartPacket`; the later `SataCdRomDispatchIo` selects the operation
handler, including `SataCdRomStartRawRead`, the key handlers, structure read,
and SCSI pass-through.

In particular, the C kernel `DeviceControl` accepts control `0x2403E` into the
packet path, while `DispatchIo` maps that code to `SataCdRomStartRawRead`.
The mapping is therefore a two-stage path, not a direct call from
`SataCdRomDeviceControl` to the raw-read routine. The same distinction applies
to the other packet operations.

The `xboxkrnlct.exe` build confirms the same structure: its
`SataCdRomDeviceControl` is at `0x800C3E18`, and `SataCdRomDispatchIo` is at
`0x800C4250`. The packet handlers and their validation logic match the C build
in the inspected routines (addresses differ). The top-level handler additionally
shows why not every DVD IOCTL should be described as a packet command: it
directly handles XAM-facing operations such as media type, drive measures,
firmware challenge response, authentication state/results, spindle controls,
and drive settings.

The `xboxkrnldt.exe` build also matches this structure: its
`SataCdRomDeviceControl` is at `0x800C85B8`, and `SataCdRomDispatchIo` is at
`0x800C89F0`. The packet-handler validation matches the C/T builds in the
inspected routines. Its `SataCdRomStartRead` includes additional
`KeDebugMonitorData` callbacks around READ(10), while retaining the same
sector-cache behavior, 2048-byte LBA conversion, and `0x20000` transfer cap.

## DVD-specific request paths

### REPORT KEY (`0x335004`)

`SataCdRomStartReadKey` (`0x800C7530`) validates that both the input and output
request lengths are at least 24 bytes. The requested transfer length is
calculated from the output length minus 20 bytes and may not exceed 32 bytes;
the kernel uses a static transfer buffer.

The request data supplies the key format, AGID, and (for format 4) an address.
For format 4, the address is shifted right by 11 before it is placed in the
packet. The routine builds CDB opcode `0xA4`, submits it, and uses a
key-specific completion handler. The exact CDB byte offsets should be checked
against the kernel's packet byte order before implementing a standalone
structure.

### SEND KEY (`0x335008`)

`SataCdRomStartSendKey` (`0x800C7620`) requires at least 24 bytes of input. It
derives the payload length from the supplied input length minus 24 and rejects
payloads larger than 32 bytes. The payload begins at request offset `+24` and
is copied into the kernel's static transfer buffer.

The request supplies key format and AGID; the handler builds CDB opcode
`0xA3` and submits the payload. The end-session path uses the same opcode with
format `0x3F` and no data phase.

### READ DVD STRUCTURE (`0x335142`)

`SataCdRomStartReadStructure` (`0x800C7788`) requires an even transfer length
not greater than 65,535 bytes. The minimum is 21 bytes when the structure
format is zero and 4 bytes for other formats.

The request supplies an address, layer, format, and allocation length. The
address is shifted right by 11 when encoded into the CDB. Format 2 also
incorporates AGID bits. If the format byte's high bit is set, the handler sets
the associated CDB flag bits. The data buffer is passed to the packet transport,
and completion uses the generic overrun-aware handler.

## SCSI pass-through request

`SataCdRomStartScsiPassThrough` (`0x800C78C8`) validates a request header whose
declared size is 36 bytes. The observed fields are:

| Offset | Observed use |
|---:|---|
| `+0` | Header size; must be 36. |
| `+7` | Direction-related flag used to select packet transfer direction. |
| `+8` | Data transfer length; must be even and no greater than `0x20000`. |
| `+16` | Data-buffer address; must be even when a transfer is requested. |
| `+20` | Start of caller-supplied CDB. |

The outer device-control request must provide at least a 36-byte header.
Before issuing a non-empty transfer, the kernel locks the pages for the
caller-supplied data buffer. The CDB length is part of the request header; the
kernel passes the CDB beginning at `+20` to `SataCdRomIssueAtapiRequest`.

The handler contains a special check for pending SSC state and CDB lengths 8
or 40. Do not infer from that special case that those are the only accepted
CDB lengths; validate the exact header semantics for the target kernel before
using pass-through in the player.

## Sector reads

The C kernel build exposes two distinct paths:

- Normal IRP reads go through `SataCdRomRead` (`0x800BEB10`) and
  `SataCdRomStartRead` (`0x800C3710`). The read request carries a byte offset,
  byte length, and destination buffer. The driver converts the byte offset to
  an LBA by shifting right 11 (2048-byte sectors), then issues READ(10)
  (`0x28`) packets. For ordinary reads, offset, length, and destination-buffer
  address must be 2048-byte aligned, and the requested range must stay within
  `SataCdRomPartitionLength`. Misaligned requests are rejected. The packet
  transfer is capped at `0x20000` bytes and the driver may split/cache reads.
- Raw-read control `0x2403E` reaches `SataCdRomStartRawRead` (`0x800C2BE0`).
  Its 16-byte input contains an address, sector count, and read mode. The
  address is converted to a 2048-byte LBA, but each returned raw sector is
  2352 bytes. The count is limited to 56 sectors (`0x38`), so the transfer is
  at most `56 * 2352 = 131,712` bytes, below the general `0x20000` packet
  limit. The request must also fit within the DVD partition and have an
  acceptable output-buffer address. The mode selects the READ CD sector
  selection bits; this is not the ordinary 2048-byte filesystem-read API.

For a first physical-drive probe, the normal 2048-byte-aligned IRP read path is
the simpler candidate. Start with a known in-range sector and one sector, use
a suitably aligned buffer, and validate the actual completion status and byte
count on-console. The raw-read control has mode-specific packet fields and
should be treated as a separate experiment.

## XAM behavior

XAM's `CDvdDrive` code references the DVD device object by name and calls
`IoSynchronousDeviceIoControlRequest`. The relevant observed calls are:

| XAM operation | Control | Buffers |
|---|---:|---|
| `Reauthenticate` | `0x24094` | No input or output buffer. |
| `AuthenticateEx` | `0x240A0` | No input; 4-byte output status. |
| `FirmwareChallengeResponse` | `0x240D0` | 40-byte input. |
| `SetUninterruptableReads` | `0x240C4` | 4-byte input. |
| `SetSpindleSpeedImmediate` | `0x24084` | 4-byte input. |
| `SetXGD2AuthenticationDisabled` | `0x240BC` | 4-byte input; followed by other drive controls. |

`AuthenticateEx` checks a protected flag before issuing the request and maps
the returned 4-byte status into XAM's authentication result. These XAM controls
are not the `REPORT KEY`/`SEND KEY` packet sequence. XAM's media scanning also
uses ordinary filesystem operations on `\Device\CdRom0`; that does not establish
the raw packet request ABI.

## Implementation implications

- Keep the Xbox drive transport behind the DVD input/device boundary; do not
  use desktop Win32 DVD ioctl behavior as the Xbox backend.
- The DVD-specific controls are the most direct known route for their matching
  operations. Use pass-through only when needed and after verifying its full
  request layout and permissions on the target kernel.
- Preserve even transfer lengths and buffer alignment constraints. Check
  status, actual transfer length, and sense/error behavior rather than treating
  a successful request submission as a successful read.
- The findings establish request construction in this kernel build, not
  successful behavior on a physical drive. Begin console validation with
  harmless/read-only requests and aligned sector reads.
- CSS protocol logic remains the responsibility of libdvdcss. This note records
  the platform transport interface, not CSS keys or authentication policy.
- Kernel addresses and observed behavior may differ across kernel revisions.

## Analysis locations

Kernel functions:

- `SataCdRomRead` — C kernel `0x800BEB10`
- `SataCdRomStartRead` — C kernel `0x800C3710`
- `SataCdRomStartRawRead` — C kernel `0x800C2BE0`
- `SataCdRomDispatchIo` — C kernel `0x800C4990`
- `SataCdRomStartScsiPassThrough` — C kernel `0x800C34D8`
- `SataCdRomStartReadKey` — C kernel `0x800C3140`
- `SataCdRomStartSendKey` — C kernel `0x800C3230`
- `SataCdRomStartEndSession` — C kernel `0x800C3308`
- `SataCdRomStartReadStructure` — C kernel `0x800C3398`
- `SataCdRomDeviceControl` — D kernel `0x800C85B8`
- `SataCdRomDispatchIo` — D kernel `0x800C89F0`
- `SataCdRomStartRawRead` — D kernel `0x800C67D0`
- `SataCdRomStartReadKey` — D kernel `0x800C6D60`
- `SataCdRomStartSendKey` — D kernel `0x800C6E50`
- `SataCdRomStartReadStructure` — D kernel `0x800C6FB8`
- `SataCdRomStartScsiPassThrough` — D kernel `0x800C70F8`
- `SataCdRomStartRead` — D kernel `0x800C7350`
- `SataCdRomDeviceControl` — T kernel `0x800C3E18`
- `SataCdRomDispatchIo` — T kernel `0x800C4250`
- `SataCdRomStartRawRead` — T kernel `0x800C24A0`
- `SataCdRomStartReadKey` — T kernel `0x800C2A00`
- `SataCdRomStartSendKey` — T kernel `0x800C2AF0`
- `SataCdRomStartReadStructure` — T kernel `0x800C2C58`
- `SataCdRomStartScsiPassThrough` — T kernel `0x800C2D98`
- `SataCdRomStartRead` — T kernel `0x800C2FD0`
- `SataCdRomDispatchIo` — `0x800C91C0`
- `SataCdRomStartReadKey` — `0x800C7530`
- `SataCdRomStartSendKey` — `0x800C7620`
- `SataCdRomStartEndSession` — `0x800C76F8`
- `SataCdRomStartReadStructure` — `0x800C7788`
- `SataCdRomStartScsiPassThrough` — `0x800C78C8`
- `SataCdRomIssueAtapiRequest` — `0x800C2F70`

XAM functions:

- `CDvdDrive::Reauthenticate` — `0x8176D150`
- `CDvdDrive::AuthenticateEx` — `0x8176D210`
- `CDvdDrive::FirmwareChallengeResponse` — `0x8176D3C8`
- `CDvdDrive::SetXGD2AuthenticationDisabled` — `0x8176DD90`
