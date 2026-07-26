---
name: tapecap
description: >-
  Capture raw DV / HDV tape over FireWire on macOS with the `tapecap` CLI. Use when the user wants
  to archive or digitize DV, DVCAM, Digital8 or HDV camcorder/deck tapes on a Mac (macOS 11–15) —
  especially HDV, where AVFoundation-based tools (ffmpeg, dvrescue, iMovie) silently drop the audio
  and transport-stream metadata. Covers enumerating FireWire AV/C devices, inspecting a deck,
  cueing/jogging/winding tape position, exploring an unknown multi-event or mixed-format tape
  without a wasteful full capture, handling ordinary event overlap and exact HDV-reset splits,
  capturing the untouched bitstream, and losslessly post-processing the result. Also trigger on
  Chinese phrasings:
  采集磁带/采带、数字化 DV/HDV 磁带、倒带/进带/定位到时间码、探索磁带内容/多事件磁带分段、
  DV/HDV 混合磁带、补采某一段.
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

## The commands

```
tapecap list                       # enumerate connected FireWire AV/C devices
tapecap info    [--guid <hex>] [--json]  # show a deck's capabilities, mode, timecode
tapecap capture [options] [output] # capture the raw stream (omit output to auto-name)
tapecap cue     [--guid <hex>] [--overlap <sec>] [--json] <timecode>  # fast-wind to a timecode
tapecap jog     [--guid <hex>] [--json] <forward|back> <sec>          # short timed fast-wind
tapecap wind    [--guid <hex>] [--timeout <sec>] [--json] <start|end> # rewind to start / wind to end
```

### Exit codes

When orchestrating, branch on the exit code — the human-readable `error: …` line
is on stderr, but the code is the contract:

| Code | Meaning |
|---|---|
| `0` | Success. |
| `1` | System failure retrying won't cure — FireWire stack/driver unavailable, output file can't be opened, write error mid-capture. |
| `2` | The request couldn't proceed — bad arguments, no device on the bus, format detection failed, a `cue`/`--seek` that didn't land (capture never started), or a capture that ended with 0 bytes. Fix the arguments, deck or tape position, then retry. |

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

### Decide whether the tape has multiple events

Unless the user identifies multiple events or asks you to discover them, treat
the physical tape as one event and capture the full reel. Do not invent splits
from isolated metadata glitches.

If the contents are unknown, take short probes at widely spaced positions into
a discovery directory, recording date, timecode, and format. Bracket sustained
date/format changes with progressively closer probes. Never make a full capture
only to map the contents.

For an **ordinary same-format boundary**, capture each event directly. Use the
sustained recording-date change as the boundary, start roughly 3 seconds early,
and let capture continue roughly 3 seconds into the next event (`--overlap 3`
when it expresses the window). Deck precision is sufficient because this margin
is intentionally approximate; do not make a large capture merely to split it
later. Adjacent event files share about 6 seconds.

Fold a brief different-date insert (typically under a minute) into the
nearer/contextually related event unless the user says otherwise. Never discard
it.

Use these exceptions:

- **HDV timecode goes backwards or resets:** still capture across the boundary,
  but write the raw window to staging and give the final `.m2t` fragments no
  overlap. A deck cannot position to the reset frame, and `--until` cannot
  express the cut reliably. Follow the tapeflow skill to split at the indexed
  GOP byte offset. This applies to both a large original and a small
  re-capture. If damage/missing data hides the boundary, capture one window
  spanning both events, split it afterward, and send each side to its event
  directory. Preserve the raw source.
- **DV timecode drops out, goes to zero, or jumps:** ignore it for partitioning.
  LP/imperfect DV can oscillate between valid and zero continuously, and DV
  plays safely through it. Use sustained recording-date changes and the ordinary
  direct-capture overlap.
- **DV/HDV format change:** make separate explicitly formatted captures. To
  protect the start of the later-format event, start `--format dv` or
  `--format hdv` while the head is still in the earlier event; the receiver
  ignores the other format and writes immediately when its format appears.
  Use `--eot-timeout 0` (or longer than the entire lead-in), and remember
  `--duration` starts at PLAY rather than at the first written byte. Use a
  generous duration or supervise the stop so a missing target format cannot run
  indefinitely. Keep the resulting `.dv` and `.m2t` files in different event
  directories.

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

## Targeted re-capture / orchestration (e.g. with tapeflow)

