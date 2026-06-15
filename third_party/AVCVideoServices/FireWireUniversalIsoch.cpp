/*
	File:		FireWireUniversalIsoch.cpp
 
 Synopsis: This is the source file for the FireWire Universal Isoch support. 
 
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

// Thread parameter structures
struct UniversalReceiverThreadParams
{
	volatile bool threadReady;
	UniversalReceiver *pReceiver;
	UniversalDataPushProc dataPushProcHandler;
	void *pDataPushProcRefCon;
	UniversalReceiverMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	unsigned int cycleBufferSize;
	unsigned int irmAllocationPacketLen;
	bool doIRMAllocations;
};

struct UniversalTransmitterThreadParams
{
	volatile bool threadReady;
	UniversalTransmitter *pTransmitter;
	UniversalTransmitterDataPullProc dataPullProcHandler;
	void *pDataPullProcRefCon;
	UniversalTransmitterMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	unsigned int clientTransmitBufferSize;
	unsigned int irmAllocationMaxPacketSize;
	bool doIRMAllocations;
	unsigned int numStartupCycleMatchBits;
};

static void *UniversalReceiverRTThreadStart(UniversalReceiverThreadParams* pParams);
static void *UniversalTransmitterRTThreadStart(UniversalTransmitterThreadParams* pParams);

//////////////////////////////////////////////////////
// CreateUniversalReceiver
//////////////////////////////////////////////////////
IOReturn CreateUniversalReceiver(UniversalReceiver **ppReceiver,
								 UniversalDataPushProc dataPushProcHandler,
								 void *pDataPushProcRefCon,
								 UniversalReceiverMessageProc messageProcHandler,
								 void *pMessageProcRefCon,
								 StringLogger *stringLogger,
								 IOFireWireLibNubRef nubInterface,
								 unsigned int cyclesPerSegment,
								 unsigned int numSegments,
								 unsigned int cycleBufferSize,
								 bool doIRMAllocations,
								 unsigned int irmAllocationPacketLen)
{
	UniversalReceiverThreadParams threadParams;
	pthread_t rtThread;
	pthread_attr_t threadAttr;
	
	threadParams.threadReady = false;
	threadParams.pReceiver = nil;
	threadParams.dataPushProcHandler = dataPushProcHandler;
	threadParams.messageProcHandler = messageProcHandler;
	threadParams.stringLogger = stringLogger;
	threadParams.nubInterface = nubInterface;
	threadParams.cyclesPerSegment = cyclesPerSegment;
	threadParams.numSegments = numSegments;
	threadParams.cycleBufferSize = cycleBufferSize;
	threadParams.irmAllocationPacketLen = irmAllocationPacketLen;
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.pDataPushProcRefCon = pDataPushProcRefCon;
	threadParams.pMessageProcRefCon = pMessageProcRefCon;
	
	// Create the real-time thread which will instantiate and setup new UniversalReceiver object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))UniversalReceiverRTThreadStart, &threadParams);
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
// DestroyUniversalReceiver
//////////////////////////////////////////////////////
IOReturn DestroyUniversalReceiver(UniversalReceiver *pReceiver)
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
// UniversalReceiverRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *UniversalReceiverRTThreadStart(UniversalReceiverThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	UniversalReceiver *receiver;
	
	// Instantiate a new receiver object
	receiver = new UniversalReceiver(pParams->stringLogger,
									 pParams->nubInterface,
									 pParams->cyclesPerSegment,
									 pParams->numSegments,
									 pParams->cycleBufferSize,
									 pParams->doIRMAllocations,
									 pParams->irmAllocationPacketLen);
	
	// Setup the receiver object
	if (receiver)
		result = receiver->setupIsocReceiver();
	
	// Install the data push proc
	if ((receiver) && (result == kIOReturnSuccess))
		receiver->registerDataPushCallback(pParams->dataPushProcHandler,pParams->pDataPushProcRefCon);
	
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

//////////////////////////////////////////////////////////////////////////////////
//
// Prototypes for UniversalTransmitter object creation/destruction helper functions.
// These functions create/prepare or destroy both a UniversalTransmitter class object, 
// as well as a dedicated real-time thread for the object's callbacks and DCL
// processing.
//
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// CreateUniversalTransmitter
//////////////////////////////////////////////////////////////////////
IOReturn CreateUniversalTransmitter(UniversalTransmitter **ppTransmitter,
									UniversalTransmitterDataPullProc dataPullProcHandler,
									void *pDataPullProcRefCon,
									UniversalTransmitterMessageProc messageProcHandler,
									void *pMessageProcRefCon,
									StringLogger *stringLogger,
									IOFireWireLibNubRef nubInterface,
									unsigned int cyclesPerSegment,
									unsigned int numSegments,
									unsigned int clientTransmitBufferSize,
									bool doIRMAllocations,
									unsigned int irmAllocationMaxPacketSize,
									unsigned int numStartupCycleMatchBits)
{
	UniversalTransmitterThreadParams threadParams;
	pthread_t rtThread;
	pthread_attr_t threadAttr;
	
	threadParams.threadReady = false;
	threadParams.pTransmitter = nil;
	threadParams.dataPullProcHandler = dataPullProcHandler;
	threadParams.messageProcHandler = messageProcHandler;
	threadParams.stringLogger = stringLogger;
	threadParams.nubInterface = nubInterface;
	threadParams.cyclesPerSegment = cyclesPerSegment;
	threadParams.numSegments = numSegments;
	threadParams.clientTransmitBufferSize = clientTransmitBufferSize;
	threadParams.irmAllocationMaxPacketSize = irmAllocationMaxPacketSize;
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.pDataPullProcRefCon = pDataPullProcRefCon;
	threadParams.pMessageProcRefCon = pMessageProcRefCon;
	threadParams.numStartupCycleMatchBits = numStartupCycleMatchBits;
	
	// Create the real-time thread which will instantiate and setup new UniversalReceiver object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))UniversalTransmitterRTThreadStart, &threadParams);
	pthread_attr_destroy(&threadAttr);
	
	// Wait forever for the new thread to be ready
	while (threadParams.threadReady == false) usleep(1000);
	
	*ppTransmitter = threadParams.pTransmitter;
	
	if (threadParams.pTransmitter)
		return kIOReturnSuccess;
	else
		return kIOReturnError;
}

//////////////////////////////////////////////////////////////////////
// DestroyUniversalTransmitter
//////////////////////////////////////////////////////////////////////
IOReturn DestroyUniversalTransmitter(UniversalTransmitter *pTransmitter)
{
	IOReturn result = kIOReturnSuccess ;
	CFRunLoopRef runLoopRef;
	
	// Save the ref to the run loop the receiver is using
	runLoopRef = pTransmitter->runLoopRef;
	
	// Delete transmitter object
	delete pTransmitter;
	
	// Stop the run-loop in the RT thread. The RT thread will then terminate
	CFRunLoopStop(runLoopRef);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *UniversalTransmitterRTThreadStart(UniversalTransmitterThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	UniversalTransmitter *transmitter;
	
	// Instantiate a new receiver object
	transmitter = new UniversalTransmitter(pParams->stringLogger,
										   pParams->nubInterface,
										   pParams->cyclesPerSegment,
										   pParams->numSegments,
										   pParams->clientTransmitBufferSize,
										   pParams->doIRMAllocations,
										   pParams->irmAllocationMaxPacketSize,
										   pParams->numStartupCycleMatchBits);
	
	// Setup the transmitter object
	if (transmitter)
		result = transmitter->setupIsocTransmitter();
	
	// Install the data push proc
	if ((transmitter) && (result == kIOReturnSuccess))
		transmitter->registerDataPullCallback(pParams->dataPullProcHandler,pParams->pDataPullProcRefCon);
	
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

//////////////////////////////////////////////////////////////////////
// IsCIPPacket
//////////////////////////////////////////////////////////////////////
bool IsCIPPacket(UInt32 isochHeaderValue)
{
	if (((isochHeaderValue & 0x0000C000) >> 14) == 1)
		return true;
	else 
		return false;
}	

//////////////////////////////////////////////////////////////////////
// ParseCIPPacket
//////////////////////////////////////////////////////////////////////
IOReturn ParseCIPPacket(UInt8 *pPacketPayload, UInt32 payloadLength, CIPPacketParserStruct *pParsedPacket)
{
	IOReturn result = kIOReturnSuccess;
	UInt8 dbs,fn;
	
	// Extract the FMT and FDF
	pParsedPacket->fmt = (pPacketPayload[4] & 0x3F);
	
	// The FDF is either 8-bits or 24-bits depending on the FMT
	if (pPacketPayload[4] & 0x20)
		pParsedPacket->fdf = ((UInt32)pPacketPayload[5] << 16) | ((UInt32)pPacketPayload[6] << 8) | pPacketPayload[7]; 
	else
		pParsedPacket->fdf = pPacketPayload[5]; 
	
	// Extract the DV mode (only applicable to DV format streams)
	pParsedPacket->dvMode = pPacketPayload[5];
	
	// Convert DBS to source-packet size (in bytes)
	dbs = pPacketPayload[1];
	fn = (pPacketPayload[2] >> 6);
	pParsedPacket->sourcePacketSize = (dbs*4)*(1 << fn);

	// Make sure we got a valid sourcePacketSize before using it in our math.
	// This prevents a divide-by-zero crash due to a bad CIP header.
	if (pParsedPacket->sourcePacketSize == 0)
	{
		result =  kIOReturnDeviceError;
	}
	else
	{
		// Determine the number of source packets
		pParsedPacket->numSourcePackets = 	(payloadLength-8)/pParsedPacket->sourcePacketSize;
		
		// Make sure we have enough room in the array of pointers to source packets 
		if (pParsedPacket->numSourcePackets > kMaxSourcePacketsPerCIPPacket)
		{
			result =  kIOReturnNoSpace;
			pParsedPacket->numSourcePackets = kMaxSourcePacketsPerCIPPacket;
		}	
		
		// Setup the array of pointers to source packets
		for (int i=0;i<pParsedPacket->numSourcePackets;i++)
			pParsedPacket->pSourcePacket[i] = &pPacketPayload[8+(i*pParsedPacket->sourcePacketSize)];
		
		// Extract the SYT
		pParsedPacket->syt = ((UInt16)pPacketPayload[6]<<8) | pPacketPayload[7];
	}
	
	return result;
}

} // namespace AVS
