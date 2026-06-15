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
    }
    st->framesOrPackets.fetch_add(tsPacketCount);
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

// Pick a capture device: by explicit GUID, else the first attached device that
// has a tape subunit and a recognised DV or HDV/MPEG output format.
static AVCDevice *SelectDevice(AVCDeviceController *controller, UInt64 wantGuid)
{
    CFIndex count = CFArrayGetCount(controller->avcDeviceArray);
    if (wantGuid != 0)
    {
        AVCDevice *d = controller->findDeviceByGuid(wantGuid);
        return d;
    }
    for (CFIndex i = 0; i < count; i++)
    {
        AVCDevice *d = (AVCDevice *)CFArrayGetValueAtIndex(controller->avcDeviceArray, i);
        if (d->isAttached && (d->isDVDevice || d->isMPEGDevice))
            return d;
    }
    // fall back to first attached device, if any
    for (CFIndex i = 0; i < count; i++)
    {
        AVCDevice *d = (AVCDevice *)CFArrayGetValueAtIndex(controller->avcDeviceArray, i);
        if (d->isAttached)
            return d;
    }
    return nullptr;
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
        char tc[32];
        if (ReadTimecode(dev, tc, sizeof(tc)))
            printf("Timecode:      %s\n", tc);
        dev->closeDevice();
    }

    DestroyAVCDeviceController(controller);
    return 0;
}

struct CaptureOptions
{
    UInt64       guid        = 0;
    std::string  format      = "auto";   // auto | dv | hdv
    std::string  outPath;
    int          durationSec = 0;          // 0 = until stop condition
    int          eotTimeoutMs = 5000;      // 0 = never auto-stop on silence
    bool         control     = true;       // send PLAY/STOP
    bool         verbose     = false;
};

static int CmdCapture(const CaptureOptions &opt)
{
    CaptureState st;
    st.verbose = opt.verbose;

    // Open output
    if (opt.outPath == "-")
    {
        st.out = stdout;
        st.toStdout = true;
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

    StringLogger logger(FrameworkLog);
    AVCDeviceController *controller = nullptr;
    if (CreateAVCDeviceController(&controller, nullptr, nullptr) != kIOReturnSuccess || !controller)
    {
        Info("error: could not create AVC device controller (is the FireWire driver present?)");
        if (!st.toStdout) fclose(st.out);
        return 1;
    }
    WaitForDevices(controller, 3000);

    AVCDevice *dev = SelectDevice(controller, opt.guid);
    if (!dev)
    {
        Info("error: no DV/HDV device found. Try 'tapecap list'.");
        DestroyAVCDeviceController(controller);
        if (!st.toStdout) fclose(st.out);
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
        if (!st.toStdout) fclose(st.out);
        return 1;
    }

    // Decide DV vs HDV
    bool useHdv;
    if (opt.format == "dv")       useHdv = false;
    else if (opt.format == "hdv") useHdv = true;
    else                          // auto
    {
        if (dev->isMPEGDevice)      useHdv = true;
        else if (dev->isDVDevice)   useHdv = false;
        else
        {
            Info("error: could not auto-detect stream format. Put the deck in Play/VTR mode,");
            Info("       or force it with --format dv|hdv.");
            dev->closeDevice();
            DestroyAVCDeviceController(controller);
            if (!st.toStdout) fclose(st.out);
            return 2;
        }
    }

    Info("Device:  %s (0x%016llX)", dev->deviceName[0] ? dev->deviceName : "Unknown", dev->guid);
    Info("Format:  %s", useHdv ? "HDV (raw MPEG-2 TS)" : "DV (raw DIF)");
    Info("Output:  %s", st.toStdout ? "<stdout>" : opt.outPath.c_str());

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
        stream = dev->CreateDVReceiverForDevicePlug(0, DvFrameProc, &st, nullptr, &st, &logger,
                                                    kCyclesPerDVReceiveSegment * 2,
                                                    kNumDVReceiveSegments, dev->dvMode);
        if (stream && stream->pDVReceiver && opt.eotTimeoutMs > 0)
            stream->pDVReceiver->registerNoDataNotificationCallback(NoDataProc, &st, opt.eotTimeoutMs);
    }
    if (!stream)
    {
        Info("error: could not create receive stream for output plug 0.");
        dev->closeDevice();
        DestroyAVCDeviceController(controller);
        if (!st.toStdout) fclose(st.out);
        return 1;
    }

    // Install Ctrl-C / SIGTERM handlers now that we are about to roll tape.
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);

    // Roll the tape.
    if (opt.control && dev->hasTapeSubunit)
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
        if (!st.toStdout) fclose(st.out);
        return 1;
    }

    Info("Capturing… press Ctrl-C to stop.%s",
         opt.eotTimeoutMs > 0 ? " (auto-stops at end of tape)" : "");

    // Main wait loop.
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

        if (!st.toStdout && opt.verbose)
            fprintf(stderr, "\r%llu frames/packets, %llu bytes   ",
                    st.framesOrPackets.load(), st.bytesWritten.load());
    }
    if (!st.toStdout && opt.verbose) fputc('\n', stderr);

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

    Info("Done (%s): %llu frames/packets, %llu bytes written.",
         reason,
         st.framesOrPackets.load(),
         st.bytesWritten.load());
    if (st.dropped.load())
        Info("Note: %llu dropped/corrupted frame(s) — check the FireWire cable/connection.",
             st.dropped.load());

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
"  tapecap capture [options] <output>\n"
"  tapecap --help | --version\n"
"\n"
"CAPTURE OPTIONS:\n"
"  --guid <hex>        Select device by GUID (default: first DV/HDV device)\n"
"  --format <fmt>      auto | dv | hdv             (default: auto-detect)\n"
"  --duration <sec>    Stop after N seconds        (default: until Ctrl-C / EOT)\n"
"  --eot-timeout <ms>  Stop after this much silence; 0 disables   (default: 5000)\n"
"  --no-control        Do NOT send AV/C PLAY/STOP (press play on the deck yourself)\n"
"  -v, --verbose       Progress + framework log on stderr\n"
"  <output>            File to write, or '-' for stdout\n"
"\n"
"OUTPUT IS RAW: DV writes the DIF stream (.dv); HDV writes the MPEG-2 Transport\n"
"Stream as-is (.m2t / .ts) — video + audio + metadata, nothing demuxed.\n"
"\n"
"EXAMPLES:\n"
"  tapecap list\n"
"  tapecap capture tape01.m2t                 # auto-detect, roll tape, stop at EOT\n"
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
        if (opt.outPath.empty())
        {
            Info("error: capture requires an <output> path (use '-' for stdout)");
            return 2;
        }
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
