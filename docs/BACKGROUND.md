# Background: how DV/HDV travel over FireWire, and why HDV audio goes missing

This is the technical investigation behind `tapecap`.

## DV and HDV on the FireWire bus

A DV/HDV camcorder streams video over FireWire as an **isochronous** stream
using the **IEC 61883** family of protocols. Each isochronous packet carries a
CIP (Common Isochronous Packet) header followed by payload:

- **DV** uses **IEC 61883-2**. The payload, reassembled, is the **DIF** (Digital
  Interface Format) blocks — i.e. the raw DV stream you'd get in a `.dv` file.
  Audio is interleaved inside the DV frames themselves.
- **HDV** uses **IEC 61883-4**, the carriage of an **MPEG-2 Transport Stream**.
  The reassembled payload is a stream of 188-byte TS packets. That TS multiplexes
  the MPEG-2 **video**, the **MPEG-1 Layer II audio**, and PSI tables
  (PAT/PMT/PCR) — everything that defines the program.

Transport (play/stop/rewind) and device identification use **AV/C** (Audio/Video
Control) command-and-control packets — the *asynchronous* side of the bus —
addressed to the device's **tape subunit**.

## Why AVFoundation gives you HDV video but not audio

On macOS the HDV camera enumerates as an `AVMediaTypeMuxed` capture device. But
AVFoundation's HDV support surfaces the program as a movie with an `hdv`
(MPEG-2) **video** track: internally it demuxes the incoming Transport Stream and
exposes the video elementary stream, dropping the separate MPEG-1 Layer II
**audio** track and the transport-stream structure. So a tool built on
AVFoundation (ffmpeg's `avfoundation` input, or MediaArea/MIPoPS *dvrescue*) ends
up with HDV **video only**.

For **DV** this is not a problem, because DV audio lives *inside* the DV frames —
AVFoundation hands back the whole DIF frame (`CMSampleBufferGetDataBuffer`
returns the raw DV data), so the audio rides along for free. HDV is different
precisely because its audio is a *separate* stream inside the TS.

The bytes are not lost on the wire — Premiere Pro captures full HDV (video +
audio) on the same Macs. It just doesn't come through the AVFoundation movie
path.

## The fix: read the raw isochronous stream from IOKit

Below AVFoundation, macOS exposes the FireWire bus through IOKit:

- `IOKit/firewire/IOFireWireLib.h` + `IOFireWireLibIsoch.h` — user-space
  isochronous receive (DCL programs that DMA incoming isoch packets into buffers).
- `IOKit/avc/IOFireWireAVCLib.h` — sending AV/C commands to the device.

Apple shipped a framework that wraps all of this — **AVCVideoServices**, in the
FireWire SDK — with ready-made receivers:

- **`MPEG2Receiver`** strips the IEC 61883-4 CIP headers and reassembles the
  **188-byte MPEG-2 TS packets** → exactly the raw HDV stream we want.
- **`DVReceiver`** reassembles IEC 61883-2 into **DV frames** (DIF) → the raw DV
  stream we want.
- **`AVCDevice` / `AVCDeviceController`** discover devices, open them, run the
  **Connection Management Procedure** (read the device's output plug, allocate an
  isoch channel + bus bandwidth, establish the point-to-point connection), and
  send **AV/C** commands.

`tapecap` uses the managed `AVCDevice` API:
`CreateMPEGReceiverForDevicePlug()` / `CreateDVReceiverForDevicePlug()` set up
the connection on output plug 0 and deliver data through a callback that simply
writes the bytes out. Transport is driven with raw AV/C `PLAY` (opcode `0xC3`,
operand `0x75`) and `WIND/STOP` (opcode `0xC4`, operand `0x60`) to the tape
subunit.

## DV-vs-HDV auto-detection

`AVCDevice` reads the tape subunit's **OUTPUT SIGNAL MODE** (AV/C opcode `0x78`)
during capability discovery and maps it to a format:

- HDV1 (`0x10`/`0x90`), HDV2 (`0x1A`/`0x9A`), and other MPEG modes → MPEG/HDV.
- SD/SDL/DVCPro modes (`0x00`, `0x80`, …) → DV.

So with the deck in Play/VTR mode, `tapecap` can pick DV vs HDV by itself; you
can always override with `--format`.

## Why macOS 15 is the ceiling

The whole approach depends on the `IOFireWireFamily` kernel driver and the
`IOKit/firewire` + `IOKit/avc` SDK headers. macOS 15 *Sequoia* is the last
release that includes them; macOS 26 *Tahoe* removed FireWire support entirely.
On Apple Silicon and modern Intel Macs (no built-in FireWire), an Apple
Thunderbolt-to-FireWire adapter bridges the bus, and the driver treats it like
any other FireWire interface.

## References

- Apple FireWire SDK — *AVCVideoServices* framework source (vendored here).
- IEC 61883-1/-2/-4 — FireWire AV data transmission; DV and MPEG-2 TS carriage.
- AV/C Digital Interface Command Set — General + Tape Recorder/Player subunit.
- *dvgrab* / *libiec61883* — the Linux equivalents (raw1394) and a reference for
  DV/HDV frame structure.
- MediaArea/MIPoPS *dvrescue* — modern DV archival tooling; its macOS capture
  path (`AvfCtl.m`) illustrates the AVFoundation route and its HDV limitation.
