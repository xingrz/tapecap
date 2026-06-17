# tapecap

**Raw DV / HDV tape capture over FireWire for macOS.**

`tapecap` pulls the bitstream a DV or HDV camcorder/deck sends over FireWire
(IEEE 1394) and writes it to disk **exactly as it comes off the tape** —
nothing demuxed, nothing re-encoded:

| Tape format | Bus protocol | What tapecap writes |
|---|---|---|
| **DV** (incl. DVCAM / Digital8) | IEC 61883-2 | raw **DIF** byte stream (`.dv`) |
| **HDV** | IEC 61883-4 | raw **MPEG-2 Transport Stream**, 188-byte TS packets (`.m2t` / `.ts`) — video **and audio** (MPEG-1 Layer II) **and** all PSI/metadata |

## Why this exists

On macOS you can already capture **SD DV** through AVFoundation (e.g. with
ffmpeg's `avfoundation` input). But for **HDV**, AVFoundation deliberately
demuxes the incoming MPEG-2 Transport Stream and only hands back the MPEG-2
**video** elementary stream — so you lose the **audio** and the transport-stream
**metadata**. The leading DV archival tool, MediaArea/MIPoPS *dvrescue*, hits the
same wall: on macOS it captures via AVFoundation and is DV-oriented.

The data is all there on the wire, though — Adobe Premiere Pro captures full HDV
on the same machines. The trick is to **bypass AVFoundation** and read the raw
isochronous IEC 61883 stream straight from IOKit's FireWire stack. That is
exactly what Apple's own **AVCVideoServices** framework (from the now-retired
FireWire SDK) does, via its `MPEG2Receiver` (HDV) and `DVReceiver` (DV) classes.
`tapecap` is a small command-line front end over that framework, rebuilt as a
modern universal binary.

See [`docs/BACKGROUND.md`](docs/BACKGROUND.md) for the gory details.

## Features

- **`list` / `info` / `capture`** — enumerate FireWire AV/C devices, inspect a
  deck's capabilities and current mode/timecode, and capture the raw stream.
- **DV / HDV auto-detect** — picks the format from the deck's output-plug signal
  format; override with `--format dv|hdv`.
- **Automatic transport control** — sends AV/C **PLAY** on start and **STOP** at
  the end; also stops on `--duration`, Ctrl-C, or end of tape (`--no-control`
  lets you drive the deck yourself).
- **Live status while capturing** — tape SMPTE timecode, recording date/time and
  size, parsed straight from the stream (DV VAUX/subcode packs and the Sony HDV
  MPEG-TS AUX stream).
- **Auto-naming** — with no output path, files are named from the recording's
  own date/time (e.g. `20101029-140926.m2t`); pass `-` to stream to stdout.
- **Universal binary** — one arm64 + x86_64 build for macOS 11–15.

## Requirements

- **macOS 11 – 15.** macOS 15 *Sequoia* is the **last** release that ships the
  `IOFireWireFamily` driver and the FireWire SDK headers; FireWire was removed in
  macOS 26 *Tahoe*, so `tapecap` cannot work there. Build and run on macOS ≤ 15.
- **Apple Silicon and Intel** are both supported (universal binary).
  - Apple Silicon and recent Intel Macs have no FireWire port: use the
    **Apple Thunderbolt-to-FireWire adapter** (plus a Thunderbolt-3→2 adapter on
    USB-C/TB3/TB4 Macs). This is the same setup Premiere/dvrescue users rely on.
- A DV/HDV camcorder or deck with a FireWire (i.LINK / DV) port.

## Install

### Download a build

Each push produces a universal binary via GitHub Actions — grab the
`tapecap-macos-15` artifact from the **Actions** tab, or download a release
tarball. Then:

```sh
tar -xzf tapecap-macos-universal.tar.gz
xattr -dr com.apple.quarantine tapecap   # clear the "downloaded" quarantine
./tapecap --help
```

### Build from source

Needs the Xcode command-line tools on macOS ≤ 15:

```sh
git clone https://github.com/xingrz/tapecap
cd tapecap
make            # -> build/tapecap (universal arm64 + x86_64)
make install    # optional, installs to /usr/local/bin
```

## Usage

