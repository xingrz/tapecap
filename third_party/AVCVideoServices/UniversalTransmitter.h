/*
	File:		UniversalTransmitter.h

    Synopsis: This is the header file for the UniversalTransmitter class. 

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

#ifndef __AVCVIDEOSERVICES_UNIVERSALTRANSMITTER__
#define __AVCVIDEOSERVICES_UNIVERSALTRANSMITTER__

namespace AVS
{

// enum for this module's messages
enum
{
	kUniversalTransmitterPreparePacketFetcher = kFWAVCStreamPreparePacketFetcher,
	kUniversalTransmitterAllocateIsochPort = kFWAVCStreamAllocateIsochPort,
	kUniversalTransmitterReleaseIsochPort = kFWAVCStreamReleaseIsochPort,
	kUniversalTransmitterDCLOverrun = kFWAVCStreamDCLOverrun,	/* Not currently used! */
	kUniversalTransmitterTimeStampAdjust = kFWAVCStreamTimeStampAdjust,
	kUniversalTransmitterBadBufferRange = kFWAVCStreamBadBufferRange,
};

// enum for transport states
enum
{
	kUniversalTransmitterTransportStopped,
	kUniversalTransmitterTransportPlaying
};

enum
{
	// Default values for transmit segment size, number of segments
	// These can be overriden in the transmitter class constructor
	kUniversalTransmitterCyclesPerSegment = kFWAVCCyclesPerUniversalTransmitSegment,
	kUniversalTransmitterNumSegments = kFWAVCNumUniversalTransmitSegments,
	kUniversalTransmitterClientTransmitBufferSize = kFWAVCUniversalTransmitterClientBufferSize,
	kUniversalTransmitterIrmAllocationMaxPacketSize = kFWAVCUniversalTransmitterIrmAllocationMaxPacketSize,
	kUniversalTransmitterNumStartupCycleMatchBits = kFWAUniversalTransmitterNumStartupCycleMatchBits
};

// The structure for the cycle object
struct UniversalTransmitterCycleInfo
{
	UInt32 index;

	UInt32 numRanges;
	IOVirtualRange ranges[5]; // Client can attach up to 5 ranges
	
	UInt8 sy;	// only low 4 bits valid!
	UInt8 tag;	// only low 2 bits valid!
	
	UInt16 nodeID;
	UInt32 expectedTransmitCycleTime;
	
	UInt64 transmitTimeInNanoSeconds;

	// If dclProgramRunning is true, then the 3-bits of "seconds field" in the 
	// expectedTransmitCycleTime are valid (bits 27:25). Otherwise, if
	// dclProgramRunning is false, the 3-bits of "seconds field" are not valid.
	// Note: This is needed because we cannot guarrantee an accurate value of the "seconds field"
	// in the frameTransmitStartCycleTime time-stamp until the DCL program is running ,and
	// we get our first DCL callback, because the DCL engine starts on any 1-second boundary.
	// Also, transmitTimeInNanoSeconds is only valid when dclProgramRunning is true.
	bool dclProgramRunning;
};

// Function prototype for data pull callback.
typedef IOReturn (*UniversalTransmitterDataPullProc) (UniversalTransmitterCycleInfo *pCycleInfo, class UniversalTransmitter *pTransmitter, void *pRefCon);

// Function prototype for message callback.
typedef void (*UniversalTransmitterMessageProc) (UInt32 msg, UInt32 param1, UInt32 param2, void *pRefCon);

//
// The UniversalTransmitter Class Declaration
//
class UniversalTransmitter
{
public:

	// Constructor
    UniversalTransmitter(StringLogger *stringLogger = nil,
						 IOFireWireLibNubRef nubInterface = nil,
						 unsigned int cyclesPerSegment = kUniversalTransmitterCyclesPerSegment,
						 unsigned int numSegments = kUniversalTransmitterNumSegments,
						 unsigned int clientTransmitBufferSize = kUniversalTransmitterClientTransmitBufferSize,
						 bool doIRMAllocations = false,
						 unsigned int irmAllocationMaxPacketSize = kUniversalTransmitterIrmAllocationMaxPacketSize,
						 unsigned int numStartupCycleMatchBits = kUniversalTransmitterNumStartupCycleMatchBits);

    // Destructor
    ~UniversalTransmitter();

    // Function to setup all the isoc stuff
    IOReturn setupIsocTransmitter(void);

	// Set the isoch transmit channel
	IOReturn setTransmitIsochChannel(unsigned int chan);

