/*
	File:		DVReceiver.cpp

    Synopsis: This is the implementation file for the DVReceiver class.
 
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

static IOReturn RemotePort_GetSupported_Helper(
											   IOFireWireLibIsochPortRef interface,
											   IOFWSpeed *outMaxSpeed,
											   UInt64 *outChanSupported);

static IOReturn RemotePort_AllocatePort_Helper(
											   IOFireWireLibIsochPortRef interface,
											   IOFWSpeed maxSpeed,
											   UInt32 channel);

static IOReturn RemotePort_ReleasePort_Helper(
											  IOFireWireLibIsochPortRef interface);

static IOReturn RemotePort_Start_Helper(
										IOFireWireLibIsochPortRef interface);

static IOReturn RemotePort_Stop_Helper(
									   IOFireWireLibIsochPortRef interface);

static void DVReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand);

static void DVReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand);

static IOReturn DVReceiveFinalizeCallback_Helper( void* refcon ) ;

static UInt32  AddFWCycleTimeToFWCycleTime( UInt32 cycleTime1, UInt32 cycleTime2 );

#ifdef kAVS_Enable_ForceStop_Handler	
static void	DVReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

//////////////////////////////////////////////////////
// NoDataTimeoutHelper
//////////////////////////////////////////////////////
static void NoDataTimeoutHelper(CFRunLoopTimerRef timer, void *data)
{
	DVReceiver *pDVReceiver = (DVReceiver*) data;
	pDVReceiver->NoDataTimeout();
}

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
DVReceiver::DVReceiver(StringLogger *stringLogger,
					   IOFireWireLibNubRef nubInterface,
					   UInt32 numReceiveFrames,
					   UInt8 receiverDVMode,
					   unsigned int cyclesPerSegment,
					   unsigned int numSegments,
					   bool doIRMAllocations)
{
    nodeNubInterface = nubInterface;
	dclCommandPool = nil;
	remoteIsocPort = nil;
	localIsocPort = nil;
	isochChannel = nil;
	receiveChannel = 0;
	receiveSpeed = kFWSpeed100MBit;
	runLoopRef = nil;
	numFrames = numReceiveFrames;
	pDVFormat = nil;
	dvMode = receiverDVMode;
	receiveCycleBufferSize = 0;
	pTimeStamps = nil;
	pCurrentFrame = nil;
	numFrameClients = 0;
	pOverrunReceiveBuffer = 0;
	noDataTimer = nil;
	noDataProc = nil;
	noDataTimeLimitInSeconds = 0.0;
	
	if (stringLogger == nil)
	{
		logger = new StringLogger(nil);	// Throws away all log strings
		noLogger = true;
	}
	else
	{
		logger = stringLogger;
		noLogger = false;
	}

	messageProc = nil;
	pReceiveBuffer = nil;
	transportState = kDVReceiverTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	doIRM = doIRMAllocations;
	receiveSegmentInfo = new DVReceiveSegment[isochSegments];

	// Initialize the frame queue access mutex as a RECURSIVE mutex
	pthread_mutexattr_t attribute;
	pthread_mutexattr_init(&attribute);
	pthread_mutexattr_settype(&attribute,PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&frameQueueMutex,&attribute);
	pthread_mutexattr_destroy(&attribute);
	
	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);

	// Initialize the no-data timer mutex
	pthread_mutex_init(&noDataTimerMutex,NULL);

	// Calculate the size of the DCL command pool needed
	dclCommandPoolSize = ((((isochCyclesPerSegment)*isochSegments)+(isochSegments*4)+16)*32);
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
DVReceiver::~DVReceiver()
{
	DVReceiveFrame* pDeleteFrame;
	DVFrameNotifyInst* pNotifyInst;

	if (transportState != kDVReceiverTransportStopped)
		stopReceive();

	// Remove isoch callback dispatcher from runloop
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->RemoveIsochCallbackDispatcherFromRunLoop(nodeNubInterface);

	// Remove asynch callback dispatcher from runloop
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->RemoveCallbackDispatcherFromRunLoop(nodeNubInterface);
	
	if (isochChannel)
	{
#ifdef kAVS_Enable_ForceStop_Handler	
		// Turn off notification
		(*isochChannel)->TurnOffNotification(isochChannel);
#endif
		(*isochChannel)->Release(isochChannel);
	}
	if (localIsocPort)
		(*localIsocPort)->Release(localIsocPort);

	if (remoteIsocPort)
		(*remoteIsocPort)->Release(remoteIsocPort);

	if (dclCommandPool)
		(*dclCommandPool)->Release(dclCommandPool);

	// Free the vm allocated DCL buffer
	if (pReceiveBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pReceiveBuffer,dclVMBufferSize);

	// If we created an internall logger, free it
	if (noLogger == true)
		delete logger;

	// Free the receive segment info structs
	delete [] receiveSegmentInfo;

	// Delete Frame notify queue
	while (!frameNotifyQueue.empty())
	{
		pNotifyInst = frameNotifyQueue.front();
		frameNotifyQueue.pop_front();
		delete pNotifyInst;
	}
	
	// Delete video frames
	while (!frameQueue.empty())
	{
		// Get a pointer to the element at the head of the queue
		pDeleteFrame = frameQueue.front();

		// Remove it from the queue
		frameQueue.pop_front();

		// Delete the frame buffer
		if (pDeleteFrame->pFrameData)
			delete [] pDeleteFrame->pFrameData;
			
		// Delete the frame struct
		delete pDeleteFrame;
	}
	
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->Release(nodeNubInterface);

	// Release the frame queue mutex
	pthread_mutex_destroy(&frameQueueMutex);
	
	// Release the transport control mutex
	pthread_mutex_destroy(&transportControlMutex);
	
	// Release the no-data timer mutex
	pthread_mutex_destroy(&noDataTimerMutex);

}

//////////////////////////////////////////////////////
// setupIsocReceiver
//////////////////////////////////////////////////////
IOReturn DVReceiver::setupIsocReceiver(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess ;
    UInt8 *pBuffer = nil;
    DCLCommandStruct *pLastDCL = nil;
    DCLCommandPtr *startUpdateDCLList = nil;
	UInt32 updateListCnt;
	UInt32 cycle;
	UInt32 seg;
	UInt32 bufCnt = 0;
	UInt32 i;
	DVReceiveFrame* pFrame;
	UInt32 curUpdateListIndex;
	UInt32 minimumReceiveCycleBufferSize;
	IOFireWireLibNubRef newNubInterface;
	
	// Find the DVFormat info for this mode
	result = FindDVFormatForMode();
	if (result != kIOReturnSuccess)
    {
		logger->log("\nDVReceive Error: Invalid dvMode: 0x%02X\n\n",dvMode);
		return kIOReturnError ;
    }

	pthread_mutex_lock(&frameQueueMutex);

	// Allocate the video frame queue
	for (i=0;i<numFrames;i++)
	{
		pFrame = new DVReceiveFrame;
		if (!pFrame)
		{
			logger->log("\nDVReceiver Error: DV frame struct memory allocation error\n");
			return kIOReturnError ;
		}
		pFrame->frameSYTTime = 0;
		pFrame->frameReceivedTimeStamp = 0;
		pFrame->refCount = 0;
		pFrame->frameBufferSize = 0;
		pFrame->currentOffset = 0;
		pFrame->frameMode = 0;
		
		// Allocate frame buffer memory
		pFrame->pFrameData = (UInt8*) new UInt8[pDVFormat->frameSize];
		if (!pFrame->pFrameData)
		{
			logger->log("\nDVReceiver Error: DV frame buffer memory allocation error\n");
			return kIOReturnError ;
		}
		pFrame->frameLen = pDVFormat->frameSize;

		// Add this frame to the frame queue
		frameQueue.push_back(pFrame);
	}

	pthread_mutex_unlock(&frameQueueMutex);


	// Calculate the size of a receive cycle buffer
	minimumReceiveCycleBufferSize = (16+((pDVFormat->dbs*4)*(1 << pDVFormat->fn)*(1 << DVspeed(dvMode))));

	// Round it up to the nearest power of 2 size
	receiveCycleBufferSize = 1;
	while (receiveCycleBufferSize < minimumReceiveCycleBufferSize)
		receiveCycleBufferSize = (receiveCycleBufferSize << 1);

	// Calculate the size of the VM buffer for the dcl receive packets
	dclVMBufferSize = ((((isochSegments*isochCyclesPerSegment)+1)*receiveCycleBufferSize) + // receive buffers
					(isochSegments*4) +												  // timestamps
					(((isochSegments*isochCyclesPerSegment) * sizeof(DCLCommandPtr))+(isochSegments*4))); // Update list

	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer,dclVMBufferSize, VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nDVReceiver Error: Error allocating isoch receive buffers.\n\n");
		return kIOReturnError ;
    }
	else
		bzero(pBuffer, dclVMBufferSize);

	// Save a pointer to the buffer
	pReceiveBuffer = pBuffer;

	// Calculate the pointer to the DCL overrun receive buffer
	pOverrunReceiveBuffer = (UInt32*) &pBuffer[(isochSegments*isochCyclesPerSegment*receiveCycleBufferSize)];

	// Set the timestamp pointer
	pTimeStamps = (UInt32*) &pBuffer[(((isochSegments*isochCyclesPerSegment)+1)*receiveCycleBufferSize)];

	// Set the memory pointer for the update list
    updateDCLList = (DCLCommandPtr *) &pBuffer[(((isochSegments*isochCyclesPerSegment)+1)*receiveCycleBufferSize)+(isochSegments*4)];
	
	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nDVReceiver Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nDVReceiver Error: Error duplicating device interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
		nodeNubInterface = newNubInterface;
	}

	// Save a reference to the current run loop
	runLoopRef = CFRunLoopGetCurrent();

	// Install a asynch callback dispatcher to this thread's run loop
	result = (*nodeNubInterface)->AddCallbackDispatcherToRunLoop( nodeNubInterface, runLoopRef ) ;
	
	// Install a isoc callback dispatcher to this thread's run loop
	result = (*nodeNubInterface)->AddIsochCallbackDispatcherToRunLoop( nodeNubInterface, runLoopRef ) ;

	// Use the nub interface to create a DCL command pool object
    dclCommandPool = (*nodeNubInterface)->CreateDCLCommandPool(
															   nodeNubInterface,
															   dclCommandPoolSize,
															   CFUUIDGetUUIDBytes( kIOFireWireDCLCommandPoolInterfaceID ));
	if (!dclCommandPool)
    {
		logger->log("\nDVReceiver Error: Error creating Receive DCL command pool: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 true,	// remote is talker
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nDVReceiver Error: Error creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this DVReceiver object in
	// the remote isoch port's refcon variable
	(*remoteIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) remoteIsocPort,this) ;

	// Use the remote port interface to install some callback handlers.
	(*remoteIsocPort)->SetGetSupportedHandler( remoteIsocPort, & RemotePort_GetSupported_Helper );
	(*remoteIsocPort)->SetAllocatePortHandler( remoteIsocPort, & RemotePort_AllocatePort_Helper );
	(*remoteIsocPort)->SetReleasePortHandler( remoteIsocPort, & RemotePort_ReleasePort_Helper );
	(*remoteIsocPort)->SetStartHandler( remoteIsocPort, & RemotePort_Start_Helper );
	(*remoteIsocPort)->SetStopHandler( remoteIsocPort, & RemotePort_Stop_Helper );

	// Create receive segements
	curUpdateListIndex = 0;
	for (seg=0;seg<isochSegments;seg++)
	{
		updateListCnt = 0;

		// Allocate the label for this segment, and save pointer in seg info
		pLastDCL = (*dclCommandPool)->AllocateLabelDCL( dclCommandPool, pLastDCL ) ;
		receiveSegmentInfo[seg].pSegmentLabelDCL = (DCLLabelPtr) pLastDCL;

		if (seg == 0)
		{
			pFirstDCL = pLastDCL;
		}

		for (cycle=0;cycle< isochCyclesPerSegment;cycle++)
		{
			// Allocate receive DCL
			pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, &pBuffer[bufCnt*receiveCycleBufferSize],receiveCycleBufferSize);

			// Save receive DCL ptr into update list
			updateDCLList[curUpdateListIndex] = pLastDCL;

			// Save a pointer to the place in the list where this segment's update list starts
			if (cycle == 0)
			{
				startUpdateDCLList = &updateDCLList[curUpdateListIndex];
				curUpdateListIndex++;
				updateListCnt++;
				pLastDCL = (*dclCommandPool)->AllocatePtrTimeStampDCL(dclCommandPool, pLastDCL, &pTimeStamps[seg]);
				updateDCLList[curUpdateListIndex] = pLastDCL;
			}

			bufCnt++;
			curUpdateListIndex++;
			updateListCnt++;
		}
		
		pLastDCL = (*dclCommandPool)->AllocateUpdateDCLListDCL(dclCommandPool, pLastDCL, startUpdateDCLList, updateListCnt);

		pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, DVReceiveDCLCallback_Helper, (DCLCallProcDataType) this);

		// Jumps to bogus address, for now. We'll fix the real jump location before we start receive
		pLastDCL = (*dclCommandPool)->AllocateJumpDCL(dclCommandPool, pLastDCL,(DCLLabelPtr) pLastDCL);
		receiveSegmentInfo[seg].pSegmentJumpDCL = (DCLJumpPtr) pLastDCL;
	}

	// Allocate Overrun label & callback DCL
	pLastDCL = (*dclCommandPool)->AllocateLabelDCL( dclCommandPool, pLastDCL ) ;
	pDCLOverrunLabel = (DCLLabelPtr) pLastDCL;
	pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, pOverrunReceiveBuffer,receiveCycleBufferSize);
	pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, DVReceiveOverrunDCLCallback_Helper,(DCLCallProcDataType) this);

	// Set the next pointer in the last DCL to nil
    pLastDCL->pNextDCLCommand = nil;

	// Using the nub interface to the local node, create
	// a local isoc port.
	localIsocPort = (*nodeNubInterface)->CreateLocalIsochPort(
														   nodeNubInterface,
														   false,	// local is listener
														   pFirstDCL,
														   0,
														   0,
														   0,
														   nil,
														   0,
														   nil,
														   0,
														   CFUUIDGetUUIDBytes( kIOFireWireLocalIsochPortInterfaceID ));
	if (!localIsocPort)
    {
		logger->log("\nDVReceiver Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	
	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, DVReceiveFinalizeCallback_Helper) ;

	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														receiveCycleBufferSize,
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nDVReceiver Error: Error creating isoch channel object: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Add a listener and a talker to the isoch channel
	result = (*isochChannel)->AddListener(isochChannel,
									   (IOFireWireLibIsochPortRef)localIsocPort ) ;

	result = (*isochChannel)->SetTalker(isochChannel,
									 (IOFireWireLibIsochPortRef) remoteIsocPort ) ;

#ifdef kAVS_Enable_ForceStop_Handler	
	// Set the refcon for the isoch channel
	(*isochChannel)->SetRefCon(isochChannel,this);
	
	// Set the force stop handler
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,DVReceiveForceStopHandler_Helper);
	
	// Turn on notification
	(*isochChannel)->TurnOnNotification(isochChannel);
#endif
	
	// Fixup all the DCL Jump targets
	fixupDCLJumpTargets();

	return result;
}

//////////////////////////////////////////////////////////////////////
// releaseFrame
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::releaseFrame(DVReceiveFrame* pFrame)
{
	pthread_mutex_lock(&frameQueueMutex);

	if (pFrame->refCount == 0)
	{
		// Just a sanity check 
		logger->log("DVReceiver Error: Client releasing frame with refCount of 0\n");
	}
	else
	{
		pFrame->refCount -= 1;
		if (pFrame->refCount == 0)
		{
			// The frame is ours again, add it to our queue
			frameQueue.push_back(pFrame);
		}
	}
	
	pthread_mutex_unlock(&frameQueueMutex);
	
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// getNextQueuedFrame
//////////////////////////////////////////////////////////////////////
DVReceiveFrame*
DVReceiver::getNextQueuedFrame(void)
{
	DVReceiveFrame* pFrame = nil;

	pthread_mutex_lock(&frameQueueMutex);

	if (!frameQueue.empty())
	{
		pFrame = frameQueue.front();
		frameQueue.pop_front();

		// Initialize some frame parameters
		pFrame->currentOffset = 0;
		pFrame->refCount = numFrameClients;
		pFrame->pDVReceiver = this;
		pFrame->pFWAVCPrivateData = nil;
	}

	pthread_mutex_unlock(&frameQueueMutex);
	
	return pFrame;
}

//////////////////////////////////////////////////////////////////////
// registerFrameReceivedCallback
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::registerFrameReceivedCallback(DVFrameReceivedProc handler, void *refCon, DVFrameNotifyInst* *ppNotifyInstance)
{
	DVFrameNotifyInst* pNotifyInst = new DVFrameNotifyInst;
	if (!pNotifyInst)
	{
		return kIOReturnNoMemory;
	}

	pthread_mutex_lock(&frameQueueMutex);

	pNotifyInst->handler = handler;
	pNotifyInst->refCon = refCon;

	// Add the new notify instance struct to notify queue
	frameNotifyQueue.push_back(pNotifyInst);

	numFrameClients += 1;

	pthread_mutex_unlock(&frameQueueMutex);

	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// unregisterFrameReceivedCallback
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::unregisterFrameReceivedCallback(DVFrameNotifyInst* pNotifyInstance)
{
	DVFrameNotifyInst* pNotifyInst;

	pthread_mutex_lock(&frameQueueMutex);

	for (std::deque<DVFrameNotifyInst*>::iterator dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
	{
		pNotifyInst = *dequeIterator;
		if (pNotifyInst == pNotifyInstance)
		{
			frameNotifyQueue.erase(dequeIterator);
			delete pNotifyInst;
			numFrameClients -= 1;
			pthread_mutex_unlock(&frameQueueMutex);
			return kIOReturnSuccess;
		}
	}
	
	pthread_mutex_unlock(&frameQueueMutex);
	return kIOReturnBadArgument;
}


//////////////////////////////////////////////////////////////////////
// registerMessageCallback
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::registerMessageCallback(DVReceiverMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerNoDataNotificationCallback
//////////////////////////////////////////////////////////////////////
IOReturn DVReceiver::registerNoDataNotificationCallback(DVNoDataProc handler, void *pRefCon, UInt32 noDataTimeInMSec)
{
	noDataProc = handler;
	pNoDataProcRefCon = pRefCon;
	noDataTimeLimitInSeconds = (noDataTimeInMSec / 1000.0);
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// fixupDCLJumpTargets
//////////////////////////////////////////////////////////////////////
void DVReceiver::fixupDCLJumpTargets(void)
{
	UInt32 i;

	for (i=0;i<isochSegments;i++)
	{
		if (i != (isochSegments-1))
			(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								   receiveSegmentInfo[i].pSegmentJumpDCL,
								   receiveSegmentInfo[i+1].pSegmentLabelDCL );
		else
			(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								   receiveSegmentInfo[i].pSegmentJumpDCL,
								   pDCLOverrunLabel );
	}
	currentSegment = 0;
}

//////////////////////////////////////////////////////////////////////
// startReceive
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::startReceive(void)
{
	IOReturn result = kIOReturnSuccess ;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	// Make sure we are not already running
	if (transportState == kDVReceiverTransportRecording)
	{
		// Unlock the transport control mutex
		pthread_mutex_unlock(&transportControlMutex);

		// Return a device already open error
		return kIOReturnExclusiveAccess;
	}
	
	// Fixup all the DCL Jump targets
	fixupDCLJumpTargets();
	
	result = (*isochChannel)->AllocateChannel( isochChannel ) ;
	
	if (result == kIOReturnSuccess)
	{
		result = (*isochChannel)->Start( isochChannel ) ;
		if (result != kIOReturnSuccess)
		{
			(*isochChannel)->ReleaseChannel( isochChannel ) ;
		}
		else
		{
			// Update transport state status var
			transportState = kDVReceiverTransportRecording;
			
			// If a registered no-data proc exists, start the timer now
			if (noDataProc)
				startNoDataTimer();
		}
	}

	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// stopReceive
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::stopReceive(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already stopped
	if (transportState == kDVReceiverTransportStopped)
	{
		// Unlock the transport control mutex
		pthread_mutex_unlock(&transportControlMutex);
		
		// Return a device already open error
		return kIOReturnExclusiveAccess;
	}
	
	// Clear the finalize flag for this stream
	finalizeCallbackCalled = false;
	
	// stop/delete the no-data timer if it exists
	stopNoDataTimer();

	result = (*isochChannel)->Stop( isochChannel ) ;
	(*isochChannel)->ReleaseChannel( isochChannel ) ;

	// Update transport state status var
	transportState = kDVReceiverTransportStopped;
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Wait for the finalize callback to fire for this stream
	if (result == kIOReturnSuccess)
		while (finalizeCallbackCalled == false) usleep(1000);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// setReceiveIsochChannel
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::setReceiveIsochChannel(unsigned int chan)
{
	receiveChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////
// DVReceiver::startNoDataTimer
//////////////////////////////////////////////////
void DVReceiver::startNoDataTimer( void )
{
	CFRunLoopTimerContext		context;
	CFAbsoluteTime				time;
	
	// Lock the no-data timer mutex
	pthread_mutex_lock(&noDataTimerMutex);
	
    // stop if necessary
	if ( noDataTimer )
	{
		CFRunLoopTimerInvalidate( noDataTimer );
		CFRelease( noDataTimer );
		noDataTimer = NULL;
	}
	
    context.version             = 0;
    context.info                = this;
    context.retain              = NULL;
    context.release             = NULL;
    context.copyDescription     = NULL;
	
    time = CFAbsoluteTimeGetCurrent() + noDataTimeLimitInSeconds;
	
	noDataTimer = CFRunLoopTimerCreate(NULL,
									   time,
									   0,
									   0,
									   0,
									   (CFRunLoopTimerCallBack)&NoDataTimeoutHelper,
									   &context);
	
	if ( noDataTimer )
		CFRunLoopAddTimer( runLoopRef, noDataTimer, kCFRunLoopDefaultMode );
	
	// Unlock the no-data timer mutex
	pthread_mutex_unlock(&noDataTimerMutex);
}

//////////////////////////////////////////////////
// DVReceiver::stopNoDataTimer
//////////////////////////////////////////////////
void DVReceiver::stopNoDataTimer( void )
{
	// Lock the no-data timer mutex
	pthread_mutex_lock(&noDataTimerMutex);
	
	if ( noDataTimer )
	{
		CFRunLoopTimerInvalidate( noDataTimer );
		CFRelease( noDataTimer );
		noDataTimer = NULL;
	}
	
	// Unlock the no-data timer mutex
	pthread_mutex_unlock(&noDataTimerMutex);
}

//////////////////////////////////////////////////
// DVReceiver::NoDataTimeout
//////////////////////////////////////////////////
void DVReceiver::NoDataTimeout(void)
{
	if (noDataProc)
	{
		noDataProc(pNoDataProcRefCon);
		
		// Re-arm the timer
		startNoDataTimer();
	}
	else
		stopNoDataTimer();
}

//////////////////////////////////////////////////////////////////////
// setReceiveIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::setReceiveIsochSpeed(IOFWSpeed speed)
{
	receiveSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// DVReceiveDCLCallback
//////////////////////////////////////////////////////////////////////
void DVReceiver::DVReceiveDCLCallback(void)
{
    UInt32 segment;
	UInt32 startBuf;
	UInt32 cycle;
	UInt32 payloadLen;
	UInt8* pPacketBuffer;
	UInt32 currentTime;
	bool startOfFrameFound;
	UInt32 dataBlock;
	UInt32 *pBlock;
	UInt32 *pFrameDataWord;
	DVFrameNotifyInst* pNotifyInst;
	IOReturn result;
	UInt32 currentWordOffset;
	UInt32 numBlocks;
	UInt32 expectedPacketSize;
	bool alreadyOnFrameQueue;
	
	UInt8 cip_fmt;
	UInt8 cip_mode;
	UInt16 cip_syt;
	UInt8 cip_dbc;
	UInt8 cip_dbs;
	UInt8 cip_fn;

	bool segmentHasData = false;

	std::deque<DVFrameNotifyInst*>::iterator dequeIterator;

	std::deque<DVReceiveFrame*>::reverse_iterator frameQueueReverseIterator;
	DVReceiveFrame* frameOnQueue;
	
	// See if this callback happened after we stopped
	if (transportState == kDVReceiverTransportStopped)
		return;
	
	//logger->log("DV_DEBUG: DVReceiveDCLCallback: Timestamp 0x%08X\n",pTimeStamps[currentSegment]);
	currentTime = pTimeStamps[currentSegment];

	segment = currentSegment;
	startBuf = segment*isochCyclesPerSegment;
	
	UInt32 *pCycleBuf = (UInt32*) (pReceiveBuffer + (startBuf*receiveCycleBufferSize));
	
	UInt32 nativeCIP0, nativeCIP1;

	for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
	{
		payloadLen = ((*pCycleBuf & 0xFFFF0000) >> 16); // Note that this quadlet is already in machine native byte order!
		pPacketBuffer = (UInt8*) pCycleBuf;
		pPacketBuffer += 4; // Skip over 4 byte 1394 header

		// Extract DV Data from cycle buffers
		if (payloadLen > 8)
		{
			segmentHasData = true;

			// Extract info from the CIP header
			nativeCIP0 = EndianU32_BtoN(pCycleBuf[1]); 
			nativeCIP1 = EndianU32_BtoN(pCycleBuf[2]);
			cip_dbc = nativeCIP0 & 0x000000FF;
			cip_dbs = ((nativeCIP0 & 0x00FF0000) >> 16);
			cip_fn = ((nativeCIP0 & 0x0000C000) >> 14);
			cip_fmt = ((nativeCIP1 & 0x1F000000) >> 24);
			cip_mode = ((nativeCIP1 & 0x00FF0000) >> 16);
			cip_syt = nativeCIP1 & 0x0000FFFF;

			// See if this is the start of a frame
			// This should work for all DV modes
			startOfFrameFound = ((EndianU16_BtoN (*(short *)(pPacketBuffer + 8))  & 0xE0FC ) == 0x0004 );

			if (startOfFrameFound)
			{
				//logger->log("DV_DEBUG: DVReceiveDCLCallback: Start of Frame Detected, Time: 0x%08X\n",currentTime);
				
				// If we have a partial frame now, then we didn't finish it.  Alert the client
				// that the current frame is a corrupted frame. Otherwise, get a frame from the
				// frame queue (if one exists), and start a new frame.
				if (pCurrentFrame)
				{
					pthread_mutex_lock(&frameQueueMutex);
					for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
					{
						pNotifyInst = *dequeIterator;
						pNotifyInst->handler(kDVFrameCorrupted, nil,pNotifyInst->refCon);
					}

					// Put the frame struct back on the queue
					frameQueue.push_back(pCurrentFrame);
					pCurrentFrame = nil;
					
					pthread_mutex_unlock(&frameQueueMutex);
				}

				// Currently, if the received DV stream is not in the same mode as this DVReceive object
				// was initialzied for, we don't process the packets, and we alert the clients via the
				// FrameReceived callback of the invalid mode error. In the future, handle receiving
				// frames for modes other than the initialized mode.
				if (cip_mode != dvMode)
				{
					pthread_mutex_lock(&frameQueueMutex);
					for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
					{
						pNotifyInst = *dequeIterator;
						pNotifyInst->handler(kDVFrameWrongMode, nil,pNotifyInst->refCon);
					}
					
					pthread_mutex_unlock(&frameQueueMutex);
				}
				else
				{
					// Get a frame off the queue, if available
					pCurrentFrame = getNextQueuedFrame();
					if (pCurrentFrame)
					{
						pCurrentFrame->frameLen = pDVFormat->frameSize;
						pCurrentFrame->frameSYTTime = cip_syt;
						pCurrentFrame->frameReceivedTimeStamp = currentTime;
						pCurrentFrame->frameMode = cip_mode;
						pCurrentFrame->currentOffset = 0;
					}
					else
					{
						// Send a dropped frame notification to the clients
						pthread_mutex_lock(&frameQueueMutex);
						for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
						{
							pNotifyInst = *dequeIterator;
							pNotifyInst->handler(kDVFrameDropped, nil,pNotifyInst->refCon);
						}
						pthread_mutex_unlock(&frameQueueMutex);
					}
				}
			}

			if (pCurrentFrame)
			{
				// Sanity check the size of this packet
				numBlocks = ((1 << cip_fn) * (1 << DVspeed(dvMode)));
				expectedPacketSize = 8 + (cip_dbs*4*numBlocks);
				if (payloadLen != expectedPacketSize)
				{
					//logger->log("DV_DEBUG: DVReceiveDCLCallback: Bad cycle packet size: %d,\n",payloadLen);

					pthread_mutex_lock(&frameQueueMutex);
					for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
					{
						pNotifyInst = *dequeIterator;
						pNotifyInst->handler(kDVFrameWrongMode, nil,pNotifyInst->refCon);
					}
					
					// Put the frame struct back on the queue
					frameQueue.push_back(pCurrentFrame);
					pCurrentFrame = nil;
					
					pthread_mutex_unlock(&frameQueueMutex);
				}
				else
				{
					// TODO: We could check to see if the dbc increment is what we expect,
					// but there's enough other sanity checks, that the dbc check should be unnecessary!
					
					// All seems correct, copy the data into the
					// frame buffer
					for(dataBlock=0;dataBlock<numBlocks;dataBlock++)
					{
						// Copy the datablock into the framebuffer
						pBlock = &pCycleBuf[3+(dataBlock*cip_dbs)];
						pFrameDataWord = (UInt32*) pCurrentFrame->pFrameData;
						currentWordOffset = pCurrentFrame->currentOffset/4;
						memcpy(&pFrameDataWord[currentWordOffset],pBlock,(cip_dbs*4));
						
						pCurrentFrame->currentOffset += (cip_dbs*4);
						
						if (pCurrentFrame->currentOffset == pCurrentFrame->frameLen)
						{
							//logger->log("DV_DEBUG: DVReceiveDCLCallback: End of Frame Detected, Time: 0x%08X\n",currentTime);
							
							// We have a whole frame, pass it to the clients
							pthread_mutex_lock(&frameQueueMutex);
							
							pCurrentFrame->refCount = numFrameClients;
							
							for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
							{
								pNotifyInst = *dequeIterator;
								// Do Callback. If the client returns an error condition,
								// it means he won't release the frame in the future, so release it here!
								result = pNotifyInst->handler(kDVFrameReceivedSuccessfully, pCurrentFrame,pNotifyInst->refCon);
								if (result != kIOReturnSuccess)
									pCurrentFrame->refCount -= 1;
							}
							
							if (pCurrentFrame->refCount == 0)
							{
								// It is possible that the client(s) called releaseFrame(...) within frame-received callback,
								// (since the frame queue mutex is now recursive). So, it's possible that this frame has already
								// been pushed back onto the frame queue. Make sure that it's not already on the frame queue 
								// before we push it on now. Note that we search the queue in reverse, since, if it was put 
								// on the back of the queue, it will be the first thing we look at, ending the iterative search faster.
								alreadyOnFrameQueue = false;
								for (frameQueueReverseIterator = frameQueue.rbegin() ; frameQueueReverseIterator != frameQueue.rend() ; ++frameQueueReverseIterator)
								{
									frameOnQueue = *frameQueueReverseIterator;
									if (frameOnQueue == pCurrentFrame)
									{
										alreadyOnFrameQueue = true;
										break;	// We found this frame on the queue, so break out of the for-loop now!
									}
								}
								
								// Put the frame back on the queue
								if (!alreadyOnFrameQueue)
									frameQueue.push_back(pCurrentFrame);
							}

							pCurrentFrame = nil;
							
							pthread_mutex_unlock(&frameQueueMutex);
							
							// If there are more blocks in this packet, which can be the case
							// for DV25 4X NTSC, then we need to allocate another frame buffer
							if (dataBlock<(numBlocks-1))
							{
								// Get a frame off the queue, if available
								pCurrentFrame = getNextQueuedFrame();
								if (pCurrentFrame)
								{
									pCurrentFrame->frameLen = pDVFormat->frameSize;
									pCurrentFrame->frameSYTTime = cip_syt;
									pCurrentFrame->frameReceivedTimeStamp = currentTime;
									pCurrentFrame->frameMode = cip_mode;
									pCurrentFrame->currentOffset = 0;
								}
								else
								{
									// Send a dropped frame notification to the clients
									pthread_mutex_lock(&frameQueueMutex);
									for (dequeIterator = frameNotifyQueue.begin() ; dequeIterator != frameNotifyQueue.end() ; ++dequeIterator)
									{
										pNotifyInst = *dequeIterator;
										pNotifyInst->handler(kDVFrameDropped, nil,pNotifyInst->refCon);
									}
									pthread_mutex_unlock(&frameQueueMutex);
								}
								
								if (pCurrentFrame == nil)
									break;	// Break out of the for loop, since we don't have a new frame buffer available.
							}						
						}
					}
				}
			}
		}

		// Bump the current time by one cycle
		// TODO:  I can get sub-cycle accuracy for the currentTime
		// value if I bump it ONLY in cycles that contain data, and I bump it
		// with the num fw clocks per source packet for the current video standard,
		// rather than the current bump of one cycle time per packet
		currentTime =  AddFWCycleTimeToFWCycleTime( currentTime, 0x00001000 );
		
		// Bump to the next cycle buffer
		pCycleBuf += (receiveCycleBufferSize/4);
	}

	// Update jump targets
	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								 receiveSegmentInfo[segment].pSegmentJumpDCL,
								 pDCLOverrunLabel );

	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								 receiveSegmentInfo[(segment == 0) ? (isochSegments-1) : (segment-1) ].pSegmentJumpDCL,
								 receiveSegmentInfo[segment].pSegmentLabelDCL );

	// Increment current segment and handle wrap
	currentSegment += 1;
	if (currentSegment == isochSegments)
		currentSegment = 0;
	
	// If the client has registered a no-data notification, see if we should reset the timer
	if ((noDataProc != nil) && (segmentHasData == true))
		startNoDataTimer();
}
	

//////////////////////////////////////////////////////////////////////
// DVReceiveOverrunDCLCallback
//////////////////////////////////////////////////////////////////////
void DVReceiver::DVReceiveOverrunDCLCallback(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	if (transportState == kDVReceiverTransportRecording)
	{
		// stop/delete the no-data timer if it exists
		stopNoDataTimer();
		
		// Stop Receiver
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kDVReceiverTransportStopped;
		
		// Post message if handler installed
		logger->log("\nDVReceiver Error: DCL Overrun!\n\n");
		if (messageProc != nil)
			messageProc(kDVReceiverDCLOverrun,0x00000000,0x00000000,pMessageProcRefCon);
		
		// Restart Receiver

		// Fixup all the DCL Jump targets
		fixupDCLJumpTargets();
		
		result = (*isochChannel)->AllocateChannel( isochChannel ) ;
		
		if (result == kIOReturnSuccess)
		{
			result = (*isochChannel)->Start( isochChannel ) ;
			if (result != kIOReturnSuccess)
			{
				(*isochChannel)->ReleaseChannel( isochChannel ) ;
			}
			else
			{
				// Update transport state status var
				transportState = kDVReceiverTransportRecording;

				// If a registered no-data proc exists, start the timer now
				if (noDataProc)
					startNoDataTimer();
			}
		}
	}
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Report to client if unable to restart DCL program
	if ((result != kIOReturnSuccess) && (messageProc != nil))
		messageProc(kFWAVCStreamDCLOverrunAutoRestartFailed,0x00000000,0x00000000,pMessageProcRefCon);
	
	return;
}

//////////////////////////////////////////////////////////////////////
// DVReceiveFinalizeCallback
//////////////////////////////////////////////////////////////////////
void DVReceiver::DVReceiveFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// DVReceiveForceStop
//////////////////////////////////////////////////////////////////////
void DVReceiver::DVReceiveForceStop(UInt32 stopCondition)
{
	DVReceiveOverrunDCLCallback();
}
#endif


//////////////////////////////////////////////////////////////////////
// FindDVFormatForMode
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::FindDVFormatForMode(void)
{
	// Note: This function finds the DVFormat table entry
	// for the specified DV mode, disregarding the 2-bit
	// speed code that's embedded in the low 2-bits of the mode
	// value.

	UInt32 i = 0;
	DVFormats* pFormat = &dvFormats[i];

	while (pFormat->frameSize != 0)
	{
		if ((dvMode & 0xFC) == pFormat->mode)
		{
			pDVFormat = pFormat;
			return kIOReturnSuccess;
		}
		i+=1;
		pFormat = &dvFormats[i];
	};

	return kIOReturnError;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::RemotePort_GetSupported(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed *outMaxSpeed,
										  UInt64 *outChanSupported)
{
	// In this routine we return the capabilities of our remote
	// device. We don't have an actual listener out on the bus
	// so we just say we run at all speeds and on all isochronous
	// channel numbers.

	// appropriate value
	*outMaxSpeed = receiveSpeed ;

	if (receiveChannel == kAnyAvailableIsochChannel)
	{
		// Allow the FireWireFamily to determine an available channel
		*outChanSupported	= ~1ULL;
	}
	else
	{
		// Use a specific channel
		*outChanSupported	= (((UInt64)0x80000000 << 32 | (UInt64)0x00000000) >> receiveChannel);
	}

	// ok!
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_AllocatePort
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kDVReceiverAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kDVReceiverReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
DVReceiver::RemotePort_Stop(
								  IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to stop listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_GetSupported_Helper(
							   IOFireWireLibIsochPortRef interface,
							   IOFWSpeed *outMaxSpeed,
							   UInt64 *outChanSupported)
{
	IOFWSpeed outMaxSpeed_Helper = *outMaxSpeed;
	UInt64 outChanSupported_Helper = *outChanSupported;
	IOReturn result;

	// Get the pointer to the DVReceiver object from the refcon
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);

	// Call the DVReceiver's remote port allocate-port callback
	result = pReceiver->RemotePort_GetSupported(interface,&outMaxSpeed_Helper,&outChanSupported_Helper);

	*outMaxSpeed = outMaxSpeed_Helper;
	*outChanSupported = outChanSupported_Helper;

	return result;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_AllocatePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_AllocatePort_Helper(
							   IOFireWireLibIsochPortRef interface,
							   IOFWSpeed maxSpeed,
							   UInt32 channel)
{
	// Get the pointer to the DVReceiver object from the refcon
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);

	// Call the DVReceiver's remote port allocate-port callback
	return pReceiver->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVReceiver object from the refcon
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);

	// Call the DVReceiver's remote port release-port callback
	return pReceiver->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVReceiver object from the refcon
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);

	// Call the DVReceiver's remote port start callback
	return pReceiver->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVReceiver object from the refcon
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);

	// Call the DVReceiver's remote port stop callback
	return pReceiver->RemotePort_Stop(interface);
}

//////////////////////////////////////////////////////////////////////
// DVReceiveDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void DVReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the DVReceiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    DVReceiver *pReceiver = (DVReceiver*) pCallProc->procData;

	pReceiver->DVReceiveDCLCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// DVReceiveOverrunDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void DVReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the DVReceiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    DVReceiver *pReceiver = (DVReceiver*) pCallProc->procData;

	pReceiver->DVReceiveOverrunDCLCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// DVReceiveFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn DVReceiveFinalizeCallback_Helper( void* refcon )
{
	DVReceiver *pDVReceiver = (DVReceiver*) refcon;
	pDVReceiver->DVReceiveFinalizeCallback();
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// AddFWCycleTimeToFWCycleTime
//////////////////////////////////////////////////////////////////////
UInt32  AddFWCycleTimeToFWCycleTime( UInt32 cycleTime1, UInt32 cycleTime2 )
{
    UInt32    secondCount,
	cycleCount,
	cycleOffset;
    UInt32    cycleTime;

    // Add cycle offsets.
    cycleOffset = (cycleTime1 & 0x0FFF) + (cycleTime2 & 0x0FFF);

    // Add cycle counts.
    cycleCount = (cycleTime1 & 0x01FFF000) + (cycleTime2 & 0x01FFF000);

    // Add any carry over from cycle offset to cycle count.
    if (cycleOffset > 3071)
    {
        cycleCount += 0x1000;
        cycleOffset -= 3072;
    }

    // Add secondCounts.
    secondCount = (cycleTime1 & 0xFE000000) + (cycleTime2 & 0xFE000000);

    // Add any carry over from cycle count to secondCount.
    if (cycleCount > (7999 << 12))
    {
        secondCount += 0x02000000;
        cycleCount -= (8000 << 12);
    }

    // Put everything together into cycle time.
    cycleTime = secondCount | cycleCount | cycleOffset;

    return (cycleTime);
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// DVReceiveForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void DVReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	DVReceiver *pReceiver = (DVReceiver*) (*interface)->GetRefCon(interface);
	
	// Call the DVReceiver's Force Stop callback
	return pReceiver->DVReceiveForceStop(stopCondition);
}
#endif

} // namespace AVS