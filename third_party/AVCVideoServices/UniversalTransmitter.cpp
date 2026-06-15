/*
	File:		UniversalTransmitter.cpp

	Synopsis: This is the implementation file for the UniversalTransmitter class. 
 
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

static void UniversalTransmitterDCLCallback_Helper( void* refcon, NuDCLRef dcl );
static void UniversalTransmitterDCLOverrunCallback_Helper( void* refcon, NuDCLRef dcl );

static IOReturn UniversalTransmitterFinalizeCallback_Helper( void* refcon ) ;

#ifdef kAVS_Enable_ForceStop_Handler	
static void	UniversalTransmitterForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

// This is the maximum number of DCLs that can be passed in a Notif(...) call.
#define kMaxNuDCLsPerNotify 30

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
UniversalTransmitter::UniversalTransmitter(StringLogger *stringLogger,
										   IOFireWireLibNubRef nubInterface,
										   unsigned int cyclesPerSegment,
										   unsigned int numSegments,
										   unsigned int clientTransmitBufferSize,
										   bool doIRMAllocations,
										   unsigned int irmAllocationMaxPacketSize,
										   unsigned int numStartupCycleMatchBits)
{
    nodeNubInterface = nubInterface;
	remoteIsocPort = nil;
	localIsocPort = nil;
	packetFetch = nil;
	isochChannel = nil;
	messageProc = nil;
	pTransmitBuffer = nil;
	xmitChannel = 0;
	xmitSpeed = kFWSpeed100MBit;
	runLoopRef = nil;
	
	pSegUpdateBags = nil;
	pProgramDCLs = nil;
	pCycleInfo = nil;
	nuDCLPool = nil;
	
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

	transportState = kUniversalTransmitterTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	doIRM = doIRMAllocations;
	clientBufferSize = clientTransmitBufferSize;
	irmAllocationPacketSize = irmAllocationMaxPacketSize;

	// Generate the startup cycle-match value to be used when creating the isoch port
	if (numStartupCycleMatchBits > 20)
		numStartupCycleMatchBits = 20;
	startupCycleMatchMask = 0;
	for (unsigned int i=0;i<numStartupCycleMatchBits;i++)
	{
		startupCycleMatchMask |= (0x00001000 << i);
	}
	//printf("AY_DEBUG: startupCycleMatchMask = 0x%08X\n",startupCycleMatchMask);
	
	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
UniversalTransmitter::~UniversalTransmitter()
{

	if (transportState != kUniversalTransmitterTransportStopped)
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

	if (nuDCLPool)
		(*nuDCLPool)->Release(nuDCLPool);
	
	// If we created an internall logger, free it
	if (noLogger == true)
		delete logger;

	// Free the vm allocated DCL buffer
	if (pTransmitBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pTransmitBuffer,transmitBufferSize);
	
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->Release(nodeNubInterface);

	if (pProgramDCLs)
		delete [] pProgramDCLs;

	if (pCycleInfo)
		delete [] pCycleInfo;
	
	if (pSegUpdateBags)
	{
		// Release bags
		for (UInt32 seg=0;seg<isochSegments;seg++)
			CFRelease(pSegUpdateBags[seg]);
		
		// Release array
		delete [] pSegUpdateBags;
	}
	
	// Release the transport control mutex
	pthread_mutex_destroy(&transportControlMutex);
}

//////////////////////////////////////////////////////
// setupIsocTransmitter
//////////////////////////////////////////////////////
IOReturn UniversalTransmitter::setupIsocTransmitter(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess ;
	UInt32 totalObjects = isochCyclesPerSegment * isochSegments;
    UInt8 *pBuffer = nil;
    UInt32 i;
	IOVirtualRange bufRange;
	IOFireWireLibNubRef newNubInterface;
	
	UInt32 programDCLCount;
	NuDCLSendPacketRef thisDCL;
	UInt32 seg;
	UInt32 cycle;

	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nUniversalTransmitter Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nUniversalTransmitter Error: Error duplicating device interface: 0x%08X\n\n",result);
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

	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// To calculate how much vm_allocated memory we need:
	//
	// Total cycles in DCL program = totalObjects = isochCyclesPerSegment * isochSegments
	//
	// We need vm memory for:
	//
	//	totalObjects * 16 for Isoch headers + masks
	//  isochSegments * 4 for time-stamps
	//  client's buffer
	//
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	
	// Allocate an array to hold pointers to our DCLs
	pProgramDCLs = new NuDCLSendPacketRef[totalObjects];
	if (!pProgramDCLs)
	{
		return kIOReturnNoMemory ;
	}
	
	// Allocate an array of bags for end-of-segment dcl update lists
	pSegUpdateBags = new CFMutableSetRef[isochSegments];
	if (!pSegUpdateBags)
	{
		return kIOReturnNoMemory ;
	}
	else
		for (i=0;i<isochSegments;i++)
			pSegUpdateBags[i] = nil;
	
	transmitBufferSize = clientBufferSize + (totalObjects * 16) + (isochSegments * 4);
	
	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer, transmitBufferSize , VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nUniversalTransmitter Error: Error allocating isoch transmit buffers\n\n");
		return kIOReturnNoMemory ;
    }
	else
		bzero(pBuffer, transmitBufferSize);

	// Set the buffer range var
	bufRange.address = (IOVirtualAddress) pBuffer;
	bufRange.length = transmitBufferSize;
	
	// Save a pointer to the transmit buffer
	pTransmitBuffer = pBuffer;
	
	// Set the various pointer into our allocated VM memory
	pClientBuffer = (UInt8*) pTransmitBuffer;	// At the base of the allocated region to guarrantee page alignment!
	pClientBufferUpperLimit = pClientBuffer + clientBufferSize;
	pIsochHeaders = (UInt32*) &pTransmitBuffer[clientBufferSize];
	pTimeStamps = (UInt32*) &pTransmitBuffer[clientBufferSize+(totalObjects * 16)];
	
	// Allocate an array of UniversalTransmitterCycleInfo objects
	pCycleInfo = new UniversalTransmitterCycleInfo[totalObjects];
	if (!pCycleInfo)
	{
		return kIOReturnNoMemory ;
	}
	for (i=0;i<totalObjects;i++)
	{
		pCycleInfo[i].numRanges = 1;
		
		pCycleInfo[i].ranges[0].address = (IOVirtualAddress) pClientBuffer;
		pCycleInfo[i].ranges[0].length = (IOByteCount) 8 ;
		
		pCycleInfo[i].sy = 0;
		pCycleInfo[i].tag = 0;
		
		pCycleInfo[i].index = i;
	}
	
	// Use the nub interface to create a NuDCL command pool object
	nuDCLPool = (*nodeNubInterface)->CreateNuDCLPool(nodeNubInterface,
													 0,
													 CFUUIDGetUUIDBytes( kIOFireWireNuDCLPoolInterfaceID ));
	if (!nuDCLPool)
    {
		logger->log("\nUniversalTransmitter Error: Error creating NuDCL command pool\n\n");
		return kIOReturnError ;
    }
	
	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 false,
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nUniversalTransmitter Error: Error creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this UniversalTransmitter object in
	// the remote isoch port's refcon variable
	(*remoteIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) remoteIsocPort,this) ;

	// Use the remote port interface to install some callback handlers.
	(*remoteIsocPort)->SetGetSupportedHandler( remoteIsocPort, & RemotePort_GetSupported_Helper );
	(*remoteIsocPort)->SetAllocatePortHandler( remoteIsocPort, & RemotePort_AllocatePort_Helper );
	(*remoteIsocPort)->SetReleasePortHandler( remoteIsocPort, & RemotePort_ReleasePort_Helper );
	(*remoteIsocPort)->SetStartHandler( remoteIsocPort, & RemotePort_Start_Helper );
	(*remoteIsocPort)->SetStopHandler( remoteIsocPort, & RemotePort_Stop_Helper );

    // Create DCL Program
	programDCLCount = 0;
	for (seg=0;seg<isochSegments;seg++)
	{
		pSegUpdateBags[seg] = CFSetCreateMutable(NULL, 1, NULL);
		
		for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
		{
			range[0].address = (IOVirtualAddress) pClientBuffer ;
			range[0].length = (IOByteCount) 8 ;
			
			thisDCL = (*nuDCLPool)->AllocateSendPacket(nuDCLPool,
													   ((cycle == (isochCyclesPerSegment-1)) ? pSegUpdateBags[seg] : NULL),
													   1,
													   &range[0]);
			
			if (!thisDCL)
			{
				logger->log("\nUniversalTransmitter Error: Error allocating Send Packet DCL\n\n");
				return kIOReturnNoMemory ;
			}
			
			// Flasgs
			(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
			
			// Refcon
			(*nuDCLPool)->SetDCLRefcon( thisDCL, this);
			
			// Isoch header and mask pointers. Note: Actual values will be set in FillCycleBuffer(...)
			(*nuDCLPool)->SetDCLUserHeaderPtr( thisDCL, 
											   &pIsochHeaders[(seg*isochCyclesPerSegment*4)+(cycle*4)], 
											   &pIsochHeaders[(seg*isochCyclesPerSegment*4)+(cycle*4)+2]) ;
			
			// Special functionality for the last dcl of the segment
			if (cycle == (isochCyclesPerSegment-1))
			{
				// Last cycle of the segment
				
				// Timestamp
				(*nuDCLPool)->SetDCLTimeStampPtr(thisDCL, &pTimeStamps[seg]) ;
				
				// Update
				(*nuDCLPool)->SetDCLUpdateList( thisDCL, pSegUpdateBags[seg] ) ;
				
				// Callback
				(*nuDCLPool)->SetDCLCallback( thisDCL, UniversalTransmitterDCLCallback_Helper) ;
			}
			
			// Save a pointer to thisDCL
			pProgramDCLs[programDCLCount] = thisDCL;
			programDCLCount += 1;
		}
	}
	
	// Allocate Overrun DCL
	thisDCL = (*nuDCLPool)->AllocateSendPacket(nuDCLPool,
											   NULL,
											   1,
											   &range[0]);
	if (!thisDCL)
	{
		logger->log("\nUniversalTransmitter Error: Error allocating Send Packet DCL\n\n");
		return kIOReturnNoMemory ;
	}
	
	// Set this DCL as the overrun DCL
	overrunDCL = thisDCL;
	
	// Flasgs
	(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
	
	// Refcon
	(*nuDCLPool)->SetDCLRefcon( thisDCL, this ) ;
	
	// Callback
	(*nuDCLPool)->SetDCLCallback( thisDCL, UniversalTransmitterDCLOverrunCallback_Helper) ;
	
	// Enable the following for debugging, to print out the DCL program!
	//(*nuDCLPool)->PrintProgram(nuDCLPool);
	
	// Using the nub interface to the local node, create
	// a local isoc port.
	localIsocPort = (*nodeNubInterface)->CreateLocalIsochPort(
															  nodeNubInterface,
															  true,
															  (*nuDCLPool)->GetProgram(nuDCLPool),
															  (startupCycleMatchMask == 0) ? 0 : kFWDCLCycleEvent,
															  0,
															  startupCycleMatchMask,
															  nil,
															  0,
															  &bufRange,
															  1,
															  CFUUIDGetUUIDBytes( kIOFireWireLocalIsochPortInterfaceID ));
	if (!localIsocPort)
    {
		logger->log("\nUniversalTransmitter Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	
	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, UniversalTransmitterFinalizeCallback_Helper) ;
	
	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														irmAllocationPacketSize,
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nUniversalTransmitter Error: Error creating isoch channel object: 0x%08X\n\n",result);
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
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,UniversalTransmitterForceStopHandler_Helper);

	// Turn on notification
	(*isochChannel)->TurnOnNotification(isochChannel);
#endif
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// startTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::startTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	// Make sure we are not already running
	if (transportState == kUniversalTransmitterTransportPlaying)
	{
		// Unlock the transport control mutex
		pthread_mutex_unlock(&transportControlMutex);
		
		// Return a device already open error
		return kIOReturnExclusiveAccess;
	}
	
	// Prepare for transmit
	prepareForTransmit();
	
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
			transportState = kUniversalTransmitterTransportPlaying;
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
UniversalTransmitter::stopTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already stopped
	if (transportState == kUniversalTransmitterTransportStopped)
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
	transportState = kUniversalTransmitterTransportStopped;
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Wait for the finalize callback to fire for this stream
	if (result == kIOReturnSuccess)
		while (finalizeCallbackCalled == false) usleep(1000);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// registerDataPullCallback
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::registerDataPullCallback(UniversalTransmitterDataPullProc handler, void *pRefCon)
{
	packetFetch = handler;
	pPacketFetchRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerMessageCallback
//////////////////////////////////////////////////////////////////////
IOReturn UniversalTransmitter::registerMessageCallback(UniversalTransmitterMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// getClientBufferPointer
//////////////////////////////////////////////////////////////////////
UInt8* UniversalTransmitter::getClientBufferPointer(void)
{
	return pClientBuffer;
}

//////////////////////////////////////////////////////////////////////
// getClientBufferLen
//////////////////////////////////////////////////////////////////////
UInt32 UniversalTransmitter::getClientBufferLen(void)
{
	return clientBufferSize;
}

//////////////////////////////////////////////////////////////////////
// getUniversalTransmitterCycleInfoArray
//////////////////////////////////////////////////////////////////////
UniversalTransmitterCycleInfo* UniversalTransmitter::getUniversalTransmitterCycleInfoArray(void)
{
	return pCycleInfo;
}

//////////////////////////////////////////////////////////////////////
// getUniversalTransmitterCycleInfoArrayCount
//////////////////////////////////////////////////////////////////////
UInt32 UniversalTransmitter::getUniversalTransmitterCycleInfoArrayCount(void)
{
	return (isochCyclesPerSegment * isochSegments);
}

//////////////////////////////////////////////////////////////////////
// prepareForTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::prepareForTransmit(void)
{
	UInt16 nodeID;
	UInt32 generation;

	IOReturn result;
	UInt32 segment, cycle;
	UInt32 numDCLsNotified,totalDCLsToNotify,numDCLsForThisNotify;
	
	// Get the local node ID
	do (*nodeNubInterface)->GetBusGeneration(nodeNubInterface, &generation);
	while  ((*nodeNubInterface)->GetLocalNodeIDWithGeneration(nodeNubInterface,generation,&nodeID) != kIOReturnSuccess);

	currentSegment = 0;
	
	// Clear the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = false;

	// Prepare the packet fetcher
	if (messageProc != nil)
			messageProc(kUniversalTransmitterPreparePacketFetcher,0x00000000,0x00000000,pMessageProcRefCon);

	// Setup the end-of-segment jump targets.
	for (segment=0;segment<isochSegments;segment++)
		(*nuDCLPool)->SetDCLBranch(pProgramDCLs[((segment+1)*isochCyclesPerSegment)-1], (segment == (isochSegments-1)) ? overrunDCL: pProgramDCLs[(segment+1)*isochCyclesPerSegment]);
		
	// Set the expectedTimeStampCycle to what it needs to be for starting to prefetching data into the cycle buffers
	expectedTimeStampCycle = 0;
	
	// Initialize all the cycles in the program with data!
	for (segment=0;segment<isochSegments;segment++)
		for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
		{
			FillCycleBuffer(pProgramDCLs[(segment*isochCyclesPerSegment)+cycle],nodeID,segment,cycle);
			expectedTimeStampCycle += 1;
			expectedTimeStampCycle %= 64000; // modulo by 8 Seconds worth of cycles
		}
			
	// Set the expectedTimeStampCycle to what we expect it to be in the first DCL callback!
	expectedTimeStampCycle = isochCyclesPerSegment-1;
		
	// Do a Modification notification for all DCLs in the program (except for overrun dcl)
	numDCLsNotified = 0;
	totalDCLsToNotify = (isochSegments * isochCyclesPerSegment);
	while (numDCLsNotified < totalDCLsToNotify)
	{
		// Calculate how many dcls to notify for this iteration!
		numDCLsForThisNotify = ((totalDCLsToNotify-numDCLsNotified) > kMaxNuDCLsPerNotify) ? 
			kMaxNuDCLsPerNotify : (totalDCLsToNotify-numDCLsNotified);
		
		// Do the notify
		result = (*localIsocPort)->Notify( localIsocPort,
										   kFWNuDCLModifyNotification,
										   (void**) &pProgramDCLs[numDCLsNotified],
										   numDCLsForThisNotify) ;
		if (result != kIOReturnSuccess)
			logger->log("\nUniversalTransmitter Error: NuDCL kFWNuDCLModifyNotification Error in prepareForTransmit: 0x%08X\n\n",result);
		
		// Bump
		numDCLsNotified += numDCLsForThisNotify;
	}
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochChannel
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::setTransmitIsochChannel(unsigned int chan)
{
	xmitChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::setTransmitIsochSpeed(IOFWSpeed speed)
{
	xmitSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// FillCycleBuffer
//////////////////////////////////////////////////////////////////////
void
UniversalTransmitter::FillCycleBuffer(NuDCLSendPacketRef dcl, UInt16 nodeID, UInt32 segment, UInt32 cycle)
{
	UInt32 *pIsochHeaderAndMask = &pIsochHeaders[(segment*isochCyclesPerSegment*4)+(cycle*4)];
	UInt32 transmitTimeInClocks;
	UInt32 currentCycleTimeInClocks;
	int timeStampDeltaInClocks;
	Boolean badRanges = false;
	
	// Determine which cycle info struct is associated with this dcl
	UniversalTransmitterCycleInfo *pInfo = &pCycleInfo[(segment*isochCyclesPerSegment)+cycle];

	// Set the current local node ID into the cycle info struct (so client can generate valid CIP headers)
	pInfo->nodeID = nodeID;

	// Set the expected transmit timestamp in the cycle info struct
	if (firstDCLCallbackOccurred != true)
	{
		// Here, we are being called from prepareForTransmit; expectedTimeStampCycle is correct for this cycle,
		// but we don't know the actual "seconds" field value
	
		pInfo->expectedTransmitCycleTime = ((expectedTimeStampCycle/8000) << 25) + ((expectedTimeStampCycle%8000) << 12);
		pInfo->dclProgramRunning = false;
	}
	else
	{
		// Here, we are being called from the DCL callback; expectedTimeStampCycle is behind by numSegments-2 worth of cycles,
		// and, at this point the "seconds" field is valid
		
		UInt32 bumpedExpectedTimeStampCycle = ((expectedTimeStampCycle + (isochCyclesPerSegment*(isochSegments-1)))%64000);
		pInfo->expectedTransmitCycleTime = ((bumpedExpectedTimeStampCycle/8000) << 25) + ((bumpedExpectedTimeStampCycle%8000) << 12);
		pInfo->dclProgramRunning = true;
	}
	
	if (firstDCLCallbackOccurred)
	{
		transmitTimeInClocks = (((pInfo->expectedTransmitCycleTime & 0x01FFF000) >> 12)*3072);
		currentCycleTimeInClocks = (((currentFireWireCycleTime & 0x01FFF000) >> 12)*3072);
		timeStampDeltaInClocks = transmitTimeInClocks - currentCycleTimeInClocks;
		if (timeStampDeltaInClocks < 0)
			timeStampDeltaInClocks += 24576000;
		
		pInfo->transmitTimeInNanoSeconds = currentUpTimeInNanoSecondsU64 + (timeStampDeltaInClocks*40.690104167);	// 40.690104167 is nanoseconds per FireWire clock tick!
	
	}
	else
	{
		pInfo->transmitTimeInNanoSeconds = 0;
	}
	
	// Call the client to get data
	if (packetFetch)
	{
		packetFetch(pInfo, this, pPacketFetchRefCon);

		// Here we validate that the ranges the client attached to this cycle are in the client's vm allcated address range 
		for(unsigned int i = 0; i < pInfo->numRanges; i++)
		{
			// Only check if length is greater than zero!
			if (pInfo->ranges[i].length)
			{
				if ((pInfo->ranges[i].address < (IOVirtualAddress) pClientBuffer) || 
					((pInfo->ranges[i].address + pInfo->ranges[i].length)  > (IOVirtualAddress) pClientBufferUpperLimit)) 
				{
					badRanges = true;
				}
			}
		}
		
		// Program new ranges into this dcl if all ranges are in range. Otherwise, notify client of bad ranges!
		if (badRanges == false)
			(*nuDCLPool)->SetDCLRanges(dcl,pInfo->numRanges,pInfo->ranges);
		else if (messageProc != nil)
			messageProc(kUniversalTransmitterBadBufferRange,0x00000000,0x00000000,pMessageProcRefCon);
	}
	
	// Setup isoch header values.
	// Note: We only take control of the SY and TAG fields. Leave the rest to the DCL engine to control!
	pIsochHeaderAndMask[0] = (((pInfo->tag & 0x3) << 14) | (pInfo->sy & 0xF));
	pIsochHeaderAndMask[2] = 0x0000C00F; // User-control of just the SY and TAG field
	pIsochHeaderAndMask[3] = 0x00000000; // All bits in this header quad are auto-set!
	
	// Note: the DCL Modification notification will happen outside of this function!
	
	return;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::RemotePort_GetSupported(
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
UniversalTransmitter::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kUniversalTransmitterAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);
	
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kUniversalTransmitterReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalTransmitter::RemotePort_Stop(
								  IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to stop listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterDCLOverrunCallback
//////////////////////////////////////////////////////////////////////
void UniversalTransmitter::UniversalTransmitterDCLOverrunCallback(void)
{
	IOReturn result = kIOReturnSuccess ;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	if (transportState == kUniversalTransmitterTransportPlaying)
	{
		logger->log("\nUniversalTransmitter Error: DCL Overrun!\n\n");
		
		// Restart transmitter
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kUniversalTransmitterTransportStopped;
		
		// Prepare for transmit
		prepareForTransmit();
		
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
				transportState = kUniversalTransmitterTransportPlaying;
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
// UniversalTransmitterDCLCallback
//////////////////////////////////////////////////////////////////////
void UniversalTransmitter::UniversalTransmitterDCLCallback(void)
{
	UInt16 nodeID;
	UInt32 generation;
	UInt32 actualTimeStampCycle;
	int lostCycles;
	NuDCLRef pLastSegEndDCL;
	IOReturn result;
	UInt32 cycle;
	UInt32 numDCLsNotified,totalDCLsToNotify,numDCLsForThisNotify;
	
	UInt32 outBusTime;
	AbsoluteTime currentUpTime;
	Nanoseconds currentUpTimeInNanoSeconds;
	
	// Special debugging check to see if we received an out-of-order callback.
	// Note: the out-of-order delivery of callbacks shouldn't cause any problems,
	// but it's interesting to know if it can actually happen.
	//if (pCallBackCycle != ppCallbackCycles[currentSegment])
	//	logger->log("UniversalTransmitter Info: Got out-of-order callback.\n");
	
	// See if this callback happened after we stopped
	if (transportState == kUniversalTransmitterTransportStopped)
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
		// to happen every 7 out of 8 times we start the UniversalTransmitter,
		// due to the fact that we don't know what the actual seconds field is going 
		// to be here unitl we've processed our first DCL callback.
		// TODO: In the future we could querry the current cycle clock, and make a good 
		// guess to what it will be when we first start the DCL program.
		if (((lostCycles % 8000) == 0) && (firstDCLCallbackOccurred == false))
		{
			//logger->log("UniversalTransmitter timestamp Seconds-Field adjust, old: %u  new:%u\n",(unsigned int) expectedTimeStampCycle,(unsigned int) actualTimeStampCycle);
			
			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
		}
		else
		{
			logger->log("UniversalTransmitter timestamp adjust, old: %u  new:%u\n",
						(unsigned int) expectedTimeStampCycle,
						(unsigned int) actualTimeStampCycle);
			
			// Notify client of timestamp adjust
			if (messageProc != nil)
				messageProc(kUniversalTransmitterTimeStampAdjust,
							(unsigned int) expectedTimeStampCycle,
							(unsigned int) actualTimeStampCycle
							,pMessageProcRefCon);
			
			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
		}
	}
	
	// Get cycle timer and up-time. Do this in quick succession to best
	// establish the relationship between the two.
	(*nodeNubInterface)->GetBusCycleTime( nodeNubInterface, &outBusTime, &currentFireWireCycleTime);
	currentUpTime = UpTime();
	currentUpTimeInNanoSeconds = AbsoluteToNanoseconds(currentUpTime);
	currentUpTimeInNanoSecondsU64 = ((UInt64) currentUpTimeInNanoSeconds.hi << 32) | currentUpTimeInNanoSeconds.lo;
	
	// Reduce currentUpTimeInNanoSecondsU64 to eliminate the FW "offset" value from currentFireWireCycleTime.
	// Since the expectedTimeStampCycle time we'll be comparing it to doesn't include any "offset" value!
	currentUpTimeInNanoSecondsU64 -= ((currentFireWireCycleTime&0x00000FFF)*40.690104167);
	
	// Set the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = true;

	// Need to bump expectedTimeStampCycle by one here, since it's currently off by one 
	expectedTimeStampCycle += 1;
	expectedTimeStampCycle %= 64000; // modulo by 8 Seconds worth of cycles
	
	// Fill this segments buffers
	for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
	{
		FillCycleBuffer(pProgramDCLs[(currentSegment*isochCyclesPerSegment)+cycle],nodeID,currentSegment,cycle);

		// Don't bump on the last cycle of this segment here. Will do in next DCL callback prior to filling cycle buffers!
		if (cycle<(isochCyclesPerSegment-1))
		{
			expectedTimeStampCycle += 1;
			expectedTimeStampCycle %= 64000; // modulo by 8 Seconds worth of cycles
		}
	}
	
	// Point this segment's callback cycle to the overrun callback DCL
	(*nuDCLPool)->SetDCLBranch(pProgramDCLs[((currentSegment+1)*isochCyclesPerSegment)-1], overrunDCL);
	
	
	numDCLsNotified = 0;
	totalDCLsToNotify = isochCyclesPerSegment;
	while (numDCLsNotified < totalDCLsToNotify)
	{
		// Calculate how many dcls to notify for this iteration!
		numDCLsForThisNotify = ((totalDCLsToNotify-numDCLsNotified) > kMaxNuDCLsPerNotify) ? 
			kMaxNuDCLsPerNotify : (totalDCLsToNotify-numDCLsNotified);
		
		// Do the notify
		result = (*localIsocPort)->Notify( localIsocPort,
										   kFWNuDCLModifyNotification,
										   (void**) &pProgramDCLs[(currentSegment*isochCyclesPerSegment)+numDCLsNotified],
										   numDCLsForThisNotify) ;
		if (result != kIOReturnSuccess)
			logger->log("\nUniversalTransmitter Error: NuDCL kFWNuDCLModifyNotification Error in UniversalTransmitterDCLCallback: 0x%08X\n\n",result);
		
		// Bump
		numDCLsNotified += numDCLsForThisNotify;
	}
	
	// Modify the overrun stuff in the current last callback cycle to not overrrun here
	pLastSegEndDCL = (currentSegment == 0) ? pProgramDCLs[(((isochSegments-1)+1)*isochCyclesPerSegment)-1] : pProgramDCLs[(((currentSegment-1)+1)*isochCyclesPerSegment)-1];
	(*nuDCLPool)->SetDCLBranch(pLastSegEndDCL, pProgramDCLs[currentSegment*isochCyclesPerSegment]);
	
	// Send a notify about this dcl modification
	result = (*localIsocPort)->Notify( localIsocPort,
									   kFWNuDCLModifyJumpNotification,
									   (void**) &pLastSegEndDCL,
									   1) ;
	if (result != kIOReturnSuccess)
	{
		logger->log("\nUniversalTransmitter Error: NuDCL kFWNuDCLModifyJumpNotification Notify Error in UniversalTransmitterDCLCallback: 0x%08X\n\n",result);
	}
	
	// Bump Current Segment
	if (currentSegment != (isochSegments-1))
		currentSegment += 1;
	else
		currentSegment = 0;
	
	return;
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterFinalizeCallback
//////////////////////////////////////////////////////////////////////
void UniversalTransmitter::UniversalTransmitterFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// UniversalTransmitterForceStop
//////////////////////////////////////////////////////////////////////
void UniversalTransmitter::UniversalTransmitterForceStop(UInt32 stopCondition)
{
	UniversalTransmitterDCLOverrunCallback();
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

	// Get the pointer to the UniversalTransmitter object from the refcon
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);

	// Call the UniversalTransmitter's remote port allocate-port callback
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
	// Get the pointer to the UniversalTransmitter object from the refcon
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);

	// Call the UniversalTransmitter's remote port allocate-port callback
	return pTransmitter->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalTransmitter object from the refcon
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);

	// Call the UniversalTransmitter's remote port release-port callback
	return pTransmitter->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalTransmitter object from the refcon
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);

	// Call the UniversalTransmitter's remote port start callback
	return pTransmitter->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalTransmitter object from the refcon
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);

	// Call the UniversalTransmitter's remote port stop callback
	return pTransmitter->RemotePort_Stop(interface);
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalTransmitterDCLCallback_Helper( void* refcon, NuDCLRef dcl )
{
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) refcon;
	pTransmitter->UniversalTransmitterDCLCallback();
	
	return;
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterDCLOverrunCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalTransmitterDCLOverrunCallback_Helper( void* refcon, NuDCLRef dcl )
{
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) refcon;
	pTransmitter->UniversalTransmitterDCLOverrunCallback();
	
	return;
}

//////////////////////////////////////////////////////////////////////
// UniversalTransmitterFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn UniversalTransmitterFinalizeCallback_Helper( void* refcon )
{
	UniversalTransmitter *pUniversalTransmitter = (UniversalTransmitter*) refcon;
	pUniversalTransmitter->UniversalTransmitterFinalizeCallback();
	return kIOReturnSuccess;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// UniversalTransmitterForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalTransmitterForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	UniversalTransmitter *pTransmitter = (UniversalTransmitter*) (*interface)->GetRefCon(interface);
	
	// Call the UniversalTransmitter's Force Stop callback
	return pTransmitter->UniversalTransmitterForceStop(stopCondition);
}
#endif

} // namespace AVS
