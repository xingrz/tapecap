/*
	File:		AVCDeviceCommandInterface.cpp
 
	Synopsis: This is the implementation file for the AVCDeviceCommandInterface class.
 
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

static void AVCDeviceServiceInterestCallback(
											 void *refcon,
											 io_service_t service,
											 natural_t messageType,
											 void * messageArgument);

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
AVCDeviceCommandInterface::AVCDeviceCommandInterface(AVCDevice *pAVCDevice)
{
	IOReturn result;

	// Initialize class vars
	avcUnit = pAVCDevice->avcUnit;
	pAVCDeviceController = pAVCDevice->pAVCDeviceController;
	avcInterface = nil;
	clientDeviceMessageProc = nil;
	pClientMessageProcRefCon = nil;
	pSavedAVCDevice = pAVCDevice;
	
	// Install a service interest notification handler for this avcUnit
	result = IOServiceAddInterestNotification(pAVCDeviceController->fNotifyPort,
											  avcUnit,
											  kIOGeneralInterest,
											  AVCDeviceServiceInterestCallback ,
											  this,
											  & interestNotification) ;
}	

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
AVCDeviceCommandInterface::~AVCDeviceCommandInterface()
{
	IOObjectRelease(interestNotification);
	
	if (avcInterface)
		deactivateAVCDeviceCommandInterface();
}

//////////////////////////////////////////////////////
// GetAVCDevice
//////////////////////////////////////////////////////
AVCDevice *AVCDeviceCommandInterface::GetAVCDevice(void)
{
	return pSavedAVCDevice;
}
////////////////////////////////////////////////////
// activateAVCDeviceCommandInterface
////////////////////////////////////////////////////
IOReturn AVCDeviceCommandInterface::activateAVCDeviceCommandInterface(AVCDeviceCommandInterfaceMessageNotification deviceMessageProc, void *pMessageProcRefCon)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;
	
	if (avcInterface)
		result = kIOReturnStillOpen;
	else
	{
		result = createAVCUnitInterface();
		if (result == kIOReturnSuccess)
		{
			clientDeviceMessageProc = deviceMessageProc;
			pClientMessageProcRefCon = pMessageProcRefCon;
		}
	}
	
	return result;
}

////////////////////////////////////////////////////
// deactivateAVCDeviceCommandInterface
////////////////////////////////////////////////////
IOReturn AVCDeviceCommandInterface::deactivateAVCDeviceCommandInterface(void)
{
	IOReturn result = kIOReturnSuccess;

	if (!avcInterface)
		result = kIOReturnNotOpen;
	
	result = releaseAVCUnitInterface();
	clientDeviceMessageProc = nil;
	pClientMessageProcRefCon = nil;
	
	return result;
}

////////////////////////////////////////////////////
// createAVCUnitInterface
////////////////////////////////////////////////////
IOReturn AVCDeviceCommandInterface::createAVCUnitInterface(void)
{
	// Local Vars
	IOCFPlugInInterface **theCFPlugInInterface;
	SInt32 theScore;
	IOReturn result = kIOReturnSuccess;
	
	result = IOCreatePlugInInterfaceForService(
											   avcUnit,
											   kIOFireWireAVCLibUnitTypeID,
											   kIOCFPlugInInterfaceID,		//interfaceType,
											   &theCFPlugInInterface,
											   &theScore);
	if (!result)
	{
		HRESULT comErr;
		comErr = (*theCFPlugInInterface)->QueryInterface(
														 theCFPlugInInterface,
														 CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID),
														 (void**) &avcInterface);
		if (comErr == S_OK)
		{
			result = (*avcInterface)->addCallbackDispatcherToRunLoop(avcInterface, pAVCDeviceController->runLoopRef );
		}
		else
			result = comErr;
		
		(*theCFPlugInInterface)->Release(theCFPlugInInterface);	// Leave just one reference.	
	}
	
    return result;
}

////////////////////////////////////////////////////
// releaseAVCUnitInterface
////////////////////////////////////////////////////
IOReturn AVCDeviceCommandInterface::releaseAVCUnitInterface(void)
{
	if (avcInterface)
	{
		// Remove callback dispatcher from run loop
		(*avcInterface)->removeCallbackDispatcherFromRunLoop(avcInterface);
		
		// Close and release interface
		(*avcInterface)->Release(avcInterface) ;
	}
	
	avcInterface = 0;
	
	return kIOReturnSuccess;
}	
	
////////////////////////////////////////////////////
// AVCCommand
////////////////////////////////////////////////////
IOReturn AVCDeviceCommandInterface::AVCCommand(const UInt8 *command, UInt32 cmdLen, UInt8 *response, UInt32 *responseLen)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;
	
	if (avcInterface)
		result = (*avcInterface)->AVCCommand(avcInterface, command, cmdLen, response, responseLen);
	else
		result = kIOReturnNotOpen;
	
    return result;
}	

//////////////////////////////////////////////////////
// AVCDeviceServiceInterestCallback
//////////////////////////////////////////////////////
static void AVCDeviceServiceInterestCallback(
											 void *refcon,
											 io_service_t service,
											 natural_t messageType,
											 void * messageArgument)
{
	AVCDeviceCommandInterface *pAVCDeviceCommandInterface = (AVCDeviceCommandInterface*) refcon;
	
	// If needed, notify client of message
	if (pAVCDeviceCommandInterface->clientDeviceMessageProc != nil)
		pAVCDeviceCommandInterface->clientDeviceMessageProc(pAVCDeviceCommandInterface, 
															messageType, 
															messageArgument,
															pAVCDeviceCommandInterface->pClientMessageProcRefCon);
}

} // namespace AVS