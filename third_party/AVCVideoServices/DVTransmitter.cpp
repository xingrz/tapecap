/*
	File:		DVTransmitter.cpp

	Synopsis: This is the implementation file for the DVTransmitter class. 
 
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

static void DVXmitDCLCallback_Helper(DCLCommandPtr pDCLCommand);

static void DVXmitDCLOverrunCallback_Helper(DCLCommandPtr pDCLCommand);

static IOReturn DVXmitFinalizeCallback_Helper( void* refcon ) ;

static void DVSilenceFrame(UInt8 mode, UInt8* frame);

#ifdef kAVS_Enable_ForceStop_Handler	
static void	DVXmitForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
DVTransmitter::DVTransmitter(StringLogger *stringLogger,
							 IOFireWireLibNubRef nubInterface,
							 UInt8 transmitterDVMode,
							 UInt32 numFrameBuffers,
							 unsigned int cyclesPerSegment,
							 unsigned int numSegments,
							 bool doIRMAllocations)
{
    nodeNubInterface = nubInterface;
	dclCommandPool = nil;
	remoteIsocPort = nil;
	localIsocPort = nil;
	isochChannel = nil;
	messageProc = nil;
	framePullProc = nil;
	frameReleaseProc = nil;
	pCurrentFrame = nil;

	pTransmitBuffer = nil;
	pFirstCycleObject = nil;
	xmitChannel = 0;
	xmitSpeed = kFWSpeed100MBit;
	runLoopRef = nil;
	dvMode = transmitterDVMode;
	dbc = 0;
	numFrames = numFrameBuffers;
	ppCallbackCycles = nil;
	
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

	transportState = kDVTransmitterTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	doIRM = doIRMAllocations;

	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);
	
	// Calculate the size of the DCL command pool needed
	dclCommandPoolSize = ((((isochCyclesPerSegment*6)*isochSegments)+(isochSegments*6)+16)*32);
	
#ifdef DVTransmitter_DCL_Callback_Timing
	// Initialize DCL Timing vars
	DCL_Timing_Accumulator = 0;
	DCL_Timing_Max = 0;
	DCL_Timing_Min = 0xFFFFFFFFFFFFFFFFLL;
	DCL_Timing_Count = 0;
#endif	

}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
DVTransmitter::~DVTransmitter()
{
	// Local Vars
	DVXmitCycle *pXmitCycle;
	unsigned int i;

	if (transportState != kDVTransmitterTransportStopped)
		stopTransmit();

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

	// If we created an internall logger, free it
	if (noLogger == true)
		delete logger;
	
	// Free the DVXmitCycle objects
	if (pFirstCycleObject != nil)
	{
		for (i=0;i<(isochCyclesPerSegment*isochSegments);i++)
		{
			pXmitCycle = pFirstCycleObject;
			if (pXmitCycle != nil)
			{
				pFirstCycleObject = pXmitCycle->pNext;
				delete pXmitCycle;
			}
		}
	}

	if (nodeNubInterface != nil)
		(*nodeNubInterface)->Release(nodeNubInterface);

	// Free the frame structures and pointers array
	if (framePtrs)
	{
		for (i=0;i<numFrames;i++)
			if (framePtrs[i])
			{
				// Free the frame buffer memory.
				if (framePtrs[i]->pFrameData != nil)
					delete [] framePtrs[i]->pFrameData;
				
				delete framePtrs[i];
			}
		delete [] framePtrs;
	}

	// Free the vm allocated DCL buffer
	if (pTransmitBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pTransmitBuffer,transmitBufferSize);
	
	// Free the list of end-of-segment DVXmitCycle pointers
	if (ppCallbackCycles)
		delete [] ppCallbackCycles;
	
	// Release the transport control mutex
	pthread_mutex_destroy(&transportControlMutex);

}

//////////////////////////////////////////////////////
// setupIsocTransmitter
//////////////////////////////////////////////////////
IOReturn DVTransmitter::setupIsocTransmitter(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess ;
	UInt32 totalObjects = isochCyclesPerSegment * isochSegments;
    UInt8 *pBuffer = nil;
    UInt32 i;
    DVXmitCycle *pCycleObject = nil;
    DVXmitCycle *pLastCycleObject = nil;
	bool hasCallback;
	UInt32 segment = 0;
	IOVirtualRange bufRange;
    DCLCommandStruct *pLastDCL = nil;
    DCLJumpPtr jumpDCL;
	IOFireWireLibNubRef newNubInterface;
	UInt32 endOfCycleIndex = 0;
	
	// Find the DVFormat info for this mode
	result = FindDVFormatForMode();
	if (result != kIOReturnSuccess)
    {
		logger->log("\nDVTransmitter Error: Invalid dvMode: 0x%02X\n\n",dvMode);
		return kIOReturnError ;
    }

	// Set the SYT offset for this DV mode
	sourcePacketCycleCountStartValue = pDVFormat->startingSYTOffset;
	
	// This is the calculation of the size of an isoch packet payload that contains DV data
	xmitPayloadSize = kDVXmitCIPOnlySize + ((pDVFormat->dbs*4) * (1 << pDVFormat->fn) * (1 << DVspeed(dvMode)));
	
	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nDVTransmitter Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nDVTransmitter Error: Error duplicating device interface: 0x%08X\n\n",result);
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
		logger->log("\nDVTransmitter Error: Error creating DCL command pool: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 false,
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nDVTransmitter Error: Error creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this DVTransmitter object in
	// the remote isoch port's refcon variable
	(*remoteIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) remoteIsocPort,this) ;
	
	// Use the remote port interface to install some callback handlers.
	(*remoteIsocPort)->SetGetSupportedHandler( remoteIsocPort, & RemotePort_GetSupported_Helper );
	(*remoteIsocPort)->SetAllocatePortHandler( remoteIsocPort, & RemotePort_AllocatePort_Helper );
	(*remoteIsocPort)->SetReleasePortHandler( remoteIsocPort, & RemotePort_ReleasePort_Helper );
	(*remoteIsocPort)->SetStartHandler( remoteIsocPort, & RemotePort_Start_Helper );
	(*remoteIsocPort)->SetStopHandler( remoteIsocPort, & RemotePort_Stop_Helper );

	// Allocate an array of pointers to DVXmitCycles to keep a list of end-of-segment DVXmitCycle objects
	ppCallbackCycles = new DVXmitCycle*[isochSegments];
	if (!ppCallbackCycles)
	{
		logger->log("\nDVTransmitter Error: Error allocating end-of-segment DVXmitCycle list\n\n");
		return kIOReturnError ;
	}
	
	transmitBufferSize = ((totalObjects*xmitPayloadSize) +	// xmit cycle transmit buffers
					   (isochSegments*sizeof(UInt32)));		// Timestamps
	
	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer, transmitBufferSize , VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nDVTransmitter Error: Error allocating isoch transmit buffers: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	else
		bzero(pBuffer, transmitBufferSize);

	// Set the buffer range var
	bufRange.address = (IOVirtualAddress) pBuffer;
	bufRange.length = transmitBufferSize;

	// Set the timestamp pointer
	pTimeStamps = (UInt32*) &pBuffer[totalObjects*xmitPayloadSize];
	
	// Save a pointer to the transmit buffer
	pTransmitBuffer = pBuffer;

	// Create the array of frame pointers
	framePtrs = new DVTransmitFrame*[numFrames];
	if (!framePtrs)
	{
		logger->log("\nDVTransmitter Error: Error creating DV frame pointer array\n");
		return kIOReturnError ;
	}

	// Allocate frame structures
	for (i=0;i<numFrames;i++)
	{
		framePtrs[i] = new DVTransmitFrame;
		if (!framePtrs[i])
		{
			logger->log("\nDVTransmitter Error: Error creating DV frame object\n");
			return kIOReturnError ;
		}
		// Initialize frame structure
		framePtrs[i]->frameIndex = i;
		framePtrs[i]->frameLen = pDVFormat->frameSize;
		framePtrs[i]->inUse = false;
		framePtrs[i]->curOffset = 0;
		framePtrs[i]->dclProgramRefCount = 0;
		framePtrs[i]->frameSYTTime = 0;
		framePtrs[i]->frameTransmitStartCycleTime = 0;
		framePtrs[i]->timeStampSecondsFieldValid = false;

		// Allocate frame buffer
		framePtrs[i]->pFrameData = (UInt8*) new UInt8[pDVFormat->frameSize];
		if (framePtrs[i]->pFrameData == nil)
		{
			logger->log("\nDVTransmitter Error: Error allocating DV frame buffer\n");
			return kIOReturnNoMemory ;
		}
	}

    // Set the isoc tag field correctly for CIP style isoc.
    pLastDCL = (*dclCommandPool)->AllocateSetTagSyncBitsDCL( dclCommandPool, nil, 1, 0 ) ;

	// Set the first DCL pointer to the first DCL created.
	pFirstDCL = pLastDCL;

    // Loop to create objects
    for (i=0;i<totalObjects;i++)
    {
		hasCallback = (( (i % isochCyclesPerSegment) == (isochCyclesPerSegment - 1 ) ) ? true : false);

		pCycleObject = new DVXmitCycle(xmitPayloadSize,
									&pBuffer[i*xmitPayloadSize],
									hasCallback,
									DVXmitDCLCallback_Helper,
									dclCommandPool,
									pLastDCL,
									&pLastDCL,
									(hasCallback == true) ? &pTimeStamps[segment++] : nil,
									this);

		// Save a pointer to the first created object
		if (i == 0)
			pFirstCycleObject = pCycleObject;
		
		// If this is an end-of-segment cycle, add it to our list
		if (hasCallback == true)
		{
			ppCallbackCycles[endOfCycleIndex] = pCycleObject;
			endOfCycleIndex += 1;
		}
		
		// Set previous links
		pCycleObject->pPrev = pLastCycleObject;
		pLastCycleObject = pCycleObject;
    }

    // Create DCL for overrun callback mechanism
    pLastDCL = (*dclCommandPool)->AllocateLabelDCL( dclCommandPool, pLastDCL ) ;
    pOverrunCallbackLabel = pLastDCL;
    pLastDCL = (*dclCommandPool)->AllocateSendPacketStartDCL(
															 dclCommandPool,
															 pLastDCL,
															 pLastCycleObject->pBuf,
															 kDVXmitCIPOnlySize );
    pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool,
													  pLastDCL,
													  DVXmitDCLOverrunCallback_Helper,
													  (DCLCallProcDataType) this);

    // Set the next pointer in the last DCL to nil
    pLastDCL->pNextDCLCommand = nil;

    // Set previous for first object to point to last object
    pFirstCycleObject->pPrev = pLastCycleObject;

    // Set next for last object to point to first object
    pCycleObject->pNext = pFirstCycleObject;

    // Set the rest of the next links
    while (pCycleObject != pFirstCycleObject)
    {
		(pCycleObject->pPrev)->pNext = pCycleObject;
		pCycleObject = pCycleObject->pPrev;
    }

    pCycleObject = pFirstCycleObject;

    // Update the jump targets in all DCLs to point to next cycle object's FullXfr DCL label
    do
    {
		jumpDCL = (DCLJumpPtr) pCycleObject->pFullXfrJump;
		jumpDCL->pJumpDCLLabel = (DCLLabelPtr) (pCycleObject->pNext)->pCIPOnlyXfrLabel;

		jumpDCL = (DCLJumpPtr) pCycleObject->pCIPOnlyXfrJump;
		jumpDCL->pJumpDCLLabel = (DCLLabelPtr) (pCycleObject->pNext)->pCIPOnlyXfrLabel;

		pCycleObject->CycleMode = CycleModeCIPOnly;
		pCycleObject->FullXfrJumpNextMode = CycleModeCIPOnly;
		pCycleObject->CIPOnlyJumpNextMode = CycleModeCIPOnly;

		pCycleObject = pCycleObject->pNext;

    }while (pCycleObject != pFirstCycleObject);

	// Set the next update cycle pointer to the start of the list
	pNextUpdateCycle = pFirstCycleObject;

	// Set the last callback cycle pointer
	pLastCallbackCycle = pFirstCycleObject->pPrev;

	// Modify the branch targets for the last callback cycle
	// to point to the overrun callback handler.
	jumpDCL = (DCLJumpPtr) pLastCallbackCycle->pFullXfrJump;
	jumpDCL->pJumpDCLLabel = (DCLLabelPtr) pOverrunCallbackLabel;
	jumpDCL = (DCLJumpPtr) pLastCallbackCycle->pCIPOnlyXfrJump;
	jumpDCL->pJumpDCLLabel = (DCLLabelPtr) pOverrunCallbackLabel;
	
	// Using the nub interface to the local node, create
	// a local isoc port.
	localIsocPort = (*nodeNubInterface)->CreateLocalIsochPort(
														   nodeNubInterface,
														   true,
														   pFirstDCL,
														   kFWDCLCycleEvent,
														   0,
														   0x01FFF000,
														   nil,
														   0,
														   &bufRange,
														   1,
														   CFUUIDGetUUIDBytes( kIOFireWireLocalIsochPortInterfaceID ));
	if (!localIsocPort)
    {
		logger->log("\nDVTransmitter Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, DVXmitFinalizeCallback_Helper) ;
	
	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														xmitPayloadSize,
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nDVTransmitter Error: Error creating isoch channel object: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Add a listener and a talker to the isoch channel
	result = (*isochChannel)->AddListener(isochChannel,
									   (IOFireWireLibIsochPortRef) remoteIsocPort ) ;

	result = (*isochChannel)->SetTalker(isochChannel,
									 (IOFireWireLibIsochPortRef) localIsocPort ) ;

#ifdef kAVS_Enable_ForceStop_Handler	
	// Set the refcon for the isoch channel
	(*isochChannel)->SetRefCon(isochChannel,this);
	
	// Set the force stop handler
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,DVXmitForceStopHandler_Helper);
	
	// Turn on notification
	(*isochChannel)->TurnOnNotification(isochChannel);
#endif
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// startTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::startTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already running
	if (transportState == kDVTransmitterTransportPlaying)
	{
		// Unlock the transport control mutex
		pthread_mutex_unlock(&transportControlMutex);
		
		// Return a device already open error
		return kIOReturnExclusiveAccess;
	}
	
	// Prepare for transmit
	prepareForTransmit(true);

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
			transportState = kDVTransmitterTransportPlaying;
		}
	}
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// stopTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::stopTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already stopped
	if (transportState == kDVTransmitterTransportStopped)
	{
		// Unlock the transport control mutex
		pthread_mutex_unlock(&transportControlMutex);
		
		// Return a device already open error
		return kIOReturnExclusiveAccess;
	}
	
	// Clear the finalize flag for this stream
	finalizeCallbackCalled = false;
	
	result = (*isochChannel)->Stop( isochChannel ) ;
	(*isochChannel)->ReleaseChannel( isochChannel ) ;

	// Update transport state status var
	transportState = kDVTransmitterTransportStopped;
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Wait for the finalize callback to fire for this stream
	if (result == kIOReturnSuccess)
		while (finalizeCallbackCalled == false) usleep(1000);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// registerFrameCallbacks
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::registerFrameCallbacks(DVFramePullProc framePullHandler,
									  void *pFramePullHandlerRefCon,
									  DVFrameReleaseProc frameReleaseHandler,
									  void *pFrameReleaseHandlerRefCon)
{
	framePullProc = framePullHandler;
	pFramePullProcRefCon = pFramePullHandlerRefCon;
	frameReleaseProc = frameReleaseHandler;
	pFrameReleaseProcRefCon = pFrameReleaseHandlerRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerMessageCallback
//////////////////////////////////////////////////////////////////////
IOReturn DVTransmitter::registerMessageCallback(DVTransmitterMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// prepareForTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::prepareForTransmit(bool notifyClient)
{
	UInt16 nodeID;
	UInt32 generation;
	UInt32 i;
	DVXmitCycle *pFirstCycle;
	
	// Get the local node ID
	do (*nodeNubInterface)->GetBusGeneration(nodeNubInterface, &generation);
	while  ((*nodeNubInterface)->GetLocalNodeIDWithGeneration(nodeNubInterface,generation,&nodeID) != kIOReturnSuccess);

	// Note that currentIsochTime must not be initialized to a value
	// less that zero, because the first transmit packet generated
	// will be a CIP only! A value of 0 or greater for currentIsochTime
	// will prevent the flow-control logic from adding source packets
	// into the first isoch cycle.
	currentIsochTime = 0.0;
	
    currentDVTime = (sourcePacketCycleCountStartValue*3072);
	currentSegment = 0;
	dbc = 0;
	pCurrentFrame = nil;
	expectedTimeStampCycle = isochCyclesPerSegment - 1;
	
	// Clear the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = false;

	if (notifyClient == true)
	{
		// Mark all frames as not inUse and set offset to 0!
		for (i=0;i<numFrames;i++)
		{
			framePtrs[i]->inUse = false;
			framePtrs[i]->curOffset = 0;
			framePtrs[i]->dclProgramRefCount = 0;
			framePtrs[i]->frameSYTTime = 0;
			framePtrs[i]->frameTransmitStartCycleTime = 0;
			framePtrs[i]->timeStampSecondsFieldValid = false;
		}
	
		// Prepare the packet fetcher
		// This is a call back to the client to alert him
		// that the transmitter is starting up, and that
		// he is now the owner of all the frame structs
		if (messageProc != nil)
			messageProc(kDVTransmitterPreparePacketFetcher,0x00000000,0x00000000,pMessageProcRefCon);
	}
				
	pNextUpdateCycle = pFirstCycleObject;

    // Fix-up dv cycle jump targets
    pFirstCycle = pNextUpdateCycle;
    do
    {
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pNextUpdateCycle->pFullXfrJump,
								  (DCLLabel*) (pNextUpdateCycle->pNext)->pCIPOnlyXfrLabel);
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pNextUpdateCycle->pCIPOnlyXfrJump,
								  (DCLLabel*) (pNextUpdateCycle->pNext)->pCIPOnlyXfrLabel);
		pNextUpdateCycle->FullXfrJumpNextMode	= CycleModeCIPOnly;
		pNextUpdateCycle->CIPOnlyJumpNextMode	= CycleModeCIPOnly;

		pNextUpdateCycle = pNextUpdateCycle->pNext;
    }while (pNextUpdateCycle != pFirstCycle);

	// Set the last callback cycle pointer
	pLastCallbackCycle = pFirstCycleObject->pPrev;

	// Modify the branch targets for the last callback cycle
	// to point to the overrun callback handler.
	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
									(DCLJump*) pLastCallbackCycle->pFullXfrJump,
									(DCLLabel*) pOverrunCallbackLabel);
	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
									(DCLJump*) pLastCallbackCycle->pCIPOnlyXfrJump,
									(DCLLabel*) pOverrunCallbackLabel);
	
	// Initialize all the cycle objects with data
	pNextUpdateCycle = pFirstCycleObject;
    do
    {
		// Set the doUpdateJumpTarget to true for all but the first call to FillCycleBuffer
		// to prevent overwriting the branch we just set to the overrun handler in the pLastCallbackCycle. 
		FillCycleBuffer(pNextUpdateCycle,nodeID, (pNextUpdateCycle == pFirstCycleObject) ? false : true);
		pNextUpdateCycle = pNextUpdateCycle->pNext;
    }while (pNextUpdateCycle != pFirstCycleObject);

	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochChannel
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::setTransmitIsochChannel(unsigned int chan)
{
	xmitChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::setTransmitIsochSpeed(IOFWSpeed speed)
{
	xmitSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// FillCycleBuffer
//////////////////////////////////////////////////////////////////////
void
DVTransmitter::FillCycleBuffer(DVXmitCycle *pCycle, UInt16 nodeID, bool doUpdateJumpTarget)
{
	IOReturn result;
	UInt32 newFrameIndex;
	UInt32 cycleFrameBytes;
	DVTransmitFrame* pReleaseFrame;
	UInt32 savedNewFrameReleaseIndex = 0;
	bool useSavedNewFrameReleaseIndex = false;
	UInt32* pXmitBufWord = (UInt32*) pCycle->pBuf;
	UInt32* pFrameBufWord;
	UInt32 *pCopyStartWord;
	int actualFrameStartCycle;
	int actualFrameStartSeconds;
	
	UInt32 sytTime;

	// CIP Header 0 - Initialize this in bus-byte order!
	pXmitBufWord[0] = EndianU32_NtoB(((nodeID & 0x3F) << 24) +
		(pDVFormat->dbs << 16) +
		(pDVFormat->fn << 14) +
		dbc);

	// CIP Header 1 - Note: Keep this quadlet in machine native byte order for now!
	// We'll adjust it to bus-byte order after we decide if the SYT needs to be applied
	// Note: SYT needs to be valid when we have a start of frame, otherwise 0xFFFF
	// We'll only put the SYT in in start-of-frame packets
	pXmitBufWord[1] = 0x80000000 +
		(dvMode << 16) + 
		0x0000FFFF;

	// See if it's time to send some DV
    if (currentIsochTime < 0.0)
    {
		// Send DV Data

		// If we don't have our first frame yet, try and get it
		if 	(pCurrentFrame == nil)
		{
			if (framePullProc)
			{
				result = framePullProc(&newFrameIndex,pFramePullProcRefCon);
				if ((result == kIOReturnSuccess) && (newFrameIndex < numFrames))
				{
					pCurrentFrame = getFrame(newFrameIndex);

					// Initialize some new frame parameters
					pCurrentFrame->inUse = true;
					pCurrentFrame->curOffset = 0;
					pCurrentFrame->dclProgramRefCount = 1;
					pCurrentFrame->frameSYTTime = 0;
					pCurrentFrame->frameTransmitStartCycleTime = 0;
					pCurrentFrame->timeStampSecondsFieldValid = false;
				}
				else
				{
					pCurrentFrame = nil;

					// Set the mode for this cycle object
					pCycle->CycleMode = CycleModeCIPOnly;

					// Deal with previous cycle objects jump target
					(pCycle->pPrev)->UpdateJumpTarget(pCycle->CycleMode, localIsocPort);

					// Bump currentDVTime to the next cycle
					currentDVTime += 3072.0;
					if (currentDVTime >= 196608000.0)
						currentDVTime -= 196608000.0;

					// Adjust CIP1 to bus-byte order
					pXmitBufWord[1] = EndianU32_NtoB(pXmitBufWord[1]);

					return;
				}
			}
			else
			{
				// Adjust CIP1 to bus-byte order
				pXmitBufWord[1] = EndianU32_NtoB(pXmitBufWord[1]);

				// No frame pull proc, so no continuing
				return;
			}
		}

		// Calculate the number of DV data bytes that are needed in this cycle
		cycleFrameBytes = pDVFormat->dbs * (1 << pDVFormat->fn) * (1 << DVspeed(dvMode)) * 4;

		// See if all this cycles bytes come from one frame, or
		// two frames (in the case of 2x or 4x frame rates)
		if  ((pCurrentFrame->curOffset+cycleFrameBytes) <= pCurrentFrame->frameLen)
		{
			// See if a frame starts in this cycle,
			if (pCurrentFrame->curOffset == 0)
			{
				// Generate SYT and add it to the CIP Header, and frame struct
				sytTime = calculateSYTTime();
				pXmitBufWord[1] &= 0xFFFF0000;
				pXmitBufWord[1] += (sytTime & 0x0000FFFF);
				pCurrentFrame->frameSYTTime = sytTime; // Set the SYT time into the current frame struct

				// Actual cycle time for transmit is sytTime, less the sourcePacketCycleCountStartValue, less 1
				actualFrameStartCycle = ((pCurrentFrame->frameSYTTime & 0x01FFF000) >> 12);
				actualFrameStartSeconds = ((pCurrentFrame->frameSYTTime & 0x0E000000) >> 25);

				actualFrameStartCycle -= (sourcePacketCycleCountStartValue - 1);
				if (actualFrameStartCycle < 0)
				{
					actualFrameStartCycle = 8000 - actualFrameStartCycle;
					actualFrameStartSeconds -= 1;
					if (actualFrameStartSeconds < 0)
						actualFrameStartSeconds = 8 - actualFrameStartSeconds;
				}
				pCurrentFrame->frameTransmitStartCycleTime = ((pCurrentFrame->frameSYTTime & 0x00000FFF) | (actualFrameStartCycle << 12) | (actualFrameStartSeconds << 25));
				pCurrentFrame->timeStampSecondsFieldValid = firstDCLCallbackOccurred;
			}

			// Copy the frame buffer data to the transmit buffer
			pFrameBufWord = (UInt32*) &pCurrentFrame->pFrameData[pCurrentFrame->curOffset];
			pCopyStartWord = &pXmitBufWord[2];
			memcpy(pCopyStartWord,pFrameBufWord,cycleFrameBytes);
				
			// Set the mode for this cycle object
			pCycle->CycleMode = CycleModeFull;
			
			// Bump the offset in the current frame
			pCurrentFrame->curOffset += cycleFrameBytes;

			// See if we need to get the next frame
			if (pCurrentFrame->curOffset == pCurrentFrame->frameLen)
			{
				// We have a frame that ends in this cycle. Save that information for future release
				savedNewFrameReleaseIndex = pCurrentFrame->frameIndex;
				useSavedNewFrameReleaseIndex = true;

				// Get the next frame
				result = framePullProc(&newFrameIndex,pFramePullProcRefCon);
				if ((result == kIOReturnSuccess) && (newFrameIndex < numFrames))
				{
					// Got the next frame
					pCurrentFrame = getFrame(newFrameIndex);

					// Initialize some new frame parameters
					pCurrentFrame->inUse = true;
					pCurrentFrame->curOffset = 0;
					pCurrentFrame->dclProgramRefCount += 1;
					pCurrentFrame->frameSYTTime = 0;
					pCurrentFrame->frameTransmitStartCycleTime = 0;
					pCurrentFrame->timeStampSecondsFieldValid = false;
				}
				else
				{
					// Didn't get another frame, so we are going to reuse the one we have
					if ((result == kIOReturnSuccess) && (newFrameIndex >= numFrames))
						logger->log("\nDVTransmitter Warning: Frame pull proc was given illegal frame index: %d\n",newFrameIndex);

					// Don't release the frame because we need it again
					useSavedNewFrameReleaseIndex = false;

					// Initialize some frame parameters
					pCurrentFrame->curOffset = 0;
					pCurrentFrame->dclProgramRefCount += 1;
					pCurrentFrame->frameSYTTime = 0;
					pCurrentFrame->frameTransmitStartCycleTime = 0;
					pCurrentFrame->timeStampSecondsFieldValid = false;

					// We should mute the audio in this frame for retransmission
					DVSilenceFrame(dvMode, pCurrentFrame->pFrameData);
				}
			}
		}
		else
		{
			// Note: No SYT in CIP for this packet, because the SYT should
			// only be in the CIP when a new frame starts at the beginning
			// of this packet, which isn't the case here. That's the spec!

			// Copy the frame buffer data to the transmit buffer
			pFrameBufWord = (UInt32*) &pCurrentFrame->pFrameData[pCurrentFrame->curOffset];
			pCopyStartWord = &pXmitBufWord[2];
			memcpy(pCopyStartWord,pFrameBufWord,(cycleFrameBytes/2));
			
			// Set the mode for this cycle object
			pCycle->CycleMode = CycleModeFull;
			
			pCurrentFrame->curOffset += (cycleFrameBytes/2);
			if (pCurrentFrame->curOffset != pCurrentFrame->frameLen)
				logger->log("\nDVTransmitter Warning: Frame not complete when expected!\n");

			// We have a frame that ends in this cycle. Save that information for future release
			savedNewFrameReleaseIndex = pCurrentFrame->frameIndex;
			useSavedNewFrameReleaseIndex = true;

			// Get the next frame
			result = framePullProc(&newFrameIndex,pFramePullProcRefCon);
			if ((result == kIOReturnSuccess) && (newFrameIndex < numFrames))
			{
				// Got the next frame
				pCurrentFrame = getFrame(newFrameIndex);

				// Initialize some new frame parameters
				pCurrentFrame->inUse = true;
				pCurrentFrame->curOffset = 0;
				pCurrentFrame->dclProgramRefCount += 1;
			}
			else
			{
				// Didn't get another frame, so we are going to reuse the one we have
				if ((result == kIOReturnSuccess) && (newFrameIndex >= numFrames))
					logger->log("\nDVTransmitter Warning: Frame pull proc was given illegal frame index: %d\n",newFrameIndex);

				// Don't release the frame because we need it again
				useSavedNewFrameReleaseIndex = false;

				// Initialize some frame parameters
				pCurrentFrame->curOffset = 0;
				pCurrentFrame->dclProgramRefCount += 1;

				// We should mute the audio in this frame for retransmission
				DVSilenceFrame(dvMode, pCurrentFrame->pFrameData);
			}

			// Set the SYT time into the current frame struct
			// Not that the SYT time is actually the SYT time for the
			// first byte of this cycle's packet, so the cycle-offset in this SYT
			// is somewhat wrong for a frame that starts in the middle of the packet
			// like this one does.
			pCurrentFrame->frameSYTTime = calculateSYTTime();

			// Actual cycle time for transmit is sytTime, less the sourcePacketCycleCountStartValue, less 1
			actualFrameStartCycle = ((pCurrentFrame->frameSYTTime & 0x01FFF000) >> 12);
			actualFrameStartSeconds = ((pCurrentFrame->frameSYTTime & 0x0E000000) >> 25);
			
			actualFrameStartCycle -= (sourcePacketCycleCountStartValue - 1);
			if (actualFrameStartCycle < 0)
			{
				actualFrameStartCycle = 8000 - actualFrameStartCycle;
				actualFrameStartSeconds -= 1;
				if (actualFrameStartSeconds < 0)
					actualFrameStartSeconds = 8 - actualFrameStartSeconds;
			}
			pCurrentFrame->frameTransmitStartCycleTime = ((pCurrentFrame->frameSYTTime & 0x00000FFF) | (actualFrameStartCycle << 12) | (actualFrameStartSeconds << 25));
			pCurrentFrame->timeStampSecondsFieldValid = firstDCLCallbackOccurred;
			
			// Copy the frame buffer data to the transmit buffer
			pFrameBufWord = (UInt32*) &pCurrentFrame->pFrameData[pCurrentFrame->curOffset];
			pCopyStartWord = &pXmitBufWord[2+(cycleFrameBytes/4)];
			memcpy(pCopyStartWord,pFrameBufWord,(cycleFrameBytes/2));
			
			// Bump current offset for this new frame
			pCurrentFrame->curOffset += (cycleFrameBytes/2);
		}
		
		// Bump currentIsochTime and currentDVTime
		if (DVstandard(dvMode) == kDVStandardNTSC)
		{
			currentDVTime += kIsochCycleClocksPerNTSCSourcePacket;
			currentIsochTime += kIsochCycleClocksPerNTSCSourcePacket;
		}
		else
		{
			currentDVTime += kIsochCycleClocksPerPALSourcePacket;
			currentIsochTime += kIsochCycleClocksPerPALSourcePacket;
		}
		if (currentDVTime >= 196608000.0)
			currentDVTime -= 196608000.0;

		// Bump dbc
		dbc += ((1 << pDVFormat->fn) * (1 << DVspeed(dvMode)));
	}
	else
	{
		// Send CIP Only

		// Set the mode for this cycle object
		pCycle->CycleMode = CycleModeCIPOnly;
	}

	// See if we have a frame to release back to the client
	if (useSavedNewFrameReleaseIndex == true)
	{

		pReleaseFrame = getFrame(savedNewFrameReleaseIndex);
		pReleaseFrame->inUse = false;
		pReleaseFrame->curOffset = 0;
		frameReleaseProc(savedNewFrameReleaseIndex,pFrameReleaseProcRefCon);
	}
	
    // Adjust currentIsoch time for next cycle
    currentIsochTime -= 3072.000;

    // Deal with previous cycle objects jump target
	if (doUpdateJumpTarget == true)
		(pCycle->pPrev)->UpdateJumpTarget(pCycle->CycleMode, localIsocPort);

	// Adjust CIP1 to bus-byte order
	pXmitBufWord[1] = EndianU32_NtoB(pXmitBufWord[1]);
	
	return;
}

//////////////////////////////////////////////////////////////////////
// calculateSYTTime
//////////////////////////////////////////////////////////////////////
unsigned int
DVTransmitter::calculateSYTTime(void)
{
	UInt32	cycle_count;
	UInt32  cycle_time;
	UInt32  cycle_seconds;
	
	UInt32 savedCurrentDVTime = (UInt32) currentDVTime;
	
	// Calculate the seconds field, and remove those clocks from savedCurrentDVTime
	cycle_seconds = savedCurrentDVTime/24576000;
	savedCurrentDVTime = savedCurrentDVTime - (cycle_seconds*24576000);
	
	// Calculate the cycles field, and remove those clocks from savedCurrentDVTime
	cycle_count = savedCurrentDVTime/3072;
	savedCurrentDVTime = savedCurrentDVTime - (cycle_count*3072);

	// Formulate a FireWire style time-stamp
	cycle_time = ((cycle_seconds << 25) | (cycle_count<<12) | savedCurrentDVTime);
	return cycle_time;
}

//////////////////////////////////////////////////////////////////////
// FindDVFormatForMode
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::FindDVFormatForMode(void)
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
// getNumFrames
//////////////////////////////////////////////////////////////////////
UInt32 DVTransmitter::getNumFrames(void)
{
	return numFrames;
}

//////////////////////////////////////////////////////////////////////
// getFrame
//////////////////////////////////////////////////////////////////////
DVTransmitFrame* DVTransmitter::getFrame(UInt32 frameIndex)
{
	return (frameIndex < numFrames) ? framePtrs[frameIndex]: 0 ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::RemotePort_GetSupported(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed *outMaxSpeed,
										  UInt64 *outChanSupported)
{
	// In this routine we return the capabilities of our remote device.

	// appropriate value
	*outMaxSpeed = xmitSpeed ;

	if (xmitChannel == kAnyAvailableIsochChannel)
	{
		// Allow the FireWireFamily to determine an available channel
		*outChanSupported	= ~1ULL;
	}
	else
	{
		// Use a specific channel
		*outChanSupported	= (((UInt64)0x80000000 << 32 | (UInt64)0x00000000) >> xmitChannel);
	}
	
	// ok!
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_AllocatePort
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kDVTransmitterAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);
	
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kDVTransmitterReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
DVTransmitter::RemotePort_Stop(
								  IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to stop listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// DVXmitDCLOverrunCallback
//////////////////////////////////////////////////////////////////////
void DVTransmitter::DVXmitDCLOverrunCallback(void)
{
	IOReturn result = kIOReturnSuccess ;
	DVTransmitFrame* pReleaseFrame;
	UInt32 i;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	if (transportState == kDVTransmitterTransportPlaying)
	{
		logger->log("\nDVTransmitter Error: DCL Overrun!\n\n");
		
		// Restart transmitter
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kDVTransmitterTransportStopped;

		// Release any frame that we have in use back to the client
		for (i=0;i<numFrames;i++)
		{
			if (framePtrs[i]->inUse == true)
			{
				pReleaseFrame = getFrame(i);
				pReleaseFrame->inUse = false;
				pReleaseFrame->curOffset = 0;
				pReleaseFrame->frameSYTTime = 0xFFFFFFFF;
				pReleaseFrame->frameTransmitStartCycleTime = 0xFFFFFFFF;
				pReleaseFrame->timeStampSecondsFieldValid = false;
				framePtrs[i]->dclProgramRefCount = 0;
				frameReleaseProc(i,pFrameReleaseProcRefCon);
			}
		}

		// Notify client of DCL overrun
		if (messageProc != nil)
			messageProc(kDVTransmitterDCLOverrun,0x00000000,0x00000000,pMessageProcRefCon);

		// Call prepareForTransmit, but don't have it call client
		prepareForTransmit(false);

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
				transportState = kDVTransmitterTransportPlaying;
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
// DVXmitDCLCallback
//////////////////////////////////////////////////////////////////////

void DVTransmitter::DVXmitDCLCallback( DVXmitCycle *pCallBackCycle )
{
	UInt16 nodeID;
	UInt32 generation;
	UInt32 actualTimeStampCycle;
	int lostCycles;
	DVXmitCycle *pThisSegmentsFirstCycle; 
	
#ifdef DVTransmitter_DCL_Callback_Timing
	AbsoluteTime startTime = UpTime();
	UInt64 thisTime;
#endif

	// Special debugging check to see if we received an out-of-order callback.
	// Note: the out-of-order delivery of callbacks shouldn't cause any problems,
	// but it's interesting to know if it can actually happen.
	//if (pCallBackCycle != ppCallbackCycles[currentSegment])
	//	logger->log("DVTransmitter Info: Got out-of-order callback.\n");
	
	// See if this callback happened after we stopped
	if (transportState == kDVTransmitterTransportStopped)
		return;
	
	// Get the local node ID
	do (*nodeNubInterface)->GetBusGeneration(nodeNubInterface, &generation);
	while  ((*nodeNubInterface)->GetLocalNodeIDWithGeneration(nodeNubInterface,generation,&nodeID) != kIOReturnSuccess);

	// Get the current end-of-cycle timestamp and convert it to cycles
	// 8000 cycles per second times 8 seconds gives a cycle number betwen 0 thru 63999
	actualTimeStampCycle = ((pTimeStamps[currentSegment] & 0x01FFF000) >> 12);
	actualTimeStampCycle += (((pTimeStamps[currentSegment] & 0x0E000000) >> 25) * 8000);
	
	// If the actual time stamp is not what we expect, we need to deal with
	// it here. 
	if (actualTimeStampCycle != expectedTimeStampCycle)
	{
		// Calculate lost cycles (deal with wrap-around condition)
		lostCycles = actualTimeStampCycle - expectedTimeStampCycle;
		if (lostCycles < 0)
			lostCycles += 64000; // based on 8 Seconds worth of cycles

		// See if the descrepency between actual and expected time-stamps is
		// only due to the fact that the initial value for the time-stsamp
		// "seconds field" was wrong. We don't notify clients, since this is expect 
		// to happen every 7 out of 8 times we start the DVTransmitter,
		// due to the fact that we don't know what the actual seconds field is going 
		// to be here unitl we've processed our first DCL callback.
		// TODO: In the future we could querry the current cycle clock, and make a good 
		// guess to what it will be when we first start the DCL program.
		if (((lostCycles % 8000) == 0) && (firstDCLCallbackOccurred == false))
		{
			//logger->log("DVTransmitter timestamp Seconds-Field adjust, old: %u  new:%u\n",(unsigned int) expectedTimeStampCycle,(unsigned int) actualTimeStampCycle);

			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
			
			// Using the actual time stamp captured, calculate the new current DV Time
			// for the start of the segment we are about to process
			actualTimeStampCycle =
				actualTimeStampCycle +
				1 +
				((isochSegments-1)*isochCyclesPerSegment) +
				sourcePacketCycleCountStartValue;
			
			actualTimeStampCycle %= 64000;  // modulo by 8 Seconds worth of cycles
			
			// Compensate for difference by modifying currentDVTime
			currentDVTime = actualTimeStampCycle*3072; 
		}
		else
		{
			logger->log("DVTransmitter timestamp adjust, old: %u  new:%u\n",
						(unsigned int) expectedTimeStampCycle,
						(unsigned int) actualTimeStampCycle);
			
			// Notify client of timestamp adjust
			if (messageProc != nil)
				messageProc(kDVTransmitterTimeStampAdjust,
							(unsigned int) expectedTimeStampCycle,
							(unsigned int) actualTimeStampCycle
							,pMessageProcRefCon);
			
			// Assume that the reason we've got a time-code discrepancy
			// here is that we lost one or more cycles during this segment.
			// If the number of cycles we lost is within a reasonable recovery
			// threshold, instead of adjusting our SYT times for subsequent frames,
			// we instead try to recover the lost time by discarding one or
			// more future CIP-only packets. If the discrepancy is greater than
			// that threshold, we have no choice but to just alter our future
			// SYT times to be correct.
			
			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
			
			// If the number of lost cycles is within our threshold,
			// recover the lost time. Otherwise, just bump the currentDVTime
			// which will affect future SYT times.
			if (lostCycles <= kDVTransmitterLostCycleRecoveryThreshold)
			{
				logger->log("DVTransmitter timestamp adjust, using lost-cycle recovery\n");
				
				// By reducing currentIsochTime, we will
				// transmit DIF data in places where there would
				// have been CIP-only packets
				currentIsochTime -= (3072.000 * lostCycles);
			}
			else
			{
				logger->log("DVTransmitter timestamp adjust, using SYT adjust\n");
				
				// Using the actual time stamp captured, calculate the new current DV Time
				// for the start of the segment we are about to process
				actualTimeStampCycle =
					actualTimeStampCycle +
					1 +
					((isochSegments-1)*isochCyclesPerSegment) +
					sourcePacketCycleCountStartValue;
				
				actualTimeStampCycle %= 64000;  // modulo by 8 Seconds worth of cycles
				
				// Compensate for difference by modifying currentDVTime
				currentDVTime = actualTimeStampCycle*3072; 
			}
		}
	}
	
	// Set the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = true;

	// Bump expected time stamp cycle value
	expectedTimeStampCycle += isochCyclesPerSegment;
	expectedTimeStampCycle %= 64000; // modulo by 8 Seconds worth of cycles

	// Fill this segments buffers
	pThisSegmentsFirstCycle = pNextUpdateCycle;
    while (pNextUpdateCycle != ppCallbackCycles[currentSegment]->pNext)
    {
		// Set the doUpdateJumpTarget to true for all but the first call to FillCycleBuffer
		// to prevent prematurely overwriting current branch to the overrun handler. 
		FillCycleBuffer(pNextUpdateCycle,nodeID, (pNextUpdateCycle == pThisSegmentsFirstCycle) ? false : true);
		pNextUpdateCycle = pNextUpdateCycle->pNext;
    };

	// Point this segment's callback cycle to the overrun callback DCL
    if (ppCallbackCycles[currentSegment]->CycleMode == CycleModeFull)
    {
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
										(DCLJump*) ppCallbackCycles[currentSegment]->pFullXfrJump,
										(DCLLabel*) pOverrunCallbackLabel);
    }
    else
    {
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
										(DCLJump*) ppCallbackCycles[currentSegment]->pCIPOnlyXfrJump,
										(DCLLabel*) pOverrunCallbackLabel);
    }
	
	// Modify the overrun stuff in the current last callback cycle to not overrrun here
    if ((pLastCallbackCycle->pNext)->CycleMode == CycleModeFull)
    {
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pLastCallbackCycle->pFullXfrJump,
								  (DCLLabel*) (pLastCallbackCycle->pNext)->pFullXfrLabel);
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pLastCallbackCycle->pCIPOnlyXfrJump,
								  (DCLLabel*) (pLastCallbackCycle->pNext)->pFullXfrLabel);
		pLastCallbackCycle->FullXfrJumpNextMode	= CycleModeFull;
		pLastCallbackCycle->CIPOnlyJumpNextMode	= CycleModeFull;
    }
    else
    {
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pLastCallbackCycle->pFullXfrJump,
								  (DCLLabel*) (pLastCallbackCycle->pNext)->pCIPOnlyXfrLabel);
		(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								  (DCLJump*) pLastCallbackCycle->pCIPOnlyXfrJump,
								  (DCLLabel*) (pLastCallbackCycle->pNext)->pCIPOnlyXfrLabel);
		pLastCallbackCycle->FullXfrJumpNextMode	= CycleModeCIPOnly;
		pLastCallbackCycle->CIPOnlyJumpNextMode	= CycleModeCIPOnly;
    }

    // Set a new last callback cycle
    pLastCallbackCycle = ppCallbackCycles[currentSegment];
	
#ifdef DVTransmitter_DCL_Callback_Timing
	thisTime = UnsignedWideToUInt64(AbsoluteDeltaToNanoseconds(UpTime(), startTime));
	
	// Add this sample to the accumulated total
	DCL_Timing_Accumulator += thisTime;
	DCL_Timing_Count += 1;

	// Check to see if this is the min or max we've seen so far
	if (thisTime > DCL_Timing_Max)
		DCL_Timing_Max = thisTime;
	if (thisTime < DCL_Timing_Min)
		DCL_Timing_Min = thisTime;
	
	// If it's time, dump timing statistics, and reset timing vars
	if (DCL_Timing_Count == 100)
	{
		// Dump timing statistics in micro-seconds
		logger->log("Min=%6qu uSec,  Max=%6qu uSec,  Avg=%6qu uSec\n",
			(DCL_Timing_Min/1000),(DCL_Timing_Max/1000),(DCL_Timing_Accumulator/100000));
		
		// Re-Initialize DCL Timing vars
		DCL_Timing_Accumulator = 0;
		DCL_Timing_Max = 0;
		DCL_Timing_Min = 0xFFFFFFFFFFFFFFFFLL;
		DCL_Timing_Count = 0;
	}
#endif
	
	// Bump Current Segment
	if (currentSegment != (isochSegments-1))
		currentSegment += 1;
	else
		currentSegment = 0;
	
	return;
}

//////////////////////////////////////////////////////////////////////
// DVXmitFinalizeCallback
//////////////////////////////////////////////////////////////////////
void DVTransmitter::DVXmitFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// DVXmitForceStop
//////////////////////////////////////////////////////////////////////
void DVTransmitter::DVXmitForceStop(UInt32 stopCondition)
{
	DVXmitDCLOverrunCallback();
}
#endif

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

	// Get the pointer to the DVTransmitter object from the refcon
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);

	// Call the DVTransmitter's remote port allocate-port callback
	result = pTransmitter->RemotePort_GetSupported(interface,&outMaxSpeed_Helper,&outChanSupported_Helper);

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
	// Get the pointer to the DVTransmitter object from the refcon
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);

	// Call the DVTransmitter's remote port allocate-port callback
	return pTransmitter->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVTransmitter object from the refcon
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);

	// Call the DVTransmitter's remote port release-port callback
	return pTransmitter->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVTransmitter object from the refcon
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);

	// Call the DVTransmitter's remote port start callback
	return pTransmitter->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the DVTransmitter object from the refcon
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);

	// Call the DVTransmitter's remote port stop callback
	return pTransmitter->RemotePort_Stop(interface);
}

//////////////////////////////////////////////////////////////////////
// DVXmitDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void DVXmitDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the xmit cycle object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    DVXmitCycle *pCallBackCycle = (DVXmitCycle*) pCallProc->procData;

	pCallBackCycle->pTransmitter->DVXmitDCLCallback(pCallBackCycle);
	return;
}

//////////////////////////////////////////////////////////////////////
// DVXmitDCLOverrunCallback_Helper
//////////////////////////////////////////////////////////////////////
static void DVXmitDCLOverrunCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the DVTransmitter object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    DVTransmitter *pTransmitter = (DVTransmitter*) pCallProc->procData;

	pTransmitter->DVXmitDCLOverrunCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// DVXmitFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn DVXmitFinalizeCallback_Helper( void* refcon )
{
	DVTransmitter *pDVTransmitter = (DVTransmitter*) refcon;
	pDVTransmitter->DVXmitFinalizeCallback();
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// DVSilenceFrame
//////////////////////////////////////////////////////////////////////
void DVSilenceFrame(UInt8 mode, UInt8* frame)
{
	// TODO: This only work correctly for 25mbps NTSC, or PAL
	// Extend support for DV50, and  DV100 formats
	
    UInt32    i,j,k,n;
    UInt8    *tPtr;
	UInt8    sType = ((mode & 0x7C) >> 2);

    //syslog(LOG_INFO, "silencing frame %p\n", frame);

    // Get DSF flag in byte 3 of header (Blue Book p. 113)
    tPtr = frame;
    if ((tPtr[3] & 0x80) == 0)
        n=10;                            // ntsc
    else
        n=12;                            // pal

	if (sType == 1)
		n /= 2;							//  SDL
	else if (sType == 0x1D)
		n *= 2;							// DVCPro-50

    // Mute all the audio samples

    for (i=0;i<n;i++)
    {
        for (j=0;j<9;j++)
        {
            tPtr = frame + (i * 12000) + ((j * 16 + 6) * 80) + 8;
            for (k=0;k<72;k++)
                *tPtr++ = 0x0;
        }
    }
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// DVXmitForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void DVXmitForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	DVTransmitter *pTransmitter = (DVTransmitter*) (*interface)->GetRefCon(interface);
	
	// Call the DVTransmitter's Force Stop callback
	return pTransmitter->DVXmitForceStop(stopCondition);
}
#endif

} // namespace AVS

