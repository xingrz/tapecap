/*
	File:		AVCDeviceController.cpp

	Synopsis: This is the implementation file for the AVCDeviceController class.

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

static void deviceArrivedCallback(void * refcon, io_iterator_t iterator);
static void cfArrayReleaseAVCUnitObject(CFAllocatorRef allocator,const void *ptr);

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
AVCDeviceController::AVCDeviceController(AVCDeviceControllerNotification clientNotificationProc, void *pRefCon, AVCDeviceMessageNotification globalAVCDeviceMessageProc)
{
	matchEnumer = 0;
	fNotifyPort = 0;
	fNotifySource = 0;
	clientCallback = clientNotificationProc;
	pClientCallbackRefCon = pRefCon;
	globalAVCDeviceMessageCallback = globalAVCDeviceMessageProc;
	runLoopRef = CFRunLoopGetCurrent();
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
AVCDeviceController::~AVCDeviceController()
{
	// Delete all objects in avcDeviceArray, and release array object
	if (avcDeviceArray)
		CFRelease(avcDeviceArray);
	
	if (matchEnumer)
		IOObjectRelease(matchEnumer);
	
	if (fNotifyPort)
	{
		if (fNotifySource)
			CFRunLoopSourceInvalidate(fNotifySource);

        IONotificationPortDestroy(fNotifyPort);
	}
}

//////////////////////////////////////////////////////
// SetupDeviceController
//////////////////////////////////////////////////////
IOReturn AVCDeviceController::SetupDeviceController(void)
{
	IOReturn result = kIOReturnSuccess;
	CFArrayCallBacks arrayCallbacks;
	
	// Create an array to hold AVCDevice class objects
	arrayCallbacks.version = 0;
	arrayCallbacks.retain = NULL;
	arrayCallbacks.copyDescription = NULL;
	arrayCallbacks.equal = NULL;
	arrayCallbacks.release = cfArrayReleaseAVCUnitObject;
	avcDeviceArray = CFArrayCreateMutable(NULL,0,&arrayCallbacks);
	if (!avcDeviceArray)
	{
		return kIOReturnNoMemory;
	}
	
	// get mach master port
	result = IOMasterPort(bootstrap_port, & fMasterDevicePort) ;
	if ( result != kIOReturnSuccess )
	{
		return result;
	}

	// get a registry enumerator for all "IOFireWireAVCUnit" devices
    fNotifyPort = IONotificationPortCreate(fMasterDevicePort);
    fNotifySource = IONotificationPortGetRunLoopSource(fNotifyPort);
    CFRunLoopAddSource(runLoopRef, fNotifySource, kCFRunLoopDefaultMode);

    result = IOServiceAddMatchingNotification(
											  fNotifyPort,
											  kIOMatchedNotification,
											  IOServiceMatching( "IOFireWireAVCUnit" ),
											  deviceArrivedCallback, this,
											  & matchEnumer );
    if(result != kIOReturnSuccess)
	{
		return result;
	}

	// Call the callback to enumerate existing devices
    deviceArrivedCallback(this, matchEnumer);

	return result;
}

//////////////////////////////////////////////////////
// findDeviceByGuid
//////////////////////////////////////////////////////
AVCDevice *AVCDeviceController::findDeviceByGuid(UInt64 guid)
{
	UInt32 i;
	AVCDevice *pAVCDevice;
	
	for (i=0;i<(UInt32)CFArrayGetCount(avcDeviceArray);i++)
	{
		pAVCDevice = (AVCDevice*) CFArrayGetValueAtIndex(avcDeviceArray,i);

		if (pAVCDevice->guid == guid)
			return pAVCDevice;
	}
	
	return nil;
}

//////////////////////////////////////////////////////
// deviceArrivedCallback
//////////////////////////////////////////////////////
static void deviceArrivedCallback(void * refcon, io_iterator_t iterator)
{
	IOReturn result = kIOReturnSuccess;
	AVCDeviceController *pDeviceController = (AVCDeviceController*) refcon;
	CFNumberRef GUIDDesc;
    io_object_t newDevice;
    UInt64 newGUID = 0;
    CFMutableDictionaryRef properties;
	AVCDevice *pAVCDevice;
	
	// Iterate through all arrived devices 
	
    while((newDevice = IOIteratorNext(iterator)) )
	{
		// Get GUID for this device
		result = IORegistryEntryCreateCFProperties(newDevice, & properties, kCFAllocatorDefault, kNilOptions);
		if (result != kIOReturnSuccess)
		{
			// TODO
			continue;
		}
		else
		{
			GUIDDesc = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR("GUID"));
			CFNumberGetValue(GUIDDesc, kCFNumberSInt64Type, &newGUID);
			//printf("AY_DEBUG: device %x GUID: 0x%016llX\n", newDevice,newGUID);
		}

		// See if this GUID is already in our database
		pAVCDevice = pDeviceController->findDeviceByGuid(newGUID);
		if (pAVCDevice == nil)
		{
			// New Device, add it to the database
			//printf("AY_DEBUG: Adding 0x%016llX to our array\n",newGUID);
			
			// Create the new device
			pAVCDevice = new AVCDevice(pDeviceController,newDevice,newGUID);

			// Add the new device to our array
			CFArrayAppendValue(pDeviceController->avcDeviceArray,pAVCDevice);
		}
		else
		{
			// It already exists, need to reinitialize the device
			//printf("AY_DEBUG: 0x%016llX already in the database\n",newGUID);
			pAVCDevice->ReInit(newDevice);
		}
		
        CFRelease( properties ) ;

		// Notifiy the client if we need to - only if we've successfully discovered the device's capabilities.
		// If we haven't discovered the capabilities yet (because we were unable to open the device to do so),
		// we will try again to discover the capabilities when we're notified that the device is closed by whoever
		// had it opened in the first place (in the AVCDevice's service interest callback).
		if ((pDeviceController->clientCallback != nil) && (pAVCDevice->capabilitiesDiscovered == true))
			pDeviceController->clientCallback(pDeviceController,pDeviceController->pClientCallbackRefCon, pAVCDevice);
	}
}

////////////////////////////////////////////////////
// cfArrayReleaseAVCUnitObject
////////////////////////////////////////////////////
static void cfArrayReleaseAVCUnitObject(CFAllocatorRef allocator,const void *ptr)
{
	AVCDevice *pAVCDevice = (AVCDevice*) ptr;
	
	// If we've allowed a client to open a device with openDevice()
	// but it never called closeDevice(), do it now
	if (pAVCDevice->isOpened() == true)
		pAVCDevice->closeDevice();
		
	// Delete the object which will call it's destructor
	delete (AVCDevice*) ptr;
	return;
}

} // namespace AVS