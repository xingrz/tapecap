/*
	File:		MPEG2Transmitter.cpp

	Synopsis: This is the implementation file for the MPEG2Transmitter class. 
 
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

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter
static void MPEG2XmitDCLCallback_Helper( void* refcon, NuDCLRef dcl );
static void MPEG2XmitDCLOverrunCallback_Helper( void* refcon, NuDCLRef dcl );
#else
static void MPEG2XmitDCLCallback_Helper(DCLCommandPtr pDCLCommand);
static void MPEG2XmitDCLOverrunCallback_Helper(DCLCommandPtr pDCLCommand);
#endif

static IOReturn MPEG2XmitFinalizeCallback_Helper( void* refcon ) ;

#ifdef kAVS_Enable_ForceStop_Handler	
static void	MPEG2XmitForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition);
#endif

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter
// This is the maximum number of DCLs that can be passed in a Notif(...) call.
#define kMaxNuDCLsPerNotify 30
#endif

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
MPEG2Transmitter::MPEG2Transmitter(StringLogger *stringLogger,
								   IOFireWireLibNubRef nubInterface,
								   unsigned int cyclesPerSegment,
								   unsigned int numSegments,
								   bool doIRMAllocations,
								   unsigned int packetsPerCycle,
								   unsigned int tsPacketQueueSizeInPackets)
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
	xmitBufferSize = kMPEG2XmitCIPOnlySize + (kMPEG2SourcePacketSize*packetsPerCycle);
	playbackMode = kMpeg2TransmitterPlaybackModeForward;
	mpegDataRate = 10521944.0;	// An decent starting place.
	runLoopRef = nil;
	numTSPacketsInPacketQueue = tsPacketQueueSizeInPackets;
	
	timeStampProc = nil;
	pTimeStampProcRefCon = nil;
	
#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter
	pTSPacketBufArray = nil;
	pSegUpdateBags = nil;
	pProgramDCLs = nil;
	nuDCLPool = nil;
	encryptionProc = nil;
#else
	dclCommandPool = nil;
	pFirstCycleObject = nil;
	ppCallbackCycles = nil;
#endif
	
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

	psiTables = new PSITables(logger);
	transportState = kMpeg2TransmitterTransportStopped;
	isochCyclesPerSegment = cyclesPerSegment;
	isochSegments = numSegments;
	doIRM = doIRMAllocations;
	tsPacketsPerCycle = packetsPerCycle;

	// Initialize the transport control mutex
	pthread_mutex_init(&transportControlMutex,NULL);

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter
	// Make sure tsPacketsPerCycle is never above kMaxTSPacketsPerCycle
	if (packetsPerCycle > kMaxTSPacketsPerCycle)
		tsPacketsPerCycle = kMaxTSPacketsPerCycle;
	else
		tsPacketsPerCycle = packetsPerCycle;
#else
	// Calculate the size of the DCL command pool needed
	dclCommandPoolSize = ((((isochCyclesPerSegment*6)*isochSegments)+(isochSegments*6)+16)*32);
#endif
	
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
MPEG2Transmitter::~MPEG2Transmitter()
{

#ifndef kAVS_Use_NuDCL_Mpeg2Transmitter
	// Local Vars
	MPEG2XmitCycle *pXmitCycle;
	unsigned int i;
	TSPacket *pTSPacket;
#endif
	
	if (transportState != kMpeg2TransmitterTransportStopped)
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

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter
	if (nuDCLPool)
		(*nuDCLPool)->Release(nuDCLPool);
#else	
	if (dclCommandPool)
		(*dclCommandPool)->Release(dclCommandPool);
#endif
	
	// Free the psi table parser object
	delete psiTables;

	// If we created an internall logger, free it
	if (noLogger == true)
		delete logger;

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	// Free the vm allocated DCL buffer
	if (pTransmitBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pTransmitBuffer,transmitBufferSize);
#else	
	// Free the vm allocated DCL buffer
	if (pTransmitBuffer != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pTransmitBuffer,
				((isochCyclesPerSegment*isochSegments*xmitBufferSize) + (isochSegments*sizeof(UInt32))) );

	// Free the MPEG2XmitCycle objects
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
#endif
	
	if (nodeNubInterface != nil)
		(*nodeNubInterface)->Release(nodeNubInterface);

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	if (pProgramDCLs)
		delete [] pProgramDCLs;
	
	if (pTSPacketBufArray)
		delete [] pTSPacketBufArray;
	
	if (pSegUpdateBags)
	{
		// Release bags
		for (UInt32 seg=0;seg<isochSegments;seg++)
			CFRelease(pSegUpdateBags[seg]);
		
		// Release array
		delete [] pSegUpdateBags;
	}
#else
	// Delete transport stream  packet processing queue buffers
	if (pPacketQueueBuf != nil)
		vm_deallocate(mach_task_self(), (vm_address_t) pPacketQueueBuf,
				(numTSPacketsInPacketQueue*kMPEG2TSPacketSize));

	// Delete transport stream packet queue
	for (i=0;i<numTSPacketsInPacketQueue;i++)
	{
		pTSPacket = pPacketQueueHead;
		pPacketQueueHead = pPacketQueueHead->pNext;
		if (pTSPacket)
			delete pTSPacket;
	}
	
	// Free the list of end-of-segment MPEG2XmitCycle pointers
	if (ppCallbackCycles)
		delete [] ppCallbackCycles;
#endif
	
	// Release the transport control mutex
	pthread_mutex_destroy(&transportControlMutex);
}

//////////////////////////////////////////////////////
// setupIsocTransmitter
//////////////////////////////////////////////////////
IOReturn MPEG2Transmitter::setupIsocTransmitter(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess ;
	UInt32 totalObjects = isochCyclesPerSegment * isochSegments;
    UInt8 *pBuffer = nil;
    UInt32 i;
	IOVirtualRange bufRange;
	IOFireWireLibNubRef newNubInterface;
	
#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	UInt32 programDCLCount;
	NuDCLSendPacketRef thisDCL;
	UInt32 seg;
	UInt32 cycle;
#else
    DCLCommandStruct *pLastDCL = nil;
	MPEG2XmitCycle *pCycleObject = nil;
    MPEG2XmitCycle *pLastCycleObject = nil;
    DCLJumpPtr jumpDCL;
	TSPacket *pTSPacket = nil;
	TSPacket *pLastTSPacket = nil;
	unsigned char *pPacketQueueBufCharPtr;
	bool hasCallback;
	UInt32 segment = 0;
	UInt32 transmitBufferSize;
	UInt32 endOfCycleIndex = 0;
#endif

	// Either create a new local node device interface, or duplicate the passed-in device interface
	if (nodeNubInterface == nil)
	{
		result = GetFireWireLocalNodeInterface(&nodeNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nMPEG2Transmitter Error: Error creating local node interface: 0x%08X\n\n",result);
			return kIOReturnError ;
		}
	}
	else
	{
		result = GetFireWireDeviceInterfaceFromExistingInterface(nodeNubInterface,&newNubInterface);
		if (result != kIOReturnSuccess)
		{
			logger->log("\nMPEG2Transmitter Error: Error duplicating device interface: 0x%08X\n\n",result);
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

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// To calculate how much vm_allocated memory we need:
	//
	// Total cycles in DCL program = totalObjects = isochCyclesPerSegment * isochSegments
	//
	// We need vm memory for:
	//
	//	totalObjects * 16 for Isoch headers + masks
	//	totalObjects * 8 for CIP headers
	//  isochSegments * 4 for time-stamps
	//  source-packet buffers for each TSPacketBuf (192-byte buffers, that don't cross page boundaries)
	//
	//  Total TSPacketBuf objects needed is numTSPacketsInPacketQueue + (tsPacketsPerCycle*totalObjects)
	//
	//  A page is 4096 bytes, so we can fit 21 192-byte buffers in each page, with 64-bytes left over at the end
	//
	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	
	// Calculate the number of TSPacketBuf objects we need, and allocate an array of them
	numTSPacketBuf = numTSPacketsInPacketQueue + (tsPacketsPerCycle*totalObjects);
	pTSPacketBufArray = new TSPacketBuf[numTSPacketBuf];
	if (!pTSPacketBufArray)
	{
		return kIOReturnNoMemory ;
	}
	
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
	
	// Calculate how much VM memory we should allocate
	numVMPagesForSourcePackets = (numTSPacketBuf/21)+1;	// 21 per page
	numVMPagesForIsochHeaders = (totalObjects/256)+1;	// 256 per page (8-bytes of isoch header, 8-bytes of header mask)
	numVMPagesForCIPHeaders = (totalObjects/512)+1;		// 512 per page
	numVMPagesForTimeStamps = (totalObjects/1024)+1;	// 1024 per page
	transmitBufferSize = 	(numVMPagesForSourcePackets + numVMPagesForIsochHeaders + numVMPagesForCIPHeaders + numVMPagesForTimeStamps)*4096;	
	
	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer, transmitBufferSize , VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nMPEG2Transmitter Error: Error allocating isoch transmit buffers\n\n");
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
	pIsochHeaders = (UInt32*) &pTransmitBuffer[numVMPagesForSourcePackets*4096];
	pCIPHeaders = (UInt32*) &pTransmitBuffer[(numVMPagesForSourcePackets+numVMPagesForIsochHeaders)*4096];
	pTimeStamps = (UInt32*) &pTransmitBuffer[(numVMPagesForSourcePackets+numVMPagesForIsochHeaders+numVMPagesForCIPHeaders)*4096];
	
	// Assign a portion of the VM memory to each TSPacketBuf to hold a 192-byte source packet
	// and add the TSPacketBuf to the freeFifo
	UInt8 *pNextSourcePacketBuffer = pTransmitBuffer;
	for (i=0;i<numTSPacketBuf;i++)
	{
		// If needed, bump the pNextSourcePacketBuffer to avoid a page-crossing
		if ((i % 21 == 0) && (i != 0))
			pNextSourcePacketBuffer += 64;
		
		pTSPacketBufArray[i].pBuf = pNextSourcePacketBuffer;
		pNextSourcePacketBuffer += 192;
		
		freeFifo.push_back(&pTSPacketBufArray[i]);
	}
	
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
		logger->log("\nMPEG2Transmitter Error: Error creating DCL command pool: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
#endif
	
	// Create a remote isoc port
	remoteIsocPort = (*nodeNubInterface)->CreateRemoteIsochPort(
															 nodeNubInterface,
															 false,
															 CFUUIDGetUUIDBytes( kIOFireWireRemoteIsochPortInterfaceID ));
	if (!remoteIsocPort)
    {
		logger->log("\nMPEG2Transmitter Error: Error creating remote isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
	}

	// Save a pointer to this MPEG2Transmitter object in
	// the remote isoch port's refcon variable
	(*remoteIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) remoteIsocPort,this) ;

	// Use the remote port interface to install some callback handlers.
	(*remoteIsocPort)->SetGetSupportedHandler( remoteIsocPort, & RemotePort_GetSupported_Helper );
	(*remoteIsocPort)->SetAllocatePortHandler( remoteIsocPort, & RemotePort_AllocatePort_Helper );
	(*remoteIsocPort)->SetReleasePortHandler( remoteIsocPort, & RemotePort_ReleasePort_Helper );
	(*remoteIsocPort)->SetStartHandler( remoteIsocPort, & RemotePort_Start_Helper );
	(*remoteIsocPort)->SetStopHandler( remoteIsocPort, & RemotePort_Stop_Helper );

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
    // Create DCL Program
	(*nuDCLPool)->SetCurrentTagAndSync(nuDCLPool,1,0) ;
	programDCLCount = 0;
	for (seg=0;seg<isochSegments;seg++)
	{
		pSegUpdateBags[seg] = CFSetCreateMutable(NULL, 1, NULL);
		
		for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
		{
			range[0].address = (IOVirtualAddress) &pCIPHeaders[(seg*isochCyclesPerSegment*2)+(cycle*2)] ;
			range[0].length = (IOByteCount) 8 ;
			
			thisDCL = (*nuDCLPool)->AllocateSendPacket(nuDCLPool,
													   ((cycle == (isochCyclesPerSegment-1)) ? pSegUpdateBags[seg] : NULL),
													   1,
													   &range[0]);
			
			if (!thisDCL)
			{
				logger->log("\nMPEG2Transmitter Error: Error allocating Send Packet DCL\n\n");
				return kIOReturnNoMemory ;
			}
			
			// Flasgs
			(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
			
			// Refcon
			(*nuDCLPool)->SetDCLRefcon( thisDCL, this ) ;
			
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
				(*nuDCLPool)->SetDCLCallback( thisDCL, MPEG2XmitDCLCallback_Helper) ;
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
		logger->log("\nMPEG2Transmitter Error: Error allocating Send Packet DCL\n\n");
		return kIOReturnNoMemory ;
	}
	
	// Set this DCL as the overrun DCL
	overrunDCL = thisDCL;
	
	// Flasgs
	(*nuDCLPool)->SetDCLFlags(thisDCL,(kNuDCLDynamic | kNuDCLUpdateBeforeCallback));
	
	// Refcon
	(*nuDCLPool)->SetDCLRefcon( thisDCL, this ) ;
	
	// Callback
	(*nuDCLPool)->SetDCLCallback( thisDCL, MPEG2XmitDCLOverrunCallback_Helper) ;
	
	// Enable the following for debugging, to print out the DCL program!
	//(*nuDCLPool)->PrintProgram(nuDCLPool);
	
	// Using the nub interface to the local node, create
	// a local isoc port.
	localIsocPort = (*nodeNubInterface)->CreateLocalIsochPort(
															  nodeNubInterface,
															  true,
															  (*nuDCLPool)->GetProgram(nuDCLPool),
															  kFWDCLCycleEvent,
															  0,
															  0x01FFF000,
															  nil,
															  0,
															  &bufRange,
															  1,
															  CFUUIDGetUUIDBytes( kIOFireWireLocalIsochPortInterfaceID ));
#else
	transmitBufferSize = ((totalObjects*xmitBufferSize) + (isochSegments*sizeof(UInt32)));
	
	// Allocate an array of pointers to MPEG2XmitCycles to keep a list of end-of-segment MPEG2XmitCycle objects
	ppCallbackCycles = new MPEG2XmitCycle*[isochSegments];
	if (!ppCallbackCycles)
	{
		logger->log("\nMPEG2Transmitter Error: Error allocating end-of-segment MPEG2XmitCycle list\n\n");
		return kIOReturnError ;
	}
	
	// Allocate memory for the isoch transmit buffers
	vm_allocate(mach_task_self(), (vm_address_t *)&pBuffer, transmitBufferSize , VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nMPEG2Transmitter Error: Error allocating isoch transmit buffers: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	else
		bzero(pBuffer, transmitBufferSize);

	// Set the buffer range var
	bufRange.address = (IOVirtualAddress) pBuffer;
	bufRange.length = transmitBufferSize;
	
	// Set the timestamp pointer
	pTimeStamps = (UInt32*) &pBuffer[(totalObjects*xmitBufferSize)];
	
	// Save a pointer to the transmit buffer
	pTransmitBuffer = pBuffer;

    // Set the isoc tag field correctly for CIP style isoc.
    pLastDCL = (*dclCommandPool)->AllocateSetTagSyncBitsDCL( dclCommandPool, nil, 1, 0 ) ;

	// Set the first DCL pointer to the first DCL created.
	pFirstDCL = pLastDCL;

    // Loop to create objects
    for (i=0;i<totalObjects;i++)
    {
		hasCallback = (( (i % isochCyclesPerSegment) == (isochCyclesPerSegment - 1 ) ) ? true : false);

		pCycleObject = new MPEG2XmitCycle(xmitBufferSize,
									&pBuffer[i*xmitBufferSize],
									hasCallback,
									MPEG2XmitDCLCallback_Helper,
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
															 kMPEG2XmitCIPOnlySize );
    pLastDCL = (*dclCommandPool)->AllocateCallProcDCL(dclCommandPool,
													  pLastDCL,
													  MPEG2XmitDCLOverrunCallback_Helper,
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
#endif
	
	if (!localIsocPort)
    {
		logger->log("\nMPEG2Transmitter Error: Error creating local isoch port: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	
	// Install the finalize callback for the local isoch port
	(*localIsocPort)->SetRefCon((IOFireWireLibIsochPortRef) localIsocPort,this) ;
	(*localIsocPort)->SetFinalizeCallback( localIsocPort, MPEG2XmitFinalizeCallback_Helper) ;
	
	// Using the nub interface to the local node, create
	// a isoc channel.
	isochChannel = (*nodeNubInterface)->CreateIsochChannel(
														nodeNubInterface,
														doIRM,
														xmitBufferSize,
														kFWSpeedMaximum,
														CFUUIDGetUUIDBytes( kIOFireWireIsochChannelInterfaceID ));
	if (!isochChannel)
    {
		logger->log("\nMPEG2Transmitter Error: Error creating isoch channel object: 0x%08X\n\n",result);
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
	(*isochChannel)->SetChannelForceStopHandler(isochChannel,MPEG2XmitForceStopHandler_Helper);

	// Turn on notification
	(*isochChannel)->TurnOnNotification(isochChannel);
#endif
	
#ifndef kAVS_Use_NuDCL_Mpeg2Transmitter	
	// Allocate memory for packet processing queue buffer
	// TODO: The current implementation of this requires one
	// extra buffer copying step then it should. Once the
	// OS-X Isoch Services are updated to modify DCL buffer
	// pointers on the fly, the DCL's will transfer directly
	// out of this buffer. To prepare for this future modification,
	// this buffer is allocated with vm_allocate instead of malloc.
	vm_allocate(mach_task_self(), (vm_address_t *)&pPacketQueueBuf,
			 (numTSPacketsInPacketQueue*kMPEG2TSPacketSize), VM_FLAGS_ANYWHERE);
    if (!pBuffer)
    {
		logger->log("\nMPEG2Transmitter Error: Error allocating transport stream processing queue buffers: 0x%08X\n\n",result);
		return kIOReturnError ;
    }
	else
		bzero(pPacketQueueBuf, (numTSPacketsInPacketQueue*kMPEG2TSPacketSize));

	pPacketQueueBufCharPtr = (unsigned char*) pPacketQueueBuf;
	
	// Create Transport Stream Packet Processing Queue
	for (i=0;i<numTSPacketsInPacketQueue;i++)
	{
		pTSPacket = new TSPacket();
		if (!pTSPacket)
		{
			// TODO: Handle this out of memory error
			return kIOReturnError ;
		}
		else
		{
			// Allocate a fixed buffer to this TSPacket object
			pTSPacket->pPacket = &pPacketQueueBufCharPtr[i*kMPEG2TSPacketSize];
		}

		if (i==0)
			pPacketQueueHead = pTSPacket;
		else
			pLastTSPacket->pNext = pTSPacket;

		pLastTSPacket = pTSPacket;
	}
	pTSPacket->pNext = pPacketQueueHead;
#endif
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// startTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::startTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);

	// Make sure we are not already running
	if (transportState == kMpeg2TransmitterTransportPlaying)
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
			transportState = kMpeg2TransmitterTransportPlaying;
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
MPEG2Transmitter::stopTransmit(void)
{
	IOReturn result = kIOReturnSuccess ;

	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	// Make sure we are not already stopped
	if (transportState == kMpeg2TransmitterTransportStopped)
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
	transportState = kMpeg2TransmitterTransportStopped;
	
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
MPEG2Transmitter::registerDataPullCallback(DataPullProc handler, void *pRefCon)
{
	packetFetch = handler;
	pPacketFetchRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerMessageCallback
//////////////////////////////////////////////////////////////////////
IOReturn MPEG2Transmitter::registerMessageCallback(MPEG2TransmitterMessageProc handler, void *pRefCon)
{
	messageProc = handler;
	pMessageProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// registerTimeStampCallback
//////////////////////////////////////////////////////////////////////
IOReturn MPEG2Transmitter::registerTimeStampCallback(MPEG2TransmitterTimeStampProc handler, void *pRefCon)
{
	timeStampProc = handler;
	pTimeStampProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// prepareForTransmit
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::prepareForTransmit(void)
{
	unsigned int i;
	UInt16 nodeID;
	UInt32 generation;

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	IOReturn result;
	TSPacketBuf *pTSPacketBuf;
	UInt32 segment, cycle;
	UInt32 numDCLsNotified,totalDCLsToNotify,numDCLsForThisNotify;
#else
	MPEG2XmitCycle *pFirstCycle;
#endif
	
	// Get the local node ID
	do (*nodeNubInterface)->GetBusGeneration(nodeNubInterface, &generation);
	while  ((*nodeNubInterface)->GetLocalNodeIDWithGeneration(nodeNubInterface,generation,&nodeID) != kIOReturnSuccess);

	// Start with a nominal bit rate of 1 packet per cycle
	mpegDataRate = kMaxDataRate_OneTSPacketPerCycle;
	isochCycleClocksPerTSPacket = 24576000.0/(mpegDataRate/1504);
	
    // Note that currentIsochTime must not be initialized to a value
	// less that zero, because the first transmit packet generated
	// will be a CIP only! A value of 0 or greater for currentIsochTime
	// will prevent the flow-control logic from adding source packets
	// into the first isoch cycle.
	currentIsochTime = 0.0;
	
    currentMPEGTime = (kMPEGSourcePacketCycleCountStartValue*3072);
    dbcCount = 0;
	lastPCR = 0.0;
	packetsBetweenPCR = 0;
	firstPCRFound = false;

	currentSegment = 0;
	expectedTimeStampCycle = isochCyclesPerSegment - 1;

	// Clear the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = false;

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	pLastPCRPacketBuf = nil;
	lastSy = 0;
#else
	// Reinitialize transport stream packet processing queue
	pPacketQueueIn = pPacketQueueHead;
	pPacketQueueOut = pPacketQueueHead;
	pLastPCRPacket = nil;
#endif
	
	// Reinitialize psiTable stuff
	psiTables->ResetPSITables();
	programIndex = 1;
	psiTables->selectProgram(programIndex);
		
	// Prepare the packet fetcher
	if (messageProc != nil)
			messageProc(kMpeg2TransmitterPreparePacketFetcher,0x00000000,0x00000000,pMessageProcRefCon);

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	// Setup the end-of-segment jump targets.
	for (segment=0;segment<isochSegments;segment++)
		(*nuDCLPool)->SetDCLBranch(pProgramDCLs[((segment+1)*isochCyclesPerSegment)-1], (segment == (isochSegments-1)) ? overrunDCL: pProgramDCLs[(segment+1)*isochCyclesPerSegment]);
	
	// Make sure all TSPacketBuf objects are on the freeFifo, and that the other Fifos are empty.
	// TODO: For the DCL overrun case, we shouldn't have to throw away everything on the analysisFifo,
	// only what's on the xmitFifo. For now we clear both!
	while (!xmitFifo.empty())
	{
		pTSPacketBuf = xmitFifo.front();
		xmitFifo.pop_front();
		freeFifo.push_back(pTSPacketBuf);
	}
	while (!analysisFifo.empty())
	{
		pTSPacketBuf = analysisFifo.front();
		analysisFifo.pop_front();
		freeFifo.push_back(pTSPacketBuf);
	}
	
	// AY_DEBUG: Sanity check to ensure all TSPacketBuf objects are on the freeFifo now
	if (freeFifo.size() != numTSPacketBuf)
		logger->log("\nMPEG2Transmitter Error: Incorrect number of TSPacketBuf objects on freeFiFo. Expected: %d, Actual: %d\n\n",numTSPacketBuf,freeFifo.size());
	
	// Fill the analysis fifo with packets
	for (i=0;i<numTSPacketsInPacketQueue;i++)
		AddPacketToTSPacketQueue();
	
	// Initialize all the cycles in the program with data!
	for (segment=0;segment<isochSegments;segment++)
		for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
			FillCycleBuffer(pProgramDCLs[(segment*isochCyclesPerSegment)+cycle],nodeID,segment,cycle);
	
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
			logger->log("\nMPEG2Transmitter Error: NuDCL kFWNuDCLModifyNotification Error in prepareForTransmit: 0x%08X\n\n",result);
		
		// Bump
		numDCLsNotified += numDCLsForThisNotify;
	}
#else
	pNextUpdateCycle = pFirstCycleObject;

    // Fix-up mpeg cycle jump targets
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
	
	// Preload the transport stream processing queue
	for (i=0;i<numTSPacketsInPacketQueue;i++)
		AddPacketToTSPacketQueue();
	
	// Initialize all the cycle objects with data
    pFirstCycle = pNextUpdateCycle;
    do
    {
		// Set the doUpdateJumpTarget to true for all but the first call to FillCycleBuffer
		// to prevent overwriting the branch we just set to the overrun handler in the pLastCallbackCycle. 
		FillCycleBuffer(pNextUpdateCycle,nodeID, (pNextUpdateCycle == pFirstCycleObject) ? false : true);
		pNextUpdateCycle = pNextUpdateCycle->pNext;
    }while (pNextUpdateCycle != pFirstCycle);
#endif

	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochChannel
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::setTransmitIsochChannel(unsigned int chan)
{
	xmitChannel = chan;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// setTransmitIsochSpeed
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::setTransmitIsochSpeed(IOFWSpeed speed)
{
	xmitSpeed = speed;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// AddPacketToTSPacketQueue
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::AddPacketToTSPacketQueue(void)
{
	IOReturn result;
	bool discontinuityFlag;
	double nextDataRate;
	UInt32 *pNextPacketBuf;
	unsigned int i;

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	UInt32 *pWordBuf;
	TSPacketBuf *pTSPacketBuf;
#else
	UInt32 *pTSPacketBuf;
	TSPacket *pTSPacket = pPacketQueueIn;
#endif
	
	// Get the packet
	discontinuityFlag = false;	// Just in case the packet fetcher code forgets to set this!
	
	if (packetFetch != nil)
		result = packetFetch(&pNextPacketBuf,&discontinuityFlag,pPacketFetchRefCon);
	else
		result = -1;

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
	// AY_DEBUG: Sanity check to make sure we have successfully got a TSPacketBuf off the free fifo
	if (freeFifo.empty())
	{
		logger->log("\nMPEG2Transmitter Error: Unable to get a TSPacketBuf off the freeFifo in AddPacketToTSPacketQueue\n\n");
		return;
	}
	
	// Get a TSPacketBuf off the free queue
	pTSPacketBuf = freeFifo.front();
	freeFifo.pop_front();
	
	// Handle the case where we were unable to get a TS packet
	if (result != kIOReturnSuccess)
	{
		// Packet Fetch Error.
		// Will result in a CIP only isoch cycle
		// to be inserted in the stream at this point
		pTSPacketBuf->packetInfo.hasPacketFetchError = true;
	}
	else
	{
		// We successfully pulled a packet from the application
		pTSPacketBuf->packetInfo.hasPacketFetchError = false;
		
		// Copy packet data into TSPacketBuf buffer
		pWordBuf = (UInt32*) pTSPacketBuf->pBuf;
		for (i=0;i<kMPEG2TSPacketSizeInWords;i++)
			pWordBuf[i+1] = pNextPacketBuf[i]; // Note: We skip over the 4-byte SPH in the pTSPacketBuf's buffer
		
		// Handle discontinunity flag here!
		// Causes the need for one more PCR before next dataRate change
		if (discontinuityFlag == true)
			pLastPCRPacketBuf = nil;
		
		// Verify TS Packet Header as a sanity check
		// Don't transmit a corrupt packet. 
		if (pTSPacketBuf->pBuf[4] != 0x47)
		{
			logger->log("MPEG2Transmitter Error: Invalid TS Packet Header!\n");
			pTSPacketBuf->packetInfo.hasPacketFetchError = true;
		}
		else
		{
			packetsBetweenPCR++;
			
			// Process packet for time synch extraction
			pTSPacketBuf->packetInfo.update(&pTSPacketBuf->pBuf[4]);
			
			// PSI Table extraction code
			if ((pTSPacketBuf->packetInfo.pid == 0) || (pTSPacketBuf->packetInfo.pid == psiTables->primaryProgramPmtPid))
				psiTables->extractTableDataFromPacket(&pTSPacketBuf->packetInfo);
			
			if ((pTSPacketBuf->packetInfo.hasPCR) && (pTSPacketBuf->packetInfo.pid == psiTables->pcrPID))
			{
				if (pLastPCRPacketBuf != nil)
				{
					// Calculate the next data rate based on the two PCR values
					nextDataRate = ((packetsBetweenPCR*1504) / (pTSPacketBuf->packetInfo.pcrTime - pLastPCRPacketBuf->packetInfo.pcrTime));
					
					// See if the new data rate seems realistic.
					// Prevent erronous data rate calculations for messed up streams
					if 	((nextDataRate > kMPEG2TransmitterLowBitRateThreshold) && (nextDataRate < kMaxDataRate_FiveTSPacketsPerCycle))
					{
						pLastPCRPacketBuf->packetInfo.dataRate = nextDataRate;
						pLastPCRPacketBuf->packetInfo.hasDataRateChange = true;
					}
					else
					{
						// Ignore out-of-range bit-rate calculations! 
						// Keep using the previous bit-rate for now!
					}
				}
				pLastPCRPacketBuf = pTSPacketBuf;
				packetsBetweenPCR = 0;
			}
			
			// If it's been too long since we've seen a PCR, we probably have stale PSI tables
			if (packetsBetweenPCR > kMPEG2TransmitterMaxPacketsBetweenPCRs)
			{
				logger->log("MPEG2Transmitter: Timeout waiting for next PCR. Searching for new PSI!\n");

				psiTables->ResetPSITables();
				programIndex += 1;
				if (programIndex > kMaxAutoPSIDetectProgramIndex)
					programIndex = 1; // Note that the PSITables program index is one based!
				psiTables->selectProgram(programIndex);

				packetsBetweenPCR = 0;
				pLastPCRPacketBuf = nil;
			}
		}
	}
	
	// Add this TSPacketBuf to the Analysis Fifo
	analysisFifo.push_back(pTSPacketBuf);
#else	
	// Handle the case where we were unable to get a TS packet
	if (result != kIOReturnSuccess)
	{
		// Packet Fetch Error.
		// Will result in a CIP only isoch cycle
		// to be inserted in the stream at this point
		pTSPacket->hasPacketFetchError = true;
	}
	else
	{
		// We successfully pulled a packet from the application
		pTSPacket->hasPacketFetchError = false;

		// Copy packet data into ts processing queue object buf
		pTSPacketBuf = (UInt32*) pTSPacket->pPacket;
		for (i=0;i<kMPEG2TSPacketSizeInWords;i++)
			pTSPacketBuf[i] = pNextPacketBuf[i];

		// Handle discontinunity flag here!
		// Causes the need for one more PCR before next dataRate change
		if (discontinuityFlag == true)
			pLastPCRPacket = nil;

		// Verify TS Packet Header as a sanity check
		// Don't transmit a 
		if (pTSPacket->pPacket[0] != 0x47)
		{
			logger->log("MPEG2Transmitter Error: Invalid TS Packet Header!\n");
			pTSPacket->hasPacketFetchError = true;
		}
		else
		{
			packetsBetweenPCR++;

			// Process packet for time synch extraction
			pTSPacket->update(pTSPacket->pPacket);

			// PSI Table extraction code
			if ((pTSPacket->pid == 0) || (pTSPacket->pid == psiTables->primaryProgramPmtPid))
				psiTables->extractTableDataFromPacket(pTSPacket);

			if ((pTSPacket->hasPCR) && (pTSPacket->pid == psiTables->pcrPID))
			{
				if (pLastPCRPacket != nil)
				{
					// Calculate the next data rate based on the two PCR values
					nextDataRate = ((packetsBetweenPCR*1504) / (pTSPacket->pcrTime - pLastPCRPacket->pcrTime));

					// See if the new data rate seems realistic.
					// Prevent erronous data rate calculations for messed up streams
					if 	((nextDataRate > kMPEG2TransmitterLowBitRateThreshold) && (nextDataRate < kMaxDataRate_FiveTSPacketsPerCycle))
					{
						pLastPCRPacket->dataRate = nextDataRate;
						pLastPCRPacket->hasDataRateChange = true;
					}
					else
					{
						// Ignore out-of-range bit-rate calculations! 
						// Keep using the previous bit-rate for now!
					}
				}
				pLastPCRPacket = pTSPacket;
				packetsBetweenPCR = 0;
			}
			
			// If it's been too long since we've seen a PCR, we probably have stale PSI tables
			if (packetsBetweenPCR > kMPEG2TransmitterMaxPacketsBetweenPCRs)
			{
				logger->log("MPEG2Transmitter: Timeout waiting for next PCR. Searching for new PSI!\n");

				psiTables->ResetPSITables();
				programIndex += 1;
				if (programIndex > kMaxAutoPSIDetectProgramIndex)
					programIndex = 1; // Note that the PSITables program index is one based!
				psiTables->selectProgram(programIndex);
				
				packetsBetweenPCR = 0;
				pLastPCRPacket = nil;
			}
		}
	}
	
	// Bump Queue In
	pPacketQueueIn = pPacketQueueIn->pNext;	
#endif
	
	return;
}

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	

//////////////////////////////////////////////////////////////////////
// registerDataEncryptionCallback
//////////////////////////////////////////////////////////////////////
IOReturn MPEG2Transmitter::registerDataEncryptionCallback(MPEG2TransmitterEncryptionProc handler, void *pRefCon)
{
	encryptionProc = handler;
	pEncryptionProcRefCon = pRefCon;
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// GetNextTSPacketQueuePacket
//////////////////////////////////////////////////////////////////////
TSPacketBuf *MPEG2Transmitter::GetNextTSPacketQueuePacket(void)
{
	// AY_DEBUG: Sanity check to make sure we have successfully got a TSPacketBuf off the free fifo
	if (analysisFifo.empty())
	{
		logger->log("\nMPEG2Transmitter Error: Unable to get a TSPacketBuf off the analysisFifo in GetNextTSPacketQueuePacket\n\n");
		return nil;
	}
	
	TSPacketBuf *pTSPacketBuf = analysisFifo.front();
	analysisFifo.pop_front();
	
	// Code to detect if the LastPCR packet is removed
	// from the queue. Need to nil it! Hopefully, the
	// queue is big enough for this never to happen
	if (pTSPacketBuf == pLastPCRPacketBuf)
		pLastPCRPacketBuf = nil;
	
	// Return a pointer to the current packet
	return pTSPacketBuf;
}

//////////////////////////////////////////////////////////////////////
// FillCycleBuffer
//////////////////////////////////////////////////////////////////////
void
MPEG2Transmitter::FillCycleBuffer(NuDCLSendPacketRef dcl, UInt16 nodeID, UInt32 segment, UInt32 cycle)
{
	double savedMPEGTime = currentMPEGTime;
	double savedIsochTime = currentIsochTime;

	UInt32 *pCIPHeader = &pCIPHeaders[(segment*isochCyclesPerSegment*2)+(cycle*2)];
	UInt32 *pIsochHeaderAndMask = &pIsochHeaders[(segment*isochCyclesPerSegment*4)+(cycle*4)];
	
	UInt32 numRanges = 0;
	UInt32 shiftedNodeID = ((nodeID & 0x3F) << 24);
	TSPacketBuf *pTSPacketBuf;
	UInt32 *pWordBuf;
	bool hadFetchErrorPacket = false;
	UInt32 *pPacketWordBuffers[kMaxTSPacketsPerCycle];
	UInt32 i;
	UInt32 sph;
	UInt32 sphInClocks;
	UInt32 currentCycleTimeInClocks;
	int prepareTimeStampDeltaInClocks;
	
	// The first range is for the CIP header
	range[numRanges].address = (IOVirtualAddress) pCIPHeader ;
	range[numRanges].length = (IOByteCount) 8 ;
	numRanges += 1;
	
	// See if we are in pause state
	if (playbackMode == kMpeg2TransmitterPlaybackModePause)
	{
		// send CIP only isoch
        pCIPHeader[0] = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
        pCIPHeader[1] = EndianU32_NtoB(0xA0000000);
		
		// Bump currentMPEGTime to the next cycle
		currentMPEGTime += 3072.0;
		if (currentMPEGTime >= 24576000.0)
			currentMPEGTime -= 24576000.0;
	}
	else
	{
		while ((currentIsochTime < 0.0) && ((numRanges-1) <= tsPacketsPerCycle)) 
		{
			// Get the next packet from the analysis fifo
			pTSPacketBuf = GetNextTSPacketQueuePacket();
			
			// Mark the segment number for this TSPacketBuf
			pTSPacketBuf->xmitSegmentNumber = segment;
			
			// Push it onto the xmit fifo
			xmitFifo.push_back(pTSPacketBuf);
			
			if (pTSPacketBuf->packetInfo.hasPacketFetchError == true)
			{
				// If this TSPacketBuf has a fetch error,
				// we need to fetch another packet (to replace the one
				// we just consumed), and then break out of the while loop!
				hadFetchErrorPacket = true;
				AddPacketToTSPacketQueue();
				break;
			}
			else
			{
				pWordBuf = (UInt32*) pTSPacketBuf->pBuf;
				pWordBuf[0] = EndianU32_NtoB(sourcePacketHeader());
				
				// See if this packet includes a dataRate adjustment
				if (pTSPacketBuf->packetInfo.hasDataRateChange == true)
				{
					mpegDataRate = pTSPacketBuf->packetInfo.dataRate;
					isochCycleClocksPerTSPacket = 24576000.0/(mpegDataRate/1504);
				}
				
				// Add a range to this dcl
				range[numRanges].address = (IOVirtualAddress) pTSPacketBuf->pBuf ;
				range[numRanges].length = (IOByteCount) 192 ;
				numRanges += 1;
				
				// If we are reporting time-stamps, and this packet has a PCR,
				// and we've had at least one DCL callback, notify the client.
				if ((timeStampProc != nil) && (pTSPacketBuf->packetInfo.hasPCR) && (firstDCLCallbackOccurred) && (pTSPacketBuf->packetInfo.pid == psiTables->pcrPID)) 
				{
					sph = sourcePacketHeader();  
					sphInClocks = (((sph & 0x01FFF000) >> 12)*3072)+(sph & 0x00000FFF);
					currentCycleTimeInClocks = (((currentFireWireCycleTime & 0x01FFF000) >> 12)*3072)+(currentFireWireCycleTime & 0x00000FFF);
					prepareTimeStampDeltaInClocks = sphInClocks - currentCycleTimeInClocks;
					if (prepareTimeStampDeltaInClocks < 0)
						prepareTimeStampDeltaInClocks += 24576000;
					
					timeStampProc(pTSPacketBuf->packetInfo.pcr,currentUpTimeInNanoSecondsU64 + (prepareTimeStampDeltaInClocks*40.690104167),pTimeStampProcRefCon);
				}
				
				// Bump currentMPEGTime
				currentMPEGTime += isochCycleClocksPerTSPacket;
				if (currentMPEGTime >= 24576000.0)
					currentMPEGTime -= 24576000.0;
				
				// Bump currentIsochTime
				currentIsochTime += isochCycleClocksPerTSPacket;
			}
			
			// Fetch another packet for the analysis fifo from the client 
			AddPacketToTSPacketQueue();
		}
		
		if (hadFetchErrorPacket == true)
		{
			// currentMPEGTime should bump by exactly one cylce from where we started in 
			// the call to this function! 
			currentMPEGTime = savedMPEGTime;
			currentMPEGTime += 3072.0;
			if (currentMPEGTime >= 24576000.0)
				currentMPEGTime -= 24576000.0;

			// currentIsochTime should be reset to exactly where we started in 
			// the call to this function! 
			currentIsochTime = savedIsochTime;
		}
		else
		{
			// Adjust currentIsoch time for next time. 
			currentIsochTime -= 3072.0;
		}
		
		// set CIP
        pCIPHeader[0] = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
        pCIPHeader[1] = EndianU32_NtoB(0xA0000000);
		
		// Bump dbc for next cycle!
		// Note: here we can calculate the number of ts packets in this cycle
		// as numRanges-1. That will have to change if we add range coalesing.
		dbcCount += ((numRanges-1)*kMPEG2DataBlocksPerPacket);
		dbcCount &= 0x000000FF;
	}
	
	// Program new ranges into this dcl
	(*nuDCLPool)->SetDCLRanges(dcl,numRanges,range);
	
	// If an encrpyion proc has been installed, call it now for the packets in this cycle
	if (encryptionProc)
	{
		// The number of ts packets in this cycle is numRanges-1.
		// The pointers to the source packetes ar in the ranges range[1]...range[numRanges-1]
		// Pass the client pointers to the ts packets, not the source-packets (i.e. strip-off the SPH)
		
		for (i=0;i<numRanges-1;i++)
		{
			pPacketWordBuffers[i] = (UInt32*) range[i+1].address;
			pPacketWordBuffers[i] += 1;	// Skip over the SPH!
		}
		
		encryptionProc(numRanges-1, pPacketWordBuffers, &lastSy, pEncryptionProcRefCon);
	}
	
	// Setup isoch header values.
	// Note: We only take control of the SY field. Leave the rest to the DCL engine to control!
	pIsochHeaderAndMask[0] = (lastSy & 0xF);
	pIsochHeaderAndMask[2] = 0x0000000F; // User-control of just the SY field
	pIsochHeaderAndMask[3] = 0x00000000; // All bits in this header quad are auto-set!
	
	// Note: the DCL Modification notification will happen outside of this function!
	
	return;
}

#else

//////////////////////////////////////////////////////////////////////
// GetNextTSPacketQueuePacket
//////////////////////////////////////////////////////////////////////
TSPacket *MPEG2Transmitter::GetNextTSPacketQueuePacket(void)
{
	TSPacket *pTSPacket = pPacketQueueOut;

	// Move the queue out point
	pPacketQueueOut = pPacketQueueOut->pNext;

	// Code to detect if the LastPCR packet is removed
	// from the queue. Need to nil it! Hopefully, the
	// queue is big enough for this never to happen
	if (pTSPacket == pLastPCRPacket)
		pLastPCRPacket = nil;
	
	// Return a pointer to the current packet
	return pTSPacket;
}

//////////////////////////////////////////////////////////////////////
// FillCycleBuffer
//////////////////////////////////////////////////////////////////////
void
MPEG2Transmitter::FillCycleBuffer(MPEG2XmitCycle *pCycle, UInt16 nodeID, bool doUpdateJumpTarget)
{
    UInt32 i;
    UInt32 *pDestBuf = (UInt32*) pCycle->pBuf;
    UInt32 pktNum;
	double savedIsochTime = currentIsochTime;
    double savedMPEGTime = currentMPEGTime;
	TSPacket *pTSPacket;
	UInt32 *pTSPacketBuf;
	UInt32 shiftedNodeID = ((nodeID & 0x3F) << 24);
	UInt32 sph;
	UInt32 sphInClocks;
	UInt32 currentCycleTimeInClocks;
	int prepareTimeStampDeltaInClocks;
	
	// First, see if we are in pause state
	if (playbackMode == kMpeg2TransmitterPlaybackModePause)
	{
		// send CIP only isoch
        *pDestBuf++ = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
        *pDestBuf++ = EndianU32_NtoB(0xA0000000);

		// Set the mode for this cycle object
		pCycle->CycleMode = CycleModeCIPOnly;

		// Deal with previous cycle objects jump target
		(pCycle->pPrev)->UpdateJumpTarget(pCycle->CycleMode, localIsocPort);

		// Bump currentMPEGTime to the next cycle
		currentMPEGTime += 3072.0;
		if (currentMPEGTime >= 24576000.0)
			currentMPEGTime -= 24576000.0;

		return;
	}

	// See if it's time to send some MPEG
    if (currentIsochTime < 0.0)
    {
        // TODO: Until we get a change to the
        // DCL languate to allow us to
        // change packet size on the fly,
        // we always send a fixed number of TS packets here.
        // Later: support arbitrary num packets
        // based on current MPEG bitrate

        // Create CIP header for this cycle
        *pDestBuf++ = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
        *pDestBuf++ = EndianU32_NtoB(0xA0000000);

        // Insert two source packets
        for(pktNum=0;pktNum<tsPacketsPerCycle;pktNum++)
        {
            // Create the source packet header for the first packet
            *pDestBuf++ = EndianU32_NtoB(sourcePacketHeader());

			// Get the next packet from the processing queue
			pTSPacket = GetNextTSPacketQueuePacket();

			// Handle a packet fetch error here!
			// TODO: Handle case where we've alread put
			// at least one packet into this cycle.
			// Instead of CIP only, the cycle should
			// be padded out with null-pid packet(s)
			// so the real packets in this cycle
			// aren't lost.
			if (pTSPacket->hasPacketFetchError == true)
			{
				pDestBuf = (UInt32*) pCycle->pBuf;

				// send CIP only isoch
				*pDestBuf++ = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
				*pDestBuf++ = EndianU32_NtoB(0xA0000000);

				// Set the mode for this cycle object
				pCycle->CycleMode = CycleModeCIPOnly;

				// Deal with previous cycle objects jump target
				(pCycle->pPrev)->UpdateJumpTarget(pCycle->CycleMode, localIsocPort);

				// Restore the current MPEG and isoch time
				currentMPEGTime = savedMPEGTime;
				currentIsochTime = savedIsochTime;

				// Bump currentMPEGTime to the next cycle
				currentMPEGTime += 3072.0;
				if (currentMPEGTime >= 24576000.0)
					currentMPEGTime -= 24576000.0;

				// Fetch a new packet from the user and add it to the queue.
				// Even though we didn't get a packet from the user
				// for this cycle we still consumed a queue entry that
				// now needs to be refilled.
				AddPacketToTSPacketQueue();
				
				// We're done with this cycle, so return here!
				return;
			}
			
			// See if this packet includes a dataRate adjustment
			if (pTSPacket->hasDataRateChange == true)
			{
				mpegDataRate = pTSPacket->dataRate;
				isochCycleClocksPerTSPacket = 24576000.0/(mpegDataRate/1504);
			}

            // Copy packet data into DCL transmit buffer
			pTSPacketBuf = (UInt32*) pTSPacket->pPacket;
			for (i=0;i<kMPEG2TSPacketSizeInWords;i++)
				*pDestBuf++ = pTSPacketBuf[i];

			// If we are reporting time-stamps, and this packet has a PCR,
			// and we've had at least one DCL callback, notify the client.
			if ((timeStampProc != nil) && (pTSPacket->hasPCR) && (firstDCLCallbackOccurred) && (pTSPacket->pid == psiTables->pcrPID)) 
			{
				sph = sourcePacketHeader();  
				sphInClocks = (((sph & 0x01FFF000) >> 12)*3072)+(sph & 0x00000FFF);
				currentCycleTimeInClocks = (((currentFireWireCycleTime & 0x01FFF000) >> 12)*3072)+(currentFireWireCycleTime & 0x00000FFF);
				prepareTimeStampDeltaInClocks = sphInClocks - currentCycleTimeInClocks;
				if (prepareTimeStampDeltaInClocks < 0)
					prepareTimeStampDeltaInClocks += 24576000;
				
				timeStampProc(pTSPacket->pcr,currentUpTimeInNanoSecondsU64 + (prepareTimeStampDeltaInClocks*40.690104167),pTimeStampProcRefCon);
			}
			
			// Fetch a new packet from the user and add it to the queue
			AddPacketToTSPacketQueue();

			// Bump currentMPEGTime
            currentMPEGTime += isochCycleClocksPerTSPacket;
            if (currentMPEGTime >= 24576000.0)
                currentMPEGTime -= 24576000.0;

            // Bump currentIsochTime
            currentIsochTime += isochCycleClocksPerTSPacket;
        }

		// Set the mode for this cycle object
		pCycle->CycleMode = CycleModeFull;

        dbcCount += (kMPEG2DataBlocksPerPacket*tsPacketsPerCycle);
        dbcCount &= 0x000000FF;
    }

    else
    {
        // Not time to send any MPEG,
        // so send CIP only isoch
        *pDestBuf++ = EndianU32_NtoB((0x0006C400 | dbcCount | shiftedNodeID));
        *pDestBuf++ = EndianU32_NtoB(0xA0000000);

		// Set the mode for this cycle object
		pCycle->CycleMode = CycleModeCIPOnly;
    }

    // Adjust currentIsoch time for next time
    currentIsochTime -= 3072.0;

    // Deal with previous cycle objects jump target
	if (doUpdateJumpTarget == true)
		(pCycle->pPrev)->UpdateJumpTarget(pCycle->CycleMode, localIsocPort);

    return;
}
#endif

//////////////////////////////////////////////////////////////////////
// sourcePacketHeader
//////////////////////////////////////////////////////////////////////
unsigned int
MPEG2Transmitter::sourcePacketHeader(void)
{
	UInt32	cycle_count;
	UInt32	cycle_offset;
	UInt32  cycle_time;

	cycle_count = ((UInt32)currentMPEGTime/3072);
	cycle_offset = ((UInt32)currentMPEGTime - (cycle_count*3072));

	cycle_count = (cycle_count%8000);
	cycle_offset = (cycle_offset%3072);

	cycle_time = ((cycle_count<<12) | cycle_offset);

	return cycle_time;
}


//////////////////////////////////////////////////////////////////////
// RemotePort_GetSupported
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::RemotePort_GetSupported(
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
MPEG2Transmitter::RemotePort_AllocatePort(
										  IOFireWireLibIsochPortRef interface,
										  IOFWSpeed maxSpeed,
										  UInt32 channel)
{
	if (messageProc != nil)
		messageProc(kMpeg2TransmitterAllocateIsochPort,maxSpeed,channel,pMessageProcRefCon);
	
	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::RemotePort_ReleasePort(
										 IOFireWireLibIsochPortRef interface)
{
	if (messageProc != nil)
		messageProc(kMpeg2TransmitterReleaseIsochPort,0x00000000,0x00000000,pMessageProcRefCon);

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::RemotePort_Start(
								   IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to start listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop
//////////////////////////////////////////////////////////////////////
IOReturn
MPEG2Transmitter::RemotePort_Stop(
								  IOFireWireLibIsochPortRef interface)
{
	// Talk to remote device and tell it to stop listening.

	return kIOReturnSuccess ;
}

//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLOverrunCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::MPEG2XmitDCLOverrunCallback(void)
{
	IOReturn result = kIOReturnSuccess ;
	
	// Lock the transport control mutex
	pthread_mutex_lock(&transportControlMutex);
	
	if (transportState == kMpeg2TransmitterTransportPlaying)
	{
		logger->log("\nMPEG2Transmitter Error: DCL Overrun!\n\n");
		
		// Restart transmitter
		(*isochChannel)->Stop( isochChannel ) ;
		(*isochChannel)->ReleaseChannel( isochChannel ) ;
		transportState = kMpeg2TransmitterTransportStopped;
		
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
				transportState = kMpeg2TransmitterTransportPlaying;
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

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::MPEG2XmitDCLCallback(void)
{
	UInt16 nodeID;
	UInt32 generation;
	UInt32 actualTimeStampCycle;
	int lostCycles;
	NuDCLRef pLastSegEndDCL;
	IOReturn result;
	UInt32 cycle;
	TSPacketBuf *pTSPacketBuf;
	UInt32 numDCLsNotified,totalDCLsToNotify,numDCLsForThisNotify;
	
	UInt32 outBusTime;
	AbsoluteTime currentUpTime;
	Nanoseconds currentUpTimeInNanoSeconds;
	
	// Special debugging check to see if we received an out-of-order callback.
	// Note: the out-of-order delivery of callbacks shouldn't cause any problems,
	// but it's interesting to know if it can actually happen.
	//if (pCallBackCycle != ppCallbackCycles[currentSegment])
	//	logger->log("MPEG2Transmitter Info: Got out-of-order callback.\n");
	
	// See if this callback happened after we stopped
	if (transportState == kMpeg2TransmitterTransportStopped)
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
		// to happen every 7 out of 8 times we start the MPEG2Transmitter,
		// due to the fact that we don't know what the actual seconds field is going 
		// to be here unitl we've processed our first DCL callback.
		// TODO: In the future we could querry the current cycle clock, and make a good 
		// guess to what it will be when we first start the DCL program.
		if (((lostCycles % 8000) == 0) && (firstDCLCallbackOccurred == false))
		{
			//logger->log("MPEG2Transmitter timestamp Seconds-Field adjust, old: %u  new:%u\n",(unsigned int) expectedTimeStampCycle,(unsigned int) actualTimeStampCycle);
			
			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
			
			// Using the actual time stamp captured, calculate the new current MPEG Time
			// for the start of the segment we are about to process
			actualTimeStampCycle =
				actualTimeStampCycle +
				1 +
				((isochSegments-1)*isochCyclesPerSegment) +
				kMPEGSourcePacketCycleCountStartValue;
			
			actualTimeStampCycle %= 64000;  // modulo by 8 Seconds worth of cycles
			
			// Compensate for difference by modifying currentMPEGTime
			currentMPEGTime = actualTimeStampCycle*3072; 
		}
		else
		{
			logger->log("MPEG2Transmitter timestamp adjust, old: %u  new:%u\n",
						(unsigned int) expectedTimeStampCycle,
						(unsigned int) actualTimeStampCycle);
			
			// Notify client of timestamp adjust
			if (messageProc != nil)
				messageProc(kMpeg2TransmitterTimeStampAdjust,
							(unsigned int) expectedTimeStampCycle,
							(unsigned int) actualTimeStampCycle
							,pMessageProcRefCon);
			
			// Assume that the reason we've got a time-code discrepancy
			// here is that we lost one or more cycles during this segment.
			// If the number of cycles we lost is within a reasonable recovery
			// threshold, instead of adjusting our source-packet-header times for subsequent frames,
			// we instead try to recover the lost time by discarding one or
			// more future CIP-only packets. If the discrepancy is greater than
			// that threshold, we have no choice but to just alter our future
			// source-packet-header times to be correct.
			
			// Adjust expected to match new compensated cycle value
			expectedTimeStampCycle = actualTimeStampCycle;
			
			// If the number of lost cycles is within our threshold,
			// recover the lost time. Otherwise, just bump the currentMPEGTime
			// which will affect future source-packet-header times.
			if (lostCycles <= kMPEG2TransmitterLostCycleRecoveryThreshold)
			{
				logger->log("MPEG2Transmitter timestamp adjust, using lost-cycle recovery\n");
				
				// By reducing currentIsochTime, we will
				// transmit DIF data in places where there would
				// have been CIP-only packets
				currentIsochTime -= (3072.000 * lostCycles);
			}
			else
			{
				logger->log("MPEG2Transmitter timestamp adjust, using SPH adjust\n");
				
				// Using the actual time stamp captured, calculate the new current MPEG Time
				// for the start of the segment we are about to process
				actualTimeStampCycle =
					actualTimeStampCycle +
					1 +
					((isochSegments-1)*isochCyclesPerSegment) +
					kMPEGSourcePacketCycleCountStartValue;
				
				actualTimeStampCycle %= 64000;  // modulo by 8 Seconds worth of cycles
				
				// Compensate for difference by modifying currentMPEGTime
				currentMPEGTime = actualTimeStampCycle*3072; 
			}
		}
	}
	
	if (timeStampProc != nil)
	{
		// Get cycle timer and up-time. Do this in quick succession to best
		// establish the relationship between the two.
		(*nodeNubInterface)->GetBusCycleTime( nodeNubInterface, &outBusTime, &currentFireWireCycleTime);
		currentUpTime = UpTime();
		currentUpTimeInNanoSeconds = AbsoluteToNanoseconds(currentUpTime);
		currentUpTimeInNanoSecondsU64 = ((UInt64) currentUpTimeInNanoSeconds.hi << 32) | currentUpTimeInNanoSeconds.lo;
	}
	
	// Set the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = true;
	
	// Bump expected time stamp cycle value
	expectedTimeStampCycle += isochCyclesPerSegment;
	expectedTimeStampCycle %= 64000; // modulo by 8 Seconds worth of cycles
	
	// Move TSPacketBuf objects previously commited to this segment from the xmitFifo to the freeFifo
	for(;;)
	{
		if (xmitFifo.empty())
			break;
		pTSPacketBuf = xmitFifo.front();
		if (pTSPacketBuf->xmitSegmentNumber != currentSegment)
			break;
		
		xmitFifo.pop_front();
		freeFifo.push_back(pTSPacketBuf);
	}
	
	// Fill this segments buffers
	for (cycle=0;cycle<isochCyclesPerSegment;cycle++)
		FillCycleBuffer(pProgramDCLs[(currentSegment*isochCyclesPerSegment)+cycle],nodeID,currentSegment,cycle);
	
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
			logger->log("\nMPEG2Transmitter Error: NuDCL kFWNuDCLModifyNotification Error in MPEG2XmitDCLCallback: 0x%08X\n\n",result);
		
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
		logger->log("\nMPEG2Transmitter Error: NuDCL kFWNuDCLModifyJumpNotification Notify Error in MPEG2XmitDCLCallback: 0x%08X\n\n",result);
	}
	
	// Bump Current Segment
	if (currentSegment != (isochSegments-1))
		currentSegment += 1;
	else
		currentSegment = 0;
	
	return;
}

#else

//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::MPEG2XmitDCLCallback(MPEG2XmitCycle *pCallBackCycle)
{
	UInt16 nodeID;
	UInt32 generation;
	UInt32 actualTimeStampCycle;
	MPEG2XmitCycle *pThisSegmentsFirstCycle; 
	
	UInt32 outBusTime;
	AbsoluteTime currentUpTime;
	Nanoseconds currentUpTimeInNanoSeconds;

	// Special debugging check to see if we received an out-of-order callback.
	// Note: the out-of-order delivery of callbacks shouldn't cause any problems,
	// but it's interesting to know if it can actually happen.
	//if (pCallBackCycle != ppCallbackCycles[currentSegment])
	//	logger->log("MPEG2Transmitter Info: Got out-of-order callback.\n");
	
	// See if this callback happened after we stopped
	if (transportState == kMpeg2TransmitterTransportStopped)
		return;
	
	// Get the local node ID
	do (*nodeNubInterface)->GetBusGeneration(nodeNubInterface, &generation);
	while  ((*nodeNubInterface)->GetLocalNodeIDWithGeneration(nodeNubInterface,generation,&nodeID) != kIOReturnSuccess);

	actualTimeStampCycle = ((pTimeStamps[currentSegment] & 0x01FFF000) >> 12);
	if (actualTimeStampCycle != expectedTimeStampCycle)
	{
		logger->log("MPEG2Transmitter timestamp adjust, old: %u  new:%u\n",
		 (unsigned int) expectedTimeStampCycle,
		 (unsigned int) actualTimeStampCycle);

		// Notify client of timestamp adjust
		if (messageProc != nil)
			messageProc(kMpeg2TransmitterTimeStampAdjust,
						(unsigned int) expectedTimeStampCycle,
						(unsigned int) actualTimeStampCycle
						,pMessageProcRefCon);
		
		// Adjust expected to match new compensated cycle value
		expectedTimeStampCycle = actualTimeStampCycle;
		
		// Using the actual time stamp captured, calculate the new currentMPEGTime
		// for the start of the segment we are about to process
		actualTimeStampCycle =
			actualTimeStampCycle +
			1 +
			((isochSegments-1)*isochCyclesPerSegment) +
			kMPEGSourcePacketCycleCountStartValue;

		actualTimeStampCycle %= 8000;
		
		// Compensate for difference by modifying currentMPEGTime
		currentMPEGTime = actualTimeStampCycle*3072; 
	}
	
	// Bump expected time stamp cycle value
	expectedTimeStampCycle += isochCyclesPerSegment;
	expectedTimeStampCycle %= 8000;

	if (timeStampProc != nil)
	{
		// Get cycle timer and up-time. Do this in quick succession to best
		// establish the relationship between the two.
		(*nodeNubInterface)->GetBusCycleTime( nodeNubInterface, &outBusTime, &currentFireWireCycleTime);
		currentUpTime = UpTime();
		currentUpTimeInNanoSeconds = AbsoluteToNanoseconds(currentUpTime);
		currentUpTimeInNanoSecondsU64 = ((UInt64) currentUpTimeInNanoSeconds.hi << 32) | currentUpTimeInNanoSeconds.lo;
	}
	
	// Set the flag that tells us if we've handled at least one DCLCallback.
	firstDCLCallbackOccurred = true;
	
	// Fill this segments buffers
	pThisSegmentsFirstCycle = pNextUpdateCycle;
    while (pNextUpdateCycle != ppCallbackCycles[currentSegment]->pNext)
    {
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

	// Bump Current Segment
	if (currentSegment != (isochSegments-1))
		currentSegment += 1;
	else
		currentSegment = 0;
	
	return;
}
#endif

//////////////////////////////////////////////////////////////////////
// MPEG2XmitFinalizeCallback
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::MPEG2XmitFinalizeCallback(void)
{
	finalizeCallbackCalled = true;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// MPEG2XmitForceStop
//////////////////////////////////////////////////////////////////////
void MPEG2Transmitter::MPEG2XmitForceStop(UInt32 stopCondition)
{
	MPEG2XmitDCLOverrunCallback();
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

	// Get the pointer to the MPEG2Transmitter object from the refcon
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Transmitter's remote port allocate-port callback
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
	// Get the pointer to the MPEG2Transmitter object from the refcon
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Transmitter's remote port allocate-port callback
	return pTransmitter->RemotePort_AllocatePort(interface,maxSpeed,channel);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_ReleasePort_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_ReleasePort_Helper(
							  IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Transmitter object from the refcon
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Transmitter's remote port release-port callback
	return pTransmitter->RemotePort_ReleasePort(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Start_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Start_Helper(
						IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Transmitter object from the refcon
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Transmitter's remote port start callback
	return pTransmitter->RemotePort_Start(interface);
}

//////////////////////////////////////////////////////////////////////
// RemotePort_Stop_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn
RemotePort_Stop_Helper(
					   IOFireWireLibIsochPortRef interface)
{
	// Get the pointer to the MPEG2Transmitter object from the refcon
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);

	// Call the MPEG2Transmitter's remote port stop callback
	return pTransmitter->RemotePort_Stop(interface);
}

#ifdef kAVS_Use_NuDCL_Mpeg2Transmitter	
//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2XmitDCLCallback_Helper( void* refcon, NuDCLRef dcl )
{
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) refcon;
	pTransmitter->MPEG2XmitDCLCallback();
	
	return;
}

//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLOverrunCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2XmitDCLOverrunCallback_Helper( void* refcon, NuDCLRef dcl )
{
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) refcon;
	pTransmitter->MPEG2XmitDCLOverrunCallback();
	
	return;
}

#else

//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2XmitDCLCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the xmit cycle object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    MPEG2XmitCycle *pCallBackCycle = (MPEG2XmitCycle*) pCallProc->procData;

	pCallBackCycle->pTransmitter->MPEG2XmitDCLCallback(pCallBackCycle);
	return;
}

//////////////////////////////////////////////////////////////////////
// MPEG2XmitDCLOverrunCallback_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2XmitDCLOverrunCallback_Helper(DCLCommandPtr pDCLCommand)
{
	// Get the pointer to the MPEG2Transmitter object from the proc data
	DCLCallProcStruct *pCallProc =  (DCLCallProcStruct*) pDCLCommand;
    MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) pCallProc->procData;

	pTransmitter->MPEG2XmitDCLOverrunCallback();
	return;
}
#endif

//////////////////////////////////////////////////////////////////////
// MPEG2XmitFinalizeCallback_Helper
//////////////////////////////////////////////////////////////////////
static IOReturn MPEG2XmitFinalizeCallback_Helper( void* refcon )
{
	MPEG2Transmitter *pMPEG2Transmitter = (MPEG2Transmitter*) refcon;
	pMPEG2Transmitter->MPEG2XmitFinalizeCallback();
	return kIOReturnSuccess;
}

#ifdef kAVS_Enable_ForceStop_Handler	
//////////////////////////////////////////////////////////////////////
// MPEG2XmitForceStopHandler_Helper
//////////////////////////////////////////////////////////////////////
static void MPEG2XmitForceStopHandler_Helper( IOFireWireLibIsochChannelRef interface, UInt32  stopCondition)
{
	MPEG2Transmitter *pTransmitter = (MPEG2Transmitter*) (*interface)->GetRefCon(interface);
	
	// Call the MPEG2Transmitter's Force Stop callback
	return pTransmitter->MPEG2XmitForceStop(stopCondition);
}
#endif

} // namespace AVS
