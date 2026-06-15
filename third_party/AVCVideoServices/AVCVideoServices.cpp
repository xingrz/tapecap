/*
	File:		AVCVideoServices.cpp

    Synopsis: This file implements the helper functions for the AVCVideoServices framework. 
 
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

// Thread parameter structures
struct AVCVideoServicesThreadParams
{
	volatile bool threadReady;
	AVCDeviceController *pDeviceController;
	AVCDeviceControllerNotification clientNotificationProc;
	void *pRefCon;
	AVCDeviceMessageNotification globalAVCDeviceMessageProc;
};

static void *AVCVideoServicesThreadStart(AVCVideoServicesThreadParams* pParams);

//////////////////////////////////////////////////////////////////////
// CreateAVCDeviceController
//////////////////////////////////////////////////////////////////////
IOReturn CreateAVCDeviceController(AVCDeviceController **ppAVCDeviceController,
								   AVCDeviceControllerNotification clientNotificationProc,
								   void *pRefCon,
								   AVCDeviceMessageNotification globalAVCDeviceMessageProc)
{
	IOReturn result = kIOReturnSuccess ;
	AVCVideoServicesThreadParams threadParams;
	pthread_t rtThread;
	pthread_attr_t threadAttr;

	threadParams.pDeviceController = nil;
	threadParams.threadReady = false;
	threadParams.clientNotificationProc = clientNotificationProc;
	threadParams.pRefCon = pRefCon;
	threadParams.globalAVCDeviceMessageProc = globalAVCDeviceMessageProc;
	
	// Create the real-time thread which will instantiate and setup new AVCDeviceController object
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&rtThread, &threadAttr, (void *(*)(void *))AVCVideoServicesThreadStart, &threadParams);
	pthread_attr_destroy(&threadAttr);

	// Wait forever for the new thread to be ready
	while (threadParams.threadReady == false) usleep(1000);

	*ppAVCDeviceController = threadParams.pDeviceController;
	return result;
}

//////////////////////////////////////////////////////////////////////
// DestroyAVCDeviceController
//////////////////////////////////////////////////////////////////////
IOReturn DestroyAVCDeviceController(AVCDeviceController *pAVCDeviceController)
{
	IOReturn result = kIOReturnSuccess ;
	CFRunLoopRef runLoopRef;

	// Save the ref to the run loop the transmitter is using
	runLoopRef = pAVCDeviceController->runLoopRef;

	// Delete transmitter object
	delete pAVCDeviceController;

	// Stop the run-loop in the RT thread. The RT thread will then terminate
	CFRunLoopStop(runLoopRef);

	return result;
}

//////////////////////////////////////////////////////////////////////
// AVCVideoServicesThreadStart
//////////////////////////////////////////////////////////////////////
static void *AVCVideoServicesThreadStart(AVCVideoServicesThreadParams* pParams)
{
	IOReturn result = kIOReturnSuccess ;
	AVCDeviceController *pDeviceController;

	// Instantiate a new receiver object
	pDeviceController = new AVCDeviceController(pParams->clientNotificationProc,pParams->pRefCon,pParams->globalAVCDeviceMessageProc);
	
	// Setup the receiver object
	if (pDeviceController)
		result = pDeviceController->SetupDeviceController();

	// Update the return parameter with a pointer to the new receiver object
	if (result == kIOReturnSuccess)
		pParams->pDeviceController = pDeviceController;
	else
	{
		delete pDeviceController;
		pParams->pDeviceController = nil;
	}

	// Signal that this thread is ready
	pParams->threadReady = true;

	// Start the run loop
	if ((pDeviceController) && (result == kIOReturnSuccess))
		CFRunLoopRun();

	return nil;
}

} // namespace AVS
