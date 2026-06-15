/*
	File:		MPEG2Receiver.cpp

    Synopsis: This is the implementation file for the MPEG2Receiver class.
 
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

static void MPEG2ReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand);

static void MPEG2ReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand);

static IOReturn MPEG2ReceiveFinalizeCallback_Helper( void* refcon ) ;

static UInt32  AddFWCycleTimeToFWCycleTime( UInt32 cycleTime1, UInt32 cycleTime2 );

#ifdef kAVS_Enable_ForceStop_Handler	
static void	MPEG2ReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

//////////////////////////////////////////////////////
// NoDataTimeoutHelper
//////////////////////////////////////////////////////
static void NoDataTimeoutHelper(CFRunLoopTimerRef timer, void *data)
{
	MPEG2Receiver *pMPEG2Receiver = (MPEG2Receiver*) data;
	pMPEG2Receiver->NoDataTimeout();
}

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
MPEG2Receiver::MPEG2Receiver(StringLogger *stringLogger,
							 IOFireWireLibNubRef nubInterface,
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
	includeSPH = false;
	noDataTimer = nil;
	
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

	packetPush = nil;
	extendedPacketPush = nil;

	structuredDataPush = nil;
	maxNumStructuredDataStructsInCallback = 1;
	pCyclDataStruct = nil;
	
	messageProc = nil;
	noDataProc = nil;
	noDataTimeLimitInSeconds = 0.0;
	pReceiveBuffer = nil;
	transportState = kMpeg2ReceiverTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	doIRM = doIRMAllocations;
	receiveSegmentInfo = new MPEGReceiveSegment[isochSegments];

	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);

	// Initialize the no-data timer mutex
	pthread_mutex_init(&noDataTimerMutex,NULL);
	
	// Calculate the size of the DCL command pool needed
	dclCommandPoolSize = ((((isochCyclesPerSegment)*isochSegments)+(isochSegments*4)+(isochSegments*4)+16)*32);

	// Calculate the size of the VM buffer for the dcl receive packets
	dclVMBufferSize = (((isochSegments*isochCyclesPerSegment)+1)*kMPEG2ReceiveBufferSize)+(isochSegments*4); // Allocate space for buffers and timestamps
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
MPEG2Receiver::~MPEG2Receiver()
{
	if (transportState != kMpeg2ReceiverTransportStopped)
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

	// Free the update DCL list
	if (updateDCLList)
		free(updateDCLList);

	// If we created an internall logger, free it
	if (noLogger == true)
		delete logger;

	// Free the receive segment info structs
	delete [] receiveSegmentInfo;

	// If we allocated memory for the array of structured-data structs, free it now
	if (pCyclDataStruct)
		delete [] pCyclDataStruct;
	
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->Release(nodeNubInterface);

	// Release the transport control mutex
	pthread_mutex_destroy(&transportControlMutex);

	// Release the no-data timer mutex
	pthread_mutex_destroy(&noDataTimerMutex);

}

//////////////////////////////////////////////////////
// setupIsocReceiver
//////////////////////////////////////////////////////
IOReturn MPEG2Receiver::setupIsocReceiver(void)
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
	IOFireWireLibNubRef newNubInterface;
	UInt32 curUpdateListIndex;
	IOVirtualRange bufRange;

	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nMPEG2Receiver Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nMPEG2Receiver Error: Error duplicating device interface: 0x%08X\n\n",result);
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
		logger->log("\nMPEG2Receiver Error: Error creating Receive DCL command pool: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 true,	// remote is talker
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nMPEG2Receiver Error: Errpr creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this MPEG2Receiver object in 
	// the remote isoch port's refcon variable
	(*remoteIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) remoteIsocPort,this) ;
	
	// Use the remote port interface to install some callback handlers.
	(*remoteIsocPort)->SetGetSupportedHandler( remoteIsocPort, & RemotePort_GetSupported_Helper );
	(*remoteIsocPort)->SetAllocatePortHandler( remoteIsocPort, & RemotePort_AllocatePort_Helper );
	(*remoteIsocPort)->SetReleasePortHandler( remoteIsocPort, & RemotePort_ReleasePort_Helper );
	(*remoteIsocPort)->SetStartHandler( remoteIsocPort, & RemotePort_Start_Helper );
	(*remoteIsocPort)->SetStopHandler( remoteIsocPort, & RemotePort_Stop_Helper );

	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer,dclVMBufferSize, VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nMPEG2Receiver Error: Error allocating isoch receive buffers.\n\n");
		return kIOReturnError ;
    }
	else
		bzero(pBuffer, dclVMBufferSize);

	// Set the buffer range var
	bufRange.address = (IOVirtualAddress) pBuffer;
	bufRange.length = dclVMBufferSize;
	
	// Save a pointer to the buffer
	pReceiveBuffer = pBuffer;
	
	// Calculate the pointer to the DCL overrun receive buffer
	pOverrunReceiveBuffer = (UInt32*) &pBuffer[(isochSegments*isochCyclesPerSegment*kMPEG2ReceiveBufferSize)];
	
	// Set the timestamp pointer
	pTimeStamps = (UInt32*) &pBuffer[(((isochSegments*isochCyclesPerSegment)+1)*kMPEG2ReceiveBufferSize)];
	
	// Allocate memory for the update list - Enough for all receive DCLs and timestamp DCLs 
    updateDCLList = (DCLCommandPtr *)malloc(((isochSegments*isochCyclesPerSegment)+isochSegments) * sizeof(DCLCommandPtr));
	
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
			pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, &pBuffer[bufCnt*kMPEG2ReceiveBufferSize],kMPEG2ReceiveBufferSize);

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

		pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, MPEG2ReceiveDCLCallback_Helper, (DCLCallProcDataType) this);

		// Jumps to bogus address, for now. We'll fix the real jump location before we start receive
		pLastDCL = (*dclCommandPool)->AllocateJumpDCL(dclCommandPool, pLastDCL,(DCLLabelPtr) pLastDCL);
		receiveSegmentInfo[seg].pSegmentJumpDCL = (DCLJumpPtr) pLastDCL;
	}

	// Allocate Overrun label & callback DCL
	pLastDCL = (*dclCommandPool)->AllocateLabelDCL( dclCommandPool, pLastDCL ) ;
	pDCLOverrunLabel = (DCLLabelPtr) pLastDCL;
	pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, pOverrunReceiveBuffer,kMPEG2ReceiveBufferSize);
	pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, MPEG2ReceiveOverrunDCLCallback_Helper, (DCLCallProcDataType) this);


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
															  &bufRange,
															  1,
															  CFUUIDGetUUIDBytes( kIOFireWireLocalIsochPortInterfaceID ));
	if (!localIsocPort)
    {
		logger->log("\nMPEG2Receiver Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, MPEG2ReceiveFinalizeCallback_Helper) ;
	
	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														kMPEG2ReceiveBufferSize,
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nMPEG2Receiver Error: Error creating isoch channel object: 0x%08X\n\n",result);
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
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,MPEG2ReceiveForceStopHandler_Helper);
	
	// Turn on notification
	(*isochChannel)->TurnOnNotification(isochChannel);
#endif
	
	// Fixup all the DCL Jump targets
	fixupDCLJumpTargets();

	// For debug, enable this line to print out some of the DCL code
	//	(*localIsocPort)->PrintDCLProgram( localIsocPort, pFirstDCL, 200) ;
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// registerDataPushCallback
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::registerDataPushCallback(DataPushProc handler, void *pRefCon)
{
	packetPush = handler;
	pPacketPushRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerExtendedDataPushCallback
//////////////////////////////////////////////////////////////////////
IOReturn 
MPEG2Receiver::registerExtendedDataPushCallback(ExtendedDataPushProc handler, void *pRefCon)
{
	extendedPacketPush = handler;
	pPacketPushRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerStructuredDataPushCallback
//////////////////////////////////////////////////////////////////////
IOReturn 
MPEG2Receiver::registerStructuredDataPushCallback(StructuredDataPushProc handler, UInt32 maxCycleStructsPerCallback, void *pRefCon)
{
	if (pCyclDataStruct == nil)
	{
		// Need to allocate array of the MPEGReceiveCycleData structs, one per cyclePerSegment
		pCyclDataStruct = new MPEGReceiveCycleData[isochCyclesPerSegment];
		if (!pCyclDataStruct)
			return kIOReturnNoMemory;
	}
	
	structuredDataPush = handler;
	pPacketPushRefCon = pRefCon;

	if (maxCycleStructsPerCallback == 0)
		maxNumStructuredDataStructsInCallback = 1;
	else if (maxCycleStructsPerCallback > isochCyclesPerSegment)
		maxNumStructuredDataStructsInCallback = isochCyclesPerSegment;
	else
		maxNumStructuredDataStructsInCallback = maxCycleStructsPerCallback;
	
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerMessageCallback
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::registerMessageCallback(MPEG2ReceiverMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerNoDataNotificationCallback
//////////////////////////////////////////////////////////////////////
IOReturn MPEG2Receiver::registerNoDataNotificationCallback(MPEG2NoDataProc handler, void *pRefCon, UInt32 noDataTimeInMSec)
{
	noDataProc = handler;
	pNoDataProcRefCon = pRefCon;
	noDataTimeLimitInSeconds = (noDataTimeInMSec / 1000.0);
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// ReceiveSourcePacketHeaders
//////////////////////////////////////////////////////////////////////
void 
MPEG2Receiver::ReceiveSourcePacketHeaders(bool wantSPH)
{
	if (wantSPH)
		includeSPH = true;
	else
		includeSPH = false;
}

//////////////////////////////////////////////////////////////////////
// fixupDCLJumpTargets
//////////////////////////////////////////////////////////////////////
void MPEG2Receiver::fixupDCLJumpTargets(void)
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
MPEG2Receiver::startReceive(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already running
	if (transportState == kMpeg2ReceiverTransportRecording)
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
			transportState = kMpeg2ReceiverTransportRecording;

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
MPEG2Receiver::stopReceive(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	// Make sure we are not already stopped
	if (transportState == kMpeg2ReceiverTransportStopped)
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
	transportState = kMpeg2ReceiverTransportStopped;
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Wait for the finalize callback to fire for this stream
	if (result == kIOReturnSuccess)
		while (finalizeCallbackCalled == false) usleep(1000);
	
	return result;
}

//////////////////////////////////////////////////
// MPEG2Receiver::startNoDataTimer
//////////////////////////////////////////////////
void MPEG2Receiver::startNoDataTimer( void )
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
// MPEG2Receiver::stopNoDataTimer
//////////////////////////////////////////////////
void MPEG2Receiver::stopNoDataTimer( void )
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
// MPEG2Receiver::NoDataTimeout
//////////////////////////////////////////////////
void MPEG2Receiver::NoDataTimeout(void)
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
// setReceiveIsochChannel
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::setReceiveIsochChannel(unsigned int chan)
{
	receiveChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setReceiveIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::setReceiveIsochSpeed(IOFWSpeed speed)
{
	receiveSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// MPEG2ReceiveDCLCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Receiver::MPEG2ReceiveDCLCallback(void)
{
	// Local Vars
    UInt32 segment;
	UInt32 startBuf;
	UInt32 i;
	UInt32 payloadLen;
	UInt32 tsCount = 0;
	UInt32 cyclePktCount = 0;
	UInt32 *pPacketPtr[kMaxNumReceivePacketsPerCycle];
	UInt32 incrementVal;
	bool segmentHasData = false;
	bool segmentBadPacketDetected = false;
	UInt32 dclTimeStamp;
	
	UInt32 outBusTime, outCycleTime;
	
	AbsoluteTime currentUpTime;
	Nanoseconds currentUpTimeInNanoSeconds;
	UInt64 currentUpTimeInNanoSecondsU64;
	
	int currentCycleTimeSecondsLo;	// Low 3-bits
	int currentCycleTimeSecondsHi;	// High 4-bits
	int currentCycleTimeSeconds;
	int currentCycleTimeCycles;
	
	int dclTimeStampSecondsLo;		// Low 3-bits
	int dclTimeStampSecondsHi;		// High 4-bits (we calculate this)
	int dclTimeStampSeconds;
	int dclTimeStampCycles;
	
	UInt32 currentCycleTimeInCycles;
	UInt32 dclTimeInCycles;
	UInt32 deltaCycles;
	UInt32 deltaNanoSeconds;
	UInt64 dclTimeStampTimeInNanoSecondsU64 = 0LL;
	
	UInt32 numCycleStructs = 0;
	UInt32 numCycleStructsDeliveredToClient = 0;
	UInt32 numCycleStructsThisCallback = 0;

	// See if this callback happened after we stopped
	if (transportState == kMpeg2ReceiverTransportStopped)
		return;
	
	dclTimeStamp = pTimeStamps[currentSegment];
	
	segment = currentSegment;
	startBuf = segment*isochCyclesPerSegment;
	
	if (includeSPH == true)
		incrementVal = 0;
	else
		incrementVal = 1;

	// Deal with calculating AbsoluteTime (in nano-seconds) info, only if the 
	// structuredDataPush callback has been installed by the client
	// since the other callback methods don't provide this info to the client
	if (structuredDataPush != nil)
	{
		// Get cycle timer and up-time. Do this in quick succession to best
		// establish the relationship between the two.
		(*nodeNubInterface)->GetBusCycleTime( nodeNubInterface, &outBusTime, &outCycleTime);
		currentUpTime = UpTime();
		
		// Add one cycle to the current cycletime, if we are currently more than half-way
		// through the cycle. Makes it a little more accurate!
		if ((outCycleTime & 0x00000FFF) > 1536) 
			outCycleTime =  AddFWCycleTimeToFWCycleTime( outCycleTime, 0x00001000 );
		
		// Process cycle timer, dcl-timestamp, and up-time to determine delta
		currentCycleTimeSecondsLo = ((outCycleTime & 0x0E000000) >> 25);	
		currentCycleTimeSecondsHi = ((outCycleTime & 0xF0000000) >> 25);
		currentCycleTimeSeconds = ((outCycleTime & 0xFE000000) >> 25);
		currentCycleTimeCycles = ((outCycleTime & 0x01FFF000) >> 12);
		
		dclTimeStampCycles = ((dclTimeStamp & 0x01FFF000) >> 12);
		dclTimeStampSecondsLo = ((dclTimeStamp & 0x0E000000) >> 25);		
		if (dclTimeStampSecondsLo > currentCycleTimeSecondsLo )
			dclTimeStampSecondsHi = currentCycleTimeSecondsHi - 8 ;	// Subtract 8 seconds from this value!
		else
			dclTimeStampSecondsHi = currentCycleTimeSecondsHi;
		dclTimeStampSeconds = dclTimeStampSecondsHi + dclTimeStampSecondsLo;
		
		currentUpTimeInNanoSeconds = AbsoluteToNanoseconds(currentUpTime);
		currentUpTimeInNanoSecondsU64 = ((UInt64) currentUpTimeInNanoSeconds.hi << 32) | currentUpTimeInNanoSeconds.lo;
		
#if 0
		printf("CycleTime = 0x%08X (%d:%d:%d)\n",(int) outCycleTime,
			   currentCycleTimeSeconds,
			   currentCycleTimeCycles,
			   (int) (outCycleTime & 0x00000FFF));
		
		printf("UpTime= %lld (%10.10f)\n",
			   currentUpTimeInNanoSecondsU64,
			   currentUpTimeInNanoSecondsU64/1000000000.0);
		
		printf("DCL TimeStamp = 0x%08X (%d:%d:%d)\n",(int) dclTimeStamp,
			   dclTimeStampSeconds,
			   dclTimeStampCycles,
			   (int) (dclTimeStamp & 0x00000FFF));
#endif
		
		// Here we are going to arbitrarily add 8 seconds to both the current cycle timer, and the dcl timestamp, to 
		// eliminate the possiblity that the dcl timestamp full seconds field is a negative number.
		// This is OK, since we're only interested in the delta between the two timetstamps, and 
		// this eliminates any rollover issue!
		dclTimeStampSeconds += 8;
		currentCycleTimeSeconds += 8;
		
		// Convert the seconds fields and cycles fields of both timestamps to a count of cycles
		currentCycleTimeInCycles = (currentCycleTimeSeconds * 8000) + currentCycleTimeCycles;
		dclTimeInCycles = (dclTimeStampSeconds * 8000) + dclTimeStampCycles;
		
		deltaCycles = currentCycleTimeInCycles-dclTimeInCycles;
		deltaNanoSeconds = (deltaCycles*125000);
		dclTimeStampTimeInNanoSecondsU64 = currentUpTimeInNanoSecondsU64 - deltaNanoSeconds;
		
#if 0
		printf("Delta between dcl timestamp and current cycle time: %d cycles (%d nsec) \n",(int)deltaCycles,(int)deltaNanoSeconds);
		
		printf("DCL TimeStamp UpTime= %lld (%10.10f)\n",
			   dclTimeStampTimeInNanoSecondsU64,
			   dclTimeStampTimeInNanoSecondsU64/1000000000.0);	
#endif
	}
		
	UInt32 *pCycleBuf = (UInt32*) (pReceiveBuffer + (startBuf*kMPEG2ReceiveBufferSize));

	for (i=0;i<isochCyclesPerSegment;i++)
	{
		payloadLen = ((*pCycleBuf & 0xFFFF0000) >> 16); // Note that this quadlet is already in machine native byte order!

		switch (payloadLen)
		{
			case 8:
				cyclePktCount = 0;
				break;
				
			case 200:
				cyclePktCount = 1;
				tsCount += 1;
				pPacketPtr[0] = &pCycleBuf[3+incrementVal];
				segmentHasData = true;
				break;

			case 392:
				cyclePktCount = 2;
				tsCount += 2;
				pPacketPtr[0] = &pCycleBuf[3+incrementVal];
				pPacketPtr[1] = &pCycleBuf[51+incrementVal];
				segmentHasData = true;
				break;

			case 584:
				cyclePktCount = 3;
				tsCount += 3;
				pPacketPtr[0] = &pCycleBuf[3+incrementVal];
				pPacketPtr[1] = &pCycleBuf[51+incrementVal];
				pPacketPtr[2] = &pCycleBuf[99+incrementVal];						
				segmentHasData = true;
				break;

			case 776:
				cyclePktCount = 4;
				tsCount += 4;
				pPacketPtr[0] = &pCycleBuf[3+incrementVal];
				pPacketPtr[1] = &pCycleBuf[51+incrementVal];
				pPacketPtr[2] = &pCycleBuf[99+incrementVal];
				pPacketPtr[3] = &pCycleBuf[147+incrementVal];									
				segmentHasData = true;
				break;

			case 968:
				cyclePktCount = 5;
				tsCount += 5;
				pPacketPtr[0] = &pCycleBuf[3+incrementVal];
				pPacketPtr[1] = &pCycleBuf[51+incrementVal];
				pPacketPtr[2] = &pCycleBuf[99+incrementVal];
				pPacketPtr[3] = &pCycleBuf[147+incrementVal];									
				pPacketPtr[4] = &pCycleBuf[195+incrementVal];									
				segmentHasData = true;
				break;
				
			default:
				// This is not a valid 61883-4 packet. Only notify the client once per DCL program segment
				if (segmentBadPacketDetected == false)
				{
					// Post message if handler installed
					logger->log("\nMPEG2Receiver Error: Received unsupported length isoch packet:%d\n\n",payloadLen);
					if (messageProc != nil)
						messageProc(kMpeg2ReceiverReceivedBadPacket,0x00000000,0x00000000,pMessageProcRefCon);
					
					segmentBadPacketDetected = true;
				}
				break;
		};
		
		// If we're using the structured-data callback method, and this cycle had mpeg data, fill in a MPEGReceiveCycleData struct
		if ((structuredDataPush != nil) && (cyclePktCount > 0))
		{
			pCyclDataStruct[numCycleStructs].tsPacketCount = cyclePktCount;
			
			for (UInt32 i=0;i<cyclePktCount;i++)
				pCyclDataStruct[numCycleStructs].pBuf[i] = pPacketPtr[i];
			
			pCyclDataStruct[numCycleStructs].pRefCon = pPacketPushRefCon;
			pCyclDataStruct[numCycleStructs].isochHeader = pCycleBuf[0];
			pCyclDataStruct[numCycleStructs].cipHeader0 = pCycleBuf[1];
			pCyclDataStruct[numCycleStructs].cipHeader1 = pCycleBuf[2];
			pCyclDataStruct[numCycleStructs].fireWireTimeStamp = dclTimeStamp;
			pCyclDataStruct[numCycleStructs].nanoSecondsTimeStamp = dclTimeStampTimeInNanoSecondsU64;
			pCyclDataStruct[numCycleStructs].pExpansionData = nil;
			
			// Bump the numbeer of cycleStructs
			numCycleStructs++;
		}
		else if (cyclePktCount > 0)
		{
			// Push received packets to application
			if (extendedPacketPush != nil)
				extendedPacketPush(cyclePktCount,pPacketPtr,pPacketPushRefCon,pCycleBuf[0],pCycleBuf[1],pCycleBuf[2],dclTimeStamp);
			else if (packetPush != nil)
				packetPush(cyclePktCount,pPacketPtr,pPacketPushRefCon);
		}
		
		// Bump the dcl timestamp by one cycle
		// Only if the client has registered an extended callback function, or a structure-data callback function
		if ((extendedPacketPush != nil) || (structuredDataPush != nil))
			dclTimeStamp =  AddFWCycleTimeToFWCycleTime( dclTimeStamp, 0x00001000 );
		
		// Bump the absolute-time for the next cycle
		// Only if the client has registered a structure-data callback function
		if (structuredDataPush != nil)
		{
			// Bump the dcl time stamp by 125uSec (or 125000 nSec)
			dclTimeStampTimeInNanoSecondsU64 += 125000LL;
		}
		
		pCycleBuf += (kMPEG2ReceiveBufferSize/4);
	}

	// If we're using the structured-data callback method, and if we received any mpeg
	// data during this segment, now is the time to do the callback(s) 
	if ((structuredDataPush != nil) && (numCycleStructs > 0))
	{
		while (numCycleStructsDeliveredToClient < numCycleStructs)
		{
			// If the number of structs remainining to be delivered is greater than the client's  
			// requested max, send only the requested max number of structs this time!
			numCycleStructsThisCallback = ((numCycleStructs-numCycleStructsDeliveredToClient) > maxNumStructuredDataStructsInCallback) ? 
				maxNumStructuredDataStructsInCallback : (numCycleStructs-numCycleStructsDeliveredToClient);

			// Callback the client 
			structuredDataPush(numCycleStructsThisCallback,&pCyclDataStruct[numCycleStructsDeliveredToClient],pPacketPushRefCon);
			
			// Bump
			numCycleStructsDeliveredToClient += numCycleStructsThisCallback;
		}
	}
	
	// Update some UI fields
	mpegDataRate = (tsCount*8.0*188.0) * (8000.0/isochCyclesPerSegment);

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
// MPEG2ReceiveOverrunDCLCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Receiver::MPEG2ReceiveOverrunDCLCallback(void)
{
	IOReturn result = kIOReturnSuccess ;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	if (transportState == kMpeg2ReceiverTransportRecording)
	{
		// stop/delete the no-data timer if it exists
		stopNoDataTimer();

		// Stop Receiver
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kMpeg2ReceiverTransportStopped;
		
		// Post message if handler installed
		logger->log("\nMPEG2Receiver Error: DCL Overrun!\n\n");
		if (messageProc != nil)
			messageProc(kMpeg2ReceiverDCLOverrun,0x00000000,0x00000000,pMessageProcRefCon);
		
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
				transportState = kMpeg2ReceiverTransportRecording;
				
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
// MPEG2ReceiveFinalizeCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Receiver::MPEG2ReceiveFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// MPEG2ReceiveForceStop
//////////////////////////////////////////////////////////////////////
void MPEG2Receiver::MPEG2ReceiveForceStop(UInt32 stopCondition)
{
	MPEG2ReceiveOverrunDCLCallback();
}
#endif

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::RemotePort_GetSupported(
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
MPEG2Receiver::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kMpeg2ReceiverAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kMpeg2ReceiverReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Receiver::RemotePort_Stop(
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

	// Get the pointer to the MPEG2Receiver object from the refcon
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Receiver's remote port allocate-port callback
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
	// Get the pointer to the MPEG2Receiver object from the refcon
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Receiver's remote port allocate-port callback
	return pReceiver->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Receiver object from the refcon
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Receiver's remote port release-port callback
	return pReceiver->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Receiver object from the refcon
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Receiver's remote port start callback
	return pReceiver->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Receiver object from the refcon
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Receiver's remote port stop callback
	return pReceiver->RemotePort_Stop(interface);
}

//////////////////////////////////////////////////////////////////////
// MPEG2ReceiveDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2ReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the MPEG2Receiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    MPEG2Receiver *pReceiver = (MPEG2Receiver*) pCallProc->procData;

	pReceiver->MPEG2ReceiveDCLCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// MPEG2ReceiveOverrunDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2ReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the MPEG2Receiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    MPEG2Receiver *pReceiver = (MPEG2Receiver*) pCallProc->procData;

	pReceiver->MPEG2ReceiveOverrunDCLCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// MPEG2ReceiveFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn MPEG2ReceiveFinalizeCallback_Helper( void* refcon )
{
	MPEG2Receiver *pMPEG2Receiver = (MPEG2Receiver*) refcon;
	pMPEG2Receiver->MPEG2ReceiveFinalizeCallback();
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
// MPEG2ReceiveForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2ReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	MPEG2Receiver *pReceiver = (MPEG2Receiver*) (*interface)->GetRefCon(interface);
	
	// Call the MPEG2Receiver's Force Stop callback
	return pReceiver->MPEG2ReceiveForceStop(stopCondition);
}
#endif

} // namespace AVS
