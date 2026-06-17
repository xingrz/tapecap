//
//  dvmeta — extract recording date/time and tape timecode from DV / HDV streams
//
//  Ported from https://github.com/xingrz/iina-dv-timecode (BSD-ish), which
//  documents the on-tape pack layouts:
//    * DV  (DIF): VAUX rec-date (0x62) / rec-time (0x63) packs + Subcode
//                 title-timecode (0x13).
//    * HDV (Sony, MPEG-TS): a private AUX stream (PMT stream_type 0xA1/0xA0)
//                 whose PES packets carry the rec date/time and tape timecode
//                 at fixed offsets from a 0x63 anchor.
//
//  This is best-effort metadata for the live display and auto-naming; it never
//  affects the raw bytes written to disk.
//
#ifndef TAPECAP_DVMETA_H
#define TAPECAP_DVMETA_H

#include <cstddef>
#include <cstdint>

namespace tapecap {

struct TapeMeta
{
    // Recording wall-clock (from the rec-date + rec-time packs).
    bool haveDate = false;
    int  year = 0, month = 0, day = 0;
    int  hour = 0, minute = 0, second = 0;

    // Tape SMPTE timecode (rec-run), independent of the wall-clock above.
    bool haveTc = false;
    int  tcH = 0, tcM = 0, tcS = 0, tcF = 0;
};

// Live diagnostics for an HDV (MPEG-2 TS) capture, accumulated by HdvTsParser as
// packets stream by. Best-effort, never affects the bytes written to disk. All
// fields are zero/false until the relevant structure has been seen.
struct HdvStats
{
    // Stream layout, learned from the PMT. The whole point of tapecap is that
    // these (audio + private metadata) survive — AVFoundation drops them.
    bool haveStreams = false;   // PMT has been parsed
    bool haveVideo   = false;   // an MPEG video ES is present (stream_type 0x01/0x02)
    bool haveAudio   = false;   // an MPEG audio ES is present (stream_type 0x03/0x04)
    bool haveAux     = false;   // the Sony HDV timecode AUX stream (0xA1/0xA0) is present
    int  audioStreamType = 0;   // 0x03 = MPEG-1 audio, 0x04 = MPEG-2 audio
};

// Parse one raw DV frame (DIF). Fills whatever it finds into *out and returns
// true if either a recording date/time or a timecode was decoded.
bool DvParseFrame(const uint8_t *data, size_t len, TapeMeta *out);

// Streaming parser for HDV (MPEG-2 Transport Stream). Feed it whole 188-byte
// TS packets as they arrive; it tracks PAT/PMT, locates the Sony HDV-AUX
// stream, and keeps the most recently decoded metadata. Single-threaded: only
// call Feed()/Latest() from the same thread (the capture callback).
class HdvTsParser
{
public:
    void Feed(const uint8_t *pkt188);
    bool Latest(TapeMeta *out) const;   // false until something has been decoded
    HdvStats Stats() const { return stats_; }

private:
    int  pmtPid_   = -1;
    int  auxPid_   = -1;
    int  videoPid_ = -1;
    bool have_     = false;
    TapeMeta meta_;
    HdvStats stats_;
};

} // namespace tapecap

#endif // TAPECAP_DVMETA_H
