# AGENTS.md — briefing for AI sessions working on tapecap

This file is context for future AI agents (and humans). Read it before changing
anything. It captures the *why* and the non-obvious gotchas that aren't visible
from the code alone.

## What this project is

`tapecap` is a macOS command-line tool that captures the **raw** bitstream a
DV or HDV camcorder/deck sends over FireWire (IEEE 1394) and writes it to disk
untouched:

- **DV** (IEC 61883-2) → raw DIF stream (`.dv`)
- **HDV** (IEC 61883-4) → raw MPEG-2 Transport Stream (`.m2t`), including the
  MPEG-1 Layer II audio and PSI/metadata.

### The core insight (why the project exists)

On macOS, **AVFoundation deliberately demuxes HDV** and only exposes the MPEG-2
*video* elementary stream, dropping the **audio** and the transport-stream
metadata. (DV is fine through AVFoundation because DV audio rides inside the DIF
frames.) The data is all on the wire, though — Premiere captures full HDV.

The fix, and what this tool does: **bypass AVFoundation and read the raw
isochronous IEC 61883 stream straight from IOKit's FireWire stack**, using
Apple's **AVCVideoServices** framework (`MPEG2Receiver` for HDV, `DVReceiver`
for DV, `AVCDevice` for connection management + AV/C control). This is the same
layer Premiere uses. See `docs/BACKGROUND.md` for the full write-up.

## Repository layout

| Path | What it is | Edit it? |
|---|---|---|
| `third_party/AVCVideoServices/` | **Vendored Apple sample code** — the isochronous capture engine (~17.5k LOC). | Avoid. It's proven; treat as a black box. |
| `src/tapecap.cpp` | The CLI: `list` / `info` / `capture`, orchestration, AV/C commands, live status, auto-naming (~900 LOC). | Yes — this is "our" code. |
| `src/dvmeta.{h,cpp}` | DV/HDV recording-date + tape-timecode parser (~400 LOC). | Yes. |
| `Makefile` | Universal (arm64+x86_64) build. | Yes. |
| `.github/workflows/build.yml` | CI build + smoke test; release job. | Yes. |
| `docs/BACKGROUND.md` | Deep technical explanation. | Keep current. |
| `RELEASE_NOTES.md` | Used as the GitHub Release body (`body_path`). | Update before each release. |
| `third_party/.../NOTICE.md` | Provenance + license of the vendored code. | Keep accurate. |

### About the vendored AVCVideoServices

- Source: Apple **FireWire SDK 26**, `Examples/AVCVideoServices-42`, obtained
  from the Internet Archive (item `13572357-firewiresdk-26`); Apple no longer
  ships it. SHA + details in `third_party/AVCVideoServices/NOTICE.md`.
- License: Apple sample-code license — **redistribution allowed with the notice
  retained**. Every file keeps its original header.
- Only the **capture path** was vendored (22 `.cpp` + their headers). The
  non-vendored upstream modules (PanelSubunitController, MusicSubunitController,
  MPEGTrickModes, the `Virtual*` device-emulation classes, FWA_IORemapper, the
  flat `FWAVC` C API, and all `*Test`/sample `main()`s) are unreferenced by the
  capture path.
- **The only edit** to the vendored sources is in `AVCVideoServices.h`: the
  `#include`s for those non-vendored modules were removed, and one constant
  (`kMaxAutoPSIDetectProgramIndex`, normally in MPEGTrickModes.h) was hoisted in
  so the compiled-but-unused `MPEG2Transmitter.cpp` still builds. The transmitter
  compiles as dead code (the managed `AVCDevice` API references it) but tapecap
  never transmits.

## Hard platform constraints

- **macOS 11–15 only.** macOS 15 (Sequoia) is the **last** release that ships
  the FireWire driver (`IOFireWireFamily`) and the `IOKit/firewire` + `IOKit/avc`
  SDK headers. macOS 26 (Tahoe) removed FireWire entirely. There is no path
  forward; this is a frozen, end-of-life platform.
- **CI must pin `runs-on: macos-15`.** Do NOT use `macos-latest` — it now points
  at macOS 26, whose SDK lacks the FireWire headers, so the build fails there.
- **Universal binary** (arm64 + x86_64). Apple Silicon has no FireWire port —
  users connect via an Apple Thunderbolt-to-FireWire adapter. (The 16 KB page
  size on Apple Silicon was a worried-about risk that never materialized;
  capture works on M1 Max + macOS 15.)
- Relies on **deprecated-but-present** IOKit FireWire isochronous APIs (DCL
  programs). They work through macOS 15.

## Behaviour & design decisions you must not regress

These were learned from real hardware (a **Sony HDR-HC9**); don't "simplify"
them away:

1. **Format auto-detect uses the output-plug (oPCR) signal format, not the tape
   signal-mode.** Some HDV camcorders (the HC9) report **DV** via the tape
   subunit's OUTPUT SIGNAL MODE status *even while streaming HDV*. So
   `DetectFormat()` queries the oPCR (AV/C OUTPUT PLUG SIGNAL FORMAT, opcode
   `0x19`): top byte `0xA0` = HDV/MPEG-TS, `0x80` = DV. Falls back to the
   capability flags, then to a play-then-recheck. `--format dv|hdv` overrides.
