# Bundled third-party code: AVCVideoServices

This directory contains **Apple's AVCVideoServices sample code**, vendored from
Apple's FireWire SDK. tapecap uses it to talk to the macOS IOKit FireWire stack
and receive the raw isochronous DV / MPEG-2 (HDV) streams off the bus.

## Provenance

| | |
|---|---|
| Source package | Apple **FireWire SDK 26** |
| Component | `Examples/AVCVideoServices-42` (the AVCVideoServices framework source) |
| Obtained from | Internet Archive item [`13572357-firewiresdk-26`](https://archive.org/details/13572357-firewiresdk-26), file `13572357-firewiresdk26.dmg` |
| `13572357-firewiresdk26.dmg` SHA-256 | `d2e3f21543ca45307e41af51f6e68e1d30ad69ccb13897132259ca2f3877018c` |

Apple no longer distributes the FireWire SDK from developer.apple.com; the
Internet Archive copy above is the source used here.

## What was kept and what was changed

To keep the build small and focused on **capture**, only the source files
needed for the DV/HDV receive path and AV/C device control were vendored:
device discovery & control (`AVCDevice*`, `TapeSubunitController`), the stream
receivers (`DVReceiver`, `MPEG2Receiver`, `UniversalReceiver`) and the helper
objects they depend on (transmitters/framers/TS helpers referenced by the
managed `AVCDevice` API), plus `AVSCommon` / `StringLogger`.

The following upstream modules were **not** vendored because nothing on the
capture path references them: `PanelSubunitController`, `MusicSubunitController`,
`MPEGTrickModes`, the `Virtual*` device-emulation classes, `FWA_IORemapper`, the
flat `FWAVC` C wrapper, and all of the `*Test` / sample `main()` programs.

The only edit to the vendored sources is in `AVCVideoServices.h`, where the
`#include` lines for those non-vendored modules were removed. Every file keeps
its original Apple copyright header.

## License

The AVCVideoServices sources are licensed under Apple's sample-code license
(reproduced in the header of every file). In short, Apple grants a
non-exclusive license to use, reproduce, modify and **redistribute** the
software in source and/or binary form, provided the copyright notice and
disclaimer are retained — which they are. The Apple name/marks may not be used
to endorse derived products, and the software is provided "AS IS" without
warranty. See any source file in this directory for the full text.

This license is separate from, and does not apply to, the rest of tapecap,
which is MIT-licensed (see the top-level `LICENSE`).
