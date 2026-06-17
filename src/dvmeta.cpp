//
//  dvmeta — see dvmeta.h. Ported from xingrz/iina-dv-timecode.
//
#include "dvmeta.h"

namespace tapecap {

static inline int bcd(uint8_t b) { return (b & 0x0f) + ((b >> 4) & 0x0f) * 10; }

// ---------------------------------------------------------------------------
// DV (DIF) parsing
// ---------------------------------------------------------------------------
static const int    DIF_BLOCK_SIZE    = 80;
static const size_t DIF_SEQUENCE_SIZE = 12000;  // 150 blocks per sequence

// VAUX rec-date pack (0x62): PC2 day, PC3 month, PC4 year (BCD).
static bool parseRecDate(const uint8_t *p, int *Y, int *M, int *D)
{
    int day   = bcd(p[2] & 0x3f);
    int month = bcd(p[3] & 0x1f);
    int yb    = bcd(p[4]);
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    *Y = (yb >= 75) ? 1900 + yb : 2000 + yb;  // 75-99 -> 19xx, 00-74 -> 20xx
    *M = month;
    *D = day;
    return true;
}

// VAUX rec-time pack (0x63): PC2 seconds, PC3 minutes, PC4 hours (BCD).
static bool parseRecTime(const uint8_t *p, int *h, int *m, int *s)
{
    int second = bcd(p[2] & 0x7f);
    int minute = bcd(p[3] & 0x7f);
    int hour   = bcd(p[4] & 0x3f);
    if (hour > 23 || minute > 59 || second > 59) return false;
    *h = hour; *m = minute; *s = second;
    return true;
}

// Subcode title-timecode pack (0x13), found in DIF blocks 1-2 of a sequence.
static bool parseSubcodeTc(const uint8_t *data, size_t len, size_t seqStart, TapeMeta *out)
{
    for (int blockIdx = 1; blockIdx <= 2; blockIdx++)
    {
        size_t blockStart = seqStart + (size_t)blockIdx * DIF_BLOCK_SIZE;
        if (blockStart + DIF_BLOCK_SIZE > len) break;
        for (int ssyb = 0; ssyb < 6; ssyb++)
        {
            size_t p = blockStart + 6 + (size_t)ssyb * 8;
            if (data[p] != 0x13) continue;
            int f = bcd(data[p + 1] & 0x3f);
            int s = bcd(data[p + 2] & 0x7f);
            int m = bcd(data[p + 3] & 0x7f);
            int h = bcd(data[p + 4] & 0x3f);
            if (h > 23 || m > 59 || s > 59 || f > 29) continue;  // blank packs are 0xff
            out->haveTc = true;
            out->tcH = h; out->tcM = m; out->tcS = s; out->tcF = f;
            return true;
        }
    }
    return false;
}

bool DvParseFrame(const uint8_t *data, size_t len, TapeMeta *out)
{
    // Require a plausible DV header (1f 07 00) to avoid false positives.
    if (len < 4 || data[0] != 0x1f || data[1] != 0x07 || data[2] != 0x00)
        return false;

    bool haveDate = false, haveTime = false;
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;

    size_t sequences = len / DIF_SEQUENCE_SIZE;
    if (sequences < 1) sequences = 1;

    for (size_t sq = 0; sq < sequences; sq++)
    {
        size_t seqStart = sq * DIF_SEQUENCE_SIZE;

        if (!out->haveTc) parseSubcodeTc(data, len, seqStart, out);

        for (int blockIdx = 3; blockIdx <= 5; blockIdx++)  // VAUX blocks
        {
            size_t blockStart = seqStart + (size_t)blockIdx * DIF_BLOCK_SIZE;
            if (blockStart + DIF_BLOCK_SIZE > len) break;
            for (size_t i = blockStart + 3; i + 5 <= blockStart + DIF_BLOCK_SIZE; i += 5)
            {
                uint8_t hdr = data[i];
                if (!haveDate && hdr == 0x62)
                    haveDate = parseRecDate(&data[i], &Y, &M, &D);
                else if (!haveTime && hdr == 0x63)
                    haveTime = parseRecTime(&data[i], &h, &m, &s);
            }
        }
        if (haveDate && haveTime && out->haveTc) break;
    }

    if (haveDate && haveTime)
    {
        out->haveDate = true;
        out->year = Y; out->month = M; out->day = D;
        out->hour = h; out->minute = m; out->second = s;
    }
    return out->haveDate || out->haveTc;
}

// ---------------------------------------------------------------------------
// HDV (MPEG-2 TS) parsing — Sony AUX stream
// ---------------------------------------------------------------------------
static const int TS = 188;

// Offset of the TS payload within a 188-byte packet, or -1 if none.
static int tsPayloadStart(const uint8_t *p)
{
    int adapt = (p[3] >> 4) & 0x3;
    if (adapt != 1 && adapt != 3) return -1;
    int start = 4;
    if (adapt == 3) start = 5 + p[4];
    return (start < TS) ? start : -1;
}

// PAT -> first program's PMT PID.
static int parsePat(const uint8_t *p, int payloadStart)
{
    int pointer    = p[payloadStart];
    int tableStart = payloadStart + 1 + pointer;
    if (tableStart + 8 > TS) return -1;
    if (p[tableStart] != 0x00) return -1;
    int sectionLength = ((p[tableStart + 1] & 0x0f) << 8) | p[tableStart + 2];
    int q   = tableStart + 8;
    int end = tableStart + 3 + sectionLength - 4;
    if (end > TS) end = TS;
    while (q + 4 <= end)
    {
        int programNumber = (p[q] << 8) | p[q + 1];
        int pid           = ((p[q + 2] & 0x1f) << 8) | p[q + 3];
        if (programNumber != 0) return pid;
        q += 4;
    }
    return -1;
}

// PMT -> AUX PID (prefer Sony 0xA1, else 0xA0), video PID, and audio PID/type.
// Out-params for streams we don't care about (audioPid/audioType) may be null.
// Returns true if a PMT table section was found and parsed at this packet.
static bool parsePmt(const uint8_t *p, int payloadStart,
                     int *auxPid, int *videoPid, int *audioPid, int *audioType)
{
    *auxPid = -1; *videoPid = -1;
    if (audioPid)  *audioPid  = -1;
    if (audioType) *audioType = 0;
    int pointer    = p[payloadStart];
    int tableStart = payloadStart + 1 + pointer;
    if (tableStart + 12 > TS) return false;
    if (p[tableStart] != 0x02) return false;
    int sectionLength      = ((p[tableStart + 1]  & 0x0f) << 8) | p[tableStart + 2];
    int programInfoLength  = ((p[tableStart + 10] & 0x0f) << 8) | p[tableStart + 11];
    int q   = tableStart + 12 + programInfoLength;
    int end = tableStart + 3 + sectionLength - 4;
    if (end > TS) end = TS;
    int a1 = -1, a0 = -1, vid = -1, aud = -1, audType = 0;
    while (q + 5 <= end)
    {
        int streamType   = p[q];
        int pid          = ((p[q + 1] & 0x1f) << 8) | p[q + 2];
        int esInfoLength = ((p[q + 3] & 0x0f) << 8) | p[q + 4];
        if (a1 < 0 && streamType == 0xa1) a1 = pid;
        if (a0 < 0 && streamType == 0xa0) a0 = pid;
        if (vid < 0 && (streamType == 0x01 || streamType == 0x02)) vid = pid;
        if (aud < 0 && (streamType == 0x03 || streamType == 0x04)) { aud = pid; audType = streamType; }
        q += 5 + esInfoLength;
    }
    *auxPid   = (a1 >= 0) ? a1 : a0;
    *videoPid = vid;
    if (audioPid)  *audioPid  = aud;
    if (audioType) *audioType = audType;
    return true;
}

// Scan a Sony HDV-AUX PES body for the rec-date / rec-time / timecode packs.
// Anchor: 0x63 ... (i+5) 0xC0..0xC3 ... (i+10) 0xFF.
static bool scanAux(const uint8_t *b, int len, TapeMeta *out)
{
    for (int i = 0; i + 14 <= len; i++)
    {
        if (b[i] != 0x63) continue;
        if ((b[i + 5] & 0xfc) != 0xc0) continue;  // 0xC0..0xC3; low 2 bits = TC hour
        if (b[i + 10] != 0xff) continue;

        // rec date — same BCD layout as DV 0x62, pack ID at i+5, fields i+7..i+9
        int day   = bcd(b[i + 7] & 0x3f);
        int month = bcd(b[i + 8] & 0x1f);
        int yb    = bcd(b[i + 9]);
        if (month < 1 || month > 12 || day < 1 || day > 31) continue;
        int year = (yb >= 75) ? 1900 + yb : 2000 + yb;

        // wall-clock — BCD SS MM HH (note: reversed from DV's HH MM SS)
        int sec  = bcd(b[i + 11] & 0x7f);
        int min  = bcd(b[i + 12] & 0x7f);
        int hr   = bcd(b[i + 13] & 0x3f);
        bool clockOk = !(sec > 59 || min > 59 || hr > 23);

        // tape timecode — 0x63 pack data `07 <frames> <seconds> <minutes>`,
        // hours digit from the low 2 bits of the rec-date pack ID.
        int tcH = b[i + 5] & 0x03;
        int tcF = bcd(b[i + 2] & 0x3f);
        int tcS = bcd(b[i + 3] & 0x7f);
        int tcM = bcd(b[i + 4] & 0x7f);
        bool tcOk = !(tcH > 23 || tcM > 59 || tcS > 59 || tcF > 29);

        out->haveDate = true;
        out->year = year; out->month = month; out->day = day;
        if (clockOk) { out->hour = hr; out->minute = min; out->second = sec; }
        else         { out->hour = 0;  out->minute = 0;   out->second = 0; }
        if (tcOk)
        {
            out->haveTc = true;
            out->tcH = tcH; out->tcM = tcM; out->tcS = tcS; out->tcF = tcF;
        }
        return true;
    }
    return false;
}

void HdvTsParser::Feed(const uint8_t *p)
{
    if (p[0] != 0x47) return;                 // sync byte
    int b1   = p[1];
    int pid  = ((b1 & 0x1f) << 8) | p[2];
    int pusi = (b1 >> 6) & 1;                 // payload_unit_start_indicator
    int afc  = (p[3] >> 4) & 0x3;             // adaptation_field_control

    // Continuity-counter check — the TS damage signal. CC increments by 1
    // (mod 16) per *payload-bearing* packet of a PID; a jump the adaptation
    // field's discontinuity_indicator does not flag means dropped/garbled
    // packets. We only count breaks here — capture is never interrupted.
    if (pid != 0x1fff && (afc == 1 || afc == 3))   // null packets carry no CC
    {
        int  cc      = p[3] & 0x0f;
        bool discont = (afc == 3 && p[4] > 0 && (p[5] & 0x80));  // discontinuity_indicator
        auto it = lastCc_.find(pid);
        if (it != lastCc_.end() && !discont)
        {
            int expected = (it->second + 1) & 0x0f;
            // The expected next value is fine; a single repeated packet (cc
            // unchanged) is legal too. Anything else is a continuity break.
            if (cc != expected && cc != it->second)
                stats_.ccErrors++;
        }
        lastCc_[pid] = cc;
    }

    int payloadStart = tsPayloadStart(p);
    if (payloadStart < 0) return;

    if (pid == 0 && pmtPid_ < 0)
    {
        pmtPid_ = parsePat(p, payloadStart);
    }
    else if (pmtPid_ >= 0 && pid == pmtPid_ && pusi && !stats_.haveStreams)
    {
        int audioPid = -1, audioType = 0;
        if (parsePmt(p, payloadStart, &auxPid_, &videoPid_, &audioPid, &audioType))
        {
            stats_.haveStreams     = true;
            stats_.haveVideo       = (videoPid_ >= 0);
            stats_.haveAux         = (auxPid_ >= 0);
            stats_.haveAudio       = (audioPid >= 0);
            stats_.audioStreamType = audioType;
        }
    }
    else if (auxPid_ >= 0 && pid == auxPid_ && pusi)
    {
        // AUX PES header: 00 00 01 BF len(2); body follows.
        if (payloadStart + 6 < TS &&
            p[payloadStart] == 0 && p[payloadStart + 1] == 0 &&
            p[payloadStart + 2] == 1 && p[payloadStart + 3] == 0xbf)
        {
            const uint8_t *body = p + payloadStart + 6;
            int bodyLen = TS - (payloadStart + 6);
            TapeMeta m;
            if (scanAux(body, bodyLen, &m))
            {
                meta_ = m;
                have_ = true;
            }
        }
    }
}

bool HdvTsParser::Latest(TapeMeta *out) const
{
    if (!have_) return false;
    *out = meta_;
    return true;
}

} // namespace tapecap
