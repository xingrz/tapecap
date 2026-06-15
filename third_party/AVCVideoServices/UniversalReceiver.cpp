/*
	File:		UniversalReceiver.cpp

    Synopsis: This is the implementation file for the UniversalReceiver class.
 
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

#ifdef kAVS_Use_NuDCL_UniversalReceiver
static void UniversalReceiveDCLCallback_Helper( void* refcon, NuDCLRef dcl );
static void UniversalReceiveOverrunDCLCallback_Helper( void* refcon, NuDCLRef dcl );
#else
static void UniversalReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand);
static void UniversalReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand);
#endif

static IOReturn UniversalReceiveFinalizeCallback_Helper( void* refcon ) ;

static UInt32  AddFWCycleTimeToFWCycleTime( UInt32 cycleTime1, UInt32 cycleTime2 );

#ifdef kAVS_Enable_ForceStop_Handler	
static void	UniversalReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

//////////////////////////////////////////////////////
// NoDataTimeoutHelper
//////////////////////////////////////////////////////
static void NoDataTimeoutHelper(CFRunLoopTimerRef timer, void *data)
{
	UniversalReceiver *pUniversalReceiver = (UniversalReceiver*) data;
	pUniversalReceiver->NoDataTimeout();
}

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
UniversalReceiver::UniversalReceiver(StringLogger *stringLogger,
									 IOFireWireLibNubRef nubInterface,
									 unsigned int cyclesPerSegment,
									 unsigned int numSegments,
									 unsigned int cycleBufferSize,
									 bool doIRMAllocations,
									 unsigned int irmAllocationPacketLen)
{
    nodeNubInterface = nubInterface;
	dclCommandPool = nil;
	remoteIsocPort = nil;
	localIsocPort = nil;
	isochChannel = nil;
	receiveChannel = 0;
	receiveSpeed = kFWSpeed100MBit;
	runLoopRef = nil;
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

	structuredDataPush = nil;
	maxNumStructuredDataStructsInCallback = 1;
	pCyclDataStruct = nil;
	
	messageProc = nil;
	noDataProc = nil;
	noDataTimeLimitInSeconds = 0.0;
	pReceiveBuffer = nil;
	transportState = kUniversalReceiverTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	cycleBufLen = cycleBufferSize;
	irmAllocationsPacketSize = irmAllocationPacketLen;
	
	doIRM = doIRMAllocations;
	receiveSegmentInfo = new UniversalReceiveSegment[isochSegments];

	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);

	// Initialize the no-data timer mutex
	pthread_mutex_init(&noDataTimerMutex,NULL);
	
#ifdef kAVS_Use_NuDCL_UniversalReceiver
	pSegUpdateBags = nil;
#else
	// Calculate the size of the DCL command pool needed
	dclCommandPoolSize = ((((isochCyclesPerSegment)*isochSegments)+(isochSegments*4)+(isochSegments*4)+16)*32);
#endif
	
	// Calculate the size of the VM buffer for the dcl receive packets
	dclVMBufferSize = (((isochSegments*isochCyclesPerSegment)+1)*cycleBufLen)+(isochSegments*4); // Allocate space for buffers and timestamps
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
UniversalReceiver::~UniversalReceiver()
{
	if (transportState != kUniversalReceiverTransportStopped)
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

#ifdef kAVS_Use_NuDCL_UniversalReceiver
	if (nuDCLPool)
		(*nuDCLPool)->Release(nuDCLPool);
	
	if (pSegUpdateBags)
	{
		// Release bags
		for (UInt32 seg=0;seg<isochSegments;seg++)
			CFRelease(pSegUpdateBags[seg]);
		
		// Release array
		delete [] pSegUpdateBags;
	}
#else
	if (dclCommandPool)
		(*dclCommandPool)->Release(dclCommandPool);

	// Free the update DCL list
	if (updateDCLList)
		free(updateDCLList);
#endif
	
	// Free the vm allocated DCL buffer
	if (pReceiveBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pReceiveBuffer,dclVMBufferSize);

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
IOReturn UniversalReceiver::setupIsocReceiver(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess ;
    UInt8 *pBuffer = nil;
	
#ifdef kAVS_Use_NuDCL_UniversalReceiver
	NuDCLReceivePacketRef thisDCL;
	IOVirtualRange range;
	UInt32 i;
#else
    DCLCommandStruct *pLastDCL = nil;
    DCLCommandPtr *startUpdateDCLList = nil;
	UInt32 updateListCnt;
	UInt32 curUpdateListIndex;
#endif	
	
	UInt32 cycle;
	UInt32 seg;
	UInt32 bufCnt = 0;
	IOFireWireLibNubRef newNubInterface;
	IOVirtualRange bufRange;

	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nUniversalReceiver Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nUniversalReceiver Error: Error duplicating device interface: 0x%08X\n\n",result);
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

#ifdef kAVS_Use_NuDCL_UniversalReceiver

	// Allocate an array of bags for end-of-segment dcl update lists
	pSegUpdateBags = new CFMutableSetRef[isochSegments];
	if (!pSegUpdateBags)
	{
		return kIOReturnNoMemory ;
	}
	else
		for (i=0;i<isochSegments;i++)
			pSegUpdateBags[i] = nil;
	
	// Use the nub interface to create a NuDCL command pool object
	nuDCLPool = (*nodeNubInterface)->CreateNuDCLPool(nodeNubInterface,
													 0,
													 CFUUIDGetUUIDBytes( kIOFireWireNuDCLPoolInterfaceID ));
	if (!nuDCLPool)
    {
		logger->log("\nMPEG2Transmitter Error: Error creating NuDCL command pool\n\n");
		return kIOReturnError ;
    }

#else
	
	// Use the nub interface to create a DCL command pool object
    dclCommandPool = (*nodeNubInterface)->CreateDCLCommandPool(
															   nodeNubInterface,
															   dclCommandPoolSize,
															   CFUUIDGetUUIDBytes( kIOFireWireDCLCommandPoolInterfaceID ));
	if (!dclCommandPool)
    {
		logger->log("\nUniversalReceiver Error: Error creating Receive DCL command pool: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

#endif	
	
	
	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 true,	// remote is talker
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nUniversalReceiver Error: Errpr creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this UniversalReceiver object in 
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
		logger->log("\nUniversalReceiver Error: Error allocating isoch receive buffers.\n\n");
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
	pOverrunReceiveBuffer = (UInt32*) &pBuffer[(isochSegments*isochCyclesPerSegment*cycleBufLen)];
	
	// Set the timestamp pointer
	pTimeStamps = (UInt32*) &pBuffer[(((isochSegments*isochCyclesPerSegment)+1)*cycleBufLen)];

#ifdef kAVS_Use_NuDCL_UniversalReceiver
	for (seg=0;seg<isochSegments;seg++)
	{
		pSegUpdateBags[seg] = CFSetCreateMutable(NULL, 0, NULL);
		
		for (cycle=0;cycle< isochCyclesPerSegment;cycle++)
		{
			range.address = (IOVirtualAddress) &pBuffer[bufCnt*cycleBufLen];
			range.length = (IOByteCount) cycleBufLen;
			
			// Allocate receive DCL
			thisDCL = (*nuDCLPool)->AllocateReceivePacket(nuDCLPool,
														  pSegUpdateBags[seg],
														  4,
														  1,
														  &range);
			if (!thisDCL)
			{
				logger->log("\nUniversalReceiver Error: Error allocating Receive Packet DCL\n\n");
				return kIOReturnNoMemory ;
			}
			
			// Flasgs
			(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
			
			// Refcon
			(*nuDCLPool)->SetDCLRefcon( thisDCL, this ) ;
			
			// Special functionality for the first dcl of the segment
			if (cycle == 0)
			{
				receiveSegmentInfo[seg].segmentStartDCL = thisDCL;
				
				// Timestamp
				(*nuDCLPool)->SetDCLTimeStampPtr(thisDCL, &pTimeStamps[seg]) ;
			}
			
			// Special functionality for the last dcl of the segment
			else if (cycle == (isochCyclesPerSegment-1))
			{
				receiveSegmentInfo[seg].segmentEndDCL = thisDCL;

				// Update
				(*nuDCLPool)->SetDCLUpdateList( thisDCL, pSegUpdateBags[seg] ) ;
				
				// Callback
				(*nuDCLPool)->SetDCLCallback( thisDCL, UniversalReceiveDCLCallback_Helper) ;
			}

			bufCnt++;
		}			
	}
	
	// Allocate Overrun DCL
	range.address = (IOVirtualAddress) pOverrunReceiveBuffer;
	range.length = (IOByteCount) cycleBufLen;
	thisDCL = (*nuDCLPool)->AllocateReceivePacket(nuDCLPool,
												  NULL,	// No update-bag needed for this DCL!
												  4,
												  1,
												  &range);
	if (!thisDCL)
	{
		logger->log("\nUniversalReceiver Error: Error allocating Overrun Receive Packet DCL\n\n");
		return kIOReturnNoMemory ;
	}
	else
	{
		overrunDCL = thisDCL;
		(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
		(*nuDCLPool)->SetDCLRefcon( thisDCL, this ) ;
		(*nuDCLPool)->SetDCLCallback( thisDCL, UniversalReceiveOverrunDCLCallback_Helper) ;
	}

#else	

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
			pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, &pBuffer[bufCnt*cycleBufLen],cycleBufLen);

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

		pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, UniversalReceiveDCLCallback_Helper, (DCLCallProcDataType) this);

		// Jumps to bogus address, for now. We'll fix the real jump location before we start receive
		pLastDCL = (*dclCommandPool)->AllocateJumpDCL(dclCommandPool, pLastDCL,(DCLLabelPtr) pLastDCL);
		receiveSegmentInfo[seg].pSegmentJumpDCL = (DCLJumpPtr) pLastDCL;
	}

	// Allocate Overrun label & callback DCL
	pLastDCL = (*dclCommandPool)->AllocateLabelDCL( dclCommandPool, pLastDCL ) ;
	pDCLOverrunLabel = (DCLLabelPtr) pLastDCL;
	pLastDCL = (*dclCommandPool)->AllocateReceivePacketStartDCL(dclCommandPool, pLastDCL, pOverrunReceiveBuffer,cycleBufLen);
	pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool, pLastDCL, UniversalReceiveOverrunDCLCallback_Helper, (DCLCallProcDataType) this);


	// Set the next pointer in the last DCL to nil
    pLastDCL->pNextDCLCommand = nil;
	
#endif
	
	// Using the nub interface to the local node, create
	// a local isoc port.
	localIsocPort = (*nodeNubInterface)->CreateLocalIsochPort(
															  nodeNubInterface,
															  false,	// local is listener
#ifdef kAVS_Use_NuDCL_UniversalReceiver
															  (*nuDCLPool)->GetProgram(nuDCLPool),
#else
															  pFirstDCL,
#endif															  
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
		logger->log("\nUniversalReceiver Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }

	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, UniversalReceiveFinalizeCallback_Helper) ;
	
	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														irmAllocationsPacketSize, // Here, we pass in the irm allocation packet size, not the receive buf packet size!
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nUniversalReceiver Error: Error creating isoch channel object: 0x%08X\n\n",result);
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
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,UniversalReceiveForceStopHandler_Helper);
	
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
UniversalReceiver::registerDataPushCallback(UniversalDataPushProc handler, void *pRefCon)
{
	packetPush = handler;
	pPacketPushRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerStructuredDataPushCallback
//////////////////////////////////////////////////////////////////////
IOReturn 
UniversalReceiver::registerStructuredDataPushCallback(StructuredUniversalDataPushProc handler, UInt32 maxCycleStructsPerCallback, void *pRefCon)
{
	if (pCyclDataStruct == nil)
	{
		// Need to allocate array of the UniversalReceiveCycleData structs, one per cyclePerSegment
		pCyclDataStruct = new UniversalReceiveCycleData[isochCyclesPerSegment];
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
UniversalReceiver::registerMessageCallback(UniversalReceiverMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerNoDataNotificationCallback
//////////////////////////////////////////////////////////////////////
IOReturn UniversalReceiver::registerNoDataNotificationCallback(UniversalNoDataProc handler, void *pRefCon, UInt32 noDataTimeInMSec, bool cipOnlyIsNoData)
{
	noDataProc = handler;
	pNoDataProcRefCon = pRefCon;
	noDataTimeLimitInSeconds = (noDataTimeInMSec / 1000.0);
	noDataCIPMode = cipOnlyIsNoData;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// fixupDCLJumpTargets
//////////////////////////////////////////////////////////////////////
void UniversalReceiver::fixupDCLJumpTargets(void)
{
	UInt32 i;

#ifdef kAVS_Use_NuDCL_UniversalReceiver
	IOReturn result;

	for (i=0;i<isochSegments;i++)
	{
		if (i != (isochSegments-1))
			(*nuDCLPool)->SetDCLBranch(receiveSegmentInfo[i].segmentEndDCL, receiveSegmentInfo[i+1].segmentStartDCL);
		else
			(*nuDCLPool)->SetDCLBranch(receiveSegmentInfo[i].segmentEndDCL, overrunDCL);
		
		// Send a notify about this dcl modification
		result = (*localIsocPort)->Notify( localIsocPort,
										   kFWNuDCLModifyJumpNotification,
										   (void**) &receiveSegmentInfo[i].segmentEndDCL,
										   1) ;
		if (result != kIOReturnSuccess)
		{
			logger->log("\nUniversalReceiver Error: NuDCL kFWNuDCLModifyJumpNotification Notify Error in fixupDCLJumpTargets: 0x%08X\n\n",result);
		}
	}
#else
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
#endif
	
	currentSegment = 0;
}

//////////////////////////////////////////////////////////////////////
// startReceive
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::startReceive(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already running
	if (transportState == kUniversalReceiverTransportRecording)
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
			transportState = kUniversalReceiverTransportRecording;

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
UniversalReceiver::stopReceive(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	// Make sure we are not already stopped
	if (transportState == kUniversalReceiverTransportStopped)
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
	transportState = kUniversalReceiverTransportStopped;
	
	// Unlock the transport control mutex
	pthread_mutex_unlock(&transportControlMutex);
	
	// Wait for the finalize callback to fire for this stream
	if (result == kIOReturnSuccess)
		while (finalizeCallbackCalled == false) usleep(1000);
	
	return result;
}

//////////////////////////////////////////////////
// UniversalReceiver::startNoDataTimer
//////////////////////////////////////////////////
void UniversalReceiver::startNoDataTimer( void )
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
// UniversalReceiver::stopNoDataTimer
//////////////////////////////////////////////////
void UniversalReceiver::stopNoDataTimer( void )
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
// UniversalReceiver::NoDataTimeout
//////////////////////////////////////////////////
void UniversalReceiver::NoDataTimeout(void)
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
UniversalReceiver::setReceiveIsochChannel(unsigned int chan)
{
	receiveChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setReceiveIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::setReceiveIsochSpeed(IOFWSpeed speed)
{
	receiveSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// UniversalReceiveDCLCallback
//////////////////////////////////////////////////////////////////////
void UniversalReceiver::UniversalReceiveDCLCallback(void)
{
	// Local Vars
    UInt32 segment;
	UInt32 startBuf;
	UInt32 i;
	UInt32 payloadLen;
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
	if (transportState == kUniversalReceiverTransportStopped)
		return;
	
	dclTimeStamp = pTimeStamps[currentSegment];
	
	segment = currentSegment;
	startBuf = segment*isochCyclesPerSegment;
	
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
		
	UInt32 *pCycleBuf = (UInt32*) (pReceiveBuffer + (startBuf*cycleBufLen));

	for (i=0;i<isochCyclesPerSegment;i++)
	{
		payloadLen = ((*pCycleBuf & 0xFFFF0000) >> 16); // Note that this quadlet is already in machine native byte order!
		
		UInt8* pPayload = (UInt8*)(&pCycleBuf[1]);

		// Check to see if we should reset the no-data timer during this callback
		if (payloadLen > ((noDataCIPMode == true) ? 8 : 0)) 
			segmentHasData = true;
		
		// Check to make sure the receive buffer was big enough for this packet. 
		// Only report it to the client once per segment.
		if (((payloadLen+4) > cycleBufLen) && (segmentBadPacketDetected == false))
		{
			// Post message if handler installed
			logger->log("\nUniversalReceiver Error: Received unsupported length isoch packet:%d\n\n",payloadLen);
			if (messageProc != nil)
				messageProc(kUniversalReceiverReceivedBadPacket,0x00000000,0x00000000,pMessageProcRefCon);
			
			segmentBadPacketDetected = true;
		}
		
		if (structuredDataPush != nil)
		{
			pCyclDataStruct[numCycleStructs].pRefCon = pPacketPushRefCon;
			pCyclDataStruct[numCycleStructs].payloadLength = payloadLen;
			pCyclDataStruct[numCycleStructs].pPayload = pPayload;
			pCyclDataStruct[numCycleStructs].isochHeader = pCycleBuf[0];
			pCyclDataStruct[numCycleStructs].fireWireTimeStamp = dclTimeStamp;
			pCyclDataStruct[numCycleStructs].nanoSecondsTimeStamp = dclTimeStampTimeInNanoSecondsU64;
			pCyclDataStruct[numCycleStructs].pExpansionData = nil;

			// Bump the numbeer of cycleStructs
			numCycleStructs++;
		}
		else if (packetPush != nil)
				packetPush(pPacketPushRefCon,payloadLen,pPayload,pCycleBuf[0],dclTimeStamp);
		
		dclTimeStamp =  AddFWCycleTimeToFWCycleTime( dclTimeStamp, 0x00001000 );
		
		// Bump the absolute-time for the next cycle
		// Only if the client has registered a structure-data callback function
		if (structuredDataPush != nil)
		{
			// Bump the dcl time stamp by 125uSec (or 125000 nSec)
			dclTimeStampTimeInNanoSecondsU64 += 125000LL;
		}
		
		pCycleBuf += (cycleBufLen/4);
	}

	// If we're using the structured-data callback method, and if we received any Universal
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

#ifdef kAVS_Use_NuDCL_UniversalReceiver
	IOReturn result;

	// Update jump targets

	(*nuDCLPool)->SetDCLBranch(receiveSegmentInfo[segment].segmentEndDCL, overrunDCL);
	result = (*localIsocPort)->Notify( localIsocPort,
									   kFWNuDCLModifyJumpNotification,
									   (void**) &receiveSegmentInfo[segment].segmentEndDCL,
									   1) ;
	if (result != kIOReturnSuccess)
		logger->log("\nUniversalReceiver Error: NuDCL kFWNuDCLModifyJumpNotification Notify Error in UniversalReceiveDCLCallback: 0x%08X\n\n",result);

	(*nuDCLPool)->SetDCLBranch(receiveSegmentInfo[(segment == 0) ? (isochSegments-1) : (segment-1) ].segmentEndDCL, receiveSegmentInfo[segment].segmentStartDCL);
	result = (*localIsocPort)->Notify( localIsocPort,
									   kFWNuDCLModifyJumpNotification,
									   (void**) &receiveSegmentInfo[(segment == 0) ? (isochSegments-1) : (segment-1) ].segmentEndDCL,
									   1) ;
	if (result != kIOReturnSuccess)
		logger->log("\nUniversalReceiver Error: NuDCL kFWNuDCLModifyJumpNotification Notify Error in UniversalReceiveDCLCallback: 0x%08X\n\n",result);
	
#else
	// Update jump targets

	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								 receiveSegmentInfo[segment].pSegmentJumpDCL,
								 pDCLOverrunLabel );

	(*localIsocPort)->ModifyJumpDCL(localIsocPort,
								 receiveSegmentInfo[(segment == 0) ? (isochSegments-1) : (segment-1) ].pSegmentJumpDCL,
								 receiveSegmentInfo[segment].pSegmentLabelDCL );
#endif
	
	// Increment current segment and handle wrap
	currentSegment += 1;
	if (currentSegment == isochSegments)
		currentSegment = 0;
	
	// If the client has registered a no-data notification, see if we should reset the timer
	if ((noDataProc != nil) && (segmentHasData == true))
			startNoDataTimer();
}

//////////////////////////////////////////////////////////////////////
// UniversalReceiveOverrunDCLCallback
//////////////////////////////////////////////////////////////////////
void UniversalReceiver::UniversalReceiveOverrunDCLCallback(void)
{
	IOReturn result = kIOReturnSuccess ;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	if (transportState == kUniversalReceiverTransportRecording)
	{
		// stop/delete the no-data timer if it exists
		stopNoDataTimer();

		// Stop Receiver
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kUniversalReceiverTransportStopped;
		
		// Post message if handler installed
		logger->log("\nUniversalReceiver Error: DCL Overrun!\n\n");
		if (messageProc != nil)
			messageProc(kUniversalReceiverDCLOverrun,0x00000000,0x00000000,pMessageProcRefCon);
		
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
				transportState = kUniversalReceiverTransportRecording;
				
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
// UniversalReceiveFinalizeCallback
//////////////////////////////////////////////////////////////////////
void UniversalReceiver::UniversalReceiveFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// UniversalReceiveForceStop
//////////////////////////////////////////////////////////////////////
void UniversalReceiver::UniversalReceiveForceStop(UInt32 stopCondition)
{
	UniversalReceiveOverrunDCLCallback();
}
#endif

//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::RemotePort_GetSupported(
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
UniversalReceiver::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kUniversalReceiverAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kUniversalReceiverReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
UniversalReceiver::RemotePort_Stop(
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

	// Get the pointer to the UniversalReceiver object from the refcon
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);

	// Call the UniversalReceiver's remote port allocate-port callback
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
	// Get the pointer to the UniversalReceiver object from the refcon
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);

	// Call the UniversalReceiver's remote port allocate-port callback
	return pReceiver->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalReceiver object from the refcon
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);

	// Call the UniversalReceiver's remote port release-port callback
	return pReceiver->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalReceiver object from the refcon
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);

	// Call the UniversalReceiver's remote port start callback
	return pReceiver->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the UniversalReceiver object from the refcon
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);

	// Call the UniversalReceiver's remote port stop callback
	return pReceiver->RemotePort_Stop(interface);
}

#ifdef kAVS_Use_NuDCL_UniversalReceiver
//////////////////////////////////////////////////////////////////////
// UniversalReceiveDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalReceiveDCLCallback_Helper( void* refcon, NuDCLRef dcl )
{
    UniversalReceiver *pReceiver = (UniversalReceiver*) refcon;
	pReceiver->UniversalReceiveDCLCallback();
	return;
}	

//////////////////////////////////////////////////////////////////////
// UniversalReceiveOverrunDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalReceiveOverrunDCLCallback_Helper( void* refcon, NuDCLRef dcl )
{
    UniversalReceiver *pReceiver = (UniversalReceiver*) refcon;
	pReceiver->UniversalReceiveOverrunDCLCallback();
	return;
}
#else
//////////////////////////////////////////////////////////////////////
// UniversalReceiveDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalReceiveDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the UniversalReceiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    UniversalReceiver *pReceiver = (UniversalReceiver*) pCallProc->procData;

	pReceiver->UniversalReceiveDCLCallback();
	return;
}

//////////////////////////////////////////////////////////////////////
// UniversalReceiveOverrunDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalReceiveOverrunDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the UniversalReceiver object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    UniversalReceiver *pReceiver = (UniversalReceiver*) pCallProc->procData;

	pReceiver->UniversalReceiveOverrunDCLCallback();
	return;
}
#endif	

//////////////////////////////////////////////////////////////////////
// UniversalReceiveFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn UniversalReceiveFinalizeCallback_Helper( void* refcon )
{
	UniversalReceiver *pUniversalReceiver = (UniversalReceiver*) refcon;
	pUniversalReceiver->UniversalReceiveFinalizeCallback();
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
// UniversalReceiveForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void UniversalReceiveForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	UniversalReceiver *pReceiver = (UniversalReceiver*) (*interface)->GetRefCon(interface);
	
	// Call the UniversalReceiver's Force Stop callback
	return pReceiver->UniversalReceiveForceStop(stopCondition);
}
#endif

} // namespace AVS
