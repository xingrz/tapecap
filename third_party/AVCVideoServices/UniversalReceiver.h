/*
	File:		UniversalReceiver.h

    Synopsis: This is the header file for the UniversalReceiver class. 
 
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

#ifndef __AVCVIDEOSERVICES_UNIVERSALRECEIVER__
#define __AVCVIDEOSERVICES_UNIVERSALRECEIVER__

namespace AVS
{

// enum for this module's messages
enum
{
	kUniversalReceiverReceivedBadPacket = kFWAVCStreamReceivedBadPacket,
	kUniversalReceiverDCLOverrun = kFWAVCStreamDCLOverrun,
	kUniversalReceiverAllocateIsochPort = kFWAVCStreamAllocateIsochPort,
	kUniversalReceiverReleaseIsochPort = kFWAVCStreamReleaseIsochPort
};

// enum for transport states
enum
{
	kUniversalReceiverTransportStopped,
	kUniversalReceiverTransportRecording
};

// Default values for receive segment size and number of segments.
// These can be overriden in the receiver class constructor
enum
{
	kNumUniversalReceiveSegments	= kFWAVCNumUniversalReceiveSegments,
	kCyclesPerUniversalReceiveSegment = kFWAVCCyclesPerUniversalReceiveSegment,
	kUniversalReceiverDefaultBufferSize  = kFWAVCUniversalReceiverDefaultBufferSize
};

// Structure containing vars for each receive segment
#ifdef kAVS_Use_NuDCL_UniversalReceiver
struct UniversalReceiveSegment
{
	NuDCLReceivePacketRef segmentStartDCL;
	NuDCLReceivePacketRef segmentEndDCL;
};
#else
struct UniversalReceiveSegment
{
	DCLLabelPtr pSegmentLabelDCL;
	DCLJumpPtr pSegmentJumpDCL;
};
#endif

typedef IOReturn (*UniversalDataPushProc) (void *pRefCon,
										   UInt32 payloadLength,
										   UInt8 *pPayload, 
										   UInt32 isochHeader,
										   UInt32 fireWireTimeStamp);

// Struct to hold all info regarding a single cycle's received Universal data. 
// Only used by the "structured-data" callback method.
typedef FWAVCUniversalReceiveCycleData UniversalReceiveCycleData;

// Function prototype for alternate "structured" data push callback.
typedef IOReturn (*StructuredUniversalDataPushProc) (UInt32 CycleDataCount, UniversalReceiveCycleData *pCycleData, void *pRefCon);

///////////////////////////////////////////////////////////////////////////////////////

// Function prototype for message callback.
typedef void (*UniversalReceiverMessageProc) (UInt32 msg, UInt32 param1, UInt32 param2, void *pRefCon);

// Function prototype for "no-data" callback.
typedef IOReturn (*UniversalNoDataProc) (void *pRefCon);

//
// The Universal Receiver Class Declaration
//
class UniversalReceiver
{

public:
	// Constructor
    UniversalReceiver(StringLogger *stringLogger = nil,
					  IOFireWireLibNubRef nubInterface = nil,
					  unsigned int cyclesPerSegment = kCyclesPerUniversalReceiveSegment,
					  unsigned int numSegments = kNumUniversalReceiveSegments,
					  unsigned int cycleBufferSize = kUniversalReceiverDefaultBufferSize,
					  bool doIRMAllocations = false,
					  unsigned int irmAllocationPacketLen = kUniversalReceiverDefaultBufferSize);

    // Destructor
    ~UniversalReceiver();

    // Function to setup all the isoc stuff
    IOReturn setupIsocReceiver(void);

	// Set the isoch receive channel
	IOReturn setReceiveIsochChannel(unsigned int chan);

	// Set the isoch receive speed -
	// Note: Only important to set if doIRMAllocations is enabled in class constructor
	IOReturn setReceiveIsochSpeed(IOFWSpeed speed);

	// Start/Stop routines
	IOReturn startReceive(void);
	IOReturn stopReceive(void);

	// Function to install a handler for receiving data packets
	IOReturn registerDataPushCallback(UniversalDataPushProc handler, void *pRefCon);

	// Function to install a handler for receiving data packets using the alternate "structured-data" callback prototype
	// Note: If a non-nil value is set for this structured-data callback, the standard callback and/or the extended
	// callback will not be called.
	IOReturn registerStructuredDataPushCallback(StructuredUniversalDataPushProc handler, UInt32 maxCycleStructsPerCallback, void *pRefCon);
	
	// Function to install a handler for receiving messages
	IOReturn registerMessageCallback(UniversalReceiverMessageProc handler, void *pRefCon);

	// Function to install a handler for "no-data" notifications
	IOReturn registerNoDataNotificationCallback(UniversalNoDataProc handler, void *pRefCon, UInt32 noDataTimeInMSec, bool cipOnlyIsNoData = true);
	
	// Publically visible vars
	unsigned int transportState;

	// A reference to the current run loop for isoch callbacks
	CFRunLoopRef runLoopRef;
	
private:

	// Interface pointers

	IOFireWireLibNubRef nodeNubInterface;
	IOFireWireLibDCLCommandPoolRef dclCommandPool;
	IOFireWireLibRemoteIsochPortRef remoteIsocPort ;
	IOFireWireLibLocalIsochPortRef localIsocPort;
	IOFireWireLibIsochChannelRef isochChannel;
	StringLogger *logger;

	// Call to fixup jump targets in receive dcl
	void fixupDCLJumpTargets(void);
	
#ifdef kAVS_Use_NuDCL_UniversalReceiver
	IOFireWireLibNuDCLPoolRef nuDCLPool;
	CFMutableSetRef *pSegUpdateBags;
	NuDCLReceivePacketRef overrunDCL;
#else
	DCLCommandPtr *updateDCLList;
	DCLLabelPtr pDCLOverrunLabel;
	DCLCommandStruct *pFirstDCL;
#endif	
	
	// Class Variables
	unsigned int receiveChannel;
	IOFWSpeed receiveSpeed;
    UInt8 *pReceiveBuffer;
	UniversalReceiveSegment *receiveSegmentInfo;
	UInt32 currentSegment;
	unsigned int isochCyclesPerSegment;
	unsigned int isochSegments;
	unsigned int cycleBufLen;
	unsigned int irmAllocationsPacketSize;
	unsigned int dclCommandPoolSize;
	bool doIRM;
	bool noLogger;
	unsigned int dclVMBufferSize;
	volatile bool finalizeCallbackCalled;
	bool includeSPH;
	pthread_mutex_t transportControlMutex;
	pthread_mutex_t noDataTimerMutex;
	
	// Registered Handler functions
	UniversalDataPushProc packetPush;

	StructuredUniversalDataPushProc structuredDataPush;
	UInt32 maxNumStructuredDataStructsInCallback;
	UniversalReceiveCycleData *pCyclDataStruct;
		
	void *pPacketPushRefCon;
	UniversalReceiverMessageProc messageProc;
	void *pMessageProcRefCon;
	UniversalNoDataProc noDataProc;
	void *pNoDataProcRefCon;
	double noDataTimeLimitInSeconds;
	bool noDataCIPMode;
	
	UInt32 *pTimeStamps;
	UInt32 *pOverrunReceiveBuffer;
	
	// No data callback timer stuff
	void startNoDataTimer( void );
	void stopNoDataTimer( void );
	CFRunLoopTimerRef noDataTimer;
	
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
	void UniversalReceiveDCLCallback(void);
	void UniversalReceiveOverrunDCLCallback(void);
	void UniversalReceiveFinalizeCallback(void);
	
	// Callback for timer timeout
	void NoDataTimeout(void);

#ifdef kAVS_Enable_ForceStop_Handler	
	// Force Stop Callback
	void UniversalReceiveForceStop(UInt32 stopCondition);
#endif
	
};

} // namespace AVS

#endif // __AVCVIDEOSERVICES_UNIVERSALRECEIVER__
