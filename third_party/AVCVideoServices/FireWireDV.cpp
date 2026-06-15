/*
	File:		FireWireDV.cpp

    Synopsis: This file implements the helper functions for the FireWire DV framework. 
 
	Copyright: 	© Copyright 2001-2003 Apple Computer, Inc. All rights reserved.

	Written by: ayanowitz

 Disclaimer:	IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
 ("Apple") in consideration of your agreement to the following terms, and your
 use, installation, modification or redistribution of this Apple software
 constitutes acceptance of these terms.  If you do not agree with these terms,
 please do not use, install, modify or redistribute this Apple software.

 In consideration of your agreement to abide by the following terms, and subject
 to these terms, Apple grants you a personal, non-exclusive license, under AppleÕs
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

//
// DV Formats
// Note: Only the 1x speed formats are included in this table.
//
DVFormats dvFormats[] =
{
// size    mode  dbs   fn    sytoffset
  {72000,  0x84, 0x3C, 0x00, 0x02},  // SDL_625_50
  {60000,  0x04, 0x3C, 0x00, 0x02},  // SDL_525_60
  {144000, 0x80, 0x78, 0x00, 0x02},  // SD_625_50
  {120000, 0x00, 0x78, 0x00, 0x02},  // SD_525_60
  {144000, 0xF8, 0x78, 0x00, 0x03},  // DVCPro25_625_50
  {120000, 0x78, 0x78, 0x00, 0x03},  // DVCPro25_525_60
  {288000, 0xF4, 0x78, 0x01, 0x03},  // DVCPro50_625_50
  {240000, 0x74, 0x78, 0x01, 0x03},  // DVCPro50_525_60
  {288000, 0x88, 0xF0, 0x00, 0x03},  // HD_1250_50
  {240000, 0x08, 0xF0, 0x00, 0x03},  // HD_1125_60
  {576000, 0xF0, 0x78, 0x02, 0x06},  // DVCPro100_50
  {480000, 0x70, 0x78, 0x02, 0x06},  // DVCPro100_60
  {0,0,0,0} // Terminator: Don't eliminate this!
};

// Thread parameter structures
struct DVTransmitterThreadParams
{
	volatile bool threadReady;
	DVTransmitter *pTransmitter;
	DVFramePullProc framePullProcHandler;
	void *pFramePullProcRefCon;
	DVFrameReleaseProc frameReleaseProcHandler;
	void *pFrameReleaseProcRefCon;
	DVTransmitterMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	bool doIRMAllocations;
	UInt8 transmitterDVMode;
	UInt32 numFrameBuffers;
};

struct DVReceiverThreadParams
{
	volatile bool threadReady;
	DVReceiver *pReceiver;
	DVFrameReceivedProc frameReceivedProcHandler;
	void *pFrameReceivedProcRefCon;
	DVReceiverMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	bool doIRMAllocations;
	UInt8 receiverDVMode;
	UInt32 numFrameBuffers;
};

// Prototypes for static functions in this file
static void *DVTransmitterRTThreadStart(DVTransmitterThreadParams* pParams);
static void *DVReceiverRTThreadStart(DVReceiverThreadParams* pParams);

//////////////////////////////////////////////////////
// CreateDVTransmitter
//////////////////////////////////////////////////////
IOReturn CreateDVTransmitter(DVTransmitter **ppTransmitter,
							 DVFramePullProc framePullProcHandler,
							 void *pFramePullProcRefCon,
							 DVFrameReleaseProc frameReleaseProcHandler,
							 void *pFrameReleaseProcRefCon,
							 DVTransmitterMessageProc messageProcHandler,
							 void *pMessageProcRefCon,
							 StringLogger *stringLogger,
							 IOFireWireLibNubRef nubInterface,
							 unsigned int cyclesPerSegment,
							 unsigned int numSegments,
							 UInt8 transmitterDVMode,
							 UInt32 numFrameBuffers,
							 bool doIRMAllocations)
{
	DVTransmitterThreadParams threadParams;
	pthread_t rtThread;
	pthread_attr_t threadAttr;

	threadParams.threadReady = false;
	threadParams.pTransmitter = nil;
	threadParams.framePullProcHandler = framePullProcHandler;
	threadParams.frameReleaseProcHandler = frameReleaseProcHandler;
	threadParams.messageProcHandler = messageProcHandler;
	threadParams.stringLogger = stringLogger;
	threadParams.nubInterface = nubInterface;
	threadParams.cyclesPerSegment = cyclesPerSegment;
	threadParams.numSegments = numSegments;
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.transmitterDVMode = transmitterDVMode;
	threadParams.numFrameBuffers = numFrameBuffers;
	threadParams.pFramePullProcRefCon = pFramePullProcRefCon;
	threadParams.pFrameReleaseProcRefCon = pFrameReleaseProcRefCon;
	threadParams.pMessageProcRefCon = pMessageProcRefCon;
	
	// Create the real-time thread which will instantiate and setup new FireWireDV object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))DVTransmitterRTThreadStart, &threadParams);
	pthread_attr_destroy(&threadAttr);

	// Wait forever for the new thread to be ready
	while (threadParams.threadReady == false) usleep(1000);

	*ppTransmitter = threadParams.pTransmitter;
	
	if (threadParams.pTransmitter)
		return kIOReturnSuccess;
	else
		return kIOReturnError;
}

//////////////////////////////////////////////////////
// DestroyDVTransmitter
//////////////////////////////////////////////////////
IOReturn DestroyDVTransmitter(DVTransmitter *pTransmitter)
{
	IOReturn result = kIOReturnSuccess ;
	CFRunLoopRef runLoopRef;

	// Save the ref to the run loop the transmitter is using
	runLoopRef = pTransmitter->runLoopRef;

	// Delete transmitter object
	delete pTransmitter;

	// Stop the run-loop in the RT thread. The RT thread will then terminate
	CFRunLoopStop(runLoopRef);

	return result;
}

//////////////////////////////////////////////////////////////////////
// DVTransmitterRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *DVTransmitterRTThreadStart(DVTransmitterThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	DVTransmitter *transmitter;

	// Instantiate a new receiver object
	transmitter = new DVTransmitter(pParams->stringLogger,
								 pParams->nubInterface,
								 pParams->transmitterDVMode,
								 pParams->numFrameBuffers,

								 pParams->cyclesPerSegment,
								 pParams->numSegments,
								 pParams->doIRMAllocations);

	// Setup the receiver object
	if (transmitter)
		result = transmitter->setupIsocTransmitter();

	// Install the data pull proc
	if ((transmitter) && (result == kIOReturnSuccess))
		transmitter->registerFrameCallbacks(pParams->framePullProcHandler,
									  pParams->pFramePullProcRefCon,
									  pParams->frameReleaseProcHandler,
									  pParams->pFrameReleaseProcRefCon);

	// Install the message handler proc
	if ((transmitter) && (result == kIOReturnSuccess))
		transmitter->registerMessageCallback(pParams->messageProcHandler,pParams->pMessageProcRefCon);

	// Update the return parameter with a pointer to the new receiver object
	if (result == kIOReturnSuccess)
		pParams->pTransmitter = transmitter;
	else
	{
		delete transmitter;
		transmitter = nil;
		pParams->pTransmitter = nil;
	}

	// Boost priority of this thread
	MakeCurrentThreadTimeContraintThread();

	// Signal that this thread is ready
	pParams->threadReady = true;

	// Start the run loop
	if ((transmitter) && (result == kIOReturnSuccess))
		CFRunLoopRun();

	return nil;
}

//////////////////////////////////////////////////////
// CreateDVReceiver
//////////////////////////////////////////////////////
IOReturn CreateDVReceiver(DVReceiver **ppReceiver,
						  DVFrameReceivedProc frameReceivedProcHandler,
						  void *pFrameReceivedProcRefCon,
						  DVReceiverMessageProc messageProcHandler,
						  void *pMessageProcRefCon,
						  StringLogger *stringLogger,
						  IOFireWireLibNubRef nubInterface,
						  unsigned int cyclesPerSegment,
						  unsigned int numSegments,
						  UInt8 receiverDVMode,
						  UInt32 numFrameBuffers,
						  bool doIRMAllocations)
{
	DVReceiverThreadParams threadParams;
	pthread_t rtThread;
	pthread_attr_t threadAttr;

	threadParams.threadReady = false;
	threadParams.pReceiver = nil;
	threadParams.frameReceivedProcHandler = frameReceivedProcHandler;
	threadParams.pFrameReceivedProcRefCon = pFrameReceivedProcRefCon,
	threadParams.messageProcHandler = messageProcHandler;
	threadParams.pMessageProcRefCon = pMessageProcRefCon,
	threadParams.stringLogger = stringLogger;
	threadParams.nubInterface = nubInterface;
	threadParams.cyclesPerSegment = cyclesPerSegment;
	threadParams.numSegments = numSegments;
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.receiverDVMode = receiverDVMode;
	threadParams.numFrameBuffers = numFrameBuffers;

	// Create the real-time thread which will instantiate and setup new FireWireDV object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))DVReceiverRTThreadStart, &threadParams);
	pthread_attr_destroy(&threadAttr);

	// Wait forever for the new thread to be ready
	while (threadParams.threadReady == false) usleep(1000);

	*ppReceiver = threadParams.pReceiver;
	
	if (threadParams.pReceiver)
		return kIOReturnSuccess;
	else
		return kIOReturnError;
}

//////////////////////////////////////////////////////
// DestroyDVReceiver
//////////////////////////////////////////////////////
IOReturn DestroyDVReceiver(DVReceiver *pReceiver)
{
	IOReturn result = kIOReturnSuccess ;
	CFRunLoopRef runLoopRef;

	// Save the ref to the run loop the receiver is using
	runLoopRef = pReceiver->runLoopRef;

	// Delete receiver object
	delete pReceiver;

	// Stop the run-loop in the RT thread. The RT thread will then terminate
	CFRunLoopStop(runLoopRef);

	return result;
}

//////////////////////////////////////////////////////////////////////
// DVReceiverRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *DVReceiverRTThreadStart(DVReceiverThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	DVReceiver *receiver;
	DVFrameNotifyInst* pNotifyInst;

	// Instantiate a new receiver object
	receiver = new DVReceiver(pParams->stringLogger,
						   pParams->nubInterface,
						   pParams->numFrameBuffers,
						   pParams->receiverDVMode,
						   pParams->cyclesPerSegment,
						   pParams->numSegments,
						   pParams->doIRMAllocations);

	// Setup the receiver object
	if (receiver)
		result = receiver->setupIsocReceiver();

	// Install the Frame received callback
	if ((receiver) && (result == kIOReturnSuccess))
		receiver->registerFrameReceivedCallback(pParams->frameReceivedProcHandler,pParams->pFrameReceivedProcRefCon,&pNotifyInst);

	// Install the message handler proc
	if ((receiver) && (result == kIOReturnSuccess))
		receiver->registerMessageCallback(pParams->messageProcHandler,pParams->pMessageProcRefCon);

	// Update the return parameter with a pointer to the new receiver object
	if (result == kIOReturnSuccess)
		pParams->pReceiver = receiver;
	else
	{
		delete receiver;
		receiver = nil;
		pParams->pReceiver = nil;
	}

	// Boost priority of this thread
	MakeCurrentThreadTimeContraintThread();

	// Signal that this thread is ready
	pParams->threadReady = true;

	// Start the run loop
	if ((receiver) && (result == kIOReturnSuccess))
		CFRunLoopRun();

	return nil;
}

///////////////////////////////////////////////////////////////////////////////////////
// GetDVModeFromFrameData - Requires a pointer to at least 480 bytes of the frame data
///////////////////////////////////////////////////////////////////////////////////////
IOReturn GetDVModeFromFrameData(UInt8 *pDVFrameData, UInt8 *pDVMode, UInt32 *pFrameSize, UInt32 *pSourcePacketSize)
{
	bool found = false;
	UInt8 *pPack;
	UInt32 vaux,pack;
	UInt8 dvMode;
	UInt32 i = 0;
	DVFormats *pDVFormat = nil;
	UInt8 apt;
	
	// Pre-initialize the DV mode to an illegal value!
	dvMode = 0xFF;
	
	// First, validate that the frame data indeed looks like a valid DV frame
	// We do this by looking at the section-type field of the first 6 DIF block sections!
	if (((pDVFrameData[0] & 0xF0) != 0x10) || ((pDVFrameData[80] & 0xF0) != 0x30) || ((pDVFrameData[160] & 0xF0) != 0x30) || ((pDVFrameData[240] & 0xF0) != 0x50) ||
		((pDVFrameData[320] & 0xF0) != 0x50) || ((pDVFrameData[400] & 0xF0) != 0x50))
		return kIOReturnError;

	// Extract the APT field from the header dif block. It's used
	// to distinguish DV25 NTSC/PAL from DVCPro25 NTSC/PAL.
	apt = pDVFrameData[4] & 0x07;
	
	// Search for VS pack data in all the VAUX sections
	for (vaux=0;vaux<3;vaux++)
	{
		for (pack=0;pack<15;pack++)
		{
			pPack = &pDVFrameData[243+(vaux*80)+(pack*5)];
			
			// The VS pack has a pack header of 0x60!
			if (pPack[0] == 0x60)
			{
				found = true;
				break;
			}
		}
		if (found == true)
			break;
	}
	
	// If we found the VS pack, determine the DV mode
	if (found == true)
	{
		switch (pPack[3] & 0x3F)
		{
			// DV25 - NTSC (or DVCPro25-NTSC)
			case 0x00:
				if (apt)
					dvMode = kFWAVCDVMode_DVCPro25_525_60; 
				else
					dvMode = kFWAVCDVMode_SD_525_60;
				break;
				
			// DV25 - PAL (or DVCPro25-PAL)
			case 0x20:
				if (apt)
					dvMode = kFWAVCDVMode_DVCPro25_625_50; 
				else
					dvMode = kFWAVCDVMode_SD_625_50; 
				break;

			// SDL - NTSC
			case 0x01:
				dvMode = kFWAVCDVMode_SDL_525_60; 
				break;
				
			// SDL - PAL
			case 0x21:
				dvMode = kFWAVCDVMode_SDL_625_50; 
				break;
				
			// 1125-60
			case 0x02:
				dvMode = kFWAVCDVMode_HD_1125_60; 
				break;
				
			// 11250-50
			case 0x22:
				dvMode = kFWAVCDVMode_HD_1250_50; 
				break;
				
			// DVCPro50 - NTSC
			case 0x04:
				dvMode = kFWAVCDVMode_DVCPro50_525_60; 
				break;
				
			// DVCPro50 - PAL
			case 0x24:
				dvMode = kFWAVCDVMode_DVCPro50_625_50; 
				break;
			
			// DVCProHD - NTSC	
			case 0x14:
			case 0x15:
			case 0x18:
				dvMode = kFWAVCDVMode_DVCPro100_60; 
				break;
				
			// DVCProHD - PAL
			case 0x34:
			case 0x35:
			case 0x38:
				dvMode = kFWAVCDVMode_DVCPro100_50; 
				break;
				
			default:
				break;
		};
	}
	else
		return kIOReturnError;

	if (dvMode == 0xFF)
		return kIOReturnUnsupportedMode;
	else
	{
		// Locate format information regarding this dvMode
		// so we can report the frame-size, and the 
		// source-packet-size.
		DVFormats* pFormat = &dvFormats[i];
		
		while (pFormat->frameSize != 0)
		{
			if ((dvMode & 0xFC) == pFormat->mode)
			{
				pDVFormat = pFormat;
				break;
			}
			i+=1;
			pFormat = &dvFormats[i];
		};
		
		if (pDVFormat)	// Since we set the dvMode (above), this should never be null here!
		{
			*pDVMode = dvMode;
			*pFrameSize = pDVFormat->frameSize;
			*pSourcePacketSize = (pDVFormat->dbs*4)*(1 << pDVFormat->fn);
			return kIOReturnSuccess;
		}
		else
		{
			return kIOReturnError;
		}
	}
}

} // namespace AVS