```
tapecap list                       # list connected FireWire AV/C devices
tapecap info    [--guid <hex>]     # show device capabilities, mode, timecode
tapecap capture [options] [output] # capture raw stream (omit output to auto-name)
```

### Capture options

| Option | Meaning |
|---|---|
| `--guid <hex>` | Pick a device by its 64-bit GUID (default: first DV/HDV device) |
| `--format auto\|dv\|hdv` | Force the stream format (default: auto-detect from the deck) |
| `--duration <sec>` | Stop after N seconds (default: until Ctrl-C / end of tape) |
| `--eot-timeout <ms>` | Auto-stop after this much silence; `0` disables (default: 5000) |
| `--no-control` | Don't send AV/C PLAY/STOP — you press play on the deck yourself |
| `-v`, `--verbose` | Also print the framework's internal log on stderr |
| `[output]` | File to write. **Omit** to auto-name from the recording's date/time; use `-` for stdout. |

While capturing, a **live status line** (tape SMPTE timecode, recording
date/time and size) updates in place on stderr — no flag needed. The timecode
and recording date/time are read straight from the stream (DV VAUX/subcode packs
and the Sony HDV MPEG-TS AUX stream).

### Examples

```sh
# Auto-detect DV vs HDV, roll the tape, stop at end of tape, and save as
# e.g. 20101029-140926.m2t (from the recording's own date/time):
tapecap capture

# Force DV, capture a fixed 60 seconds to a named file:
tapecap capture --format dv --duration 60 clip.dv

# Don't drive the transport; pipe a live HDV stream straight into ffmpeg:
tapecap capture --no-control - | ffmpeg -i - -c copy out.mkv
```

By default `tapecap` issues an AV/C **PLAY** when capture starts and **STOP**
when it ends, so you can leave the deck alone. Use `--no-control` if you prefer
to drive playback yourself (or the deck ignores AV/C transport commands).

## Permissions

macOS may gate FireWire AV/C access behind the **camera/recording** privacy
permission (the device shows up as a muxed A/V capture device). If a capture
fails to open the device:

- Run it once from **Terminal** and allow the prompt, or grant your terminal
  app access under **System Settings → Privacy & Security → Camera**.
- Make sure no other app (iMovie, Final Cut, Premiere, dvrescue, QuickTime) is
  holding the device open.

## What you get out, and post-processing

- **DV** → a raw DIF stream. Players/ffmpeg read it directly (`ffmpeg -i reel.dv …`).
- **HDV** → a raw MPEG-2 Transport Stream. Remux losslessly, e.g.
  `ffmpeg -i reel.m2t -c copy reel.mkv`, or demux audio/video as needed. The
  audio that AVFoundation drops is present here.

## Status & limitations

- **Verified on real hardware.** HDV capture (full video + audio + metadata)
  confirmed against a **Sony HDR-HC9** on both a **MacBook Pro 2018 (Intel,
  macOS 12.6)** and an **M1 Max MacBook Pro (Apple Silicon, macOS 15)** — the
  latter through an Apple Thunderbolt-to-FireWire adapter. CI additionally
  builds and smoke-tests the universal binary on macOS 14/15.
- This tool is built around Apple's proven AVCVideoServices code and relies on
  **deprecated-but-present** IOKit FireWire isochronous APIs. They work through
  macOS 15; there is no path on macOS 26+.
- Stream format (DV vs HDV) is auto-detected from the device's output-plug
  signal format; if a particular deck misreports it, force it with
  `--format hdv` / `--format dv`. `tapecap info` shows what was detected.
- Only the device's **output plug 0** is used (the normal tape-playback plug).

## Credits & license

- `tapecap`'s own code is **MIT** licensed — see [`LICENSE`](LICENSE).
- Bundles Apple's **AVCVideoServices** sample code under
  [`third_party/AVCVideoServices/`](third_party/AVCVideoServices/) (Apple
  sample-code license; provenance and details in its
  [`NOTICE.md`](third_party/AVCVideoServices/NOTICE.md)).
- The DV/HDV recording-date and timecode parser (`src/dvmeta.*`) is ported from
  [xingrz/iina-dv-timecode](https://github.com/xingrz/iina-dv-timecode).
- Thanks to the IEEE 1394 / DV archival community — *dvgrab*, *libiec61883*, and
  MediaArea/MIPoPS *dvrescue* — for keeping this knowledge alive.
