//
//  tapecap — Raw DV / HDV tape capture over FireWire for macOS
//
//  Captures the *raw* isochronous bitstream a DV/HDV camcorder or deck sends
//  over FireWire (IEEE 1394), and writes it to disk untouched:
//
//    * DV  (IEC 61883-2)  ->  raw DIF byte stream      (.dv)
//    * HDV (IEC 61883-4)  ->  raw MPEG-2 Transport Stream, 188-byte TS packets,
//                             including video, audio (MPEG-1 Layer II) and all
//                             PSI/metadata exactly as on tape                (.m2t)
//
//  Unlike AVFoundation (which demuxes HDV and only hands back the MPEG-2 video
//  elementary stream, dropping the audio and stream metadata), this tool talks
//  straight to IOKit's FireWire stack via Apple's AVCVideoServices framework,
//  so nothing is lost. This is the same layer Premiere Pro uses to capture HDV.
//
//  Copyright (c) tapecap contributors. MIT licensed (see LICENSE).
//  Bundles Apple's AVCVideoServices sample code; see third_party/.
//

#include "AVCVideoServices.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#include <mutex>

#include "dvmeta.h"

using namespace AVS;

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
static const char *kTapecapVersion = "0.2.0";

// ---------------------------------------------------------------------------
// AV/C tape-transport command operands (subunit-level CONTROL commands).
// The opcode constants come from TapeSubunitController.h; the operands below
// match the AV/C Tape Recorder/Player spec (and libavc1394 / dvgrab).
// ---------------------------------------------------------------------------
static const UInt8 kTapePlayForward = 0x75;  // PLAY  opcode 0xC3, normal-speed forward
static const UInt8 kTapePlayReverse = 0x65;  // PLAY  opcode 0xC3, normal-speed reverse
static const UInt8 kTapeWindStop    = 0x60;  // WIND  opcode 0xC4, stop transport
static const UInt8 kTapeWindFastFwd = 0x75;  // WIND  opcode 0xC4, high-speed forward
static const UInt8 kTapeWindRewind  = 0x65;  // WIND  opcode 0xC4, rewind toward tape start

// ---------------------------------------------------------------------------
// Global stop signalling (set from a Unix signal handler and from callbacks
// running on the AVCVideoServices real-time thread).
// ---------------------------------------------------------------------------
static std::atomic<bool> gStopRequested{false};   // Ctrl-C / SIGTERM
static std::atomic<bool> gNoData{false};           // no isoch data for a while (end of tape?)
static std::atomic<bool> gDeviceGone{false};       // device unplugged / suspended

static void HandleSignal(int /*sig*/)
{
    gStopRequested.store(true);
}

// ---------------------------------------------------------------------------
// Per-capture shared state, handed to the data callbacks via their refcon.
// ---------------------------------------------------------------------------
struct CaptureState
{
    FILE       *out          = nullptr;
    bool        toStdout     = false;
    bool        verbose      = false;

    // running counters (touched only on the RT callback thread + read on main)
    std::atomic<unsigned long long> bytesWritten{0};
    std::atomic<unsigned long long> framesOrPackets{0};
    std::atomic<unsigned long long> dropped{0};
    bool        writeError   = false;

    // Live tape metadata parsed from the stream. The HDV parser is touched only
    // on the callback thread; the snapshots below are shared with the main
    // thread and guarded by metaMutex.
    tapecap::HdvTsParser     hdvParser;
    bool                     isHdv = false;     // selects HDV-specific diagnostics
    mutable std::mutex       metaMutex;
    tapecap::TapeMeta        latestMeta;        // most recent (for live display)
    bool                     haveLatestMeta = false;
    tapecap::TapeMeta        firstMeta;         // first rec date/time (for auto-name)
    bool                     haveFirstMeta = false;
    tapecap::HdvStats        hdvStats;          // latest HDV diagnostics snapshot
    bool                     haveHdvStats = false;

    // Record a freshly decoded metadata snapshot (called from the callback).
    void recordMeta(const tapecap::TapeMeta &m)
    {
        std::lock_guard<std::mutex> lk(metaMutex);
        latestMeta = m;
        haveLatestMeta = true;
        if (!haveFirstMeta && m.haveDate)
        {
            firstMeta = m;
            haveFirstMeta = true;
        }
    }

    // Record the HDV parser's running diagnostics (called from the callback).
    void recordHdvStats(const tapecap::HdvStats &s)
    {
        std::lock_guard<std::mutex> lk(metaMutex);
        hdvStats = s;
        haveHdvStats = true;
    }

    // Snapshot the HDV diagnostics for the main thread. False until first set.
    bool getHdvStats(tapecap::HdvStats *out) const
    {
        std::lock_guard<std::mutex> lk(metaMutex);
        if (!haveHdvStats) return false;
        *out = hdvStats;
        return true;
    }

