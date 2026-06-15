/*
	File:		DVFramer.cpp
 
 Synopsis: This is the source for the DVFramer Class 
 
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

#include "AVCVideoServices.h"

namespace AVS
{

/////////////////////////////////////////////////////////
// Constructor
/////////////////////////////////////////////////////////
DVFramer::DVFramer(DVFramerCallback fCallback, void *pCallbackRefCon, UInt8 initialDVMode, UInt32 initialDVFrameCount, StringLogger *stringLogger)
{
	logger = stringLogger;
	currentDVMode = initialDVMode;
	pCurrentDVFrame = nil;
	currentFrameOffset = 0;
	pDVFormat = nil;
	frameCount = initialDVFrameCount;
	framerIsSetup = false;
	clientCallback = fCallback;
	pClientCallbackProcRefCon = pCallbackRefCon;
}
	
/////////////////////////////////////////////////////////
// Destructor
/////////////////////////////////////////////////////////
DVFramer::~DVFramer()
{
	DVFrame* pDeleteFrame;
	
	// If we have a current frame, put it back on the queue
	if (pCurrentDVFrame)
		frameQueue.push_back(pCurrentDVFrame);
	
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

	// Delete the mutex
	pthread_mutex_destroy(&queueProtectMutex);
}

/////////////////////////////////////////////////////////
// DVFramer::setupDVFramer
/////////////////////////////////////////////////////////
IOReturn DVFramer::setupDVFramer(void)
{
	DVFrame *pDVFrame;
	UInt32 i;
	IOReturn result;
	
	// Get the info for the specified DV mode
	result = FindDVFormatInfoForCurrentMode();
	if (result != kIOReturnSuccess)
		return result;
		
	// Initialize the mutex for DV frame queue access
    pthread_mutex_init(&queueProtectMutex, NULL);
	
	// Initialzie the DV frame queue
	for (i=0;i<frameCount;i++)
	{
		// Allocate a dv frame 
		pDVFrame = new DVFrame;
		if (!pDVFrame)
			return kIOReturnNoMemory;
		
		// Allocate a frame buffer for the dv frame
		pDVFrame->pFrameData = new UInt8[pDVFormat->frameSize];
		if (!pDVFrame->pFrameData)
			return kIOReturnNoMemory;

		// Initialize the dv frame
		pDVFrame->frameBufferSize = pDVFormat->frameSize;
		pDVFrame->pDVFramer = this;
		
		// Add this dv frame to the frame queue
		frameQueue.push_back(pDVFrame);
	}
	
	if (result == kIOReturnSuccess)
		framerIsSetup = true;

	return result;
}

/////////////////////////////////////////////////////////
// DVFramer::nextDVSourcePacket
/////////////////////////////////////////////////////////
IOReturn DVFramer::nextDVSourcePacket(UInt8 *pSourcePacket, UInt32 packetLen, UInt8 dvMode, UInt16 syt, UInt32 packetTimeStamp, UInt64 packetU64TimeStamp)
{
	bool startOfFrameFound;
	IOReturn result;
	UInt32 expectedSourcePacketSize;
	UInt8 *pReallocatedBuffer;
	UInt8 savedDVMode;

	if (framerIsSetup != true)
		return kIOReturnNotReady;
	
	// See if this packet represents a mode change
	if (dvMode != currentDVMode)
	{	
		if (logger)
			logger->log("DVFramer Info: DV mode change! Old: 0x%02X, New: 0x%02X\n",currentDVMode,dvMode);

		// See if we have a frame-in-progress
		if (pCurrentDVFrame)
		{
			// Report it to the client
			if (clientCallback)
				clientCallback(kDVFramerPartialFrameReceived, nil, pClientCallbackProcRefCon,this);

			// Put the current frame back on the queue
			ReleaseDVFrame(pCurrentDVFrame);
			pCurrentDVFrame = nil;
		}

		// Change modes
		savedDVMode = currentDVMode;
		currentDVMode = dvMode;
		
		// Get the info for the new mode
		result = FindDVFormatInfoForCurrentMode();
		if (result != kIOReturnSuccess)
		{
			if (logger)
				logger->log("DVFramer Error: Client passed in illegal DV mode value:0x%02X\n",dvMode);

			// Restore the current mode back to the last good-mode!
			currentDVMode = savedDVMode;
			
			// Return an error. Don't go any further!
			return	kIOReturnBadArgument;
		}			
	}

	// Sanity check the packet length!
	expectedSourcePacketSize = (pDVFormat->dbs*4)*(1 << pDVFormat->fn);
	if (packetLen != expectedSourcePacketSize)
	{
		if (logger)
			logger->log("DVFramer Error: Client passed in wrong source packet size (%d) for specified DV mode (0x%02X)\n",packetLen,dvMode);

		// Return an error. Don't go any further!
		return	kIOReturnBadArgument;
	}
	
	// See if this is the start of a frame. This should work for all DV modes
	startOfFrameFound = ((EndianU16_BtoN (*(short *)(pSourcePacket))  & 0xE0FC ) == 0x0004 );

	if (startOfFrameFound)
	{
		// If we have a partial frame now, then we didn't finish it.  Alert the client
		// that the current frame is a corrupted frame. Otherwise, get a frame from the
		// frame queue (if one exists)
		if (pCurrentDVFrame)
		{
			if (clientCallback)
				clientCallback(kDVFramerPartialFrameReceived, nil, pClientCallbackProcRefCon,this);
		}
		else
			pCurrentDVFrame = getNextQueuedFrame();

		if (pCurrentDVFrame)
		{
			// We have a dv frame. Ensure the buffer is big enough, and if not, attempt to reallocate one.
			if (pDVFormat->frameSize > pCurrentDVFrame->frameBufferSize)
			{
				pReallocatedBuffer = new UInt8[pDVFormat->frameSize];		
				if (pReallocatedBuffer)
				{
					delete [] pCurrentDVFrame->pFrameData;
					pCurrentDVFrame->pFrameData = pReallocatedBuffer;
					pCurrentDVFrame->frameBufferSize = pDVFormat->frameSize;
				}
				else
				{
					// Release this dv frame, since it's buffer isn't big enough to use anyway
					ReleaseDVFrame(pCurrentDVFrame);
					pCurrentDVFrame = nil;

					// Alert the client
					if (clientCallback)
						clientCallback(kDVFramerNoMemoryForFrameBuffer, nil, pClientCallbackProcRefCon,this);
					result = kIOReturnError;
				}
			
			}
		}
		else
		{
			if (clientCallback)
				clientCallback(kDVFramerNoFrameBufferAvailable, nil, pClientCallbackProcRefCon,this);
			result = kIOReturnError;
		}

		// Initialize the dv frame parameters
		if (pCurrentDVFrame)
		{
			pCurrentDVFrame->frameSYTTime = syt;
			pCurrentDVFrame->packetStartTimeStamp = packetTimeStamp;
			pCurrentDVFrame->packetStartU64TimeStamp = packetU64TimeStamp;
			pCurrentDVFrame->frameLen = pDVFormat->frameSize;
			pCurrentDVFrame->frameMode = currentDVMode;
			currentFrameOffset = 0;
		}		
	}	
	
	if (pCurrentDVFrame)
	{
		// Ensure that there's space in this frame buffer for this source-packet (to prevent crashing if something has gone wrong)
		if ((currentFrameOffset + packetLen) > pCurrentDVFrame->frameLen)
		{
			// Report this unusal condition
			if (clientCallback)
				clientCallback(kDVFramerCorruptFrameReceived, nil, pClientCallbackProcRefCon,this);

			// Release this dv frame
			ReleaseDVFrame(pCurrentDVFrame);
			pCurrentDVFrame = nil;
		}
		else
		{
			// Copy the source packet into the frame buffer
			memcpy(&pCurrentDVFrame->pFrameData[currentFrameOffset],pSourcePacket,packetLen);
			
			// Increment the currentFrameOffset
			currentFrameOffset += packetLen;
			
			// See if the frame-buffer is full, and, if so,
			// pass it to the client, and get another empty frame
			if (currentFrameOffset == pCurrentDVFrame->frameLen)
			{
				if (clientCallback)
					clientCallback(kDVFramerFrameReceivedSuccessfully, pCurrentDVFrame, pClientCallbackProcRefCon,this);
				pCurrentDVFrame = nil;
			}
		}
	}
	
	// Don't always return success!
	// A return value of kIOReturnSuccess implies that the packet was "consumed"
	// by the DV framer. An error implies that it wasn't (i.e. in the case of
	// no available frame buffers).
	return kIOReturnSuccess;
}
	
/////////////////////////////////////////////////////////
// DVFramer::resetDVFramer
/////////////////////////////////////////////////////////
IOReturn DVFramer::resetDVFramer(void)
{
	if (pCurrentDVFrame)
	{
		ReleaseDVFrame(pCurrentDVFrame);
		pCurrentDVFrame = nil;
	}
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// DVFramer::getNextQueuedFrame
//////////////////////////////////////////////////////////////////////
DVFrame* DVFramer::getNextQueuedFrame(void)
{
	DVFrame* pFrame = nil;
	
	pthread_mutex_lock(&queueProtectMutex);
	
	if (!frameQueue.empty())
	{
		pFrame = frameQueue.front();
		frameQueue.pop_front();
	}
#ifdef kAVS_DVFramer_AllocateNewDVFramesWhenNeeded
	else
	{
		// Here, we allocate another dv frame on-the-fly
		pFrame = new DVFrame;
		if (pFrame)
		{
			// Allocate a frame buffer for the dv frame
			pFrame->pFrameData = new UInt8[pDVFormat->frameSize];
			if (!pFrame->pFrameData)
			{
				delete pFrame;
			}
			else
			{
				// Initialize the dv frame
				pFrame->frameBufferSize = pDVFormat->frameSize;
				pFrame->pDVFramer = this;
			}
		}
	}
#endif
	
	pthread_mutex_unlock(&queueProtectMutex);
	
	return pFrame;
}

/////////////////////////////////////////////////////////
// DVFramer::ReleaseDVFrame
/////////////////////////////////////////////////////////
IOReturn DVFramer::ReleaseDVFrame(DVFrame* pDVFrame)
{
	if (pDVFrame)
	{
		// Take the mutex lock
		pthread_mutex_lock(&queueProtectMutex);
		
		frameQueue.push_back(pDVFrame);
		
		// Release the mutex lock
		pthread_mutex_unlock(&queueProtectMutex);
	}
	
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////////////////////
// DVFramer::FindDVFormatInfoForCurrentMode
//////////////////////////////////////////////////////////////////////
IOReturn
DVFramer::FindDVFormatInfoForCurrentMode(void)
{
	// Note: This function finds the DVFormat table entry
	// for the current DV mode, disregarding the 2-bit
	// speed code that's embedded in the low 2-bits of the mode value.
	
	UInt32 i = 0;
	DVFormats* pFormat = &dvFormats[i];
	
	while (pFormat->frameSize != 0)
	{
		if ((currentDVMode & 0xFC) == pFormat->mode)
		{
			pDVFormat = pFormat;
			return kIOReturnSuccess;
		}
		i+=1;
		pFormat = &dvFormats[i];
	};
	
	return kIOReturnBadArgument;
}


} // namespace AVS