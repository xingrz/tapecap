# tapecap v0.1.0

First release. **Raw DV / HDV tape capture over FireWire for macOS** — it writes
the bitstream straight off the tape, untouched:

- **DV** (IEC 61883-2) → raw DIF stream (`.dv`)
- **HDV** (IEC 61883-4) → raw **MPEG-2 Transport Stream** (`.m2t`) — the video
  **and** the MPEG-1 Layer II audio **and** the PSI/metadata that AVFoundation
  silently drops when it demuxes HDV.

It bypasses AVFoundation and talks to IOKit's FireWire stack via Apple's
AVCVideoServices — the same layer Premiere uses to capture HDV.

## Highlights

- `list` / `info` / `capture` commands.
- **Auto-detects DV vs HDV** from the deck's output-plug signal format (override
  with `--format dv|hdv`).
- **Automatic transport control**: sends AV/C PLAY on start and STOP at the end;
  stops on `--duration`, Ctrl-C, or end of tape.
- **Live status while capturing**: tape SMPTE timecode + recording date/time +
  size, parsed straight from the stream (DV VAUX/subcode packs and the Sony HDV
  MPEG-TS AUX stream).
- **Auto-naming**: with no output path, the file is named from the recording's
  own date/time (e.g. `20101029-140926.m2t`); pass `-` to write to stdout.

## Requirements

- **macOS 11–15.** Sequoia (15) is the **last** macOS with the FireWire driver
  and SDK headers — macOS 26 removed FireWire. Universal binary (Apple Silicon +
  Intel); Apple Silicon needs a Thunderbolt-to-FireWire adapter.
- Verified on a **Sony HDR-HC9** on a MacBook Pro 2018 (Intel, macOS 12.6) and an
  M1 Max MacBook Pro (Apple Silicon, macOS 15).

## Install

```sh
tar -xzf tapecap-macos-universal.tar.gz
xattr -dr com.apple.quarantine tapecap
./tapecap --help
```

## Credits

Bundles Apple's AVCVideoServices sample code (Apple sample-code license). The
DV/HDV timecode + recording-date parser is ported from
[xingrz/iina-dv-timecode](https://github.com/xingrz/iina-dv-timecode). tapecap's
own code is MIT licensed.
