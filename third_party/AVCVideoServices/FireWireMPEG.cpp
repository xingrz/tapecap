/*
	File:		FireWireMPEG.cpp

    Synopsis: This file implements the helper functions for the FireWire MPEG framework. 
 
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

// Thread parameter structures
struct MPEG2ReceiverThreadParams
{
	volatile bool threadReady;
	MPEG2Receiver *pReceiver;
	DataPushProc dataPushProcHandler;
	void *pDataPushProcRefCon;
	MPEG2ReceiverMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	bool doIRMAllocations;
};

struct MPEG2TransmitterThreadParams
{
	volatile bool threadReady;
	MPEG2Transmitter *pTransmitter;
	DataPullProc dataPullProcHandler;
	void *pDataPullProcRefCon;
	MPEG2TransmitterMessageProc messageProcHandler;
	void *pMessageProcRefCon;
	StringLogger *stringLogger;
	IOFireWireLibNubRef nubInterface;
	unsigned int cyclesPerSegment;
	unsigned int numSegments;
	bool doIRMAllocations;
	unsigned int packetsPerCycle;
	unsigned int tsPacketQueueSizeInPackets;
};

// Prototypes for static functions in this file
static void *MPEG2ReceiverRTThreadStart(MPEG2ReceiverThreadParams* pParams);
static void *MPEG2TransmitterRTThreadStart(MPEG2TransmitterThreadParams* pParams);

//////////////////////////////////////////////////////
// CreateMPEG2Receiver
//////////////////////////////////////////////////////
IOReturn CreateMPEG2Receiver(MPEG2Receiver **ppReceiver,
							 DataPushProc dataPushProcHandler,
							 void *pDataPushProcRefCon,
							 MPEG2ReceiverMessageProc messageProcHandler,
							 void *pMessageProcRefCon,
							 StringLogger *stringLogger,
							 IOFireWireLibNubRef nubInterface,
							 unsigned int cyclesPerSegment,
							 unsigned int numSegments,
							 bool doIRMAllocations)
{
	MPEG2ReceiverThreadParams threadParams;
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
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.pDataPushProcRefCon = pDataPushProcRefCon;
	threadParams.pMessageProcRefCon = pMessageProcRefCon;

	// Create the real-time thread which will instantiate and setup new FireWireMPEG object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))MPEG2ReceiverRTThreadStart, &threadParams);
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
// DestroyMPEG2Receiver
//////////////////////////////////////////////////////
IOReturn DestroyMPEG2Receiver(MPEG2Receiver *pReceiver)
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

//////////////////////////////////////////////////////
// CreateMPEG2Transmitter
//////////////////////////////////////////////////////
IOReturn CreateMPEG2Transmitter(MPEG2Transmitter **ppTransmitter,
								DataPullProc dataPullProcHandler,
								void *pDataPullProcRefCon,
								MPEG2TransmitterMessageProc messageProcHandler,
								void *pMessageProcRefCon,
								StringLogger *stringLogger,
								IOFireWireLibNubRef nubInterface,
								unsigned int cyclesPerSegment,
								unsigned int numSegments,
								bool doIRMAllocations,
								unsigned int packetsPerCycle,
								unsigned int tsPacketQueueSizeInPackets)
{
	MPEG2TransmitterThreadParams threadParams;
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
	threadParams.doIRMAllocations = doIRMAllocations;
	threadParams.packetsPerCycle = 	packetsPerCycle;
	threadParams.pDataPullProcRefCon = pDataPullProcRefCon;
	threadParams.pMessageProcRefCon = pMessageProcRefCon;	
	threadParams.tsPacketQueueSizeInPackets = tsPacketQueueSizeInPackets;
	
	// Create the real-time thread which will instantiate and setup new FireWireMPEG object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))MPEG2TransmitterRTThreadStart, &threadParams);
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
// DestroyMPEG2Transmitter
//////////////////////////////////////////////////////
IOReturn DestroyMPEG2Transmitter(MPEG2Transmitter *pTransmitter)
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
// MPEG2ReceiverRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *MPEG2ReceiverRTThreadStart(MPEG2ReceiverThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	MPEG2Receiver *receiver;
	
	// Instantiate a new receiver object
	receiver = new MPEG2Receiver(pParams->stringLogger,
							  pParams->nubInterface,
							  pParams->cyclesPerSegment,
							  pParams->numSegments,
							  pParams->doIRMAllocations);
							  
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

//////////////////////////////////////////////////////////////////////
// MPEG2TransmitterRTThreadStart
//////////////////////////////////////////////////////////////////////
static void *MPEG2TransmitterRTThreadStart(MPEG2TransmitterThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	MPEG2Transmitter *transmitter;

	// Instantiate a new receiver object
	transmitter = new MPEG2Transmitter(pParams->stringLogger,
									pParams->nubInterface,
									pParams->cyclesPerSegment,
									pParams->numSegments,
									pParams->doIRMAllocations,
									pParams->packetsPerCycle,
									pParams->tsPacketQueueSizeInPackets);

	// Setup the receiver object
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

} // namespace AVS