	// Set the isoch transmit speed
	IOReturn setTransmitIsochSpeed(IOFWSpeed speed);

	// Start/Stop routines
	IOReturn startTransmit(void);
	IOReturn stopTransmit(void);

	// Function to install a handler for pulling data
	IOReturn registerDataPullCallback(UniversalTransmitterDataPullProc handler, void *pRefCon);

	// Function to install a handler for receiving messages
	IOReturn registerMessageCallback(UniversalTransmitterMessageProc handler, void *pRefCon);
	
	UInt8* getClientBufferPointer(void);
	UInt32 getClientBufferLen(void);
	
	UniversalTransmitterCycleInfo* getUniversalTransmitterCycleInfoArray(void);
	UInt32 getUniversalTransmitterCycleInfoArrayCount(void);
	
	// Publically visible vars
	unsigned int transportState;

	// A reference to the current run loop for isoch callbacks
	CFRunLoopRef runLoopRef;

private:

	// Function to pre-initialize all the isoch xmit buffers
	IOReturn prepareForTransmit(void);

	// Registered Handler functions
	UniversalTransmitterDataPullProc packetFetch;
	void *pPacketFetchRefCon;
	UniversalTransmitterMessageProc messageProc;
	void *pMessageProcRefCon;
	
	// Interface pointers
	IOFireWireLibNubRef nodeNubInterface;
	IOFireWireLibRemoteIsochPortRef remoteIsocPort ;
	IOFireWireLibLocalIsochPortRef localIsocPort;
	IOFireWireLibIsochChannelRef isochChannel;
	IOFireWireLibNuDCLPoolRef nuDCLPool;
	
	void FillCycleBuffer(NuDCLSendPacketRef dcl, UInt16 nodeID, UInt32 segment, UInt32 cycle);

	UInt32 transmitBufferSize;
	UInt32 clientBufferSize;
	UInt32 irmAllocationPacketSize;
	
	UInt8 *pTransmitBuffer;	// Points to the entire vm_allocated buffer
	UInt32 *pTimeStamps;	// Points to the portion of the vm_allocated buffer allocated to time-stamps 
	UInt32 *pIsochHeaders;	// Points to the portion of the vm_allocated buffer allocated to isoch-headers
	UInt8 *pClientBuffer;	// Points to the portion of the vm_allocated buffer allocated to the client
	UInt8 *pClientBufferUpperLimit;
	
	NuDCLSendPacketRef overrunDCL;
	CFMutableSetRef *pSegUpdateBags;
	IOVirtualRange range[7];
	NuDCLSendPacketRef *pProgramDCLs;
	UniversalTransmitterCycleInfo *pCycleInfo; 
	
    // Other vars
	unsigned int xmitChannel;
	IOFWSpeed xmitSpeed;
	unsigned int isochCyclesPerSegment;
	unsigned int isochSegments;
	bool firstDCLCallbackOccurred;
	unsigned int startupCycleMatchMask;
	
	bool doIRM;
	bool noLogger;
	UInt32 currentSegment;
	UInt32 expectedTimeStampCycle;
	volatile bool finalizeCallbackCalled;
	pthread_mutex_t transportControlMutex;
	UInt32 programIndex;

	UInt32 currentFireWireCycleTime;
	UInt64 currentUpTimeInNanoSecondsU64;
	
	StringLogger *logger;
	
public:
	// Callbacks for remote isoch port object
	IOReturn RemotePort_GetSupported(
								  IOFireWireLibIsochPortRef interface,
								  IOFWSpeed *outMaxSpeed,
								  UInt64 *outChanSupported);
	IOReturn RemotePort_AllocatePort(
								  IOFireWireLibIsochPortRef interface,
								  IOFWSpeed maxSpeed,
								  UInt32 channel);
	IOReturn RemotePort_ReleasePort(
								 IOFireWireLibIsochPortRef interface);
	IOReturn RemotePort_Start(
						   IOFireWireLibIsochPortRef interface);
	IOReturn RemotePort_Stop(
						  IOFireWireLibIsochPortRef interface);

	// Callbacks for DCL
	void UniversalTransmitterDCLCallback(void);
	
	void UniversalTransmitterDCLOverrunCallback();
	void UniversalTransmitterFinalizeCallback(void);

#ifdef kAVS_Enable_ForceStop_Handler	
	// Force Stop Callback
	void UniversalTransmitterForceStop(UInt32 stopCondition);
#endif
	
};

} // namespace AVS

#endif // __AVCVIDEOSERVICES_UNIVERSALTRANSMITTER__
