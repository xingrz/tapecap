/*
	File:		DVFramer.h
 
 Synopsis: This is the header for the DVFramer Class 
 
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

#ifndef __AVCVIDEOSERVICES_DVFRAMER__
#define __AVCVIDEOSERVICES_DVFRAMER__

namespace AVS
{
	
// enum for message passed in DV frame callback
typedef unsigned int DVFramerCallbackMessage;
enum
{
	kDVFramerFrameReceivedSuccessfully = kFWAVCDVFramerFrameReceivedSuccessfully, 
	kDVFramerPartialFrameReceived = kFWAVCDVFramerPartialFrameReceived,
	kDVFramerCorruptFrameReceived = kFWAVCDVFramerCorruptFrameReceived,
	kDVFramerNoFrameBufferAvailable = kFWAVCDVFramerNoFrameBufferAvailable,
	kDVFramerNoMemoryForFrameBuffer = kFWAVCDVFramerNoMemoryForFrameBuffer
};
	
// Misc constants
enum
{
	kDVFramerDefaultFrameCount = kFWAVCDVFramerDefaultFrameCount
};


// The DVFrame structure passed to the client in the DV frame callback
struct DVFrame
{
	// The client can access these, but should never modify them
	UInt8 *pFrameData;
	UInt32 frameLen;
	UInt8 frameMode;
	UInt32 frameSYTTime;
	UInt32 packetStartTimeStamp;
	UInt64 packetStartU64TimeStamp;
	class DVFramer *pDVFramer; // A pointer to the specific DVFramer object that created this frame struct
	
	// The client can use/modify these as needed
	void *pClientPrivateData;
	void *pFWAVCPrivateData;

	// These parameter(s) should be considered off-limits to the clients
	UInt32 frameBufferSize;
};

// Function prototype for DV frame ready callback.
typedef IOReturn (*DVFramerCallback) (DVFramerCallbackMessage msg, DVFrame* pDVFrame, void *pRefCon, DVFramer *pDVFramer);

//
// The DVFramer Class Declaration
//
class DVFramer
{
	
public:
	// Constructor
    DVFramer(DVFramerCallback fCallback = nil,
			 void *pCallbackRefCon = nil,
			 UInt8 initialDVMode = 0x00,
			 UInt32 initialDVFrameCount = kDVFramerDefaultFrameCount,
			 StringLogger *stringLogger = nil);
	
    // Destructor
    ~DVFramer();
	
	// Setup function
	IOReturn setupDVFramer(void);
	
	// Input a DV DIF Packet into the framer
	IOReturn nextDVSourcePacket(UInt8 *pSourcePacket, 
								UInt32 packetLen, 
								UInt8 dvMode, 
								UInt16 syt = 0xFFFFFFFF, 
								UInt32 packetTimeStamp = 0xFFFFFFFF,
								UInt64 packetU64TimeStamp = 0xFFFFFFFFFFFFFFFFLL);
	
	// Reset function
	IOReturn resetDVFramer(void);
	
	// Release a DV frame buffer passed in a callback
	IOReturn ReleaseDVFrame(DVFrame* pDVFrame);
			
private:

	DVFrame* getNextQueuedFrame(void);
	IOReturn FindDVFormatInfoForCurrentMode(void);
	
	StringLogger *logger;
	DVFrame* pCurrentDVFrame;
	UInt32 currentFrameOffset;
	UInt8 currentDVMode;
	DVFormats *pDVFormat;
	DVFramerCallback clientCallback;
	void *pClientCallbackProcRefCon;
	std::deque<DVFrame*> frameQueue;
	pthread_mutex_t queueProtectMutex;
	UInt32 frameCount;
	bool framerIsSetup;
};
	
} // namespace AVS

#endif /* __AVCVIDEOSERVICES_DVFRAMER__ */
