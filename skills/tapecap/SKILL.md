---
name: tapecap
description: Capture raw DV / HDV tape over FireWire on macOS with the `tapecap` CLI. Use when the user wants to archive or digitize DV, DVCAM, Digital8 or HDV camcorder/deck tapes on a Mac (macOS 11–15) — especially HDV, where AVFoundation-based tools (ffmpeg, dvrescue, iMovie) silently drop the audio and transport-stream metadata. Covers enumerating FireWire AV/C devices, inspecting a deck, capturing the untouched bitstream, and losslessly post-processing the result.
---

# tapecap — raw DV / HDV tape capture over FireWire

`tapecap` reads the bitstream a DV or HDV camcorder/deck sends over FireWire
(IEEE 1394) and writes it to disk **exactly as it comes off the tape** — nothing
demuxed, nothing re-encoded.

| Tape format | What you get | File |
|---|---|---|
| **DV** (incl. DVCAM / Digital8) | raw **DIF** byte stream | `.dv` |
| **HDV** | raw **MPEG-2 Transport Stream** (188-byte TS packets): video **and** audio (MPEG-1 Layer II) **and** all PSI/metadata | `.m2t` / `.ts` |

## When to reach for this (vs. ffmpeg / dvrescue)

For **SD DV**, AVFoundation tools work fine — DV audio rides inside the DIF
frames, so nothing is lost. The reason `tapecap` exists is **HDV**: on macOS,
AVFoundation deliberately demuxes the incoming MPEG-2 Transport Stream and hands
back only the MPEG-2 *video* elementary stream, **dropping the audio and the
transport metadata**. ffmpeg's `avfoundation` input and MediaArea/MIPoPS
*dvrescue* both hit this wall. `tapecap` bypasses AVFoundation and reads the raw
isochronous IEC 61883 stream straight from IOKit's FireWire stack, so the audio
and metadata survive. If the user mentions losing HDV audio, that's the tell.

## Hard requirements — check these first

- **macOS 11–15 only.** macOS 15 (Sequoia) is the **last** release that ships the
  FireWire driver; macOS 26 (Tahoe) removed FireWire entirely, so `tapecap`
  cannot run there. If the user is on macOS 26+, there is no path — say so.
- **Apple Silicon and recent Intel Macs have no FireWire port.** They need an
  **Apple Thunderbolt-to-FireWire adapter** (plus a Thunderbolt-3→2 adapter on
  USB-C/TB3/TB4 Macs). This is the same rig Premiere/dvrescue users use.
- A DV/HDV camcorder or deck with a FireWire (i.LINK / DV) port, connected and
  powered on, in **VTR/VCR (playback)** mode — not camera mode.

## Install

```sh
brew install xingrz/tap/tapecap     # prebuilt universal binary, no quarantine step
```

Or build from source on macOS ≤ 15 with the Xcode command-line tools:

```sh
git clone https://github.com/xingrz/tapecap && cd tapecap
make            # -> build/tapecap (universal arm64 + x86_64)
make install    # optional, -> /usr/local/bin
```

## The three commands

```
tapecap list                       # enumerate connected FireWire AV/C devices
tapecap info    [--guid <hex>]     # show a deck's capabilities, mode, timecode
tapecap capture [options] [output] # capture the raw stream (omit output to auto-name)
tapecap cue     [--guid <hex>] [--overlap <sec>] <timecode>  # fast-wind to a timecode
```

### Recommended workflow

1. **`tapecap list`** — confirm the deck is on the bus. If nothing shows up, it's
   a connection/power/mode problem (see Troubleshooting), not a `tapecap` bug.
2. **`tapecap info`** — verify the detected format (DV vs HDV), current transport
   mode and tape timecode look right. This is also how you confirm auto-detect
   picked the right format before committing to a long capture.
3. **`tapecap capture`** — roll the tape. With no output path it auto-names the
   file from the recording's own date/time (e.g. `20101029-140926.m2t`).

By default `tapecap` sends an AV/C **PLAY** when capture starts and **STOP** when
it ends, so the user can leave the deck alone. It also stops on `--duration`,
Ctrl-C, or end of tape.

### Capture options

| Option | Meaning |
|---|---|
| `--guid <hex>` | Pick a device by its 64-bit GUID (default: first DV/HDV device). Use when several devices are listed. |
| `--format auto\|dv\|hdv` | Force the stream format. Default `auto` detects from the deck's output-plug signal format. Override only if a deck misreports (`tapecap info` shows what was detected). |
| `--duration <sec>` | Stop after N seconds (default: until Ctrl-C / end of tape). |
| `--eot-timeout <ms>` | Auto-stop after this much stream silence; `0` disables (default: 5000; `--seek` uses at least 15000 for stream startup). |
| `--seek <timecode>` | Fast-wind to this tape timecode before capturing (targeted re-capture). Needs AV/C control. |
| `--until <timecode>` | Stop once the captured stream's timecode passes this point. |
| `--overlap <sec>` | Pre-/post-roll kept around `--seek` / `--until` (default: 4) so re-capture windows overlap. |
| `--no-control` | Don't send AV/C PLAY/STOP — the user presses play on the deck themselves. Use for decks that ignore AV/C transport commands, or to capture mid-tape. |
| `-v`, `--verbose` | Also print the framework's internal log to stderr. |
| `[output]` | File to write. **Omit** to auto-name from the recording's date/time; use `-` to stream to stdout. |

