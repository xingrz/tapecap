/*
	File:		TSDemuxer.h

 Synopsis: This is the header for the TSDemuxer Class 

	Copyright: 	© Copyright 2001-2003 Apple Computer, Inc. All rights reserved.

	Written by: ayanowitz

 Disclaimer:	IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
 ("Apple") in consideration of your agreement to the following terms, and your
 use, installation, modification or redistribution of this Apple software
 constitutes acceptance of these terms.  If you do not agree with these terms,
 please do not use, install, modify or redistribute this Apple software.

 In consideration of your agreement to abide by the following terms, and subject
 to these terms, Apple grants you a personal, non-exclusive license, under Apple’s
 copyrights in this original Apple software (the "Apple Software"), to use,
 reproduce, modify and redistribute the Apple Software, with or without
 modifications, in source and/or binary forms; provided that if you redistribute
 the Apple Software in its entirety and without modifications, you must retain
 this notice and the following text and disclaimers in all such redistributions of
 the Apple Software.  Neither the name, trademarks, service marks or logos of
 Apple Computer, Inc. may be used to endorse or promote products derived from the
 Apple Software without specific prior written permission from Apple.  Except as
 expressly stated in this notice, no other rights or licenses, express or implied,
 are granted by Apple herein, including but not limited to any patent rights that
 may be infringed by your derivative works or by other works in which the Apple
 Software may be incorporated.

 The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
 WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
 WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
 COMBINATION WITH YOUR PRODUCTS.

 IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION
 OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF CONTRACT, TORT
 (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF APPLE HAS BEEN
 ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef __AVCVIDEOSERVICES_TSDEMUXER__
#define __AVCVIDEOSERVICES_TSDEMUXER__

namespace AVS
{

class TSDemuxer;
	
enum
{
	kMaxVideoPESSizeDefault = kFWAVCTSDemuxerMaxVideoPESSizeDefault,
	kMaxAudioPESSizeDefault = kFWAVCTSDemuxerMaxAudioPESSizeDefault,
	kIgnoreStream = 0x1FFF,
	kDefaultProgramSelection = kFWAVCTSDemuxerDefaultProgramSelection,
	kDefaultVideoPESBufferCount = kFWAVCTSDemuxerDefaultVideoPESBufferCount,
	kDefaultAudioPESBufferCount	= kFWAVCTSDemuxerDefaultAudioPESBufferCount,
	kPartialDemuxBufSize = 564,
	kNumDemuxStreamTypes = 2,

	// Define how many transport stream packets we allow between client 
	// callbacks before giving up, invalidating the current PSI tables and
	// rescanning for new PSI. Used only when autoPSIDecoding is true 
	kTSDemuxerMaxPacketsBetweenClientCallback = 10000
};
	
enum
{
	kDemuxerConfig_KeepTSPackets = 0x00000001,
	kDemuxerConfig_PartialDemux = 0x00000002
};

// enum for message passed in PES Callback
typedef unsigned int TSDemuxerMessage;
enum
{
	kTSDemuxerPESReceived = kFWAVCTSDemuxerPESReceived,
	kTSDemuxerPacketError = kFWAVCTSDemuxerPacketError,
	kTSDemuxerDiscontinuity = kFWAVCTSDemuxerDiscontinuity,
	kTSDemuxerIllegalAdaptationFieldCode = kFWAVCTSDemuxerIllegalAdaptationFieldCode,
	kTSDemuxerBadAdaptationFieldLength = kFWAVCTSDemuxerBadAdaptationFieldLength,
	kTSDemuxerPESLargerThanAllocatedBuffer = kFWAVCTSDemuxerPESLargerThanAllocatedBuffer,
	kTSDemuxerFlushedPESBuffer = kFWAVCTSDemuxerFlushedPESBuffer,
	kTSDemuxerRescanningForPSI = kFWAVCTSDemuxerRescanningForPSI
};

typedef unsigned int TSDemuxerStreamType;
enum
{
	kTSDemuxerStreamTypeVideo = kFWAVCTSDemuxerStreamTypeVideo,
	kTSDemuxerStreamTypeAudio = kFWAVCTSDemuxerStreamTypeAudio	
};

// Structure for PES packets passed to user
struct PESPacketBuf
{
	PESPacketBuf *pNext;
	TSDemuxerStreamType streamType;
	UInt8 *pPESBuf;
	UInt32 pesBufLen;
	UInt32 pid;
	UInt32 startTSPacketTimeStamp;
	TSDemuxer *pTSDemuxer;
	CFMutableArrayRef tsPacketArray;
	UInt64 startTSPacketU64TimeStamp;

	void *pClientPrivateData;	// A place for the client to attach some additional private data!
	void *pFWAVCPrivateData;
};

// Structures for VAux and Pack data - now contained in AVSShared.h
typedef FWAVCTSDemuxerHDV2VideoFramePack HDV2VideoFramePack;
typedef FWAVCTSDemuxerHDV1PackData HDV1PackData; 

// Function prototype for PES packet ready callback.
typedef IOReturn (*TSDemuxerCallback) (TSDemuxerMessage msg, PESPacketBuf* pPESBuf, void *pRefCon);

// Function prototype for PSI packet arrival callback.
typedef IOReturn (*TSDemuxerPSICallback) (UInt8 *pPSIBuf, void *pRefCon);

// Function prototype for HDV2 VAux data callback.
typedef void (*HDV2VAUXCallback) (HDV2VideoFramePack *pVAux, void *pRefCon);

// Function prototype for HDV1 Embedded Pack Data callback.
typedef void (*HDV1PackDataCallback) (HDV1PackData *pPack, void *pRefCon);

//
// The MPEG2-TS Demultiplexer Class Declaration
//
class TSDemuxer
{

public:
	// Constructor
    TSDemuxer(
			  UInt32 videoPid,
			  UInt32 audioPid = kIgnoreStream,
			  UInt32 pmtPid = kIgnoreStream,
			  TSDemuxerCallback fCallback = nil,
			  void *pCallbackRefCon = nil,
			  TSDemuxerPSICallback fPSICallback = nil,
			  void *pPSICallbackRefCon = nil,
			  UInt32 maxVideoPESSize = kMaxVideoPESSizeDefault,
			  UInt32 maxAudioPESSize = kMaxAudioPESSizeDefault,
			  UInt32 initialVideoPESBufferCount = kDefaultVideoPESBufferCount,
			  UInt32 initialAudioPESBufferCount = kDefaultAudioPESBufferCount,
			  StringLogger *stringLogger = nil);

	// Alternate Constructor - Auto PAT/PMT Decoding
    TSDemuxer(TSDemuxerCallback fCallback = nil,
			  void *pCallbackRefCon = nil,
			  TSDemuxerPSICallback fPSICallback = nil,
			  void *pPSICallbackRefCon = nil,
			  UInt32 selectedProgram = kDefaultProgramSelection,	// NOTE: Here a 1 means the first program in the PAT, 2 the second program, etc.
			  UInt32 maxVideoPESSize = kMaxVideoPESSizeDefault,
			  UInt32 maxAudioPESSize = kMaxAudioPESSizeDefault,
			  UInt32 initialVideoPESBufferCount = kDefaultVideoPESBufferCount,
			  UInt32 initialAudioPESBufferCount = kDefaultAudioPESBufferCount,
			  StringLogger *stringLogger = nil);
	
    // Destructor
    ~TSDemuxer();

	// Input a TS Packet into the demuxer
	IOReturn nextTSPacket(UInt8 *pPacket, UInt32 packetTimeStamp = 0xFFFFFFFF, UInt64 packetU64TimeStamp = 0xFFFFFFFFFFFFFFFFLL);

	// Reset demuxer function
	IOReturn resetTSDemuxer(UInt32 videoPid,
						 UInt32 audioPid = kIgnoreStream,
						 UInt32 pmtPid = kIgnoreStream);

	// Alternate Reset demuxer function - for Auto PAT/PMT Decoding mode
	IOReturn resetTSDemuxer(void);

	// Release a PES packet buffer passed in a callback
	IOReturn ReleasePESPacketBuf(PESPacketBuf* pPacketBuf);

	// Register a callback for HDV2 VAux data available
	void InstallHDV2VAuxCallback(HDV2VAUXCallback fVAuxCallback, void *pRefCon);

	// Register a callback for HDV2 VAux data available
	void InstallHDV1PackDataCallback(HDV1PackDataCallback fPackDataCallback, void *pRefCon);
	
	// Flush any in-process PES buffers
	void Flush(void);
	
	// Enable/Disable various demuxer configuration bits
	void SetDemuxerConfigurationBits(UInt32 configBits);
	
	// The PSITables object is public to give clients access to the PMT descriptors
	// Note clients of the TSDemuxer should only access this during callbacks for
	// PES and PSI delivery. Or, when no other thread is calling nextTSPacket(...)
	PSITables *psiTables;
	
private:

	// Get a PES buffer from the audio or video buffer queue, or allocate a new one if empty	
	IOReturn GetNextPESPacketBuf(TSDemuxerStreamType streamType, PESPacketBuf* *ppPacketBuf);

	void DoPESCallback(TSDemuxerMessage msg);
	
	void ParseHDV2VAux(UInt8 *pPacket);
	void ParseHDV1Pack(UInt8 *pPack, UInt32 packLen);
	
	StringLogger *logger;
	bool autoPSIDecoding;
	PESPacketBuf* PESPacket[kNumDemuxStreamTypes];
	UInt32 PESPacketPos[kNumDemuxStreamTypes];
	int streamCont[kNumDemuxStreamTypes];
	bool foundFirst[kNumDemuxStreamTypes];
	TSDemuxerStreamType currentStreamType;
	UInt32 programVideoPid;
	UInt32 programAudioPid;
	UInt32 programPmtPID;
	UInt32 program;
	TSDemuxerCallback PESCallback;
	void *pPESCallbackProcRefCon;
	TSDemuxerPSICallback PSICallback;
	void *pPSICallbackProcRefCon;
	UInt32 videoPESBufSize;
	UInt32 audioPESBufSize;
	UInt32 videoPESBufCount;
	UInt32 audioPESBufCount;
	PESPacketBuf* pVideoPESBufQueueHead;
	PESPacketBuf* pAudioPESBufQueueHead;
	pthread_mutex_t queueProtectMutex;
	
	HDV2VAUXCallback VAuxCallback;
	void *VAuxCallbackRefCon;
	HDV2VideoFramePack vAux;
	
	HDV1PackDataCallback PackDataCallback;
	void *PackDataCallbackRefCon;
	HDV1PackData packData;
		
	UInt32 configurationBits;
	UInt32 	tsPacketsSinceLastClientCallback;
};

} // namespace AVS

#endif /* __AVCVIDEOSERVICES_TSDEMUXER__ */



