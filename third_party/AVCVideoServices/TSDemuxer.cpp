/*
	File:		TSDemuxer.cpp

 Synopsis: This is the sourcecode for the TSDemuxer Class

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

#include "AVCVideoServices.h"

namespace AVS
{

// Static prototypes
static void cfArrayReleaseTSPacketBuf(CFAllocatorRef allocator,const void *ptr);
	
//////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////
TSDemuxer::TSDemuxer(UInt32 videoPid,
					 UInt32 audioPid,
					 UInt32 pmtPid,
					 TSDemuxerCallback fCallback,
					 void *pCallbackRefCon,
					 TSDemuxerPSICallback fPSICallback,
					 void *pPSICallbackRefCon,
					 UInt32 maxVideoPESSize,
					 UInt32 maxAudioPESSize,
					 UInt32 initialVideoPESBufferCount,
					 UInt32 initialAudioPESBufferCount,
					 StringLogger *stringLogger)

{
	// Local Vars
	UInt32 i;
	PESPacketBuf* pPacketBuf;
	
	// Initialize class variables
	autoPSIDecoding = false;
	programVideoPid = videoPid;
	programAudioPid = audioPid;
	programPmtPID = pmtPid;
	PESCallback = fCallback;
	PSICallback = fPSICallback;
	videoPESBufSize = maxVideoPESSize;
	audioPESBufSize = maxAudioPESSize;
	videoPESBufCount = initialVideoPESBufferCount;
	audioPESBufCount = initialAudioPESBufferCount;
	logger = stringLogger;
	pVideoPESBufQueueHead = nil;
	pAudioPESBufQueueHead = nil;
	VAuxCallback = nil;
	VAuxCallbackRefCon = nil;
	PackDataCallback = nil;
	PackDataCallbackRefCon = nil;
	configurationBits = 0;
	tsPacketsSinceLastClientCallback = 0;

	pPESCallbackProcRefCon = pCallbackRefCon;
	pPSICallbackProcRefCon = pPSICallbackRefCon;
		
	program = 0x01;	// Not used in this mode 

	// Initialize the mutex for PES buffer queue access
    pthread_mutex_init(&queueProtectMutex, NULL);

	// Allocate initial Video PES Buffer

	// Allocate the video PES buffer queue
	for (i=0;i<initialVideoPESBufferCount;i++)
	{
		pPacketBuf = new PESPacketBuf;
		pPacketBuf->pNext = pVideoPESBufQueueHead;
		pVideoPESBufQueueHead = pPacketBuf;
		pPacketBuf->pPESBuf = new UInt8[videoPESBufSize];
		pPacketBuf->streamType = kTSDemuxerStreamTypeVideo;
		pPacketBuf->pTSDemuxer = this;
		pPacketBuf->tsPacketArray = nil;
	}

	// get the first entry from the queue for the
	// current video PES buffer.
	GetNextPESPacketBuf(kTSDemuxerStreamTypeVideo, &PESPacket[kTSDemuxerStreamTypeVideo]);
	
	PESPacketPos[kTSDemuxerStreamTypeVideo] = 0;
	streamCont[kTSDemuxerStreamTypeVideo] = -1;
	foundFirst[kTSDemuxerStreamTypeVideo] = false;

	// Allocate initial Audio PES Buffer, if needed
	if (audioPid != kIgnoreStream)
	{
		for (i=0;i<initialAudioPESBufferCount;i++)
		{
			pPacketBuf = new PESPacketBuf;
			pPacketBuf->pNext = pAudioPESBufQueueHead;
			pAudioPESBufQueueHead = pPacketBuf;
			pPacketBuf->pPESBuf = new UInt8[audioPESBufSize];
			pPacketBuf->streamType = kTSDemuxerStreamTypeAudio;
			pPacketBuf->pTSDemuxer = this;
			pPacketBuf->tsPacketArray = nil;
		}

		// get the first entry from the queue for the
		// current audio PES buffer.
		GetNextPESPacketBuf(kTSDemuxerStreamTypeAudio, &PESPacket[kTSDemuxerStreamTypeAudio]);

		PESPacketPos[kTSDemuxerStreamTypeAudio] = 0;
		streamCont[kTSDemuxerStreamTypeAudio] = -1;
		foundFirst[kTSDemuxerStreamTypeAudio] = false;
	}
	else
		PESPacket[kTSDemuxerStreamTypeAudio] = nil;

	// Allocate a PSI Tables object - It might be needed later
	// if the no-parameter reset call is made.
	psiTables = new PSITables(stringLogger);
	psiTables->selectProgram(program);
}

//////////////////////////////////////////////
// Alternate Constructor - Auto PAT/PMT Decoding
//////////////////////////////////////////////
TSDemuxer::TSDemuxer(TSDemuxerCallback fCallback,
					 void *pCallbackRefCon,
					 TSDemuxerPSICallback fPSICallback,
					 void *pPSICallbackRefCon,
					 UInt32 selectedProgram,
					 UInt32 maxVideoPESSize,
					 UInt32 maxAudioPESSize,
					 UInt32 initialVideoPESBufferCount,
					 UInt32 initialAudioPESBufferCount,
					 StringLogger *stringLogger)
{
	// Local Vars
	UInt32 i;
	PESPacketBuf* pPacketBuf;
	
	pPESCallbackProcRefCon = pCallbackRefCon;
	pPSICallbackProcRefCon = pPSICallbackRefCon;
	
	// Initialize class variables
	autoPSIDecoding = true;
	program = selectedProgram;
	programVideoPid = kReservedPid;
	programAudioPid = kReservedPid;
	programPmtPID = kReservedPid;
	PESCallback = fCallback;
	PSICallback = fPSICallback;
	videoPESBufSize = maxVideoPESSize;
	audioPESBufSize = maxAudioPESSize;
	videoPESBufCount = initialVideoPESBufferCount;
	audioPESBufCount = initialAudioPESBufferCount;
	logger = stringLogger;
	pVideoPESBufQueueHead = nil;
	pAudioPESBufQueueHead = nil;
	VAuxCallback = nil;
	VAuxCallbackRefCon = nil;
	PackDataCallback = nil;
	PackDataCallbackRefCon = nil;
	configurationBits = 0;
	tsPacketsSinceLastClientCallback = 0;

	// Initialize the mutex for PES buffer queue access
    pthread_mutex_init(&queueProtectMutex, NULL);

	// Allocate the video PES buffer queue
	for (i=0;i<initialVideoPESBufferCount;i++)
	{
		pPacketBuf = new PESPacketBuf;
		pPacketBuf->pNext = pVideoPESBufQueueHead;
		pVideoPESBufQueueHead = pPacketBuf;
		pPacketBuf->pPESBuf = new UInt8[videoPESBufSize];
		pPacketBuf->streamType = kTSDemuxerStreamTypeVideo;
		pPacketBuf->pTSDemuxer = this;
		pPacketBuf->tsPacketArray = nil;
	}

	// get the first entry from the queue for the
	// current video PES buffer.
	GetNextPESPacketBuf(kTSDemuxerStreamTypeVideo, &PESPacket[kTSDemuxerStreamTypeVideo]);

	PESPacketPos[kTSDemuxerStreamTypeVideo] = 0;
	streamCont[kTSDemuxerStreamTypeVideo] = -1;
	foundFirst[kTSDemuxerStreamTypeVideo] = false;
	
	// Allocate initial Audio PES Buffer, if needed
	if (audioPESBufSize != 0)
	{
		for (i=0;i<initialAudioPESBufferCount;i++)
		{
			pPacketBuf = new PESPacketBuf;
			pPacketBuf->pNext = pAudioPESBufQueueHead;
			pAudioPESBufQueueHead = pPacketBuf;
			pPacketBuf->pPESBuf = new UInt8[audioPESBufSize];
			pPacketBuf->streamType = kTSDemuxerStreamTypeAudio;
			pPacketBuf->pTSDemuxer = this;
			pPacketBuf->tsPacketArray = nil;
		}
		
		// get the first entry from the queue for the
		// current audio PES buffer.
		GetNextPESPacketBuf(kTSDemuxerStreamTypeAudio, &PESPacket[kTSDemuxerStreamTypeAudio]);
		
		PESPacketPos[kTSDemuxerStreamTypeAudio] = 0;
		streamCont[kTSDemuxerStreamTypeAudio] = -1;
		foundFirst[kTSDemuxerStreamTypeAudio] = false;
	}
	else
	{
		PESPacket[kTSDemuxerStreamTypeAudio] = nil;
		programAudioPid = kIgnoreStream;
	}

	// Allocate a PSI Tables object
	psiTables = new PSITables(stringLogger);
	psiTables->selectProgram(program);
}

//////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////
TSDemuxer::~TSDemuxer()
{
	PESPacketBuf* pDeleteBuf;

	if (PESPacket[kTSDemuxerStreamTypeVideo])
		ReleasePESPacketBuf(PESPacket[kTSDemuxerStreamTypeVideo]);

	if (PESPacket[kTSDemuxerStreamTypeAudio])
		ReleasePESPacketBuf(PESPacket[kTSDemuxerStreamTypeAudio]);
	
	if (psiTables)
		delete psiTables;

	while(pVideoPESBufQueueHead != nil)
	{
		delete [] pVideoPESBufQueueHead->pPESBuf;
		pDeleteBuf = pVideoPESBufQueueHead;
		pVideoPESBufQueueHead = pDeleteBuf->pNext;
		delete pDeleteBuf;
	};

	while(pAudioPESBufQueueHead != nil)
	{
		delete [] pAudioPESBufQueueHead->pPESBuf;
		pDeleteBuf = pAudioPESBufQueueHead;
		pAudioPESBufQueueHead = pDeleteBuf->pNext;
		delete pDeleteBuf;
	};

	pthread_mutex_destroy(&queueProtectMutex);
}

//////////////////////////////////////////////
// nextTSPacket
//////////////////////////////////////////////
IOReturn TSDemuxer::nextTSPacket(UInt8 *pPacket, UInt32 packetTimeStamp, UInt64 packetU64TimeStamp)
{
	UInt32 pid;
	bool packetHasStartIndicator;
	bool packetHasErrorIndicator;
	int continuityCounter;
	UInt32 adaptationFieldControl;
	UInt32 adaptationFieldLength;
	TSPacket *tsPacket;
	UInt8 *pTSPacketBuf;
	UInt32 thisBufSize;
	
	// Check sync byte
	if (pPacket[0] != 0x47)
	{
		if (logger)
			logger->log("TSDemuxer Error: Bad Sync Byte in Packet\n");
		return kIOReturnBadArgument;
	}
	
	// For autoPSIDecoding mode, prevent stale PSI tables from messing us up  
	tsPacketsSinceLastClientCallback += 1;
	if ((autoPSIDecoding == true) && (tsPacketsSinceLastClientCallback > kTSDemuxerMaxPacketsBetweenClientCallback) && (psiTables != nil))
	{
		// Log it
		if (logger)
			logger->log("TSDemuxer: Timeout waiting for next PES. Searching for new PSI!\n\n");

		// Report it to the client
		currentStreamType = kTSDemuxerStreamTypeVideo;
		DoPESCallback(kTSDemuxerRescanningForPSI);

		// Reset the PSI tables
		psiTables->ResetPSITables();
		tsPacketsSinceLastClientCallback = 0;
	}
		
	// Get pid
	pid = (((pPacket[1] & 0x1F) << 8) | pPacket[2]) & 0x1FFF;
	if (pid == programVideoPid)
	{
		currentStreamType = kTSDemuxerStreamTypeVideo;
	}
	else if ((pid == programAudioPid) && (pid != kIgnoreStream))
	{
		currentStreamType = kTSDemuxerStreamTypeAudio;
	}
	else if (((pid == programPmtPID) || (pid == 0x0000)))
	{
		if (autoPSIDecoding == true)
		{
			tsPacket = new TSPacket(pPacket);
			psiTables->extractTableDataFromPacket(tsPacket);
			// Update this class's search PIDs
			programPmtPID = psiTables->primaryProgramPmtPid;
			programVideoPid = psiTables->primaryProgramVideoPid;
			if (audioPESBufSize != 0)
				programAudioPid = psiTables->primaryProgramAudioPid; // For autoPSIDecoding, a audioPESBufSize of 0, means ignore audio
			delete tsPacket;
		}

		if (PSICallback != nil)
		{
			// Make PSI callback to client
			PSICallback(pPacket,pPSICallbackProcRefCon);
		}
		return kIOReturnSuccess ;
	}
	else if ((pid == 0x811) && (VAuxCallback != nil)) // HDV2 V-Aux Table
	{
		ParseHDV2VAux(pPacket);
		VAuxCallback(&vAux,VAuxCallbackRefCon);
		return kIOReturnSuccess ;
	}
	else if ((pid == 0x0F02) && (PackDataCallback != nil)) // HDV1 Pack Data
	{
		// Parse the packet
		tsPacket = new TSPacket(pPacket);

		// Callback to client only if private data found and id-string matches!
		if ((tsPacket->hasAdaptationPrivateData == true) && 
			(tsPacket->adaptationPrivateDataLen > 4) &&
			(tsPacket->pAdaptationPrivateData[0] == 0x44) &&
			(tsPacket->pAdaptationPrivateData[1] == 0x56) &&
			(tsPacket->pAdaptationPrivateData[2] == 0x50) &&
			(tsPacket->pAdaptationPrivateData[3] == 0x4B))
		{
			ParseHDV1Pack(tsPacket->pAdaptationPrivateData, tsPacket->adaptationPrivateDataLen);
			PackDataCallback(&packData,PackDataCallbackRefCon);
		}
		
		delete tsPacket;
		return kIOReturnSuccess ;
	}
	else
	{
		// Throw this packet away.
		return kIOReturnSuccess ;
	}

	// Extract some information from the TS packet header
	packetHasStartIndicator = ((pPacket[1] & 0x40) != 0);
	packetHasErrorIndicator = ((pPacket[1] & 0x80) != 0);
	continuityCounter = (pPacket[3] & 0xF);
	adaptationFieldControl = (pPacket[3] & 0x30) >> 4;

	if (packetHasErrorIndicator)
	{
		if (logger)
			logger->log("TSDemuxer Error: Packet has error indicator set\n");
		DoPESCallback(kTSDemuxerPacketError);
		foundFirst[currentStreamType] = false;
		PESPacketPos[currentStreamType] = 0;
		return kIOReturnSuccess ;
	}
	
	// Check continuityCounter
	if (streamCont[currentStreamType] != -1)
	{
		if (continuityCounter != ((streamCont[currentStreamType] + 1) & 0xF))
		{
			// Found a discontinuity
			if (logger)
				logger->log("TSDemuxer Error: Discontinuity in PID: 0x%04X\n",pid);
			DoPESCallback(kTSDemuxerDiscontinuity);
			foundFirst[currentStreamType] = false;
			PESPacketPos[currentStreamType] = 0;
			streamCont[currentStreamType] = -1;
		}
		else
			streamCont[currentStreamType] = continuityCounter;
	}

	// Get adaptation header size
	if (adaptationFieldControl == 0)
	{
		// Reserved adaptation field value
		if (logger)
			logger->log("TSDemuxer Error: Illegal adaptation field code\n");
		DoPESCallback(kTSDemuxerIllegalAdaptationFieldCode);
		packetHasStartIndicator = false;
		foundFirst[currentStreamType] = false;
		PESPacketPos[currentStreamType] = 0;
		return kIOReturnSuccess ;
	}
	else if (adaptationFieldControl == 0x2)
	{
		// adaptation only, no payload
		adaptationFieldLength = 184;
	}
	else if (adaptationFieldControl == 0x3)
	{
		// Packet contains adaptation and payload
		adaptationFieldLength = pPacket[4] + 1;
		if (adaptationFieldLength > 184)
		{
			// Bogus adaptation length field
			if (logger)
				logger->log("TSDemuxer Error: Bad adaptation field length\n");
			DoPESCallback(kTSDemuxerBadAdaptationFieldLength);
			packetHasStartIndicator = false;
			foundFirst[currentStreamType] = false;
			PESPacketPos[currentStreamType] = 0;
			return kIOReturnSuccess ;
		}
	}
	else
	{
		// No adaptation field, payload only
		adaptationFieldLength = 0;
	}

	if (packetHasStartIndicator)
	{
		foundFirst[currentStreamType] = true;
		streamCont[currentStreamType] = continuityCounter;

		// Found a start indicator - if we have accumulated any PES packet data
		// for this stream type, pass it up to the client
		if (PESPacketPos[currentStreamType] > 0)
		{
			DoPESCallback(kTSDemuxerPESReceived);
			PESPacketPos[currentStreamType] = 0;
			// Note: Fixed! Don't muck with the streamCont here since we just set it correctly a few lines above!
		}
		
		// This TS packet is the start of a PES packet.
		// Record its client-supplied timestamp(s) in the PES packet struct.
		PESPacket[currentStreamType]->startTSPacketTimeStamp = packetTimeStamp;
		PESPacket[currentStreamType]->startTSPacketU64TimeStamp = packetU64TimeStamp;
	}

	if (PESPacket[currentStreamType]->tsPacketArray)
	{
		pTSPacketBuf = new UInt8[kMPEG2TSPacketSize];
		if (pTSPacketBuf)
		{
			// Copy the TS packet data into our new buffer, and add it to the array
			memcpy(pTSPacketBuf,pPacket,kMPEG2TSPacketSize);
			CFArrayAppendValue(PESPacket[currentStreamType]->tsPacketArray,pTSPacketBuf);
		}
		else
		{
			// We ran out of memory, and cannot save this TS packet in the array.
			// Instead, release the array!
			CFRelease(PESPacket[currentStreamType]->tsPacketArray);
			PESPacket[currentStreamType]->tsPacketArray = nil;
		}
	}
	
	// Extract the payload from this TS packet into the current PES packet buffer
	if (((184 - adaptationFieldLength) > 0) && (PESPacket[currentStreamType] != nil) && (foundFirst[currentStreamType] == true))
	{
		if (configurationBits & kDemuxerConfig_PartialDemux)
		{
			if (currentStreamType == kTSDemuxerStreamTypeVideo)
			{
				if (videoPESBufSize < kPartialDemuxBufSize)
					thisBufSize = videoPESBufSize;
				else
					thisBufSize = kPartialDemuxBufSize;
			}
			else
			{
				if (audioPESBufSize < kPartialDemuxBufSize)
					thisBufSize = audioPESBufSize;
				else
					thisBufSize = kPartialDemuxBufSize;
			}
			
			// Make sure this won't go beyond the thisBufSize len!
			if ((PESPacketPos[currentStreamType]+(184 - adaptationFieldLength)) < thisBufSize)
			{
				memcpy(PESPacket[currentStreamType]->pPESBuf + PESPacketPos[currentStreamType], pPacket + 4 + adaptationFieldLength, 184 - adaptationFieldLength);
				PESPacketPos[currentStreamType] += 184 - adaptationFieldLength;
			}
			else
			{
				// In the kDemuxerConfig_PartialDemux mode, we don't bail on TS packet accumulation for the PES,
				// because we may also be in kDemuxerConfig_KeepTSPackets mode.
			}
		}
		else
		{
			// Make sure this won't go beyond the PESPacket[currentStreamType] len!
			if ((PESPacketPos[currentStreamType]+(184 - adaptationFieldLength)) < ((currentStreamType == kTSDemuxerStreamTypeVideo) ? videoPESBufSize : audioPESBufSize))
			{
				memcpy(PESPacket[currentStreamType]->pPESBuf + PESPacketPos[currentStreamType], pPacket + 4 + adaptationFieldLength, 184 - adaptationFieldLength);
				PESPacketPos[currentStreamType] += 184 - adaptationFieldLength;
			}
			else
			{
				if (logger)
					logger->log("TSDemuxer Error: PES Packet larger than allocated buffer\n");
				DoPESCallback(kTSDemuxerPESLargerThanAllocatedBuffer);
				PESPacketPos[currentStreamType] = 0;
				streamCont[currentStreamType] = -1;
				foundFirst[currentStreamType] = false;
			}
		}
	}
	
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////
// resetTSDemuxer
//////////////////////////////////////////////
IOReturn TSDemuxer::resetTSDemuxer(UInt32 videoPid,
						UInt32 audioPid,
						UInt32 pmtPid)
{
	autoPSIDecoding = false;
	programVideoPid = videoPid;
	programAudioPid = audioPid;
	programPmtPID = pmtPid;
	PESPacketPos[kTSDemuxerStreamTypeVideo] = 0;
	streamCont[kTSDemuxerStreamTypeVideo] = -1;
	foundFirst[kTSDemuxerStreamTypeVideo] = false;
	PESPacketPos[kTSDemuxerStreamTypeAudio] = 0;
	streamCont[kTSDemuxerStreamTypeAudio] = -1;
	foundFirst[kTSDemuxerStreamTypeAudio] = false;
	tsPacketsSinceLastClientCallback = 0;

	if (psiTables)
		psiTables->ResetPSITables();
	
	return kIOReturnSuccess ;
}

/////////////////////////////////////////////////////////////
// alternate resetTSDemuxer - for Auto PAT/PMT Decoding mode
/////////////////////////////////////////////////////////////
IOReturn TSDemuxer::resetTSDemuxer(void)
{
	autoPSIDecoding = true;
	programVideoPid = kReservedPid;
	programAudioPid = kReservedPid;
	programPmtPID = kReservedPid;
	PESPacketPos[kTSDemuxerStreamTypeVideo] = 0;
	streamCont[kTSDemuxerStreamTypeVideo] = -1;
	foundFirst[kTSDemuxerStreamTypeVideo] = false;
	PESPacketPos[kTSDemuxerStreamTypeAudio] = 0;
	streamCont[kTSDemuxerStreamTypeAudio] = -1;
	foundFirst[kTSDemuxerStreamTypeAudio] = false;
	tsPacketsSinceLastClientCallback = 0;

	if (psiTables)
		psiTables->ResetPSITables();
	
	if (audioPESBufSize == 0)
		programAudioPid = kIgnoreStream;
	
	return kIOReturnSuccess ;
}

/////////////////////////////////////////////////////////////
// Flush
/////////////////////////////////////////////////////////////
void TSDemuxer::Flush(void)
{
	// Flush Video PES data
	currentStreamType = kTSDemuxerStreamTypeVideo;
	if (PESPacketPos[currentStreamType] > 0)
	{
		DoPESCallback(kTSDemuxerFlushedPESBuffer);
		PESPacketPos[currentStreamType] = 0;
	}
	streamCont[currentStreamType] = -1;
	foundFirst[currentStreamType] = false;

	// Flush Audio PES data, if available
	if (programAudioPid != kIgnoreStream)
	{
		currentStreamType = kTSDemuxerStreamTypeAudio;
		if (PESPacketPos[currentStreamType] > 0)
		{
			DoPESCallback(kTSDemuxerFlushedPESBuffer);
			PESPacketPos[currentStreamType] = 0;
		}
		streamCont[currentStreamType] = -1;
		foundFirst[currentStreamType] = false;
	}
}

/////////////////////////////////////////////////////////////
// InstallHDV2VAuxCallback
/////////////////////////////////////////////////////////////
void TSDemuxer::InstallHDV2VAuxCallback(HDV2VAUXCallback fVAuxCallback, void *pRefCon)
{
	VAuxCallback = fVAuxCallback;
	VAuxCallbackRefCon = pRefCon;
}

/////////////////////////////////////////////////////////////
// InstallHDV1PackDataCallback
/////////////////////////////////////////////////////////////
void TSDemuxer::InstallHDV1PackDataCallback(HDV1PackDataCallback fPackDataCallback, void *pRefCon)
{
	PackDataCallback = fPackDataCallback;
	PackDataCallbackRefCon = pRefCon;
}

/////////////////////////////////////////////////////////////
// SetDemuxerConfigurationBits
/////////////////////////////////////////////////////////////
void TSDemuxer::SetDemuxerConfigurationBits(UInt32 configBits)
{
	configurationBits = configBits;
	
	// We need to discard any in-process demux'ed PES packets
	if (PESPacket[kTSDemuxerStreamTypeVideo])
	{	
		ReleasePESPacketBuf(PESPacket[kTSDemuxerStreamTypeVideo]);
		GetNextPESPacketBuf(kTSDemuxerStreamTypeVideo, &PESPacket[kTSDemuxerStreamTypeVideo]);
	}
	
	if (PESPacket[kTSDemuxerStreamTypeAudio])
	{
		ReleasePESPacketBuf(PESPacket[kTSDemuxerStreamTypeAudio]);
		GetNextPESPacketBuf(kTSDemuxerStreamTypeAudio, &PESPacket[kTSDemuxerStreamTypeAudio]);
	}
}

/////////////////////////////////////////////////////////////
// ParseHDV1Pack
/////////////////////////////////////////////////////////////
void TSDemuxer::ParseHDV1Pack(UInt8 *pPack, UInt32 packLen)
{
	UInt32 numPacks = (packLen-5)/5;
	UInt8 *pPackBytes;
	
	packData.pPackDataBytes = pPack;
	packData.packDataLen = packLen;

	packData.idString = 0x4456504b;	// 'DVPK'
	packData.seamlessPlayBackPoint = (pPack[4] & 0x80) ? true : false;
	packData.has_2_3_pullDown = (pPack[4] & 0x40) ? true : false;
	packData.pullDownRepetition = (pPack[4] & 0x20) ? true : false;
	
	// Pre-Init these bools prior to parsing through the remaining pack bytes
	packData.hasMPEGSourcePack = false;
	packData.hasMPEGSourceControlPack = false;
	packData.hasRecDatePack = false;
	packData.hasRecTimePack = false;
	packData.hasTitleTimeCodePack = false;
	packData.hasBinaryGroupPack = false;
	packData.hasStreamPack = false;

	// Parse through the 5-byte packs
	for (unsigned int i=0;i<numPacks;i++)
	{
		pPackBytes = pPack + 5 + (5*i);
		switch (pPackBytes[0])
		{
			case 0x90:
				packData.hasMPEGSourcePack = true;
				packData.serviceID = (pPackBytes[2]*256) + pPackBytes[1];
				packData.sourceCode = (pPackBytes[3]>>6);
				packData.is50HzSystem = (pPackBytes[3] & 0x20) ? true : false;
				packData.sType = (pPackBytes[3] & 0x1F);
				packData.tunerCategory = pPackBytes[4];
				break;
			
			case 0x91:
				packData.hasMPEGSourceControlPack = true;
				packData.cgms = (pPackBytes[1]>>6);
				packData.tph = ((pPackBytes[1] & 0x38) >> 3);
				packData.tpl = (pPackBytes[1] & 0x04) ? true : false;
				packData.ss = (pPackBytes[1] & 0x03);
				packData.recST = (pPackBytes[2] & 0x80) ? true : false;
				packData.recMode = ((pPackBytes[2] & 0x30) >> 4);
				packData.mr = (pPackBytes[2] & 0x08) ? true : false;
				packData.isHD = (pPackBytes[2] & 0x04) ? true : false;
				packData.audMode = (pPackBytes[2] & 0x03);
				packData.maxBitRate = pPackBytes[3];
				packData.recEnd = (pPackBytes[4] & 0x80) ? true : false;
				packData.genreCategory = (pPackBytes[4] & 0x7F);
				break;
			
			case 0x92:
				packData.hasRecDatePack = true;
				packData.recTimeZone = (((pPackBytes[1] & 0x30) >> 4)*10)+(pPackBytes[1] & 0x0F);
				packData.recDay = (((pPackBytes[2] & 0x30) >> 4)*10)+(pPackBytes[2] & 0x0F);
				packData.recWeek =  ((pPackBytes[3] & 0xE0) >> 5);
				packData.recMonth =  (((pPackBytes[3] & 0x10) >> 4)*10)+(pPackBytes[3] & 0x0F);
				packData.recYear = pPackBytes[4];
				break;
			
			case 0x93:
				packData.hasRecTimePack = true;
				packData.recFrames = (((pPackBytes[1] & 0x30) >> 4)*10)+(pPackBytes[1] & 0x0F);
				packData.recSeconds = (((pPackBytes[2] & 0x70) >> 4)*10)+(pPackBytes[2] & 0x0F);
				packData.recMinutes =  (((pPackBytes[3] & 0x70) >> 4)*10)+(pPackBytes[3] & 0x0F);
				packData.recHours = (((pPackBytes[4] & 0x30) >> 4)*10)+(pPackBytes[4] & 0x0F);
				break;

			case 0x95:
				packData.hasStreamPack = true;
				packData.streamType = pPackBytes[2];
				packData.elementaryPID = ((pPackBytes[4] & 0x1F) << 8) + pPackBytes[3];
				packData.pidType = ((pPackBytes[4] & 0x60) >> 5);
				break;
				
			case 0x13:
				packData.hasTitleTimeCodePack = true;
				packData.ttcFrames = (((pPackBytes[1] & 0x30) >> 4)*10)+(pPackBytes[1] & 0x0F);
				packData.ttcSeconds = (((pPackBytes[2] & 0x70) >> 4)*10)+(pPackBytes[2] & 0x0F);
				packData.ttcMinutes =  (((pPackBytes[3] & 0x70) >> 4)*10)+(pPackBytes[3] & 0x0F);
				packData.ttcHours = (((pPackBytes[4] & 0x30) >> 4)*10)+(pPackBytes[4] & 0x0F);
				packData.s1 = (pPackBytes[1] & 0x40) ? true : false;
				packData.s2 = (pPackBytes[1] & 0x80) ? true : false;
				packData.s3 = (pPackBytes[2] & 0x80) ? true : false;
				packData.s4 = (pPackBytes[3] & 0x80) ? true : false;
				packData.s5 = (pPackBytes[4] & 0x40) ? true : false;
				packData.s6 = (pPackBytes[4] & 0x80) ? true : false;
				break;

			case 0x14:
				packData.hasBinaryGroupPack = true;
				packData.binaryGroup1 = (pPackBytes[1] & 0x0F);
				packData.binaryGroup2 = ((pPackBytes[1] & 0xF0) >> 4);
				packData.binaryGroup3 = (pPackBytes[2] & 0x0F);
				packData.binaryGroup4 = ((pPackBytes[2] & 0xF0) >> 4);
				packData.binaryGroup5 = (pPackBytes[3] & 0x0F);
				packData.binaryGroup6 = ((pPackBytes[3] & 0xF0) >> 4);
				packData.binaryGroup7 = (pPackBytes[4] & 0x0F);
				packData.binaryGroup8 = ((pPackBytes[4] & 0xF0) >> 4);
				break;
			
			default:
				// Unknown pack type!
				break;
		}
	}
}

/////////////////////////////////////////////////////////////
// ParseHDV2VAux
/////////////////////////////////////////////////////////////
void TSDemuxer::ParseHDV2VAux(UInt8 *pPacket)
{
	TSPacket *tsPacket;
	UInt32 i;
	
	tsPacket = new TSPacket(pPacket);
	
	vAux.pVAuxDataBytes = &tsPacket->pPayload[6];
	vAux.vAuxDataLen = 59;
	vAux.hasMakerCodeAndImagingFrameRate = false;
	vAux.TSDemuxerHDV2VideoFramePackStructureVersion = 1;
	
	// Parse V-Aux table
	vAux.keyWord = tsPacket->pPayload[6];
	
	vAux.length = tsPacket->pPayload[7];
	
	vAux.extendedTrackNumber =
		((tsPacket->pPayload[10]*65536L) + (tsPacket->pPayload[9]*256) + tsPacket->pPayload[8]);
	
	vAux.numVideoFrames = tsPacket->pPayload[11];
	
	vAux.dataH = (tsPacket->pPayload[12] & 0x0F);
	
	vAux.vbvDelay = (((tsPacket->pPayload[14]*256)) + tsPacket->pPayload[13]);
	
	vAux.headerSize = tsPacket->pPayload[15];
	
	vAux.dts =
		((tsPacket->pPayload[20]*4294967296LL) + 
		 (tsPacket->pPayload[19]*16777216LL) + 
		 (tsPacket->pPayload[18]*65536L) + 
		 (tsPacket->pPayload[17]*256) + 
		 tsPacket->pPayload[16]);
	
	vAux.progressive = (tsPacket->pPayload[21] & 0x80) ? true : false;
	
	vAux.topFieldFirst = (tsPacket->pPayload[21] & 0x40) ? true : false;
	
	vAux.repeatFirstField = (tsPacket->pPayload[21] & 0x20) ? true : false;
	
	vAux.sourceFrameRate = (tsPacket->pPayload[21] & 0x0F);
	
	vAux.searchDataMode = tsPacket->pPayload[22];
	
	vAux.horizontalSize = ((((tsPacket->pPayload[24] & 0x0F)*256)) + tsPacket->pPayload[23]);
	
	vAux.verticalSize = ((((tsPacket->pPayload[26] & 0x0F)*256)) + tsPacket->pPayload[25]);
	
	vAux.aspectRatio = ((tsPacket->pPayload[27] & 0xF0) >> 4);
	
	vAux.frameRate = (tsPacket->pPayload[27] & 0x0F);
	
	vAux.bitRate = 
		((tsPacket->pPayload[30]*65536L) + (tsPacket->pPayload[29]*256) + tsPacket->pPayload[28]);
	
	vAux.vbvBufferSize = (((tsPacket->pPayload[32]*256)) + tsPacket->pPayload[31]);
	
	vAux.mpegProfile = ((tsPacket->pPayload[33] & 0x70) >> 4);
	
	vAux.mpegLevel = (tsPacket->pPayload[33] & 0x0F);
	
	vAux.videoFormat = ((tsPacket->pPayload[34] & 0x70) >> 4);
	
	vAux.chroma = ((tsPacket->pPayload[34] & 0x0C) >> 2);
	
	vAux.gopN = ((tsPacket->pPayload[35] & 0xF0) >> 4);
	
	vAux.gopM = (tsPacket->pPayload[35] & 0x0F);
	
	vAux.packDataEnable0 = (tsPacket->pPayload[36] & 0x01) ? true : false;

	vAux.packDataEnable1 = (tsPacket->pPayload[36] & 0x02) ? true : false;

	vAux.packDataEnable2 = (tsPacket->pPayload[36] & 0x04) ? true : false;

	vAux.bf = (tsPacket->pPayload[37] & 0x80) ? true : false;

	vAux.ttc_df = (tsPacket->pPayload[37] & 0x40) ? true : false;
	
	vAux.tc_frames = ((((tsPacket->pPayload[37] & 0x30)>>4)*10) + (tsPacket->pPayload[37] & 0x0f));
	
	vAux.tc_seconds = ((((tsPacket->pPayload[38] & 0x70)>>4)*10) + (tsPacket->pPayload[38] & 0x0f));
	
	vAux.tc_minutes = ((((tsPacket->pPayload[39] & 0x70)>>4)*10) + (tsPacket->pPayload[39] & 0x0f));
	
	vAux.tc_hours = ((((tsPacket->pPayload[40] & 0x30)>>4)*10) + (tsPacket->pPayload[40] & 0x0f));
	
	vAux.ds = (tsPacket->pPayload[41] & 0x80) ? true : false;
	
	vAux.tm = (tsPacket->pPayload[41] & 0x40) ? true : false;
		
	if ((tsPacket->pPayload[41] & 0x0f) != 0x0F)
		vAux.recDate_timeZone = ((((tsPacket->pPayload[41] & 0x30)>>4)*10) + (tsPacket->pPayload[41] & 0x0f));
	else
		vAux.recDate_timeZone = 0;
			
	vAux.recDate_day = ((((tsPacket->pPayload[42] & 0x30)>>4)*10) + (tsPacket->pPayload[42] & 0x0f));
	
	vAux.recDate_month = (tsPacket->pPayload[43] & 0x0F);
	
	if (tsPacket->pPayload[43] & 0x10)
		vAux.recDate_month += 10;
	
	vAux.recDate_week = ((tsPacket->pPayload[43] & 0xE0) >> 5);
			
	vAux.recDate_year = ((((tsPacket->pPayload[44] & 0xF0)>>4)*10) + (tsPacket->pPayload[44] & 0x0f));

	if ((tsPacket->pPayload[45] & 0x0f) != 0x0F)
		vAux.recTime_frames = ((((tsPacket->pPayload[45] & 0x30)>>4)*10) + (tsPacket->pPayload[45] & 0x0f));
	else
		vAux.recTime_frames = 0;
		
	vAux.recTime_seconds = ((((tsPacket->pPayload[46] & 0x70)>>4)*10) + (tsPacket->pPayload[46] & 0x0f));
	
	vAux.recTime_minutes = ((((tsPacket->pPayload[47] & 0x70)>>4)*10) + (tsPacket->pPayload[47] & 0x0f));
	
	vAux.recTime_hours = ((((tsPacket->pPayload[48] & 0x30)>>4)*10) + (tsPacket->pPayload[48] & 0x0f));
	
	vAux.copyGenerationManagementSystem = ((tsPacket->pPayload[49] & 0xC0) >> 6);
	
	vAux.rec_st = (tsPacket->pPayload[49] & 0x20) ? true : false;

	vAux.atn_bf = (tsPacket->pPayload[49] & 0x10) ? true : false;
	
	for (i=0;i<5;i++)
		vAux.extendedDVPack1[i] = tsPacket->pPayload[50+i];
		
	for (i=0;i<5;i++)
		vAux.extendedDVPack2[i] = tsPacket->pPayload[55+i];

	for (i=0;i<5;i++)
		vAux.extendedDVPack3[i] = tsPacket->pPayload[60+i];
	
	// Parse the rest of this TS packet, looking for a DV multi-pack containing Maker Code and Imaging Frame Rate
	unsigned char *pOtherAuxPacks = &tsPacket->pPayload[65];
	while ((pOtherAuxPacks < (tsPacket->pPacket+188)) && (vAux.hasMakerCodeAndImagingFrameRate == false))
	{
		// See if we found the DV multi-pack containing Maker Code and Imaging Frame Rate
		if ((pOtherAuxPacks[0] == 0x48) &&	// Keyword is a multi-pack, valid for all PES-V in Pack-V
			(pOtherAuxPacks[1] >= 0x0A) &&  // At least 2 DV packs in the multi-pack
			(pOtherAuxPacks[2] == 0xF0) &&  // First DV pack is keyword 0xF0
			(pOtherAuxPacks[7] == 0xF1) &&  // Second DV pack is keyword 0xF1
			((pOtherAuxPacks+8) < (tsPacket->pPacket+188))) // Verify we don't go beyond the end of this TS packet.
		{
			vAux.hasMakerCodeAndImagingFrameRate = true;
			vAux.makerCode = pOtherAuxPacks[3];  
			vAux.imagingFrameRate = pOtherAuxPacks[8];
		}

		// Bump past this AUX
		if (pOtherAuxPacks[0] < 0x40)
			pOtherAuxPacks += 5;	// All AUX with keyword less than 0x40 are 5 bytes long!
		else if (pOtherAuxPacks[0] < 0x80) 
			pOtherAuxPacks += (2+pOtherAuxPacks[1]); // All AUX with keyword from 0x40 thru 0x7F have length in second byte.
		else 
			break; // Break out here, since AUX Keyword byte value is invalid!
	}

	delete tsPacket;
}

/////////////////////////////////////////////////////////////
// GetNextPESPacketBuf
/////////////////////////////////////////////////////////////
IOReturn TSDemuxer::GetNextPESPacketBuf(TSDemuxerStreamType streamType, PESPacketBuf* *ppPacketBuf)
{
	// Local Vars
	IOReturn result =  kIOReturnSuccess;
	PESPacketBuf* pPacketBuf = nil;
	CFArrayCallBacks arrayCallbacks;

	// Take the mutex lock
    pthread_mutex_lock(&queueProtectMutex);

	if (streamType == kTSDemuxerStreamTypeVideo)
	{
		if (pVideoPESBufQueueHead != nil)
		{
			pPacketBuf = pVideoPESBufQueueHead;
			pVideoPESBufQueueHead = pPacketBuf->pNext;
			pPacketBuf->pNext = nil;
		}
		else
		{
			// Allocate a new PES buffer struct since there weren't any in the queue
			if (logger)
				logger->log("TSDemuxer Info: Needed to allocate another Video PES Buffer\n");
			pPacketBuf = new PESPacketBuf;
			if (pPacketBuf)
			{
				pPacketBuf->pNext = nil;
				pPacketBuf->streamType = kTSDemuxerStreamTypeVideo;
				pPacketBuf->pTSDemuxer = this;
				pPacketBuf->tsPacketArray = nil;
				pPacketBuf->pPESBuf = new UInt8[videoPESBufSize];
				if (!pPacketBuf->pPESBuf)
					result = kIOReturnNoMemory;
			}
			else
			{
				result = kIOReturnNoMemory;
				if (logger)
					logger->log("TSDemuxer Info: Video PES Buffer Allocate - No Memory\n");
			}
				
		}
	}
	else if (streamType == kTSDemuxerStreamTypeAudio)
	{
		if (pAudioPESBufQueueHead != nil)
		{
			pPacketBuf = pAudioPESBufQueueHead;
			pAudioPESBufQueueHead = pPacketBuf->pNext;
			pPacketBuf->pNext = nil;
		}
		else
		{
			// Allocate a new PES buffer struct since there weren't any in the queue
			if (logger)
				logger->log("TSDemuxer Info: Needed to allocate another Audio PES Buffer\n");
			pPacketBuf = new PESPacketBuf;
			if (pPacketBuf)
			{
				pPacketBuf->pNext = nil;
				pPacketBuf->streamType = kTSDemuxerStreamTypeAudio;
				pPacketBuf->pTSDemuxer = this;
				pPacketBuf->tsPacketArray = nil;
				pPacketBuf->pPESBuf = new UInt8[audioPESBufSize];
				if (!pPacketBuf->pPESBuf)
					result = kIOReturnNoMemory;
			}
			else
			{
				result = kIOReturnNoMemory;
				if (logger)
					logger->log("TSDemuxer Info: Audio PES Buffer Allocate - No Memory\n");
			}
		}
	}
	else
		result =  kIOReturnBadArgument;

	if (configurationBits & kDemuxerConfig_KeepTSPackets)
	{
		// Create an array to hold the raw TS packets
		arrayCallbacks.version = 0;
		arrayCallbacks.retain = NULL;
		arrayCallbacks.copyDescription = NULL;
		arrayCallbacks.equal = NULL;
		arrayCallbacks.release = cfArrayReleaseTSPacketBuf;
		pPacketBuf->tsPacketArray = CFArrayCreateMutable(NULL,0,&arrayCallbacks);
	}
	
	*ppPacketBuf = pPacketBuf;

	// Release the mutex lock
	pthread_mutex_unlock(&queueProtectMutex);

	return result ;
}

/////////////////////////////////////////////////////////////
// ReleasePESPacketBuf
/////////////////////////////////////////////////////////////
IOReturn TSDemuxer::ReleasePESPacketBuf(PESPacketBuf* pPacketBuf)
{
	// Local Vars
	IOReturn result =  kIOReturnSuccess;

	// Take the mutex lock
    pthread_mutex_lock(&queueProtectMutex);

	if (pPacketBuf->tsPacketArray != nil)
	{	
		CFRelease(pPacketBuf->tsPacketArray);
		pPacketBuf->tsPacketArray = nil;
	}
		
	if (pPacketBuf->streamType == kTSDemuxerStreamTypeVideo)
	{
		pPacketBuf->pNext = pVideoPESBufQueueHead;
		pVideoPESBufQueueHead = pPacketBuf;
	}
	else if (pPacketBuf->streamType == kTSDemuxerStreamTypeAudio)
	{
		pPacketBuf->pNext = pAudioPESBufQueueHead;
		pAudioPESBufQueueHead = pPacketBuf;
	}
	else
		result =  kIOReturnBadArgument;
	
	// Release the mutex lock
	pthread_mutex_unlock(&queueProtectMutex);

	return result;
}

/////////////////////////////////////////////////////////////
// DoPESCallback
/////////////////////////////////////////////////////////////
void TSDemuxer::DoPESCallback(TSDemuxerMessage msg)
{
	if (PESCallback)
	{
		PESPacket[currentStreamType]->streamType = currentStreamType;
		PESPacket[currentStreamType]->pid = (currentStreamType == kTSDemuxerStreamTypeVideo) ? programVideoPid : programAudioPid;
		PESPacket[currentStreamType]->pesBufLen = PESPacketPos[currentStreamType];
		
		// Callback the user
		PESCallback(msg,PESPacket[currentStreamType],pPESCallbackProcRefCon);
		
		// Get another buffer
		GetNextPESPacketBuf(currentStreamType, &PESPacket[currentStreamType]);
	}
	
	// Reset the tsPacketsSinceLastClientCallback
	tsPacketsSinceLastClientCallback = 0;
}

////////////////////////////////////////////////////
// cfArrayReleaseTSPacketBuf
////////////////////////////////////////////////////
static void cfArrayReleaseTSPacketBuf(CFAllocatorRef allocator,const void *ptr)
{
	// Delete the TS packet buffer
	delete [] (UInt8*) ptr;
	return;
}

} // namespace AVS