[tapeflow](https://github.com/xingrz/tapeflow) merges several capture passes of a
worn tape and reports the **remaining gaps, each labelled with a timecode**. To
re-capture an ordinary gap inside one event, use `--seek <tc>` (fast-wind to the
start) and `--until <tc>` (stop after the end) — `tapecap` drives the deck with
AV/C fast-forward/rewind. Use this when the user wants to automate or speed up
filling tapeflow's gaps.

Key things to get right when driving this:

- **A `<timecode>` is `HH:MM:SS`** (also `HH:MM:SS:FF` with frames ignored,
  `MM:SS`, or a bare number of seconds).
- **`cue` / `--seek` need a readable current timecode anchor**, not just a target
  timecode. They work from recorded footage. If the deck is parked in the blank
  head/tail and `Position:` / `info --json` says `timecode_readable: false`, do
  **not** run `capture --seek` and do **not** fall back to normal capture from
  the current position. Use short probes like `tapecap jog --json forward 3`
  (or `jog back`) until a timecode is readable, then cue/seek.
- **DV supports cue/seek.** The position comes from the deck's AV/C TIME CODE
  status query, not from HDV-only AUX metadata. If DV cue fails, suspect blank
  tape, missing AV/C timecode, or transport-control failure — not the DV format.
- **Seeking is coarse, on purpose.** Decks coast past a stop, and aged tapes (the
  reason for re-capturing) drop their timecode mid-travel, so the landing point is
  approximate. `--overlap <sec>` (default 4) keeps extra footage on both sides so
  each re-capture **overlaps** the neighbouring good material — exactly what the
  merge step needs. Don't try to make it frame-accurate; lean on overlap instead.
- **A target at an HDV reset boundary is not an ordinary seek window.** Capture
  it wide into staging, then split it as described above before adding derived
  fragments to the affected tapeflow directories.
- **`--seek`/`--until`/`cue` need AV/C control**, so they can't be combined with
  `--no-control`.
- **`cue <tc>` positions only** (no capture) — for an orchestrator that prefers to
  cue the deck, then run `capture --no-control` itself. Mind the default: `cue`'s
  `--overlap` is **0** (land on the target), unlike `capture`'s 4 — pass
  `--overlap` explicitly when the follow-up capture needs pre-roll.
- **Movement commands return their final position.** `cue`, `jog`, and `wind`
  print `Position: 00:12:34` or `Position: --:--:-- (blank/no timecode)`. Pass
  `--json` to also get one stdout line such as
  `{"ok":true,"timecode_readable":true,"timecode":"00:12:34"}`. Prefer this over
  a separate `tapecap info` call when orchestrating.
- **`capture --seek` aborts if the initial seek fails.** Treat that as a hard
  stop and fix the deck position first; don't start a plain `capture` from the
  same no-timecode location.
- The capture output is still the untouched raw stream; keep treating each
  re-captured segment as an archival fragment to hand back to tapeflow.

## Winding to the blank head/tail (`wind`)

`cue`/`--seek` target a **timecode**, so they only work where the tape carries
timecode. Use `wind start|end` for the blank physical ends, and short
`jog forward|back <sec>` moves to step from blank tape until the final
`Position:` is readable. These commands only position; they never capture.
Decks that cannot report transport state require Ctrl-C or `--timeout <sec>`
(default 900).

## Full-reel capture, and leaving the tape as you found it

When archiving a whole tape from the top, mind these — they trip up agents:

- **Rewind first.** Start a full capture from the beginning with
  `tapecap wind start`, then `tapecap capture`.
- **The blank head trips the end-of-tape timeout.** A tape usually starts with a
  blank, signal-less leader. On a from-the-top capture that silence looks exactly
  like end-of-tape, so the default `--eot-timeout` (5000 ms) **fires immediately
  and the capture stops before any footage**. For a full-reel capture, **disable
  it with `--eot-timeout 0`** (and bound the run another way, e.g. `--duration`,
  Ctrl-C, or just let it run to the real end) or raise it well above the leader
  length. Don't report a 0-byte/instant capture as "end of tape" — suspect this.
- **After a full capture the deck sits in the blank tail.** It has run past the
  last footage into the blank end, where there's no timecode, so a follow-up
  `cue`/`--seek` has nothing to lock onto and won't position correctly. For
  another targeted capture, use short `jog back` probes until the final
  `Position:` reports a readable timecode, then cue/seek from that anchor. If you
  need a full rewind first, `wind start` returns to the physical start, which is
  usually another blank/no-timecode region; use short `jog forward` probes before
  cue/seek.
- **After the first pass, inspect tapeflow's report before any retry.** Live
  continuity/drop telemetry is not a repair plan. Do not automatically capture
  the whole reel a second time because the first pass showed many errors; add
  the file to its tapeflow working directory, run `tapeflow analyze`, and let
  the reported spans decide what to re-capture. Make another full pass only
  after that report shows it is justified (or the user explicitly requests it).
- **Finish by rewinding.** Once all captures are done and the user confirms
  there's nothing more to grab, **`tapecap wind start`** to return the tape to its
  original (rewound) state. Leaving the tape fully rewound is part of completing
  the job — don't consider the task done until the tape is back at the start.

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
- **Capture stops too early / 0 bytes captured.** A quiet/blank section trips the
  end-of-tape silence timeout — most often the **blank leader at the very start**
  on a from-the-top capture. Set `--eot-timeout 0` (or raise it), and/or bound the
  capture with `--duration` instead. See "Full-reel capture" above.

## Notes & limits

- Only the device's **output plug 0** (the normal tape-playback plug) is used.
- Relies on deprecated-but-present IOKit FireWire isochronous APIs that work
  through macOS 15 only.
- Verified on real hardware (Sony HDR-HC9) on Intel macOS 12.6 and Apple Silicon
  M1 Max macOS 15.

See the project README and `docs/BACKGROUND.md` for the full technical write-up.