2. **Device auto-selection is lenient.** It returns the first enumerated
   tape/DV/HDV device (else the first device), *without* gating on `isAttached`
   or the async capability flags — otherwise `capture` fails to find a device
   that `list` happily shows (capability discovery lags enumeration).
3. **DV receiver needs a concrete DV mode.** `dev->dvMode == 0xFF` (unknown) is
   replaced with NTSC SD (`0x00`) so the receiver doesn't reject it.
4. **Transport control** (AV/C to the tape subunit, addr `IOAVCAddress(kAVCTapeRecorder,0)`):
   PLAY = opcode `0xC3` operand `0x75`; STOP = WIND opcode `0xC4` operand `0x60`.
5. Only **output plug 0** is used.
6. **Metadata parsing is best-effort and must never affect the raw bytes.** If
   `dvmeta` has a bug, capture still writes a correct file.

### dvmeta specifics

Ported from **https://github.com/xingrz/iina-dv-timecode** (see that repo for the
authoritative pack-layout docs). It parses:

- **DV (DIF):** VAUX rec-date (`0x62`) / rec-time (`0x63`) packs + Subcode
  title-timecode (`0x13`).
- **Sony HDV (MPEG-TS):** a private AUX stream (PMT `stream_type` `0xA1`,
  fallback `0xA0`); its PES packets carry, anchored on a `0x63` byte:
  rec-date (`0xC0..0xC3` pack — low 2 bits of the pack ID are the timecode
  hours), wall-clock as BCD `SS MM HH` (**reversed** from DV's `HH MM SS`), and
  the tape timecode frames/sec/min. Cross-checked against mediainfo's
  `Encoded_Date`.
- **Not ported:** the JVC/Canon HDV fallback (timecode in MPEG-2 GOP user_data).
  Only Sony's AUX path is implemented. If a non-Sony HDV deck shows no live
  timecode, that's why — capture is unaffected.

## How to build / test / release

### Building

```sh
make            # -> build/tapecap (universal); needs Xcode CLT on macOS <= 15
make lipo       # verify both arches
```

Build notes for the vendored C++: no QuickTime/Carbon deps; uses deprecated
`IOMasterPort` (fine, just a warning); compiled with `-std=c++14` and warning
suppressions in the Makefile. The bundled `.xcode` project is ancient and
unusable — the Makefile is the build system. The Makefile globs
`third_party/AVCVideoServices/*.cpp` and `src/*.cpp`, so adding a `src/*.cpp`
file just works.

### Testing reality

- **CI only builds + smoke-tests** (`--version`, `--help`, `list`). The runners
  have **no FireWire bus**, so capture itself is never exercised in CI.
- **Real validation requires the user's hardware** (Sony HDR-HC9). Especially
  for `dvmeta` changes: ask the user for a sample capture (or the first ~1 MB of
  one) to regression-test the parser.

### Releasing

- A `v*` **tag push** triggers the `release` job (builds + publishes a GitHub
  Release with `tapecap-macos-universal.tar.gz`).
- OR trigger it manually: Actions → "build" → Run workflow → set the
  `release_tag` input (e.g. `v0.1.0`). This path also *creates* the tag — it was
  added precisely because some environments can't push tags (see below).
- Release notes come from `RELEASE_NOTES.md` (`body_path`). Edit that file first.

## Environment gotchas (if you're a remote/sandboxed agent, not on the user's Mac)

If you're running Claude Code on the user's Mac, ignore this — you can `make` and
test against the deck directly. If you're in a remote sandbox (no macOS, no
hardware), the prior sessions hit these:

- **You can't compile or hardware-test locally.** CI on `macos-15` is your
  compiler — push and read the run. The vendored code's SDK headers (`IOKit/firewire`,
  `IOKit/avc`) DO exist in the macos-15 runner's SDK.
- **The git proxy only accepts pushes to the active/default branch (`master`).**
  Pushing tags or other branches returns HTTP 403/503. Create branches/tags via
  the GitHub MCP tools, and cut releases via the `workflow_dispatch` path above.
- **`api.github.com` is blocked** from the sandbox (403). Use the GitHub MCP
  tools for GitHub state; use the local git proxy (`127.0.0.1`) for git.
- The repo is **public**; develop directly on `master`, no PR required (per the
  owner). Don't open PRs unless asked.

## Conventions

- Commit messages end with a co-author trailer naming the AI model, e.g.
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` — and do **not**
  include any session URL. (Use whatever model is running the session.)
- Keep `README.md`, `docs/BACKGROUND.md` and this file in sync when behaviour
  changes.

## Status

v0.1.0 is released and validated on real hardware (Sony HDR-HC9 on Intel
macOS 12.6 and Apple Silicon M1 Max macOS 15). The tool is feature-complete for
its purpose. The platform is frozen, so the bar for new work is "does it help
someone archive tapes on macOS ≤ 15", not "modernize".