    // Most recent tape timecode parsed from the stream, in whole seconds (frames
    // dropped). False until a timecode has been decoded — on an aged tape that
    // has lost its signal this can stay false, so callers must keep a backstop.
    bool getLiveTcSeconds(long *out) const
    {
        std::lock_guard<std::mutex> lk(metaMutex);
        if (!haveLatestMeta || !latestMeta.haveTc) return false;
        *out = (long)latestMeta.tcH * 3600 + latestMeta.tcM * 60 + latestMeta.tcS;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Small logging helpers
// ---------------------------------------------------------------------------
static void Info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// StringLogger sink for the AVCVideoServices framework. Only shown with -v.
static bool gVerboseLog = false;
static void FrameworkLog(char *s)
{
    if (gVerboseLog)
        fputs(s, stderr);
}

// ---------------------------------------------------------------------------
// Capture data callbacks
// ---------------------------------------------------------------------------

// HDV: called with a batch of complete 188-byte MPEG-2 TS packets.
static IOReturn MpegDataProc(UInt32 tsPacketCount, UInt32 **ppBuf, void *refcon)
{
    CaptureState *st = static_cast<CaptureState *>(refcon);
    for (UInt32 i = 0; i < tsPacketCount; i++)
    {
        size_t n = fwrite(ppBuf[i], 1, kMPEG2TSPacketSize, st->out);
        if (n != (size_t)kMPEG2TSPacketSize)
        {
            st->writeError = true;
            gStopRequested.store(true);
            return kIOReturnError;
        }
        st->bytesWritten.fetch_add(kMPEG2TSPacketSize);
        st->hdvParser.Feed(reinterpret_cast<const uint8_t *>(ppBuf[i]));
    }
    st->framesOrPackets.fetch_add(tsPacketCount);

    tapecap::TapeMeta m;
    if (st->hdvParser.Latest(&m))
        st->recordMeta(m);
    st->recordHdvStats(st->hdvParser.Stats());
    return kIOReturnSuccess;
}

// DV: called once per received frame. We keep corrupted frames too (they still
// hold real data off the tape) so the capture is a faithful raw dump.
static IOReturn DvFrameProc(DVFrameReceiveMessage msg, DVReceiveFrame *pFrame, void *refcon)
{
    CaptureState *st = static_cast<CaptureState *>(refcon);

    if (msg == kDVFrameDropped)
    {
        st->dropped.fetch_add(1);
        return kIOReturnSuccess;
    }

    // kDVFrameReceivedSuccessfully or kDVFrameCorrupted -> data present
    if (pFrame && pFrame->pFrameData && pFrame->frameLen)
    {
        size_t n = fwrite(pFrame->pFrameData, 1, pFrame->frameLen, st->out);
        if (n != pFrame->frameLen)
        {
            st->writeError = true;
            gStopRequested.store(true);
            return kIOReturnError;
        }
        st->bytesWritten.fetch_add(pFrame->frameLen);
        st->framesOrPackets.fetch_add(1);
        if (msg == kDVFrameCorrupted)
            st->dropped.fetch_add(1);

        tapecap::TapeMeta m;
        if (tapecap::DvParseFrame(pFrame->pFrameData, pFrame->frameLen, &m))
            st->recordMeta(m);
    }
    return kIOReturnSuccess;
}

// Fired by either receiver if no isoch data arrives for the configured window.
static IOReturn NoDataProc(void * /*refcon*/)
{
    gNoData.store(true);
    return kIOReturnSuccess;
}

// Device-level messages (termination/suspend) from the AVCDevice.
static IOReturn DeviceMessageProc(AVCDevice * /*dev*/, natural_t messageType,
                                  void * /*arg*/, void * /*refcon*/)
{
    switch (messageType)
    {
        case kIOMessageServiceIsTerminated:
        case kIOMessageServiceIsRequestingClose:
            gDeviceGone.store(true);
            gStopRequested.store(true);
            break;
        default:
            break;
    }
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Device discovery helpers
// ---------------------------------------------------------------------------

// Wait up to timeoutMs for the controller to enumerate at least one device,
// then let the set settle briefly so capability discovery can complete.
static void WaitForDevices(AVCDeviceController *controller, int timeoutMs)
{
    const int stepMs = 100;
    int waited = 0;
    while (waited < timeoutMs)
    {
        if (CFArrayGetCount(controller->avcDeviceArray) > 0)
            break;
        usleep(stepMs * 1000);
        waited += stepMs;
    }
    // settle: give discoverAVCDeviceCapabilities() a moment on the ctrl thread
    usleep(400 * 1000);
}

static UInt64 ParseGuid(const char *s)
{
    if (!s) return 0;
    return (UInt64)strtoull(s, nullptr, 16);
}

// Pick a capture device: by explicit GUID, else prefer a tape/DV/HDV device,
// otherwise just the first device the controller enumerated (the same set that
// 'list' shows). We deliberately don't gate on isAttached or the DV/HDV flags
// here: capability discovery is asynchronous and can lag behind enumeration, and
// the caller re-runs discovery and opens the device anyway.
static AVCDevice *SelectDevice(AVCDeviceController *controller, UInt64 wantGuid)
{
    CFIndex count = CFArrayGetCount(controller->avcDeviceArray);
    if (wantGuid != 0)
        return controller->findDeviceByGuid(wantGuid);

    AVCDevice *first = nullptr;
    for (CFIndex i = 0; i < count; i++)
    {
        AVCDevice *d = (AVCDevice *)CFArrayGetValueAtIndex(controller->avcDeviceArray, i);
        if (!first) first = d;
        if (d->hasTapeSubunit || d->isDVDevice || d->isMPEGDevice)
            return d;
    }
    return first;  // nothing flagged yet — fall back to the first enumerated device
}

static const char *FormatName(AVCDevice *d)
{
    if (d->isMPEGDevice) return "HDV / MPEG-TS";
    if (d->isDVDevice)   return "DV";
    return "unknown";
}

// ---------------------------------------------------------------------------
// AV/C helpers
// ---------------------------------------------------------------------------
static const char *AvcResponseName(UInt8 response)
{
    switch (response)
    {
        case kAVCAcceptedStatus:       return "accepted";
        case kAVCRejectedStatus:       return "rejected";
        case kAVCNotImplementedStatus: return "not implemented";
        case kAVCImplementedStatus:    return "implemented/status";
        case kAVCInTransitionStatus:   return "in transition";
        default:                       return "unknown";
    }
}

static IOReturn SendTransport(AVCDevice *dev, UInt8 opcode, UInt8 operand,
                              UInt8 *outResponse = nullptr)
{
    UInt8 cmd[4]   = { kAVCControlCommand, IOAVCAddress(kAVCTapeRecorder, 0), opcode, operand };
    UInt8 resp[8]  = { 0 };
    UInt32 respLen = sizeof(resp);
    IOReturn r = dev->AVCCommand(cmd, sizeof(cmd), resp, &respLen);
    UInt8 response = respLen > 0 ? resp[0] : 0xFF;
    if (outResponse) *outResponse = response;
    if (r != kIOReturnSuccess) return r;
    return response == kAVCAcceptedStatus ? kIOReturnSuccess : kIOReturnError;
}

static bool SendTransportOrWarn(AVCDevice *dev, UInt8 opcode, UInt8 operand,
                                const char *name)
{
    UInt8 response = 0xFF;
    IOReturn r = SendTransport(dev, opcode, operand, &response);
    if (r == kIOReturnSuccess) return true;
    Info("warning: %s command was not accepted (IOReturn 0x%08X, AV/C 0x%02X %s).",
         name, r, response, AvcResponseName(response));
    return false;
}

static unsigned BcdToBin(unsigned v) { return ((v >> 4) * 10) + (v & 0x0F); }

// Best-effort read of the tape's current timecode. Returns false if unavailable.
static bool ReadTimecode(AVCDevice *dev, char *buf, size_t buflen)
{
    UInt8 cmd[8]   = { kAVCStatusInquiryCommand, IOAVCAddress(kAVCTapeRecorder, 0),
                       0x51 /* TIME CODE */, 0x71, 0xFF, 0xFF, 0xFF, 0xFF };
    UInt8 resp[8]  = { 0 };
    UInt32 respLen = sizeof(resp);
    if (dev->AVCCommand(cmd, sizeof(cmd), resp, &respLen) == kIOReturnSuccess &&
        resp[0] == kAVCImplementedStatus)
    {
        snprintf(buf, buflen, "%02u:%02u:%02u.%02u",
                 BcdToBin(resp[7]), BcdToBin(resp[6]), BcdToBin(resp[5]), BcdToBin(resp[4]));
        return true;
    }
    return false;
}

// Same AV/C TIME CODE status inquiry as ReadTimecode(), but returns the tape
// position in whole seconds (frames ignored — seeking is coarse). Rejects
// obviously-bogus BCD so a garbled reply doesn't derail a seek.
static bool ReadTapeTcSeconds(AVCDevice *dev, long *outSec)
{
    UInt8 cmd[8]   = { kAVCStatusInquiryCommand, IOAVCAddress(kAVCTapeRecorder, 0),
                       0x51 /* TIME CODE */, 0x71, 0xFF, 0xFF, 0xFF, 0xFF };
    UInt8 resp[8]  = { 0 };
    UInt32 respLen = sizeof(resp);
    if (dev->AVCCommand(cmd, sizeof(cmd), resp, &respLen) == kIOReturnSuccess &&
        resp[0] == kAVCImplementedStatus)
    {
        unsigned h = BcdToBin(resp[7]), m = BcdToBin(resp[6]), s = BcdToBin(resp[5]);
        if (h > 23 || m > 59 || s > 59) return false;
        *outSec = (long)h * 3600 + (long)m * 60 + (long)s;
        return true;
    }
    return false;
}

// Parse a timecode argument into whole seconds. Accepts HH:MM:SS[:FF] (frames
// ignored), MM:SS, or a bare integer number of seconds.
static bool ParseTimecodeArg(const char *s, long *out)
{
    if (!s || !*s) return false;
    int a, b, c, d;
    if (sscanf(s, "%d:%d:%d:%d", &a, &b, &c, &d) == 4)
        { if (b > 59 || c > 59) return false; *out = (long)a * 3600 + b * 60 + c; return true; }
    if (sscanf(s, "%d:%d:%d", &a, &b, &c) == 3)
        { if (b > 59 || c > 59) return false; *out = (long)a * 3600 + b * 60 + c; return true; }
    if (sscanf(s, "%d:%d", &a, &b) == 2)
        { if (b > 59) return false; *out = (long)a * 60 + b; return true; }
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end && *end == '\0' && v >= 0) { *out = v; return true; }
    return false;
}

static const char *FormatHMS(long sec, char *buf, size_t n)
{
    if (sec < 0) sec = 0;
    snprintf(buf, n, "%02ld:%02ld:%02ld", sec / 3600, (sec / 60) % 60, sec % 60);
    return buf;
}

static double NowMono()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool FineSeekToTimecode(AVCDevice *dev, long goal, long cur)
{
    bool forward = goal > cur;
    UInt8 playOp = forward ? kTapePlayForward : kTapePlayReverse;
    const char *name = forward ? "PLAY" : "REVERSE PLAY";
    if (!SendTransportOrWarn(dev, kAVCTapePlayOpcode, playOp, name))
        return false;

    long lastTc = cur;
    double lastMoveAt = NowMono();
    double started = lastMoveAt;
    double timeout = (double)labs(goal - cur) + 8.0;
    if (timeout < 10.0) timeout = 10.0;

    bool reached = false;
    while (!gStopRequested.load())
    {
        usleep(200 * 1000);
        double now = NowMono();
        if (now - started > timeout)
        {
            Info("seek: fine positioning timed out; stopping.");
            break;
        }

        long tc;
        if (!ReadTapeTcSeconds(dev, &tc))
            continue;

        if (labs(tc - lastTc) >= 1)
        {
            lastTc = tc;
            lastMoveAt = now;
        }
        else if (now - lastMoveAt > 5.0)
        {
            Info("seek: fine positioning is not advancing; stopping.");
            break;
        }

        if ((forward && tc >= goal) || (!forward && tc <= goal))
        {
            reached = true;
            break;
        }
    }

    SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
    usleep(300 * 1000);
    long fin;
    if (ReadTapeTcSeconds(dev, &fin))
    { char b[16]; Info("seek: stopped at %s.", FormatHMS(fin, b, sizeof b)); }
    return reached;
}

// Fast-wind the tape to roughly `targetSec` (tape timecode, in seconds),
// deliberately stopping `overlapSec` BEFORE it so the capture that follows has
// pre-roll that overlaps the previous good section.
//
// Tape seeking is inherently coarse — decks coast past a stop, and (the reason
// tapeflow exists) aged tapes lose their timecode mid-travel. So this does not
// assume a continuous timecode: it reads the AV/C TIME CODE status when it can,
// estimates the wind rate from whatever samples arrive, and dead-reckons across
// blackouts, periodically stopping to re-anchor. Coasting overshoot lands the
// tape a little further from the target (i.e. more pre-roll), which is the safe
// direction. Returns true if it confirmed a position at/inside the goal.
static bool SeekToTimecode(AVCDevice *dev, long targetSec, long overlapSec)
{
    long goal = targetSec - overlapSec;
    if (goal < 0) goal = 0;

    // Where are we now? An idle deck may not report timecode; nudge it with a
    // brief play, then stop again.
    long cur = 0;
    bool have = ReadTapeTcSeconds(dev, &cur);
    if (!have)
    {
        if (SendTransportOrWarn(dev, kAVCTapePlayOpcode, kTapePlayForward, "PLAY"))
        {
            usleep(800 * 1000);
            have = ReadTapeTcSeconds(dev, &cur);
            SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
            usleep(300 * 1000);
        }
    }
    if (!have)
    {
        Info("seek: cannot read tape timecode; capturing from the current position.");
        return false;
    }

    char b1[16], b2[16], b3[16];
    Info("seek: at %s, target %s, stopping ~%lds early at %s.",
         FormatHMS(cur, b1, sizeof b1), FormatHMS(targetSec, b2, sizeof b2),
         overlapSec, FormatHMS(goal, b3, sizeof b3));

    const long tol = 2;         // seconds — close enough, no winding needed
    const long fineWindow = 20; // seconds — normal play avoids short-hop overshoot
    if (labs(cur - goal) <= tol) { Info("seek: already in range."); return true; }
    if (labs(cur - goal) <= fineWindow)
    {
        Info("seek: close enough for play-speed positioning.");
        return FineSeekToTimecode(dev, goal, cur);
    }

    bool  forward  = goal > cur;
    UInt8 windOp   = forward ? kTapeWindFastFwd : kTapeWindRewind;
    double rate    = 15.0;   // tape-seconds per wall-second; refined from samples
    bool   haveRate = false;
    long   lastTc  = cur;
    double lastAt  = NowMono();
    double started = lastAt;
    long   stallTc = cur;
    double stallAt = lastAt;
    int    blindChecks = 0;
    bool   reached = false;

    if (!SendTransportOrWarn(dev, kAVCTapeWindOpcode, windOp,
                             forward ? "FAST FORWARD" : "REWIND"))
        return false;

    while (!gStopRequested.load())
    {
        usleep(250 * 1000);
        double now = NowMono();
        if (now - started > 1200.0) { Info("seek: timed out; stopping."); break; }

        long tc;
        if (ReadTapeTcSeconds(dev, &tc))
        {
            double dtWall = now - lastAt;
            long   dtTape = tc - lastTc;
            if (dtWall > 0.4 && ((forward && dtTape > 0) || (!forward && dtTape < 0)))
            {
                double r = (double)labs(dtTape) / dtWall;
                rate = haveRate ? (rate * 0.5 + r * 0.5) : r;
                haveRate = true;
            }
            lastTc = tc; lastAt = now; blindChecks = 0;

            if (labs(tc - stallTc) >= 1) { stallTc = tc; stallAt = now; }
            else if (now - stallAt > 12.0)
            { Info("seek: transport not advancing (end of tape?); stopping."); break; }

            if (labs(goal - tc) <= fineWindow)
            {
                SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
                usleep(500 * 1000);
                return FineSeekToTimecode(dev, goal, tc);
            }
            if ((forward && tc >= goal) || (!forward && tc <= goal)) { reached = true; break; }
        }
        else
        {
            // Blackout: predict where we are by dead reckoning.
            double pred = (double)lastTc + (forward ? 1.0 : -1.0) * rate * (now - lastAt);
            if ((forward && pred >= goal) || (!forward && pred <= goal))
            {
                // Believed to be at the goal — stop and try to confirm.
                SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
                usleep(500 * 1000);
                if (ReadTapeTcSeconds(dev, &tc))
                {
                    lastTc = tc; lastAt = NowMono(); stallTc = tc; stallAt = lastAt;
                    if ((forward && tc >= goal) || (!forward && tc <= goal)) { reached = true; break; }
                    forward = goal > tc;
                    windOp  = forward ? kTapeWindFastFwd : kTapeWindRewind;
                    if (!SendTransportOrWarn(dev, kAVCTapeWindOpcode, windOp,
                                             forward ? "FAST FORWARD" : "REWIND"))
                        break;
                }
                else
                {
                    Info("seek: timecode lost in this region; stopping at the estimated position.");
                    return true;  // tape already stopped
                }
            }
            else if (now - lastAt > 6.0)
            {
                // Long blackout, not yet at goal — stop to re-anchor.
                SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
                usleep(500 * 1000);
                long t2;
                if (ReadTapeTcSeconds(dev, &t2)) { lastTc = t2; lastAt = NowMono(); blindChecks = 0; }
                else if (++blindChecks >= 4)
                { Info("seek: cannot re-acquire timecode; stopping at the estimated position."); return true; }
                else { lastAt = NowMono(); }  // advance anchor to avoid a tight loop
                forward = goal > lastTc;
                windOp  = forward ? kTapeWindFastFwd : kTapeWindRewind;
                if (!SendTransportOrWarn(dev, kAVCTapeWindOpcode, windOp,
                                         forward ? "FAST FORWARD" : "REWIND"))
                    break;
            }
        }
    }

    SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
    usleep(300 * 1000);
    long fin;
    if (ReadTapeTcSeconds(dev, &fin))
    { char b[16]; Info("seek: stopped at %s.", FormatHMS(fin, b, sizeof b)); }
    return reached;
}

enum DetectedFormat { kFmtUnknown, kFmtDV, kFmtHDV };

// Read the *actual* stream format from the device's output plug (oPCR). This
// reflects what the deck is really putting on the bus, which is more reliable
// than the tape subunit's signal-mode status: some HDV camcorders (e.g. the Sony
// HDR-HC9) report DV there even while streaming HDV. Mirrors AVCVideoServices'
// outputPlugSignalFormat(). Returns kFmtHDV, kFmtDV, or kFmtUnknown.
// Requires the device to be open.
static DetectedFormat ReadOutputPlugFormat(AVCDevice *dev, UInt8 plug)
{
    UInt8 cmd[8]  = { kAVCStatusInquiryCommand, kAVCUnitAddress,
                      kAVCOutputPlugSignalFormatOpcode, plug, 0xFF, 0xFF, 0xFF, 0xFF };
    UInt8 resp[8] = { 0 };
    UInt32 rlen   = sizeof(resp);
    if (dev->AVCCommand(cmd, sizeof(cmd), resp, &rlen) != kIOReturnSuccess)
        return kFmtUnknown;
    if (resp[0] != kAVCImplementedStatus || rlen < 8)
        return kFmtUnknown;
    UInt32 fmt = ((UInt32)resp[4] << 24) | ((UInt32)resp[5] << 16) |
                 ((UInt32)resp[6] << 8)  |  (UInt32)resp[7];
    if ((fmt & 0xFF000000) == kAVCPlugSignalFormatMPEGTS) return kFmtHDV;
    if ((fmt & 0xFF000000) == kAVCPlugSignalFormatNTSCDV) return kFmtDV;
    return kFmtUnknown;
}

// Decide DV vs HDV. Trust the live output-plug format first; fall back to the
// capability flags derived from the (less reliable) tape signal-mode query.
static DetectedFormat DetectFormat(AVCDevice *dev)
{
    DetectedFormat f = ReadOutputPlugFormat(dev, 0);
    if (f != kFmtUnknown) return f;
    if (dev->isMPEGDevice) return kFmtHDV;
    if (dev->isDVDevice)   return kFmtDV;
    return kFmtUnknown;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static int CmdList()
{
    AVCDeviceController *controller = nullptr;
    StringLogger logger(FrameworkLog);
    if (CreateAVCDeviceController(&controller, nullptr, nullptr) != kIOReturnSuccess || !controller)
    {
        Info("error: could not create AVC device controller (is the FireWire driver present?)");
        return 1;
    }

    WaitForDevices(controller, 2500);

    CFIndex count = CFArrayGetCount(controller->avcDeviceArray);
    if (count == 0)
    {
        Info("No FireWire AV/C devices found.");
        Info("Check the cable, that the camera is ON and in VTR/Play mode, and that a");
        Info("Thunderbolt-to-FireWire adapter chain is connected (Apple Silicon).");
    }
    else
    {
        printf("Found %ld FireWire AV/C device(s):\n\n", (long)count);
        for (CFIndex i = 0; i < count; i++)
        {
            AVCDevice *d = (AVCDevice *)CFArrayGetValueAtIndex(controller->avcDeviceArray, i);
            printf("  [%ld] %s\n", (long)i, d->deviceName[0] ? d->deviceName : "Unknown");
            printf("       GUID:       0x%016llX\n", d->guid);
            printf("       Vendor:     %s\n", d->vendorName[0] ? d->vendorName : "?");
            printf("       Format:     %s\n", FormatName(d));
            printf("       Tape subunit: %s   FCP: %s   In/Out plugs: %u/%u\n",
                   d->hasTapeSubunit ? "yes" : "no",
                   d->supportsFCP ? "yes" : "no",
                   (unsigned)d->numInputPlugs, (unsigned)d->numOutputPlugs);
            printf("       Attached:   %s\n\n", d->isAttached ? "yes" : "no");
        }
    }

    DestroyAVCDeviceController(controller);
    return count > 0 ? 0 : 2;
}

static int CmdInfo(UInt64 guid)
{
    AVCDeviceController *controller = nullptr;
    StringLogger logger(FrameworkLog);
    if (CreateAVCDeviceController(&controller, nullptr, nullptr) != kIOReturnSuccess || !controller)
    {
        Info("error: could not create AVC device controller");
        return 1;
    }
    WaitForDevices(controller, 2500);

    AVCDevice *dev = SelectDevice(controller, guid);
    if (!dev)
    {
        Info("No matching device found.");
        DestroyAVCDeviceController(controller);
        return 2;
    }

    // Refresh capabilities so the reported format reflects the camera's
    // *current* DV/HDV playback mode.
    dev->discoverAVCDeviceCapabilities();

    printf("Device:        %s\n", dev->deviceName[0] ? dev->deviceName : "Unknown");
    printf("GUID:          0x%016llX\n", dev->guid);
    printf("Vendor:        %s\n", dev->vendorName[0] ? dev->vendorName : "?");
    printf("Format:        %s\n", FormatName(dev));
    if (dev->isMPEGDevice) printf("MPEG mode:     0x%02X\n", dev->mpegMode);
    if (dev->isDVDevice)   printf("DV mode:       0x%02X\n", dev->dvMode);
    printf("Tape subunit:  %s\n", dev->hasTapeSubunit ? "yes" : "no");
    printf("Supports FCP:  %s\n", dev->supportsFCP ? "yes" : "no");
    printf("Output plugs:  %u\n", (unsigned)dev->numOutputPlugs);

    IOReturn openResult = dev->openDevice(DeviceMessageProc, nullptr);
    if (openResult == kIOReturnSuccess)
    {
        // The output-plug (oPCR) format reflects what's actually on the bus and
        // is the value capture's auto-detect trusts. It can disagree with the
        // signal-mode "Format" above (e.g. Sony HDR-HC9 reports DV there while
        // streaming HDV) — so show it explicitly.
        DetectedFormat op = ReadOutputPlugFormat(dev, 0);
        printf("Output plug 0: %s\n",
               op == kFmtHDV ? "HDV / MPEG-TS" : op == kFmtDV ? "DV" : "not reported (idle?)");

        char tc[32];
        if (ReadTimecode(dev, tc, sizeof(tc)))
            printf("Timecode:      %s\n", tc);
        dev->closeDevice();
    }
    else
    {
        printf("Open:          failed (0x%08X)\n", openResult);
    }

    DestroyAVCDeviceController(controller);
    return 0;
}

// Human-readable byte size.
static std::string HumanSize(unsigned long long bytes)
{
    char b[32];
    double mb = bytes / 1e6;
    if (mb >= 1000.0) snprintf(b, sizeof(b), "%.2f GB", mb / 1000.0);
    else              snprintf(b, sizeof(b), "%.1f MB", mb);
    return b;
}

// Erase the in-place status line so a normal log line prints cleanly. No-op
// unless stderr is a TTY (otherwise there is no '\r' line to clear).
static void ClearStatusLine()
{
    if (isatty(fileno(stderr)))
        fprintf(stderr, "\r%80s\r", "");
}

// HDV only: once the PMT has been parsed, print a single line confirming which
// elementary streams are present — the reassurance that the audio and the
// timecode AUX (both dropped by AVFoundation) are actually being captured.
// Returns true once it has printed, so the caller stops asking.
static bool AnnounceHdvStreams(const CaptureState &st)
{
    tapecap::HdvStats s;
    if (!st.getHdvStats(&s) || !s.haveStreams) return false;

    const char *audioName = s.audioStreamType == 0x03 ? "MPEG-1 Layer II"
                          : s.audioStreamType == 0x04 ? "MPEG-2 audio"
                          : "audio";
    std::string line = "Stream:  ";
    line += s.haveVideo ? "video" : "(no video)";
    if (s.haveAudio) { line += " + audio ("; line += audioName; line += ")"; }
    else             { line += " + (no audio in PMT)"; }
    line += s.haveAux ? " + timecode-AUX" : " + (no timecode-AUX)";

    ClearStatusLine();
    Info("%s", line.c_str());
    return true;
}

// HDV only: once the first sequence_header is decoded, print the video geometry,
// frame rate and bit rate. Returns true once it has printed.
static bool AnnounceHdvVideo(const CaptureState &st)
{
    tapecap::HdvStats s;
    if (!st.getHdvStats(&s) || !s.haveSeq) return false;

    char line[96];
    int n = snprintf(line, sizeof(line), "Video:   %dx%d", s.width, s.height);
    if (s.frameRate > 0)
        n += snprintf(line + n, sizeof(line) - n, "  %.2f fps", s.frameRate);
    if (s.bitRate > 0)
        snprintf(line + n, sizeof(line) - n, "  ~%.1f Mbit/s", s.bitRate / 1e6);

    ClearStatusLine();
    Info("%s", line);
    return true;
}

// One-line live status on stderr (in-place via '\r'): tape timecode, recording
// date/time, bytes captured and elapsed time. Fields only grow once decoded, so
// the trailing pad is enough to avoid leftover characters.
static void PrintLiveStatus(CaptureState &st, time_t startTime)
{
    tapecap::TapeMeta m;
    tapecap::HdvStats s;
    bool haveMeta = false, haveStats = false;
    {
        std::lock_guard<std::mutex> lk(st.metaMutex);
        haveMeta = st.haveLatestMeta;
        if (haveMeta) m = st.latestMeta;
        haveStats = st.haveHdvStats;
        if (haveStats) s = st.hdvStats;
    }
    char tcbuf[16]  = "--:--:--:--";
    char recbuf[24] = "------------------";
    if (haveMeta && m.haveTc)
        snprintf(tcbuf, sizeof(tcbuf), "%02d:%02d:%02d:%02d", m.tcH, m.tcM, m.tcS, m.tcF);
    if (haveMeta && m.haveDate)
        snprintf(recbuf, sizeof(recbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                 m.year, m.month, m.day, m.hour, m.minute, m.second);

    // Error count: transport continuity breaks for HDV, dropped/corrupted
    // frames for DV. Either way, 0 is the live "clean capture" reassurance.
    unsigned long long errs = st.isHdv ? (haveStats ? s.ccErrors : 0)
                                       : st.dropped.load();
    // Coded-frame count: true picture count for HDV, DV frames for DV.
    unsigned long long frames = st.isHdv ? (haveStats ? s.pictures : 0)
                                         : st.framesOrPackets.load();

    long el = (long)(time(nullptr) - startTime);
    std::string sz = HumanSize(st.bytesWritten.load());
    fprintf(stderr, "\r  tc %s   rec %s   %s   %llu fr   err %llu   (%ld:%02ld)        ",
            tcbuf, recbuf, sz.c_str(), frames, errs, el / 60, el % 60);
    fflush(stderr);
}

// Build an auto filename from the recording date/time (falls back to wall clock).
static std::string MakeAutoName(CaptureState &st, const char *ext)
{
    tapecap::TapeMeta m;
    bool haveFirst = false;
    {
        std::lock_guard<std::mutex> lk(st.metaMutex);
        haveFirst = st.haveFirstMeta;
        if (haveFirst) m = st.firstMeta;
    }
    char name[64];
    if (haveFirst)
        snprintf(name, sizeof(name), "%04d%02d%02d-%02d%02d%02d.%s",
                 m.year, m.month, m.day, m.hour, m.minute, m.second, ext);
    else
    {
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        snprintf(name, sizeof(name), "tapecap-%04d%02d%02d-%02d%02d%02d.%s",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
    }
    return name;
}

// Return `path` if free, else insert -1, -2, … before the extension.
static std::string UniquePath(const std::string &path)
{
    if (access(path.c_str(), F_OK) != 0) return path;
    std::string base = path, ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) { base = path.substr(0, dot); ext = path.substr(dot); }
    for (int n = 1; n < 1000; n++)
    {
        std::string cand = base + "-" + std::to_string(n) + ext;
        if (access(cand.c_str(), F_OK) != 0) return cand;
    }
    return path;
}

struct CaptureOptions
{
    UInt64       guid        = 0;
    std::string  format      = "auto";   // auto | dv | hdv
    std::string  outPath;                  // empty = auto-name; "-" = stdout
    int          durationSec = 0;          // 0 = until stop condition
    int          eotTimeoutMs = 5000;      // 0 = never auto-stop on silence
    bool         control     = true;       // send PLAY/STOP
    bool         verbose     = false;
    bool         seek        = false;      // fast-wind to seekSec before capturing
    long         seekSec     = 0;          // target tape timecode (seconds)
    bool         until       = false;      // stop once the tape timecode passes untilSec
    long         untilSec    = 0;          // stop tape timecode (seconds)
    long         overlapSec  = 4;          // pre-/post-roll kept around a [seek,until] window
};

static int CmdCapture(const CaptureOptions &opt)
{
    CaptureState st;
    st.verbose = opt.verbose;
    int eotTimeoutMs = opt.eotTimeoutMs;
    if (opt.seek && eotTimeoutMs > 0 && eotTimeoutMs < 15000)
        eotTimeoutMs = 15000;

    // Open output. With no path we capture into a hidden temp file and rename it
    // to the recording's date/time (parsed from the stream) when we're done.
    const bool toStdout = (opt.outPath == "-");
    const bool autoName = opt.outPath.empty();
    std::string tmpPath;
    if (toStdout)
    {
        st.out = stdout;
        st.toStdout = true;
    }
    else if (autoName)
    {
        tmpPath = std::string(".tapecap-") + std::to_string((long)getpid()) + ".part";
        st.out = fopen(tmpPath.c_str(), "wb");
        if (!st.out)
        {
            Info("error: cannot open temp output file '%s'", tmpPath.c_str());
            return 1;
        }
    }
    else
    {
        st.out = fopen(opt.outPath.c_str(), "wb");
        if (!st.out)
        {
            Info("error: cannot open output file '%s'", opt.outPath.c_str());
            return 1;
        }
    }

    // Close (and, for auto-name, delete) a half-open output on an error exit.
    auto cleanupOut = [&]() {
        if (st.out && st.out != stdout) fclose(st.out);
        if (autoName && !tmpPath.empty()) remove(tmpPath.c_str());
    };

    StringLogger logger(FrameworkLog);
    AVCDeviceController *controller = nullptr;
    if (CreateAVCDeviceController(&controller, nullptr, nullptr) != kIOReturnSuccess || !controller)
    {
        Info("error: could not create AVC device controller (is the FireWire driver present?)");
        cleanupOut();
        return 1;
    }
    WaitForDevices(controller, 3000);

    AVCDevice *dev = SelectDevice(controller, opt.guid);
    if (!dev)
    {
        Info("error: no DV/HDV device found. Try 'tapecap list'.");
        DestroyAVCDeviceController(controller);
        cleanupOut();
        return 2;
    }

    // Refresh capabilities so the detected format reflects the deck's *current*
    // DV/HDV mode. This is safe before opening — it's the same call the
    // controller makes during discovery, using the unit's AVC interface.
    dev->discoverAVCDeviceCapabilities();

    if (dev->openDevice(DeviceMessageProc, nullptr) != kIOReturnSuccess)
    {
        Info("error: could not open device '%s'. Another app may be using it,", dev->deviceName);
        Info("       or capture permission was denied (see README: permissions).");
        DestroyAVCDeviceController(controller);
        cleanupOut();
        return 1;
    }

    // Decide DV vs HDV
    bool useHdv;
    bool playStarted = false;
    if (opt.format == "dv")       useHdv = false;
    else if (opt.format == "hdv") useHdv = true;
    else                          // auto
    {
        DetectedFormat det = DetectFormat(dev);

        // Some decks only expose a meaningful output-plug format once the
        // transport is actually rolling. If we can't tell yet, start playback
        // and look again before giving up.
        if (det == kFmtUnknown && opt.control)
        {
            Info("Format not reported yet — starting playback to detect…");
            if (SendTransportOrWarn(dev, kAVCTapePlayOpcode, kTapePlayForward, "PLAY"))
            {
                playStarted = true;
                usleep(1500 * 1000);
                det = DetectFormat(dev);
            }
        }

        if (det == kFmtHDV)      useHdv = true;
        else if (det == kFmtDV)  useHdv = false;
        else
        {
            Info("error: could not auto-detect the stream format.");
            Info("       Make sure the deck is in Play/VTR mode, or force it with");
            Info("       --format hdv   (or --format dv).");
            if (playStarted) SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
            dev->closeDevice();
            DestroyAVCDeviceController(controller);
            cleanupOut();
            return 2;
        }
    }

    st.isHdv = useHdv;

    // Optional fast-wind to the requested start timecode (tapeflow re-capture).
    // Runs after format detection — which may itself have briefly rolled the
    // tape — and leaves the transport stopped, so the normal PLAY below rolls
    // again from the sought position.
    if (opt.seek)
    {
        if (!dev->hasTapeSubunit)
            Info("warning: device did not report a tape subunit; attempting AV/C transport anyway.");
        SeekToTimecode(dev, opt.seekSec, opt.overlapSec);
        playStarted = false;
    }

    Info("Device:  %s (0x%016llX)", dev->deviceName[0] ? dev->deviceName : "Unknown", dev->guid);
    Info("Format:  %s", useHdv ? "HDV (raw MPEG-2 TS)" : "DV (raw DIF)");
    Info("Output:  %s", toStdout ? "<stdout>"
                        : autoName ? "(auto-named from recording date/time)"
                        : opt.outPath.c_str());

    // Create the managed receive stream on output plug 0. The AVCDevice handles
    // isoch channel + bandwidth allocation and the point-to-point connection.
    AVCDeviceStream *stream = nullptr;
    if (useHdv)
    {
        stream = dev->CreateMPEGReceiverForDevicePlug(0, MpegDataProc, &st, nullptr, &st, &logger);
        if (stream && stream->pMPEGReceiver && eotTimeoutMs > 0)
            stream->pMPEGReceiver->registerNoDataNotificationCallback(NoDataProc, &st, eotTimeoutMs);
    }
    else
    {
        // The DV receiver needs a concrete DV mode to size its frame buffers.
        // discoverAVCDeviceCapabilities() fills dev->dvMode from the deck's
        // signal mode; if that's unknown (0xFF), fall back to NTSC SD DV25 so
        // the receiver doesn't reject it outright.
        UInt8 dvMode = dev->dvMode;
        if (dvMode == 0xFF)
        {
            dvMode = kDVModeSD_525_60;  // 0x00
            Info("warning: DV mode unknown; assuming NTSC SD (DV25). Put the deck in");
            Info("         Play/VTR mode for reliable auto-detection (PAL would differ).");
        }
        stream = dev->CreateDVReceiverForDevicePlug(0, DvFrameProc, &st, nullptr, &st, &logger,
                                                    kCyclesPerDVReceiveSegment * 2,
                                                    kNumDVReceiveSegments, dvMode);
        if (stream && stream->pDVReceiver && eotTimeoutMs > 0)
            stream->pDVReceiver->registerNoDataNotificationCallback(NoDataProc, &st, eotTimeoutMs);
    }
    if (!stream)
    {
        Info("error: could not create receive stream for output plug 0.");
        dev->closeDevice();
        DestroyAVCDeviceController(controller);
        cleanupOut();
        return 1;
    }

    // Install Ctrl-C / SIGTERM handlers now that we are about to roll tape.
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);

    // Roll the tape (unless detection already started it).
    if (opt.control && !playStarted)
    {
        SendTransportOrWarn(dev, kAVCTapePlayOpcode, kTapePlayForward, "PLAY");
    }

    if (dev->StartAVCDeviceStream(stream) != kIOReturnSuccess)
    {
        Info("error: could not start isochronous receive.");
        if (opt.control) SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
        dev->DestroyAVCDeviceStream(stream);
        dev->closeDevice();
        DestroyAVCDeviceController(controller);
        cleanupOut();
        return 1;
    }

    Info("Capturing… press Ctrl-C to stop.%s",
         opt.eotTimeoutMs > 0 ? " (auto-stops at end of tape)" : "");

    // Main wait loop. Show a live status line in place when stderr is a TTY.
    const bool showProgress = isatty(fileno(stderr));
    bool announcedStreams = false;
    bool announcedVideo   = false;
    time_t start = time(nullptr);
    const char *reason = "stopped";
    while (true)
    {
        usleep(150 * 1000);

        if (gStopRequested.load())
        {
            reason = st.writeError ? "write error" : (gDeviceGone.load() ? "device disconnected" : "interrupted");
            break;
        }
        if (gNoData.load())
        {
            reason = "end of tape (no data)";
            break;
        }
        if (opt.durationSec > 0 && (time(nullptr) - start) >= opt.durationSec)
        {
            reason = "duration reached";
            break;
        }
        if (opt.until)
        {
            long tc;
            if (st.getLiveTcSeconds(&tc) && tc >= opt.untilSec + opt.overlapSec)
            {
                reason = "until timecode reached";
                break;
            }
        }

        if (st.isHdv && !announcedStreams)
            announcedStreams = AnnounceHdvStreams(st);
        if (st.isHdv && !announcedVideo)
            announcedVideo = AnnounceHdvVideo(st);
        if (showProgress)
            PrintLiveStatus(st, start);
    }
    if (showProgress) fputc('\n', stderr);

    // Teardown
    dev->StopAVCDeviceStream(stream);
    if (opt.control)
        SendTransportOrWarn(dev, kAVCTapeWindOpcode, kTapeWindStop, "STOP");
    dev->DestroyAVCDeviceStream(stream);
    dev->closeDevice();
    DestroyAVCDeviceController(controller);

    if (!st.toStdout)
    {
        fflush(st.out);
        fclose(st.out);
    }

    // Auto-name: rename the temp file to the recording's date/time, or discard
    // it if nothing usable was captured.
    std::string savedPath;
    if (autoName && !st.toStdout)
    {
        if (!st.writeError && st.bytesWritten.load() > 0)
        {
            savedPath = UniquePath(MakeAutoName(st, useHdv ? "m2t" : "dv"));
            if (rename(tmpPath.c_str(), savedPath.c_str()) != 0)
                savedPath = tmpPath;  // rename failed; keep the temp file as-is
        }
        else
        {
            remove(tmpPath.c_str());
        }
    }

    Info("Done (%s): %llu frames/packets, %llu bytes written.",
         reason,
         st.framesOrPackets.load(),
         st.bytesWritten.load());
    if (!savedPath.empty())
        Info("Saved:   %s", savedPath.c_str());
    if (st.dropped.load())
        Info("Note: %llu dropped/corrupted frame(s) — check the FireWire cable/connection.",
             st.dropped.load());
    {
        tapecap::HdvStats s;
        if (st.isHdv && st.getHdvStats(&s))
        {
            if (s.pictures)
            {
                std::string line = "HDV video: " + std::to_string(s.pictures)
                                 + " frames in " + std::to_string(s.gops) + " GOP(s)";
                if (s.lastGopPictures)
                    line += " (last GOP " + std::to_string(s.lastGopPictures) + " frames)";
                if (s.haveSeq && s.frameRate > 0)
                {
                    long secs = (long)(s.pictures / s.frameRate);
                    char extra[48];
                    snprintf(extra, sizeof(extra), ", ~%ld:%02ld at %.2f fps",
                             secs / 60, secs % 60, s.frameRate);
                    line += extra;
                }
                Info("%s", line.c_str());
            }
            if (s.ccErrors)
                Info("Note: %llu transport continuity error(s) — possible tape damage or dropped packets.",
                     s.ccErrors);
        }
    }

    if (st.writeError) return 1;
    if (st.bytesWritten.load() == 0)
    {
        Info("Warning: no data captured. Is the deck playing and in the expected DV/HDV mode?");
        return 2;
    }
    return 0;
}

struct CueOptions
{
    UInt64 guid       = 0;
    long   targetSec  = 0;
    long   overlapSec = 0;   // default: land on the target (no pre-roll)
    bool   haveTarget = false;
};

// Position-only: fast-wind the tape to a timecode and stop, without capturing.
// Lets an orchestrator (tapeflow) cue the deck, then run `capture --no-control`.
static int CmdCue(const CueOptions &opt)
{
    StringLogger logger(FrameworkLog);
    AVCDeviceController *controller = nullptr;
    if (CreateAVCDeviceController(&controller, nullptr, nullptr) != kIOReturnSuccess || !controller)
    {
        Info("error: could not create AVC device controller (is the FireWire driver present?)");
        return 1;
    }
    WaitForDevices(controller, 3000);

    AVCDevice *dev = SelectDevice(controller, opt.guid);
    if (!dev)
    {
        Info("error: no DV/HDV device found. Try 'tapecap list'.");
        DestroyAVCDeviceController(controller);
        return 2;
    }
    dev->discoverAVCDeviceCapabilities();

    if (dev->openDevice(DeviceMessageProc, nullptr) != kIOReturnSuccess)
    {
        Info("error: could not open device '%s'. Another app may be using it,", dev->deviceName);
        Info("       or capture permission was denied (see README: permissions).");
        DestroyAVCDeviceController(controller);
        return 1;
    }
    if (!dev->hasTapeSubunit)
        Info("warning: device did not report a tape subunit; attempting AV/C transport anyway.");

    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);

    char b[16];
    Info("Device:  %s (0x%016llX)", dev->deviceName[0] ? dev->deviceName : "Unknown", dev->guid);
    Info("Cueing to %s…", FormatHMS(opt.targetSec, b, sizeof b));
    bool ok = SeekToTimecode(dev, opt.targetSec, opt.overlapSec);

    dev->closeDevice();
    DestroyAVCDeviceController(controller);
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Usage / argument parsing
// ---------------------------------------------------------------------------
static void Usage(FILE *f)
{
    fprintf(f,
"tapecap %s — raw DV/HDV FireWire tape capture for macOS\n"
"\n"
"USAGE:\n"
"  tapecap list\n"
"  tapecap info    [--guid <hex>]\n"
"  tapecap capture [options] [output]\n"
"  tapecap cue     [--guid <hex>] [--overlap <sec>] <timecode>\n"
"  tapecap --help | --version\n"
"\n"
"CAPTURE OPTIONS:\n"
"  --guid <hex>        Select device by GUID (default: first DV/HDV device)\n"
"  --format <fmt>      auto | dv | hdv             (default: auto-detect)\n"
"  --duration <sec>    Stop after N seconds        (default: until Ctrl-C / EOT)\n"
"  --eot-timeout <ms>  Stop after this much silence; 0 disables   (default: 5000;\n"
"                      --seek uses at least 15000 for stream startup)\n"
"  --seek <timecode>   Fast-wind to this tape timecode before capturing\n"
"  --until <timecode>  Stop once the tape timecode passes this point\n"
"  --overlap <sec>     Pre-/post-roll kept around --seek/--until    (default: 4)\n"
"  --no-control        Do NOT send AV/C PLAY/STOP (press play on the deck yourself)\n"
"  -v, --verbose       Also print the framework's internal log on stderr\n"
"  [output]            File to write. Omit to auto-name from the recording's\n"
"                      date/time; use '-' to write to stdout.\n"
"\n"
"A <timecode> is HH:MM:SS (or HH:MM:SS:FF — frames ignored, MM:SS, or seconds).\n"
"--seek/--until/cue drive the transport, so they need AV/C control (not\n"
"--no-control). Tape seeking is coarse and aged tapes drop timecode, so the\n"
"position is approximate; --overlap deliberately keeps extra footage on each\n"
"side so re-capture windows overlap (use it for tapeflow gap re-capture).\n"
"\n"
"While capturing, a live status line (tape timecode, recording date/time, size,\n"
"frame count, and a transport continuity-error count) is shown in place on\n"
"stderr. For HDV it also reports the streams present and the video format.\n"
"\n"
"OUTPUT IS RAW: DV writes the DIF stream (.dv); HDV writes the MPEG-2 Transport\n"
"Stream as-is (.m2t / .ts) — video + audio + metadata, nothing demuxed.\n"
"\n"
"EXAMPLES:\n"
"  tapecap list\n"
"  tapecap capture                            # auto-detect, auto-name e.g. 20101029-140926.m2t\n"
"  tapecap capture --format dv reel.dv\n"
"  tapecap capture --no-control - | ffmpeg -i - -c copy out.mkv\n"
"  tapecap capture --seek 00:12:30 --until 00:14:00 gap.m2t   # re-capture one gap\n"
"  tapecap cue 00:30:00                       # just wind the tape to 30:00\n",
    kTapecapVersion);
}

static const char *NextArg(int argc, char **argv, int &i, const char *name)
{
    if (i + 1 >= argc)
    {
        Info("error: option %s requires a value", name);
        exit(2);
    }
    return argv[++i];
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        Usage(stderr);
        return 2;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h" || cmd == "help")
    {
        Usage(stdout);
        return 0;
    }
    if (cmd == "--version" || cmd == "-V")
    {
        printf("tapecap %s\n", kTapecapVersion);
        return 0;
    }

    if (cmd == "list")
        return CmdList();

    if (cmd == "info")
    {
        UInt64 guid = 0;
        for (int i = 2; i < argc; i++)
        {
            std::string a = argv[i];
            if (a == "--guid")        guid = ParseGuid(NextArg(argc, argv, i, "--guid"));
            else if (a == "-v" || a == "--verbose") gVerboseLog = true;
            else { Info("error: unknown option for 'info': %s", a.c_str()); return 2; }
        }
        return CmdInfo(guid);
    }

    if (cmd == "capture")
    {
        CaptureOptions opt;
        for (int i = 2; i < argc; i++)
        {
            std::string a = argv[i];
            if (a == "--guid")             opt.guid = ParseGuid(NextArg(argc, argv, i, "--guid"));
            else if (a == "--format")      opt.format = NextArg(argc, argv, i, "--format");
            else if (a == "--duration")    opt.durationSec = atoi(NextArg(argc, argv, i, "--duration"));
            else if (a == "--eot-timeout") opt.eotTimeoutMs = atoi(NextArg(argc, argv, i, "--eot-timeout"));
            else if (a == "--seek")
            {
                opt.seek = true;
                if (!ParseTimecodeArg(NextArg(argc, argv, i, "--seek"), &opt.seekSec))
                { Info("error: --seek wants a timecode (HH:MM:SS[:FF], MM:SS, or seconds)"); return 2; }
            }
            else if (a == "--until")
            {
                opt.until = true;
                if (!ParseTimecodeArg(NextArg(argc, argv, i, "--until"), &opt.untilSec))
                { Info("error: --until wants a timecode (HH:MM:SS[:FF], MM:SS, or seconds)"); return 2; }
            }
            else if (a == "--overlap")
            {
                opt.overlapSec = atol(NextArg(argc, argv, i, "--overlap"));
                if (opt.overlapSec < 0) opt.overlapSec = 0;
            }
            else if (a == "--no-control")  opt.control = false;
            else if (a == "-v" || a == "--verbose") { opt.verbose = true; gVerboseLog = true; }
            else if (!a.empty() && a[0] == '-' && a != "-")
            {
                Info("error: unknown option for 'capture': %s", a.c_str());
                return 2;
            }
            else
            {
                if (!opt.outPath.empty())
                {
                    Info("error: multiple output paths given");
                    return 2;
                }
                opt.outPath = a;
            }
        }
        // An empty outPath is valid: it means "auto-name from the recording".
        if (opt.format != "auto" && opt.format != "dv" && opt.format != "hdv")
        {
            Info("error: --format must be auto, dv or hdv");
            return 2;
        }
        if (opt.seek && !opt.control)
        {
            Info("error: --seek needs transport control; cannot combine with --no-control");
            return 2;
        }
        if (opt.seek && opt.until && opt.untilSec <= opt.seekSec)
        {
            Info("error: --until must be later than --seek");
            return 2;
        }
        return CmdCapture(opt);
    }

    if (cmd == "cue")
    {
        CueOptions opt;
        for (int i = 2; i < argc; i++)
        {
            std::string a = argv[i];
            if (a == "--guid")         opt.guid = ParseGuid(NextArg(argc, argv, i, "--guid"));
            else if (a == "--overlap") { opt.overlapSec = atol(NextArg(argc, argv, i, "--overlap")); if (opt.overlapSec < 0) opt.overlapSec = 0; }
            else if (a == "-v" || a == "--verbose") gVerboseLog = true;
            else if (!a.empty() && a[0] == '-')
            { Info("error: unknown option for 'cue': %s", a.c_str()); return 2; }
            else
            {
                if (opt.haveTarget) { Info("error: 'cue' takes one timecode"); return 2; }
                if (!ParseTimecodeArg(a.c_str(), &opt.targetSec))
                { Info("error: bad timecode '%s' (use HH:MM:SS[:FF], MM:SS, or seconds)", a.c_str()); return 2; }
                opt.haveTarget = true;
            }
        }
        if (!opt.haveTarget) { Info("error: 'cue' needs a target timecode"); return 2; }
        return CmdCue(opt);
    }

    Info("error: unknown command '%s'", cmd.c_str());
    Usage(stderr);
    return 2;
}
