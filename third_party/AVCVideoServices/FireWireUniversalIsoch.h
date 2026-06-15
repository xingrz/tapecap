/*
	File:		FireWireUniversalIsoch.h
 
 Synopsis: This is the header file for the FireWire Universal Isoch support. 
 
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

#ifndef __AVCVIDEOSERVICES_FIREWIREUNIVERSAL__
#define __AVCVIDEOSERVICES_FIREWIREUNIVERSAL__

namespace AVS
{

enum
{
	k61883Fmt_DV = kFWAVC61883Fmt_DV,
	k61883Fmt_BT601 = kFWAVC61883Fmt_BT601,
	k61883Fmt_AudioMidi = kFWAVC61883Fmt_AudioMidi,
	k61883Fmt_MPEG2TS = kFWAVC61883Fmt_MPEG2TS,
	k61883Fmt_Invalid = kFWAVC61883Fmt_Invalid,
	kMaxSourcePacketsPerCIPPacket = kFWAVCMaxSourcePacketsPerCIPPacket
};
	
typedef FWAVCCIPPacketParserInfo CIPPacketParserStruct;

// Evaluate the "tag" field from an isoch-header value, to determine if
// this packet is CIP-based.
bool IsCIPPacket(UInt32 isochHeaderValue);

// Parse a CIP-based packet
IOReturn ParseCIPPacket(UInt8 *pPacketPayload, UInt32 payloadLength, CIPPacketParserStruct *pParsedPacket);

//////////////////////////////////////////////////////////////////////////////////
//
// Prototypes for UniversalReceiver object creation/destruction helper functions.
// These functions create/prepare or destroy both a UniversalReceiver class object, 
// as well as a dedicated real-time thread for the object's callbacks and DCL
// processing.
//
//////////////////////////////////////////////////////////////////////////////////

// Create and setup a UniversalReceiver object and it's dedicated thread
IOReturn CreateUniversalReceiver(UniversalReceiver **ppReceiver,
								 UniversalDataPushProc dataPushProcHandler,
								 void *pDataPushProcRefCon = nil,
								 UniversalReceiverMessageProc messageProcHandler = nil,
								 void *pMessageProcRefCon = nil,
								 StringLogger *stringLogger = nil,
								 IOFireWireLibNubRef nubInterface = nil,
								 unsigned int cyclesPerSegment = kCyclesPerUniversalReceiveSegment,
								 unsigned int numSegments = kNumUniversalReceiveSegments,
								 unsigned int cycleBufferSize = kUniversalReceiverDefaultBufferSize,
								 bool doIRMAllocations = false,
								 unsigned int irmAllocationPacketLen = kUniversalReceiverDefaultBufferSize);

// Destroy a UniversalReceiver object created with CreateUniversalReceiver(), and it's dedicated thread
IOReturn DestroyUniversalReceiver(UniversalReceiver *pReceiver);

//////////////////////////////////////////////////////////////////////////////////
//
// Prototypes for UniversalTransmitter object creation/destruction helper functions.
// These functions create/prepare or destroy both a UniversalTransmitter class object, 
// as well as a dedicated real-time thread for the object's callbacks and DCL
// processing.
//
//////////////////////////////////////////////////////////////////////////////////

// Create and setup a UniversalTransmitter object and it's dedicated thread
IOReturn CreateUniversalTransmitter(UniversalTransmitter **ppTransmitter,
									UniversalTransmitterDataPullProc dataPullProcHandler,
									void *pDataPullProcRefCon = nil,
									UniversalTransmitterMessageProc messageProcHandler = nil,
									void *pMessageProcRefCon = nil,
									StringLogger *stringLogger = nil,
									IOFireWireLibNubRef nubInterface = nil,
									unsigned int cyclesPerSegment = kUniversalTransmitterCyclesPerSegment,
									unsigned int numSegments = kUniversalTransmitterNumSegments,
									unsigned int clientTransmitBufferSize = kUniversalTransmitterClientTransmitBufferSize,
									bool doIRMAllocations = false,
									unsigned int irmAllocationMaxPacketSize = kUniversalTransmitterIrmAllocationMaxPacketSize,
									unsigned int numStartupCycleMatchBits = kUniversalTransmitterNumStartupCycleMatchBits);

// Destroy a UniversalTransmitter object created with CreateUniversalTransmitter(), and it's dedicated thread
IOReturn DestroyUniversalTransmitter(UniversalTransmitter *pTransmitter);

} // namespace AVS

#endif // __AVCVIDEOSERVICES_FIREWIREUNIVERSAL__