### Live status

While capturing, a status line updates in place on **stderr** (no flag needed):
tape SMPTE timecode, recording date/time, bytes written, a real coded-frame
count, and a **continuity-error** count. The error count is the tape/transport
damage signal — it is **detect-and-report only and never interrupts the
capture**, so even a damaged tape yields the most complete raw dump possible. For
HDV, `tapecap` also prints once which elementary streams are present (video,
audio, timecode AUX) plus the video geometry / frame rate / bit rate — the audio
and AUX lines are the proof the data AVFoundation drops made it into the `.m2t`.

### Examples

```sh
# Auto-detect DV vs HDV, roll the tape, stop at end of tape, auto-name the file:
tapecap capture

# Force DV, capture a fixed 60 seconds to a named file:
tapecap capture --format dv --duration 60 clip.dv

# Drive the deck yourself and pipe a live HDV stream straight into ffmpeg:
tapecap capture --no-control - | ffmpeg -i - -c copy out.mkv

# Pick a specific deck by GUID:
tapecap capture --guid 0x0800460102345678

# Re-capture just one damaged section by tape timecode (e.g. a tapeflow gap):
tapecap capture --seek 00:12:30 --until 00:14:00 gap.m2t

# Position only — fast-wind to 30:00 and stop, then capture however you like:
tapecap cue 00:30:00
```

## Targeted re-capture / orchestration (e.g. with tapeflow)

[tapeflow](https://github.com/xingrz/tapeflow) merges several capture passes of a
worn tape and reports the **remaining gaps, each labelled with a timecode**. To
re-capture one gap without replaying the whole tape, use `--seek <tc>` (fast-wind
to the start) and `--until <tc>` (stop after the end) — `tapecap` drives the deck
with AV/C fast-forward/rewind. Use this when the user wants to automate or
speed up filling tapeflow's gaps.

Key things to get right when driving this:

- **A `<timecode>` is `HH:MM:SS`** (also `HH:MM:SS:FF` with frames ignored,
  `MM:SS`, or a bare number of seconds).
- **Seeking is coarse, on purpose.** Decks coast past a stop, and aged tapes (the
  reason for re-capturing) drop their timecode mid-travel, so the landing point is
  approximate. `--overlap <sec>` (default 4) keeps extra footage on both sides so
  each re-capture **overlaps** the neighbouring good material — exactly what the
  merge step needs. Don't try to make it frame-accurate; lean on overlap instead.
- **`--seek`/`--until`/`cue` need AV/C control**, so they can't be combined with
  `--no-control`.
- **`cue <tc>` positions only** (no capture) — for an orchestrator that prefers to
  cue the deck, then run `capture --no-control` itself.
- The capture output is still the untouched raw stream; keep treating each
  re-captured segment as an archival fragment to hand back to tapeflow.

## What you get out, and post-processing

The output is the **untouched** stream — keep the original as the archival master.

- **DV** → a raw DIF stream; players and ffmpeg read it directly:
  `ffmpeg -i reel.dv …`
- **HDV** → a raw MPEG-2 Transport Stream. Remux losslessly with
  `ffmpeg -i reel.m2t -c copy reel.mkv`, or demux audio/video as needed. The
  audio that AVFoundation would have dropped is present here.

When suggesting post-processing, prefer **`-c copy`** (stream copy / remux) so the
archival capture stays lossless; only re-encode if the user explicitly wants a
smaller or more compatible derivative, and keep it separate from the master.

## Troubleshooting

- **`list` shows nothing / `capture` can't find a device.** Check the FireWire
  cable and adapter chain, power on the deck, and put it in **VTR/VCR playback**
  mode. On Apple Silicon / modern Intel, confirm the Thunderbolt-to-FireWire
  adapter chain is seated.
- **"failed to open device" / permission errors.** macOS may gate FireWire AV/C
  access behind the **Camera/recording** privacy permission (the deck appears as
  a muxed A/V capture device). Run once from **Terminal** and allow the prompt,
  or grant the terminal app access under **System Settings → Privacy & Security →
  Camera**. Also make sure no other app (iMovie, Final Cut, Premiere, dvrescue,
  QuickTime) is holding the device open.
- **Wrong format detected.** Some HDV camcorders (e.g. Sony HDR-HC9) report DV via
  the tape subunit even while streaming HDV; `tapecap` works around this by
  reading the output-plug (oPCR) signal format. If a deck still misreports, force
  it with `--format hdv` / `--format dv`; `tapecap info` shows the detected value.
- **No live timecode on a non-Sony HDV deck.** Only Sony's HDV AUX timecode path
  is parsed; the JVC/Canon GOP-user-data fallback isn't. This affects the live
  *display only* — the raw capture is complete and unaffected.
- **Capture stops too early.** A quiet/blank section can trip the end-of-tape
  silence timeout. Raise `--eot-timeout` or set it to `0` to disable, and/or use
  `--duration` to bound the capture instead.

## Notes & limits

- Only the device's **output plug 0** (the normal tape-playback plug) is used.
- Relies on deprecated-but-present IOKit FireWire isochronous APIs that work
  through macOS 15 only.
- Verified on real hardware (Sony HDR-HC9) on Intel macOS 12.6 and Apple Silicon
  M1 Max macOS 15.

See the project README and `docs/BACKGROUND.md` for the full technical write-up.
