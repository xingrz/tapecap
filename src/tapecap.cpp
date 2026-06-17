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
static const char *kTapecapVersion = "0.1.0";

// ---------------------------------------------------------------------------
// AV/C tape-transport command operands (subunit-level CONTROL commands).
// The opcode constants come from TapeSubunitController.h; the operands below
// match the AV/C Tape Recorder/Player spec (and libavc1394 / dvgrab).
// ---------------------------------------------------------------------------
static const UInt8 kTapePlayForward = 0x75;  // PLAY  opcode 0xC3, normal-speed forward
static const UInt8 kTapeWindStop    = 0x60;  // WIND  opcode 0xC4, stop transport

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
static IOReturn SendTransport(AVCDevice *dev, UInt8 opcode, UInt8 operand)
{
    UInt8 cmd[4]   = { kAVCControlCommand, IOAVCAddress(kAVCTapeRecorder, 0), opcode, operand };
    UInt8 resp[8]  = { 0 };
    UInt32 respLen = sizeof(resp);
    return dev->AVCCommand(cmd, sizeof(cmd), resp, &respLen);
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

    if (dev->openDevice(DeviceMessageProc, nullptr) == kIOReturnSuccess)
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
};

static int CmdCapture(const CaptureOptions &opt)
{
    CaptureState st;
    st.verbose = opt.verbose;

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
        if (det == kFmtUnknown && opt.control && dev->hasTapeSubunit)
        {
            Info("Format not reported yet — starting playback to detect…");
            SendTransport(dev, kAVCTapePlayOpcode, kTapePlayForward);
            playStarted = true;
            usleep(1500 * 1000);
            det = DetectFormat(dev);
        }

        if (det == kFmtHDV)      useHdv = true;
        else if (det == kFmtDV)  useHdv = false;
        else
        {
            Info("error: could not auto-detect the stream format.");
            Info("       Make sure the deck is in Play/VTR mode, or force it with");
            Info("       --format hdv   (or --format dv).");
            if (playStarted) SendTransport(dev, kAVCTapeWindOpcode, kTapeWindStop);
            dev->closeDevice();
            DestroyAVCDeviceController(controller);
            cleanupOut();
            return 2;
        }
    }

    st.isHdv = useHdv;

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
        if (stream && stream->pMPEGReceiver && opt.eotTimeoutMs > 0)
            stream->pMPEGReceiver->registerNoDataNotificationCallback(NoDataProc, &st, opt.eotTimeoutMs);
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
        if (stream && stream->pDVReceiver && opt.eotTimeoutMs > 0)
            stream->pDVReceiver->registerNoDataNotificationCallback(NoDataProc, &st, opt.eotTimeoutMs);
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
    if (opt.control && dev->hasTapeSubunit && !playStarted)
    {
        IOReturn r = SendTransport(dev, kAVCTapePlayOpcode, kTapePlayForward);
        if (r != kIOReturnSuccess)
            Info("warning: PLAY command was not accepted (0x%08X); press play manually if needed.", r);
    }

    if (dev->StartAVCDeviceStream(stream) != kIOReturnSuccess)
    {
        Info("error: could not start isochronous receive.");
        if (opt.control && dev->hasTapeSubunit) SendTransport(dev, kAVCTapeWindOpcode, kTapeWindStop);
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
    if (opt.control && dev->hasTapeSubunit)
        SendTransport(dev, kAVCTapeWindOpcode, kTapeWindStop);
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
"  tapecap --help | --version\n"
"\n"
"CAPTURE OPTIONS:\n"
"  --guid <hex>        Select device by GUID (default: first DV/HDV device)\n"
"  --format <fmt>      auto | dv | hdv             (default: auto-detect)\n"
"  --duration <sec>    Stop after N seconds        (default: until Ctrl-C / EOT)\n"
"  --eot-timeout <ms>  Stop after this much silence; 0 disables   (default: 5000)\n"
"  --no-control        Do NOT send AV/C PLAY/STOP (press play on the deck yourself)\n"
"  -v, --verbose       Also print the framework's internal log on stderr\n"
"  [output]            File to write. Omit to auto-name from the recording's\n"
"                      date/time; use '-' to write to stdout.\n"
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
"  tapecap capture --no-control - | ffmpeg -i - -c copy out.mkv\n",
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
        return CmdCapture(opt);
    }

    Info("error: unknown command '%s'", cmd.c_str());
    Usage(stderr);
    return 2;
}
