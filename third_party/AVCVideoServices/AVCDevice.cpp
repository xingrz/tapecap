/*
	File:		AVCDevice.cpp

	Synopsis: This is the implementation file for the AVCDevice class.

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

#ifndef kIOFireWireAVCLibUnitInterfaceID_v2
#define kIOFireWireAVCLibUnitInterfaceID_v2  CFUUIDGetConstantUUIDWithBytes(NULL, \
0x85, 0xB5, 0xE9, 0x54, 0x0A, 0xEF, 0x11, 0xD8, 0x8D, 0x19, 0x00, 0x03, 0x93, 0x91, 0x4A, 0xBA)
#endif

namespace AVS
{

static void AVCDeviceServiceInterestCallback(
											 void *refcon,
											 io_service_t service,
											 natural_t messageType,
											 void * messageArgument);

static bool isDVCPro(IOFireWireAVCLibUnitInterface **avc, UInt8 *pMode);
static UInt32 getDeviceSubunits(IOFireWireAVCLibUnitInterface **avc);
static IOReturn getSignalMode(IOFireWireAVCLibUnitInterface **avc, UInt8 *mode);
static IOReturn outputPlugSignalFormat(IOFireWireAVCLibUnitInterface **avc, UInt32 plugNum, UInt32 *pSignalFormat);
static void AVCDeviceStreamMessageProc(UInt32 msg, UInt32 param1, UInt32 param2, void *pRefCon);
static IOReturn updateP2PCount(IOFireWireLibDeviceRef interface, UInt32 plugAddr, SInt32 inc, bool failOnBusReset, UInt32 chan, IOFWSpeed speed);
static IOReturn makeConnection(IOFireWireLibDeviceRef interface, UInt32 addr, UInt32 chan, IOFWSpeed speed);
static void breakConnection(IOFireWireLibDeviceRef interface, UInt32 addr);
static IOReturn makeP2PInputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo, UInt32 chan);
static IOReturn breakP2PInputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo);
static IOReturn makeP2POutputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo, UInt32 chan, IOFWSpeed speed);
static IOReturn breakP2POutputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo);
static IOReturn getIsochPlugInfo(IOFireWireAVCLibUnitInterface **avc, UInt32 *pNumInputPlugs, UInt32 *pNumOutputPlugs);

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
AVCDevice::AVCDevice(AVCDeviceController *pDeviceController,io_object_t newDevice,UInt64 newGUID)
{
	CFStringRef strDesc;
	CFBooleanRef hasFCP;

	CFNumberRef unitVendorID;
	CFNumberRef unitModelID;
	CFStringRef unitVendorStrDesc;
	
	IOReturn result = kIOReturnSuccess;
	CFMutableDictionaryRef properties;

	// Initialize some class memebers
	avcUnit = newDevice;
	guid = newGUID;
	pAVCDeviceController = pDeviceController;
	avcInterface = nil;
	deviceInterface = nil;
	isAttached = true;
	numInputPlugs = 0;
	numOutputPlugs = 0;
	capabilitiesDiscovered = false;
	subUnits = 0xFFFFFFFF;
	hasTapeSubunit = false;
	hasMonitorOrTunerSubunit = false;
	hasMusicSubunit = false;
	hasAudioSubunit = false;
	isDVDevice = false;
	isMPEGDevice = false;
	isDVCProDevice = false;
	dvMode = 0x00;
	mpegMode = 0x00;
	clientDeviceMessageProc = nil;
	pClientMessageProcRefCon = nil;
	pAVCDeviceStreamQueueHead = nil;
	deviceName[0] = 0;
	vendorName[0] = 0;
	pClientPrivateData = 0;
	
	// Traverse up the tree from this AVCUnit to the FWUnit to the FWDevice, to the busController
	IORegistryEntryGetParentEntry(avcUnit,kIOServicePlane,&fwUnit);
	IORegistryEntryGetParentEntry(fwUnit,kIOServicePlane,&fwDevice);
	IORegistryEntryGetParentEntry(fwDevice,kIOServicePlane,&busController);

	// Initialize the open/close mutex
	pthread_mutex_init(&deviceOpenCloseMutex,NULL);
	
	// Initialize the stream queue protect mutex
	pthread_mutex_init(&deviceStreamQueueMutex,NULL);
	
	result = IORegistryEntryCreateCFProperties(newDevice, &properties, kCFAllocatorDefault, kNilOptions);
	// TODO: Check result
	
	// Extract the product name from the IORegistry properties
    strDesc = (CFStringRef)CFDictionaryGetValue(properties, CFSTR("FireWire Product Name"));
	if(strDesc)
	{
		deviceName[0] = 0;
		CFStringGetCString(strDesc, deviceName, sizeof(deviceName), kCFStringEncodingMacRoman);
	}
	//printf("AY_DEBUG: Device Name = %s\n",deviceName);

	// Check the registry to see if this device supports FCP commands
	// If it doesn't, it's a pretty brain-dead device, most likely
	// a Analog/DV media converter box.
	hasFCP = (CFBooleanRef)CFDictionaryGetValue(properties, CFSTR("supportsFCP"));
	if(hasFCP)
		supportsFCP = CFBooleanGetValue(hasFCP);
	else
		supportsFCP = true;

	// Install a service interest notification handler for this avcUnit
	result = IOServiceAddInterestNotification(pAVCDeviceController->fNotifyPort,
										   newDevice,
										   kIOGeneralInterest,
										   AVCDeviceServiceInterestCallback ,
										   this,
										   & interestNotification) ;
	
	
	// Release the properties CF dictionary object
	CFRelease(properties);

	// Get some additional properties from the "FireWire Unit" in the registry
	result = IORegistryEntryCreateCFProperties(fwUnit, &properties, kCFAllocatorDefault, kNilOptions);
	// TODO: Check result

	// Extract the Vendor ID, Model ID, and Vendor Name from the registry properties

	unitVendorID = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR("Vendor_ID"));
	if (unitVendorID)
		CFNumberGetValue(unitVendorID, kCFNumberLongType, &vendorID);
	else
		vendorID = 0xFFFFFFFF;
	
	unitModelID = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR("Model_ID"));
	if (unitModelID)
		CFNumberGetValue(unitModelID, kCFNumberLongType, &modelID);
	else
		modelID = 0xFFFFFFFF;
	
	unitVendorStrDesc = (CFStringRef)CFDictionaryGetValue(properties, CFSTR("FireWire Vendor Name"));
	if (unitVendorStrDesc)
	{
		vendorName[0] = 0;
		CFStringGetCString(unitVendorStrDesc, vendorName, sizeof(vendorName), kCFStringEncodingMacRoman);
	}
	
	// Release the properties CF dictionary object
	CFRelease(properties);
	
	// Discover this AVC Device's capabilities
	discoverAVCDeviceCapabilities();
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
AVCDevice::~AVCDevice()
{
	// Release the IO registry objects
	IOObjectRelease(interestNotification);
	IOObjectRelease(busController);
	IOObjectRelease(fwDevice);
	IOObjectRelease(fwUnit);
	IOObjectRelease(avcUnit);
	
	// Release the open/close mutex
	pthread_mutex_destroy(&deviceOpenCloseMutex);

	// Release the stream queue protect mutex
	pthread_mutex_destroy(&deviceStreamQueueMutex);
}

//////////////////////////////////////////////////////
// reInit
//////////////////////////////////////////////////////
IOReturn AVCDevice::ReInit(io_object_t newDevice)
{
	IOReturn result = kIOReturnSuccess;

	// Release the previous IO registry objects
	IOObjectRelease(interestNotification);
	IOObjectRelease(busController);
	IOObjectRelease(fwDevice);
	IOObjectRelease(fwUnit);
	IOObjectRelease(avcUnit);
	
	// Reinitialize some class memebers
	avcUnit = newDevice;
	isAttached = true;

#ifndef kAVS_Bypass_AVCDevice_Rediscovery
	capabilitiesDiscovered = false;
#endif	
	
	// Traverse up the tree from this AVCUnit to the FWUnit to the FWDevice
	IORegistryEntryGetParentEntry(avcUnit,kIOServicePlane,&fwUnit);
	IORegistryEntryGetParentEntry(fwUnit,kIOServicePlane,&fwDevice);
	IORegistryEntryGetParentEntry(fwDevice,kIOServicePlane,&busController);

	// Install a service interest notification handler for this avcUnit
	result = IOServiceAddInterestNotification(pAVCDeviceController->fNotifyPort,
										   newDevice,
										   kIOGeneralInterest,
										   AVCDeviceServiceInterestCallback ,
										   this,
										   & interestNotification) ;

#ifndef kAVS_Bypass_AVCDevice_Rediscovery
	// Rediscover this AVC Device's capabilities
	discoverAVCDeviceCapabilities();
#endif	
	
	return result;
}

//////////////////////////////////////////////////////
// isOpened
//////////////////////////////////////////////////////
bool AVCDevice::isOpened()
{
	if (avcInterface)
		return true;
	else
		return false;
}

//////////////////////////////////////////////////////
// openDevice
//////////////////////////////////////////////////////
IOReturn AVCDevice::openDevice(AVCDeviceMessageNotification deviceMessageProc, void *pMessageProcRefCon)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;

	if (avcInterface || deviceInterface)
		result = kIOReturnExclusiveAccess;
	else
	{
		result = open();
		if (result == kIOReturnSuccess)
		{
			clientDeviceMessageProc = deviceMessageProc;
			pClientMessageProcRefCon = pMessageProcRefCon;
		}
	}
	
	return result;
}

//////////////////////////////////////////////////////
// closeDevice
//////////////////////////////////////////////////////
IOReturn AVCDevice::closeDevice()
{
	IOReturn result = kIOReturnSuccess;

	// Remove the client callback notification for this device
	clientDeviceMessageProc = nil;
	pClientMessageProcRefCon = nil;
	
	// Tear-down any streams objects associated with this device
	while (pAVCDeviceStreamQueueHead != nil) 
		DestroyAVCDeviceStream(pAVCDeviceStreamQueueHead);

	result = close();
	if (result == kIOReturnSuccess)
	{
		clientDeviceMessageProc = nil;
		pClientMessageProcRefCon = nil;
	}

	return result;
}

//////////////////////////////////////////////////////
// open
//////////////////////////////////////////////////////
IOReturn AVCDevice::open(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;
	
	// Lock the open/close mutex
	pthread_mutex_lock(&deviceOpenCloseMutex);
	
	if (avcInterface || deviceInterface)
		result = kIOReturnExclusiveAccess;
	else
	{
#ifdef kAVS_Delay_AVCDevice_Open_And_Close
		usleep(500000);
#endif
		result = createAVCUnitInterface();
		if (result == kIOReturnSuccess)
		{
			result = createFWDeviceInterface();
			if (result != kIOReturnSuccess)
			{
				releaseAVCUnitInterface();
			}
		}
	}
	
	// Unlock the open/close mutex
	pthread_mutex_unlock(&deviceOpenCloseMutex);
	
    return result;
}

//////////////////////////////////////////////////////
// close
//////////////////////////////////////////////////////
IOReturn AVCDevice::close(void)
{
#ifdef kAVS_Delay_AVCDevice_Open_And_Close
	usleep(500000);
#endif

	// Lock the open/close mutex
	pthread_mutex_lock(&deviceOpenCloseMutex);
	
	releaseFWDeviceInterface();
	releaseAVCUnitInterface();

	// Unlock the open/close mutex
	pthread_mutex_unlock(&deviceOpenCloseMutex);
	
	return kIOReturnSuccess;
}

////////////////////////////////////////////////////
// discoverAVCDeviceCapabilities
////////////////////////////////////////////////////
IOReturn AVCDevice::discoverAVCDeviceCapabilities(void)
{
#ifdef kAVS_Open_AVCDevice_For_Discovery
	IOReturn result = kIOReturnSuccess;
	UInt8 mode;
	UInt32 signalFormat;

	//printf("AY_DEBUG: AVCDevice::discoverAVCDeviceCapabilities\n");

	// Attempt to open the device
	result = open();
	if (result != kIOReturnSuccess)
	{
		//printf("AY_DEBUG: Error Opening Device = 0x%08X\n",result);
	}
	else
	{
		// Get the isoch plug count for this device
		getIsochPlugCount();

		if (supportsFCP == true)
		{
			// Get the device's subunit list
			subUnits = getDeviceSubunits(avcInterface);

			// See if this device has a monitor or tuner subunit
			if (((subUnits & 0xFF000000) == 0x00000000) ||
				((subUnits & 0x00FF0000) == 0x00000000) ||
				((subUnits & 0x0000FF00) == 0x00000000) ||
				((subUnits & 0x000000FF) == 0x00000000) ||
				((subUnits & 0xFF000000) == 0x28000000) ||
				((subUnits & 0x00FF0000) == 0x00280000) ||
				((subUnits & 0x0000FF00) == 0x00002800) ||
				((subUnits & 0x000000FF) == 0x00000028))
			{
				hasMonitorOrTunerSubunit = true;
			}
			else
			{
				hasMonitorOrTunerSubunit = false;
			}
			
			// See if this device has a music subunit
			if (((subUnits & 0xFF000000) == 0x60000000) ||
				((subUnits & 0x00FF0000) == 0x00600000) ||
				((subUnits & 0x0000FF00) == 0x00006000) ||
				((subUnits & 0x000000FF) == 0x00000060))
			{
				hasMusicSubunit = true;
			}
			else
			{
				hasMusicSubunit = false;
			}
			
			// See if this device has an audio subunit
			// See if this device has a music subunit
			if (((subUnits & 0xFF000000) == 0x08000000) ||
				((subUnits & 0x00FF0000) == 0x00080000) ||
				((subUnits & 0x0000FF00) == 0x00000800) ||
				((subUnits & 0x000000FF) == 0x00000008))
			{
				hasAudioSubunit = true;
			}
			else
			{
				hasAudioSubunit = false;
			}
			
			// If this device has a tape subunit, try the tape subunit's
			// Signal-Mode command first to discover the device's stream format
			if (((subUnits & 0xFF000000) == 0x20000000) ||
	   ((subUnits & 0x00FF0000) == 0x00200000) ||
	   ((subUnits & 0x0000FF00) == 0x00002000) ||
	   ((subUnits & 0x000000FF) == 0x00000020))
			{
				hasTapeSubunit = true;

				if (isDVCPro(avcInterface,&mode) == true)
				{
					// DVCPro devices respond to the Panasonic
					// Vendor unique command, and report their
					// signal mode via output plug signal format command
					isDVCProDevice = true;
					isDVDevice = true;
					dvMode = mode;
				}
				else
				{
					result = getSignalMode(avcInterface, &mode);
					if (result == kIOReturnSuccess)
					{
						switch (mode)
						{
							case kAVCTapeSigModeHDV1_60:
							case kAVCTapeSigModeMPEG12Mbps_60:
							case kAVCTapeSigModeMPEG6Mbps_60:
							case kAVCTapeSigModeHDV1_50:
							case kAVCTapeSigModeMPEG12Mbps_50:
							case kAVCTapeSigModeMPEG6Mbps_50:
							case kAVCTapeSigModeDVHS:
							case kAVCTapeSigModeMicroMV12Mbps_60:
							case kAVCTapeSigModeMicroMV6Mbps_60:
							case kAVCTapeSigModeMicroMV12Mbps_50:
							case kAVCTapeSigModeMicroMV6Mbps_50:
							case kAVCTapeSigModeHDV2_60:
							case kAVCTapeSigModeHDV2_50:
								isDVDevice = false;
								isMPEGDevice = true;
								mpegMode = mode;
								break;
								
							case kAVCTapeSigModeSD525_60:
							case kAVCTapeSigModeSDL525_60:
							case kAVCTapeSigModeHD1125_60:
							case kAVCTapeSigModeSD625_50:
							case kAVCTapeSigModeSDL625_50:
							case kAVCTapeSigModeHD1250_50:
							case kAVCTapeSigModeDVCPro25_625_50:
							case kAVCTapeSigModeDVCPro25_525_60:
							case kAVCTapeSigModeDVCPro50_625_50:
							case kAVCTapeSigModeDVCPro50_525_60:
							case kAVCTapeSigModeDVCPro100_50:
							case kAVCTapeSigModeDVCPro100_60:
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = mode;
								break;
								
							// Older digital-8 camcorders may report 
							// these modes instead of standard NTSC DV25
							case kAVCTapeSigMode8mmNTSC:
							case kAVCTapeSigModeHi8NTSC:
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = kAVCTapeSigModeSD525_60;
								break;
								
							// Older digital-8 camcorders may report 
							// these modes instead of standard PAL DV25
							case kAVCTapeSigMode8mmPAL:	
							case kAVCTapeSigModeHi8PAL:	
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = kAVCTapeSigModeSD625_50;
								break;
								
							case kAVCTapeSigModeVHSNTSC:
							case kAVCTapeSigModeVHSMPAL:
							case kAVCTapeSigModeVHSPAL:	
							case kAVCTapeSigModeVHSNPAL:
							case kAVCTapeSigModeVHSSECAM:
							case kAVCTapeSigModeVHSMESECAM:
							case kAVCTapeSigModeSVHS525_60:	
							case kAVCTapeSigModeSVHS625_50:	
							case kAVCTapeSigModeAudido:	
							default:
								isMPEGDevice = false;
								isDVDevice = false;
								dvMode = 0xFF;
								mpegMode = 0xFF;
						}
					}
					else
					{
						// Couldn't determine stream format via tape subunit SignalMode command.
						// Try output plug signal format command
						result = outputPlugSignalFormat(avcInterface, 0, &signalFormat);
						if (result == kIOReturnSuccess)
						{
							if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatMPEGTS)
							{
								isDVDevice = false;
								isMPEGDevice = true;
								mpegMode = kAVCTapeSigModeDVHS;	// Here, default to D-VHS MPEG
							}
							else if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatNTSCDV)
							{
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = ((signalFormat & 0x00FF0000) >> 16);
							}
						}
					}
				}
			}
			else
			{
				// Device doesn't have a tape subunit
				hasTapeSubunit = false;

				// Use output plug signal format command
				result = outputPlugSignalFormat(avcInterface, 0, &signalFormat);
				if (result == kIOReturnSuccess)
				{
					if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatMPEGTS)
					{
						isDVDevice = false;
						isMPEGDevice = true;
						mpegMode = kAVCTapeSigModeDVHS;	// Here, default to D-VHS MPEG
					}
					else if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatNTSCDV)
					{
						isMPEGDevice = false;
						isDVDevice = true;
						dvMode = ((signalFormat & 0x00FF0000) >> 16);
					}
				}
			}
		}
		else
		{
			// Device doesn't support FCP. Assume it's a
			// NTSC DV media converter
			isMPEGDevice = false;
			isDVDevice = true;
			dvMode = kAVCTapeSigModeSD525_60;
		}
		
		capabilitiesDiscovered = true;

		// Close the device
		close();
	}
	
	return result;
	
#else
	
	IOReturn result = kIOReturnSuccess;
	UInt8 mode;
	UInt32 signalFormat;
	AVCDeviceCommandInterface *pCommandInterface;
	
	//printf("AY_DEBUG: AVCDevice::discoverAVCDeviceCapabilities\n");
	
	pCommandInterface = new AVCDeviceCommandInterface(this);
	if (!pCommandInterface)
		return kIOReturnNoMemory;

	// Attempt to create a command interface for the device
	result = pCommandInterface->activateAVCDeviceCommandInterface(NULL, NULL);
	if (result != kIOReturnSuccess)
	{
		//printf("AY_DEBUG: Error Creating Command Interface for AVCDevice = 0x%08X\n",result);
	}
	else
	{
		if (supportsFCP == true)
		{
			// Get the isoch plug count for this device (only using the plug-info command)
			getIsochPlugInfo(pCommandInterface->avcInterface,&numInputPlugs,&numOutputPlugs);

			// Get the device's subunit list
			subUnits = getDeviceSubunits(pCommandInterface->avcInterface);
			
			// See if this device has a monitor or tuner subunit
			if (((subUnits & 0xFF000000) == 0x00000000) ||
				((subUnits & 0x00FF0000) == 0x00000000) ||
				((subUnits & 0x0000FF00) == 0x00000000) ||
				((subUnits & 0x000000FF) == 0x00000000) ||
				((subUnits & 0xFF000000) == 0x28000000) ||
				((subUnits & 0x00FF0000) == 0x00280000) ||
				((subUnits & 0x0000FF00) == 0x00002800) ||
				((subUnits & 0x000000FF) == 0x00000028))
			{
				hasMonitorOrTunerSubunit = true;
			}
			else
			{
				hasMonitorOrTunerSubunit = false;
			}
			
			// See if this device has a music subunit
			if (((subUnits & 0xFF000000) == 0x60000000) ||
				((subUnits & 0x00FF0000) == 0x00600000) ||
				((subUnits & 0x0000FF00) == 0x00006000) ||
				((subUnits & 0x000000FF) == 0x00000060))
			{
				hasMusicSubunit = true;
			}
			else
			{
				hasMusicSubunit = false;
			}
			
			// See if this device has an audio subunit
			// See if this device has a music subunit
			if (((subUnits & 0xFF000000) == 0x08000000) ||
				((subUnits & 0x00FF0000) == 0x00080000) ||
				((subUnits & 0x0000FF00) == 0x00000800) ||
				((subUnits & 0x000000FF) == 0x00000008))
			{
				hasAudioSubunit = true;
			}
			else
			{
				hasAudioSubunit = false;
			}
			
			// If this device has a tape subunit, try the tape subunit's
			// Signal-Mode command first to discover the device's stream format
			if (((subUnits & 0xFF000000) == 0x20000000) ||
				((subUnits & 0x00FF0000) == 0x00200000) ||
				((subUnits & 0x0000FF00) == 0x00002000) ||
				((subUnits & 0x000000FF) == 0x00000020))
			{
				hasTapeSubunit = true;
				
				if (isDVCPro(pCommandInterface->avcInterface,&mode) == true)
				{
					// DVCPro devices respond to the Panasonic
					// Vendor unique command, and report their
					// signal mode via output plug signal format command
					isDVCProDevice = true;
					isDVDevice = true;
					dvMode = mode;
				}
				else
				{
					result = getSignalMode(pCommandInterface->avcInterface, &mode);
					if (result == kIOReturnSuccess)
					{
						switch (mode)
						{
							case kAVCTapeSigModeHDV1_60:
							case kAVCTapeSigModeMPEG12Mbps_60:
							case kAVCTapeSigModeMPEG6Mbps_60:
							case kAVCTapeSigModeHDV1_50:
							case kAVCTapeSigModeMPEG12Mbps_50:
							case kAVCTapeSigModeMPEG6Mbps_50:
							case kAVCTapeSigModeDVHS:
							case kAVCTapeSigModeMicroMV12Mbps_60:
							case kAVCTapeSigModeMicroMV6Mbps_60:
							case kAVCTapeSigModeMicroMV12Mbps_50:
							case kAVCTapeSigModeMicroMV6Mbps_50:
							case kAVCTapeSigModeHDV2_60:
							case kAVCTapeSigModeHDV2_50:
								isDVDevice = false;
								isMPEGDevice = true;
								mpegMode = mode;
								break;
								
							case kAVCTapeSigModeSD525_60:
							case kAVCTapeSigModeSDL525_60:
							case kAVCTapeSigModeHD1125_60:
							case kAVCTapeSigModeSD625_50:
							case kAVCTapeSigModeSDL625_50:
							case kAVCTapeSigModeHD1250_50:
							case kAVCTapeSigModeDVCPro25_625_50:
							case kAVCTapeSigModeDVCPro25_525_60:
							case kAVCTapeSigModeDVCPro50_625_50:
							case kAVCTapeSigModeDVCPro50_525_60:
							case kAVCTapeSigModeDVCPro100_50:
							case kAVCTapeSigModeDVCPro100_60:
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = mode;
								break;
								
								// Older digital-8 camcorders may report 
								// these modes instead of standard NTSC DV25
							case kAVCTapeSigMode8mmNTSC:
							case kAVCTapeSigModeHi8NTSC:
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = kAVCTapeSigModeSD525_60;
								break;
								
								// Older digital-8 camcorders may report 
								// these modes instead of standard PAL DV25
							case kAVCTapeSigMode8mmPAL:	
							case kAVCTapeSigModeHi8PAL:	
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = kAVCTapeSigModeSD625_50;
								break;
								
							case kAVCTapeSigModeVHSNTSC:
							case kAVCTapeSigModeVHSMPAL:
							case kAVCTapeSigModeVHSPAL:	
							case kAVCTapeSigModeVHSNPAL:
							case kAVCTapeSigModeVHSSECAM:
							case kAVCTapeSigModeVHSMESECAM:
							case kAVCTapeSigModeSVHS525_60:	
							case kAVCTapeSigModeSVHS625_50:	
							case kAVCTapeSigModeAudido:	
							default:
								isMPEGDevice = false;
								isDVDevice = false;
								dvMode = 0xFF;
								mpegMode = 0xFF;
						}
					}
					else
					{
						// Couldn't determine stream format via tape subunit SignalMode command.
						// Try output plug signal format command
						result = outputPlugSignalFormat(pCommandInterface->avcInterface, 0, &signalFormat);
						if (result == kIOReturnSuccess)
						{
							if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatMPEGTS)
							{
								isDVDevice = false;
								isMPEGDevice = true;
								mpegMode = kAVCTapeSigModeDVHS;	// Here, default to D-VHS MPEG
							}
							else if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatNTSCDV)
							{
								isMPEGDevice = false;
								isDVDevice = true;
								dvMode = ((signalFormat & 0x00FF0000) >> 16);
							}
						}
					}
				}
			}
			else
			{
				// Device doesn't have a tape subunit
				hasTapeSubunit = false;
				
				// Use output plug signal format command
				result = outputPlugSignalFormat(pCommandInterface->avcInterface, 0, &signalFormat);
				if (result == kIOReturnSuccess)
				{
					if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatMPEGTS)
					{
						isDVDevice = false;
						isMPEGDevice = true;
						mpegMode = kAVCTapeSigModeDVHS;	// Here, default to D-VHS MPEG
					}
					else if ((signalFormat & 0xFF000000) == kAVCPlugSignalFormatNTSCDV)
					{
						isMPEGDevice = false;
						isDVDevice = true;
						dvMode = ((signalFormat & 0x00FF0000) >> 16);
					}
				}
			}
		}
		else
		{
			// Device doesn't support FCP. Assume it's a
			// NTSC DV media converter
			isMPEGDevice = false;
			isDVDevice = true;
			dvMode = kAVCTapeSigModeSD525_60;
		}
		
		capabilitiesDiscovered = true;
		
		// Deactivate and destroy the Command Interface
		pCommandInterface->deactivateAVCDeviceCommandInterface();
		delete pCommandInterface;
	}
	
	return result;
#endif	
}

////////////////////////////////////////////////////
// CreateMPEGTransmitterForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateMPEGTransmitterForDevicePlug(UInt8 deviceInputPlugNum,
													DataPullProc dataPullProcHandler,
													void *pDataPullProcRefCon,
													MPEG2TransmitterMessageProc messageProcHandler,
													void *pMessageProcRefCon,
													StringLogger *stringLogger,
													unsigned int cyclesPerSegment,
													unsigned int numSegments,
													unsigned int packetsPerCycle,
													unsigned int tsPacketQueueSizeInPackets)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;

	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;

	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;
	
	bzero(pDeviceStream, sizeof(AVCDeviceStream));
	
	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeMPEGTransmitter;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceInputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->packetsPerCycle = packetsPerCycle;
	pDeviceStream->mpegTransmitterMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->mpegDataPullProc = dataPullProcHandler;
	pDeviceStream->pDataProcRefCon = pDataPullProcRefCon;
	pDeviceStream->tsPacketQueueSizeInPackets = tsPacketQueueSizeInPackets;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;
	
	// Read the device's iMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000980;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);
	
	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}

	// Throttle the MPEG transmitter's isoch speed to a maximum of S200 for HDV1
	// devices, because of an problem we've seen with some HDV1 devices when 
	// receiving S400 isoch packets from some Macs.
	if ((pDeviceStream->isochSpeed == kFWSpeed400MBit) && ((mpegMode == kAVCTapeSigModeHDV1_60) || (mpegMode == kAVCTapeSigModeHDV1_50)))
		pDeviceStream->isochSpeed = kFWSpeed200MBit;	
	
	// Create the MPEG Transmitter thread and object
	result = CreateMPEG2Transmitter(&pDeviceStream->pMPEGTransmitter,
								 pDeviceStream->mpegDataPullProc,
								 pDeviceStream->pDataProcRefCon,
								 AVCDeviceStreamMessageProc,
								 pDeviceStream,
								 pDeviceStream->stringLogger,
								 deviceInterface,
								 pDeviceStream->cyclesPerSegment,
								 pDeviceStream->numSegments,
								 pDeviceStream->doP2PConnection,
								 pDeviceStream->packetsPerCycle,
								 pDeviceStream->tsPacketQueueSizeInPackets);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);
		
		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;

		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
	
	return pDeviceStream;
}

////////////////////////////////////////////////////
// CreateMPEGReceiverForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateMPEGReceiverForDevicePlug(UInt8 deviceOutputPlugNum,
												 DataPushProc dataPushProcHandler,
												 void *pDataPushProcRefCon,
												 MPEG2ReceiverMessageProc messageProcHandler,
												 void *pMessageProcRefCon,
												 StringLogger *stringLogger,
												 unsigned int cyclesPerSegment,
												 unsigned int numSegments)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;
	
	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;
	
	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;
	
	bzero(pDeviceStream, sizeof(AVCDeviceStream));

	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeMPEGReceiver;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceOutputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->mpegReceiverMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->mpegDataPushProc = dataPushProcHandler;
	pDeviceStream->pDataProcRefCon = pDataPushProcRefCon;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;

	// Read the device's oMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000900;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);

	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}
	
	// Figure out if the device is already tranmitting, in which case use 
	// that channel and don't allocate bandwidth
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000904;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);

	if ((result == kIOReturnSuccess) && (plugVal & (kIOFWPCRBroadcast | kIOFWPCRP2PCount)))
	{
		pDeviceStream->isochChannel = (plugVal & kIOFWPCRChannel)>>kIOFWPCRChannelPhase;
		pDeviceStream->doP2PConnection = false;
	}
	
	// Create the MPEG Receiver thread and object
	result = CreateMPEG2Receiver(&pDeviceStream->pMPEGReceiver,
							  pDeviceStream->mpegDataPushProc,
							  pDeviceStream->pDataProcRefCon,
							  AVCDeviceStreamMessageProc,
							  pDeviceStream,
							  pDeviceStream->stringLogger,
							  deviceInterface,
							  pDeviceStream->cyclesPerSegment,
							  pDeviceStream->numSegments,
							  pDeviceStream->doP2PConnection);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);

		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;

		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
	
	return pDeviceStream;
}

////////////////////////////////////////////////////
// CreateDVTransmitterForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateDVTransmitterForDevicePlug(UInt8 deviceInputPlugNum,
												  DVFramePullProc framePullProcHandler,
												  void *pFramePullProcRefCon,
												  DVFrameReleaseProc frameReleaseProcHandler,
												  void *pFrameReleaseProcRefCon,
												  DVTransmitterMessageProc messageProcHandler,
												  void *pMessageProcRefCon,
												  StringLogger *stringLogger,
												  unsigned int cyclesPerSegment,
												  unsigned int numSegments,
												  UInt8 transmitterDVMode,
												  UInt32 numFrameBuffers)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;
	
	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;
	
	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;
	
	bzero(pDeviceStream, sizeof(AVCDeviceStream));

	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeDVTransmitter;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceInputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->numFrameBuffers = numFrameBuffers;
	pDeviceStream->dvMode = transmitterDVMode;
	pDeviceStream->dvTransmitterMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->dvFramePullProc = framePullProcHandler;
	pDeviceStream->pDataProcRefCon = pFramePullProcRefCon;
	pDeviceStream->dvFrameReleaseProc = frameReleaseProcHandler;
	pDeviceStream->pDVFrameReleaseProcRefCon = pFrameReleaseProcRefCon;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;

	// Read the device's iMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000980;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);

	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}
	
	// Create the DV Transmitter thread and object
	result = CreateDVTransmitter(&pDeviceStream->pDVTransmitter,
							  pDeviceStream->dvFramePullProc,
							  pDeviceStream->pDataProcRefCon,
							  pDeviceStream->dvFrameReleaseProc,
							  pDeviceStream->pDVFrameReleaseProcRefCon,
							  AVCDeviceStreamMessageProc,
							  pDeviceStream,
							  pDeviceStream->stringLogger,
							  deviceInterface,
							  pDeviceStream->cyclesPerSegment,
							  pDeviceStream->numSegments,
							  pDeviceStream->dvMode,
							  pDeviceStream->numFrameBuffers,
							  pDeviceStream->doP2PConnection);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);

		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;

		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
		
	return pDeviceStream;
}

////////////////////////////////////////////////////
// CreateDVReceiverForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateDVReceiverForDevicePlug(UInt8 deviceOutputPlugNum,
											   DVFrameReceivedProc frameReceivedProcHandler,
											   void *pFrameReceivedProcRefCon,
											   DVReceiverMessageProc messageProcHandler,
											   void *pMessageProcRefCon,
											   StringLogger *stringLogger,
											   unsigned int cyclesPerSegment,
											   unsigned int numSegments,
											   UInt8 receiverDVMode,
											   UInt32 numFrameBuffers)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;
	
	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;
	
	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;
	
	bzero(pDeviceStream, sizeof(AVCDeviceStream));
	
	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeDVReceiver;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceOutputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->numFrameBuffers = numFrameBuffers;
	pDeviceStream->dvMode = receiverDVMode;
	pDeviceStream->dvReceiverMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->dvFrameReceivedProc = frameReceivedProcHandler;
	pDeviceStream->pDataProcRefCon = pFrameReceivedProcRefCon;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;

	// Read the device's oMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000900;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);

	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}
	
	// Figure out if the device is already tranmitting, in which case use 
	// that channel and don't allocate bandwidth
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000904;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);
	
	if ((result == kIOReturnSuccess) && (plugVal & (kIOFWPCRBroadcast | kIOFWPCRP2PCount)))
	{
		pDeviceStream->isochChannel = (plugVal & kIOFWPCRChannel)>>kIOFWPCRChannelPhase;
		pDeviceStream->doP2PConnection = false;
	}
	
	// Create the DV Receiver thread and object
	result = CreateDVReceiver(&pDeviceStream->pDVReceiver,
						   pDeviceStream->dvFrameReceivedProc,
						   pDeviceStream->pDataProcRefCon,
						   AVCDeviceStreamMessageProc,
						   pDeviceStream,
						   pDeviceStream->stringLogger,
						   deviceInterface,
						   pDeviceStream->cyclesPerSegment,
						   pDeviceStream->numSegments,
						   pDeviceStream->dvMode,
						   pDeviceStream->numFrameBuffers,
						   pDeviceStream->doP2PConnection);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);

		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;

		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
	
	return pDeviceStream;
}

////////////////////////////////////////////////////
// CreateUniversalReceiverForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateUniversalReceiverForDevicePlug(UInt8 deviceOutputPlugNum,
																 UniversalDataPushProc dataPushProcHandler,
																 void *pDataPushProcRefCon,
																 UniversalReceiverMessageProc messageProcHandler,
																 void *pMessageProcRefCon,
																 StringLogger *stringLogger,
																 unsigned int cyclesPerSegment,
																 unsigned int numSegments,
																 unsigned int cycleBufferSize)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;
	
	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;
	
	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;

	bzero(pDeviceStream, sizeof(AVCDeviceStream));

	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeUniversalReceiver;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceOutputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->universalReceiverMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->universalDataPushProc = dataPushProcHandler;
	pDeviceStream->pDataProcRefCon = pDataPushProcRefCon;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;
	pDeviceStream->cycleBufferSize = cycleBufferSize;
	pDeviceStream->irmAllocationPacketLen = cycleBufferSize;
	
	// Read the device's oMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000900;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);
	
	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}
	
	// Figure out if the device is already tranmitting, in which case use 
	// that channel and don't allocate bandwidth
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000904;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);
	
	if (result == kIOReturnSuccess)
	{
		if (plugVal & (kIOFWPCRBroadcast | kIOFWPCRP2PCount))
		{
			pDeviceStream->isochChannel = (plugVal & kIOFWPCRChannel)>>kIOFWPCRChannelPhase;
			pDeviceStream->doP2PConnection = false;
		}
		
		// Determine how much IRM to allocate based on oPCR payload-size parameter! 
		// (if we're doing the allocation).
		pDeviceStream->irmAllocationPacketLen = ((plugVal & 0x000003FF) * 4);
	}
	
	// Create the Universal Receiver thread and object
	result = CreateUniversalReceiver(&pDeviceStream->pUniversalReceiver,
									 pDeviceStream->universalDataPushProc,
									 pDeviceStream->pDataProcRefCon,
									 AVCDeviceStreamMessageProc,
									 pDeviceStream,
									 pDeviceStream->stringLogger,
									 deviceInterface,
									 pDeviceStream->cyclesPerSegment,
									 pDeviceStream->numSegments,
									 pDeviceStream->cycleBufferSize,
									 pDeviceStream->doP2PConnection,
									 pDeviceStream->irmAllocationPacketLen);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);
		
		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;
		
		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
	
	return pDeviceStream;
}

////////////////////////////////////////////////////
// CreateUniversalTransmitterForDevicePlug
////////////////////////////////////////////////////
AVCDeviceStream* AVCDevice::CreateUniversalTransmitterForDevicePlug(UInt8 deviceInputPlugNum,
																	UniversalTransmitterDataPullProc dataPullProcHandler,
																	void *pDataPullProcRefCon,
																	UniversalTransmitterMessageProc messageProcHandler,
																	void *pMessageProcRefCon,
																	StringLogger *stringLogger,
																	unsigned int cyclesPerSegment,
																	unsigned int numSegments,
																	unsigned int clientTransmitBufferSize,
																	unsigned int irmAllocationMaxPacketSize)
{
	IOReturn result;
	AVCDeviceStream *pDeviceStream;
	UInt32 plugVal;
	io_object_t obj;
	FWAddress addr;
	
	// First, make sure that the AVCDevice is open.
	if (!avcInterface)
		return nil;
	
	// Create a new device stream object
	pDeviceStream = new AVCDeviceStream;
	if (!pDeviceStream)
		return nil;
	
	bzero(pDeviceStream, sizeof(AVCDeviceStream));
	
	// Initialize some parameters of the device stream struct
	pDeviceStream->pNext = nil;
	pDeviceStream->streamType = kStreamTypeUniversalTransmitter;
	pDeviceStream->pAVCDevice = this;
	pDeviceStream->devicePlugNum = deviceInputPlugNum;
	pDeviceStream->stringLogger = stringLogger;
	pDeviceStream->cyclesPerSegment = cyclesPerSegment;
	pDeviceStream->numSegments = numSegments;
	pDeviceStream->universalTransmitterMessageProc = messageProcHandler;
	pDeviceStream->pMessageProcRefCon = pMessageProcRefCon;
	pDeviceStream->universalDataPullProc = dataPullProcHandler;
	pDeviceStream->pDataProcRefCon = pDataPullProcRefCon;
	pDeviceStream->clientTransmitBufferSize = clientTransmitBufferSize; 
	pDeviceStream->irmAllocationMaxPacketSize = irmAllocationMaxPacketSize;
	pDeviceStream->isochChannel = kAnyAvailableIsochChannel;
	pDeviceStream->doP2PConnection = true;
	pDeviceStream->isochSpeed = kFWSpeed100MBit;
	pDeviceStream->plugConnected = false;
	
	// Read the device's iMPR to determine speed to node
	// TODO: Would it be better to use the IOFireWireFamily's "device to node" function?
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = 0xf0000980;
	obj = (*deviceInterface)->GetDevice(deviceInterface);
	result = (*deviceInterface)->ReadQuadlet(deviceInterface, obj, &addr, &plugVal, false, 0);
	
	// Handle endianess issues with the read-quadlet payload
	plugVal = EndianU32_BtoN(plugVal);
	
	if (result == kIOReturnSuccess)
	{
		pDeviceStream->isochSpeed = (IOFWSpeed) ((plugVal & 0xC0000000) >> 30);
	}
	
	// Create the Universal Transmitter thread and object
	result = CreateUniversalTransmitter(&pDeviceStream->pUniversalTransmitter,
										pDeviceStream->universalDataPullProc,
										pDeviceStream->pDataProcRefCon,
										AVCDeviceStreamMessageProc,
										pDeviceStream,
										pDeviceStream->stringLogger,
										deviceInterface,
										pDeviceStream->cyclesPerSegment,
										pDeviceStream->numSegments,
										pDeviceStream->clientTransmitBufferSize,
										pDeviceStream->doP2PConnection,
										pDeviceStream->irmAllocationMaxPacketSize);
	if (result != kIOReturnSuccess)
	{
		delete pDeviceStream;
		pDeviceStream = nil;
	}
	else
	{
		// Lock the stream queue protect mutex
		pthread_mutex_lock(&deviceStreamQueueMutex);
		
		// Add this stream to the device's linked list of streams
		pDeviceStream->pNext = pAVCDeviceStreamQueueHead;
		pAVCDeviceStreamQueueHead = pDeviceStream;
		
		// Unlock the stream queue protect mutex
		pthread_mutex_unlock(&deviceStreamQueueMutex);
	}
	
	return pDeviceStream;
}

////////////////////////////////////////////////////
// StartAVCDeviceStream
////////////////////////////////////////////////////
IOReturn AVCDevice::StartAVCDeviceStream(AVCDeviceStream *pAVCDeviceStream)
{
	IOReturn result = kIOReturnSuccess;
	AVCDeviceStream *pStream;
	bool found = false;
	
	// Make sure that the passed in stream object is non-nil
	if (!pAVCDeviceStream)
		return kIOReturnBadArgument;
	
	// Lock the stream queue protect mutex
	pthread_mutex_lock(&deviceStreamQueueMutex);

	// Look for this stream object in the queue
	if (pAVCDeviceStreamQueueHead == pAVCDeviceStream)
	{
		found = true;
	}
	else
	{
		pStream = pAVCDeviceStreamQueueHead;
		while (pStream != nil)
		{
			if (pStream->pNext == pAVCDeviceStream)
			{
				found = true;
				break;
			}
			else
				pStream = pStream->pNext;
		}
	}
	
	if (found == true)
	{
		// Start the stream object
		switch (pAVCDeviceStream->streamType)
		{
			case kStreamTypeDVTransmitter:
				pAVCDeviceStream->pDVTransmitter->setTransmitIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pDVTransmitter->setTransmitIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pDVTransmitter->startTransmit();
				break;
				
			case kStreamTypeDVReceiver:
				pAVCDeviceStream->pDVReceiver->setReceiveIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pDVReceiver->setReceiveIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pDVReceiver->startReceive();
				break;
				
			case kStreamTypeMPEGTransmitter:
				pAVCDeviceStream->pMPEGTransmitter->setTransmitIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pMPEGTransmitter->setTransmitIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pMPEGTransmitter->startTransmit();
				break;
				
			case kStreamTypeMPEGReceiver:
				pAVCDeviceStream->pMPEGReceiver->setReceiveIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pMPEGReceiver->setReceiveIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pMPEGReceiver->startReceive();
				break;
				
			case kStreamTypeUniversalReceiver:	
				pAVCDeviceStream->pUniversalReceiver->setReceiveIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pUniversalReceiver->setReceiveIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pUniversalReceiver->startReceive();
				break;
				
			case kStreamTypeUniversalTransmitter:
				pAVCDeviceStream->pUniversalTransmitter->setTransmitIsochChannel(pAVCDeviceStream->isochChannel);
				pAVCDeviceStream->pUniversalTransmitter->setTransmitIsochSpeed(pAVCDeviceStream->isochSpeed);
				result = pAVCDeviceStream->pUniversalTransmitter->startTransmit();
				break;
				
			default:
				result = kIOReturnError;
				break;
		};
	}
	else
		result = kIOReturnBadArgument;

	// Unlock the stream queue protect mutex
	pthread_mutex_unlock(&deviceStreamQueueMutex);
	
	return result;
}

////////////////////////////////////////////////////
// StopAVCDeviceStream
////////////////////////////////////////////////////
IOReturn AVCDevice::StopAVCDeviceStream(AVCDeviceStream *pAVCDeviceStream)
{
	IOReturn result = kIOReturnSuccess;
	AVCDeviceStream *pStream;
	bool found = false;
	
	// Make sure that the passed in stream object is non-nil
	if (!pAVCDeviceStream)
		return kIOReturnBadArgument;
	
	// Lock the stream queue protect mutex
	pthread_mutex_lock(&deviceStreamQueueMutex);

	// Look for this stream object in the queue
	if (pAVCDeviceStreamQueueHead == pAVCDeviceStream)
	{
		found = true;
	}
	else
	{
		pStream = pAVCDeviceStreamQueueHead;
		while (pStream != nil)
		{
			if (pStream->pNext == pAVCDeviceStream)
			{
				found = true;
				break;
			}
			else
				pStream = pStream->pNext;
		}
	}
	
	if (found == true)
	{
		// Stop the stream object
		switch (pAVCDeviceStream->streamType)
		{
			case kStreamTypeDVTransmitter:
				result = pAVCDeviceStream->pDVTransmitter->stopTransmit();
				break;
				
			case kStreamTypeDVReceiver:
				result = pAVCDeviceStream->pDVReceiver->stopReceive();
				break;
				
			case kStreamTypeMPEGTransmitter:
				result = pAVCDeviceStream->pMPEGTransmitter->stopTransmit();
				break;
				
			case kStreamTypeMPEGReceiver:
				result = pAVCDeviceStream->pMPEGReceiver->stopReceive();
				break;
				
			case kStreamTypeUniversalReceiver:	
				result = pAVCDeviceStream->pUniversalReceiver->stopReceive();
				break;
	
			case kStreamTypeUniversalTransmitter:
				result = pAVCDeviceStream->pUniversalTransmitter->stopTransmit();
				break;

			default:
				result = kIOReturnError;
				break;
		};
	}
	else
		result = kIOReturnBadArgument;
	
	// Unlock the stream queue protect mutex
	pthread_mutex_unlock(&deviceStreamQueueMutex);
	
	return result;
}

////////////////////////////////////////////////////
// DestroyAVCDeviceStream
////////////////////////////////////////////////////
IOReturn AVCDevice::DestroyAVCDeviceStream(AVCDeviceStream *pAVCDeviceStream)
{
	AVCDeviceStream *pStream;
	bool found = false;
	IOReturn result = kIOReturnSuccess;
	
	// Make sure that the passed in stream object is non-nil
	if (!pAVCDeviceStream)
		return kIOReturnBadArgument;
	
	// Lock the stream queue protect mutex
	pthread_mutex_lock(&deviceStreamQueueMutex);
	
	// Look for this stream object in the queue, and remove it
	if (pAVCDeviceStreamQueueHead == pAVCDeviceStream)
	{
		pAVCDeviceStreamQueueHead = pAVCDeviceStream->pNext;
		found = true;
	}
	else
	{
		pStream = pAVCDeviceStreamQueueHead;
		while (pStream != nil)
		{
			if (pStream->pNext == pAVCDeviceStream)
			{
				pStream->pNext = pAVCDeviceStream->pNext;
				found = true;
				break;
			}
			else
				pStream = pStream->pNext;
		}
	}
	
	if (found == true)
	{
		// Tear down the stream object
		switch (pAVCDeviceStream->streamType)
		{
			case kStreamTypeDVTransmitter:
				DestroyDVTransmitter(pAVCDeviceStream->pDVTransmitter);
				break;
			
			case kStreamTypeDVReceiver:
				DestroyDVReceiver(pAVCDeviceStream->pDVReceiver);
				break;

			case kStreamTypeMPEGTransmitter:
				DestroyMPEG2Transmitter(pAVCDeviceStream->pMPEGTransmitter);
				break;

			case kStreamTypeMPEGReceiver:
				DestroyMPEG2Receiver(pAVCDeviceStream->pMPEGReceiver);
				break;

			case kStreamTypeUniversalReceiver:	
				DestroyUniversalReceiver(pAVCDeviceStream->pUniversalReceiver);
				break;
				
			case kStreamTypeUniversalTransmitter:
				DestroyUniversalTransmitter(pAVCDeviceStream->pUniversalTransmitter);
				break;

			default:
				break;
		};

		delete pAVCDeviceStream;

		result = kIOReturnSuccess;
	}
	else
		result = kIOReturnBadArgument;

	// Unlock the stream queue protect mutex
	pthread_mutex_unlock(&deviceStreamQueueMutex);
	
	return result;
}

////////////////////////////////////////////////////
// createAVCUnitInterface
////////////////////////////////////////////////////
IOReturn AVCDevice::createAVCUnitInterface(void)
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
		
		// First, try to get the v2 interface (the interface without the 8 mSec penalty per AVC command)
		// Then, if we fail, try to get the original interface (so we still can work with Jaguar systems)
		comErr = (*theCFPlugInInterface)->QueryInterface(
												   theCFPlugInInterface,
												   CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID_v2),
												   (void**) &avcInterface);
		if (comErr != S_OK)
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

		// Open the interface
		if (!result)
		{
			result = (*avcInterface)->open(avcInterface);
			if (result)
			{
				(*avcInterface)->Release(avcInterface);
				avcInterface = 0;
			}
		}
	}

    return result;
}

////////////////////////////////////////////////////
// releaseAVCUnitInterface
////////////////////////////////////////////////////
IOReturn AVCDevice::releaseAVCUnitInterface(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;

	if (avcInterface)
	{
		// Remove callback dispatcher from run loop
		(*avcInterface)->removeCallbackDispatcherFromRunLoop(avcInterface);

		// Close and release interface
		(*avcInterface)->close(avcInterface) ;
		(*avcInterface)->Release(avcInterface) ;
	}

	avcInterface = 0;

	return result;
}

////////////////////////////////////////////////////
// createFWDeviceInterface
////////////////////////////////////////////////////
IOReturn AVCDevice::createFWDeviceInterface(void)
{
	// Local Vars
	IOCFPlugInInterface **theCFPlugInInterface;
	SInt32 theScore;
	IOReturn result = kIOReturnSuccess;

	result = IOCreatePlugInInterfaceForService(
											fwDevice,
											kIOFireWireLibTypeID,
											kIOCFPlugInInterfaceID,		//interfaceType,
											&theCFPlugInInterface,
											&theScore);
	if (!result)
	{
		HRESULT comErr;
		comErr = (*theCFPlugInInterface)->QueryInterface(
												   theCFPlugInInterface,
												   CFUUIDGetUUIDBytes( kIOFireWireNubInterfaceID ),
												   (void**) &deviceInterface);
		if (comErr == S_OK)
		{
			result = (*deviceInterface)->AddCallbackDispatcherToRunLoop(deviceInterface, pAVCDeviceController->runLoopRef );
		}
		else
			result = comErr;

		(*theCFPlugInInterface)->Release(theCFPlugInInterface);	// Leave just one reference.

		// Open the interface
		if (!result)
		{
			// If the avc interface is already open, use it's session ref to open the device interface
			if (avcInterface)
				result = (*deviceInterface)->OpenWithSessionRef(deviceInterface,
													(*avcInterface)->getSessionRef(avcInterface));
			else
				result = (*deviceInterface)->Open(deviceInterface);
			if (result)
			{
				(*deviceInterface)->Release(deviceInterface) ;
				deviceInterface = 0;
			}
		}
	}

    return result;
}

////////////////////////////////////////////////////
// releaseFWDeviceInterface
////////////////////////////////////////////////////
IOReturn AVCDevice::releaseFWDeviceInterface(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;

	if (deviceInterface)
	{
		// Remove callback dispatcher from run loop
		(*deviceInterface)->RemoveCallbackDispatcherFromRunLoop(deviceInterface);

		// Close and release interface
		(*deviceInterface)->Close(deviceInterface) ;
		(*deviceInterface)->Release(deviceInterface) ;
	}

	deviceInterface = 0;

	return result;
}

////////////////////////////////////////////////////
// AVCCommand
////////////////////////////////////////////////////
IOReturn AVCDevice::AVCCommand(const UInt8 *command, UInt32 cmdLen, UInt8 *response, UInt32 *responseLen)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;

	if (avcInterface)
		result = (*avcInterface)->AVCCommand(avcInterface, command, cmdLen, response, responseLen);
	else
		result = kIOReturnNotOpen;

    return result;
}

////////////////////////////////////////////////////
// GetPowerState
////////////////////////////////////////////////////
IOReturn AVCDevice::GetPowerState(UInt8 *pPowerState)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = 0xFF;
	cmd[2] = 0xB2; // Power Opcode
	cmd[3] = 0x7F;
	size = 4;
	res = AVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus)))
	{
		res = kIOReturnError;
	}
	else
	{
		*pPowerState = response[3];
	}
	
	return res;
}

////////////////////////////////////////////////////
// SetPowerState
////////////////////////////////////////////////////
IOReturn AVCDevice::SetPowerState(UInt8 powerState)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = 0xFF;
	cmd[2] = 0xB2; // Power Opcode
	cmd[3] = powerState;
	size = 4;
	res = AVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

////////////////////////////////////////////////////
// GetCurrentNodeID
////////////////////////////////////////////////////
IOReturn AVCDevice::GetCurrentNodeID(UInt16 *pNodeID)
{
	IOReturn result;
	CFMutableDictionaryRef properties;
	CFNumberRef cfNodeID;
	UInt16 nodeID;
	UInt32 retryCnt = 0;

	while (1)
	{
		// Get the node ID for this device from the IO Registry
		result = IORegistryEntryCreateCFProperties(fwDevice, &properties, kCFAllocatorDefault, kNilOptions);
		if (result == kIOReturnSuccess)
		{
			cfNodeID = (CFNumberRef)CFDictionaryGetValue(properties, CFSTR("FireWire Node ID"));
			if(cfNodeID)
			{
				CFNumberGetValue(cfNodeID, kCFNumberShortType, &nodeID);
				*pNodeID = nodeID; 
			}
			else
				result = kIOReturnError;
			
			// Release the properties CF dictionary object
			CFRelease(properties);
		}
		
		// If we got an error, then break out of the endless loop
		if (result != kIOReturnSuccess)
			break;
		
		// If the nodeID is not 0xFFFF, then break out of the endless loop
		if (nodeID != 0xFFFF)
			break;
		
		// We didn't get an error, but we did get a 0xFFFF node ID, retry up to 5 times
		if (++retryCnt > 4)
			break;
	}

	return result;
}

////////////////////////////////////////////////////
// SetClientPrivateData
////////////////////////////////////////////////////
void AVCDevice::SetClientPrivateData(void *pClientData)
{
	pClientPrivateData = pClientData;
}

////////////////////////////////////////////////////
// GetClientPrivateData
////////////////////////////////////////////////////
void *AVCDevice::GetClientPrivateData(void)
{
	return pClientPrivateData;
}

////////////////////////////////////////////////////
// getIsochPlugCount
////////////////////////////////////////////////////
IOReturn AVCDevice::getIsochPlugCount(void)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;
    UInt32 size;
    UInt8 cmd[8],response[8];
	FWAddress addr;
	UInt32 readVal;

	if (avcInterface)
	{
		// build AVC command command
		cmd[0] = kAVCStatusInquiryCommand;
		cmd[1] = kAVCUnitAddress;
		cmd[2] = kAVCPlugInfoOpcode;
		cmd[3] = 0x00;
		cmd[4] = 0xFF;
		cmd[5] = 0xFF;
		cmd[6] = 0xFF;
		cmd[7] = 0xFF;

		size = 8;
		result = (*avcInterface)->AVCCommand(avcInterface, cmd, 8, response, &size);
		if(result == kIOReturnSuccess)
		{
			// Check response type
			if (response[0] == kAVCImplementedStatus)
			{
				numInputPlugs = response[4];
				numOutputPlugs = response[5];
				return result;
			}
			else
				result = kIOReturnIOError;
		}

		// We're still here, so we need to read oMPR/iMPR
		if (deviceInterface)
		{
			// Read oMPR
			addr.nodeID = 0;	// Unused
			addr.addressHi = 0xFFFF;
			addr.addressLo = 0xF0000900;
			result = (*deviceInterface)->ReadQuadlet(deviceInterface,fwDevice,&addr,
											&readVal,false,0);
											
			// Handle endianess issues with the read-quadlet payload
			readVal = EndianU32_BtoN(readVal);
											
			if (!result)
				numOutputPlugs = readVal & 0x0000001F;

			// Read iMPR
			addr.addressHi = 0xFFFF;
			addr.addressLo = 0xF0000980;
			result = (*deviceInterface)->ReadQuadlet(deviceInterface,fwDevice,&addr,
											&readVal,false,0);
			
			// Handle endianess issues with the read-quadlet payload
			readVal = EndianU32_BtoN(readVal);
											
			if (!result)
				numInputPlugs = readVal & 0x0000001F;
		}
		else
			result = kIOReturnNotOpen;
	}
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
	AVCDevice *pAVCDevice = (AVCDevice*) refcon;
	AVCDeviceStream *pStream;
	UInt32 plugAddr;

	// Handle Specific Messages, Ignore the rest
	switch(messageType)
	{
		case kIOMessageServiceIsTerminated:
			pAVCDevice->isAttached = false;
			break;

		case kIOMessageServiceWasClosed:
			if (pAVCDevice->capabilitiesDiscovered == false)
			{
				// Try now to discover the device's capabilities
				pAVCDevice->discoverAVCDeviceCapabilities();

				// If we now have succeeed in discovering the device's capabilites, it's time to notify the client.
				if ((pAVCDevice->pAVCDeviceController->clientCallback != nil) && (pAVCDevice->capabilitiesDiscovered == true))
					pAVCDevice->pAVCDeviceController->clientCallback(pAVCDevice->pAVCDeviceController,
																  pAVCDevice->pAVCDeviceController->pClientCallbackRefCon, 
																  pAVCDevice);
			}
			break;

		case kIOMessageServiceIsResumed:
			// Lock the stream queue protect mutex
			pthread_mutex_lock(&pAVCDevice->deviceStreamQueueMutex);

			// Restore any active p2p connections
			pStream = pAVCDevice->pAVCDeviceStreamQueueHead;
			while (pStream != nil)
			{
				if (pStream->plugConnected == true)
				{
					// Calculate the plug address
					plugAddr = kPCRBaseAddress+4+4*pStream->devicePlugNum;
					if ((pStream->streamType == kStreamTypeDVTransmitter) || (pStream->streamType == kStreamTypeMPEGTransmitter))
						plugAddr += 0x80;

					// Re-establish the point 2 point connection
					if (pAVCDevice->deviceInterface)
						updateP2PCount(pAVCDevice->deviceInterface, plugAddr, 1, true, pStream->isochChannel, kFWSpeedInvalid);
				}
				pStream = pStream->pNext;
			}

			// Unlock the stream queue protect mutex
			pthread_mutex_unlock(&pAVCDevice->deviceStreamQueueMutex);
			break;
			
		default:
			break;
	};

	// If needed, notify client of message
	if (pAVCDevice->clientDeviceMessageProc != nil)
		pAVCDevice->clientDeviceMessageProc(pAVCDevice, messageType, messageArgument,pAVCDevice->pClientMessageProcRefCon);
	
	// If needed, notify the AVCDeviceControllers global message notification callback
	if (pAVCDevice->pAVCDeviceController->globalAVCDeviceMessageCallback != nil)
		pAVCDevice->pAVCDeviceController->globalAVCDeviceMessageCallback(pAVCDevice, 
																		 messageType, 
																		 messageArgument,
																		 pAVCDevice->pAVCDeviceController->pClientCallbackRefCon);
}

//////////////////////////////////////////////////////
// isDVCPro
//////////////////////////////////////////////////////
static bool isDVCPro(IOFireWireAVCLibUnitInterface **avc, UInt8 *pMode)
{
    UInt32 size;
    UInt8 cmd[10],response[10];
    IOReturn res;

    // build query vender-dependent command (is DVCPro?).
    cmd[0] = kAVCStatusInquiryCommand;
    cmd[1] = kAVCUnitAddress;
    cmd[2] = kAVCVendorDependentOpcode;
    cmd[3] = 0;
    cmd[4] = 0x80;
    cmd[5] = 0x45;
    cmd[6] = 0x82;
    cmd[7] = 0x48;
    cmd[8] = 0xff;
    cmd[9] = 0xff;
    size = 10;
    res = (*avc)->AVCCommand(avc, cmd, 10, response, &size);

	// If it is DVCPro50, see if its 25 or 50
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		cmd[0] = kAVCStatusInquiryCommand;
		cmd[1] = kAVCUnitAddress;
		cmd[2] = kAVCOutputPlugSignalFormatOpcode;
		cmd[3] = 0;
		cmd[4] = 0xFF;
		cmd[5] = 0xFF;
		cmd[6] = 0xFF;
		cmd[7] = 0xFF;
		size = 8;

		res = (*avc)->AVCCommand(avc, cmd, 8, response, &size);

		if (res == kIOReturnSuccess && response[0] == kAVCImplementedStatus)
			*pMode = response[5];
		else
			*pMode = 0x00;

		return true;
	}
	else
		return false;
}

//////////////////////////////////////////////////////
// outputPlugSignalFormat
//////////////////////////////////////////////////////
static IOReturn outputPlugSignalFormat(IOFireWireAVCLibUnitInterface **avc, UInt32 plugNum, UInt32 *pSignalFormat)
{
    UInt32 size;
    UInt8 cmd[10],response[10];
    IOReturn res = kIOReturnSuccess;

	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = kAVCUnitAddress;
	cmd[2] = kAVCOutputPlugSignalFormatOpcode;
	cmd[3] = plugNum;
	cmd[4] = 0xFF;
	cmd[5] = 0xFF;
	cmd[6] = 0xFF;
	cmd[7] = 0xFF;
	size = 8;

	res = (*avc)->AVCCommand(avc, cmd, 8, response, &size);

	if (res == kIOReturnSuccess && response[0] == kAVCImplementedStatus)
	{
		*pSignalFormat = (((UInt32)response[4] <<24) +
		((UInt32)response[5] <<16) +
		((UInt32)response[6] <<8) +
		((UInt32)response[7]));
	}
	else
	{
		res = kIOReturnError;
	}

	return res;
}


//////////////////////////////////////////////////////
// getDeviceSubunits
//////////////////////////////////////////////////////
static UInt32 getDeviceSubunits(IOFireWireAVCLibUnitInterface **avc)
{
    UInt32 size;
    UInt8 cmd[8],response[8];
    IOReturn res;

    // build subunit info command.
    cmd[0] = kAVCStatusInquiryCommand;
    cmd[1] = kAVCUnitAddress;
    cmd[2] = kAVCSubunitInfoOpcode;
    cmd[3] = 0x07;
    cmd[4] = 0xFF;
    cmd[5] = 0xFF;
    cmd[6] = 0xFF;
    cmd[7] = 0xFF;
    size = 8;
    res = (*avc)->AVCCommand(avc, cmd, 8, response, &size);

	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
		return (((UInt32)response[4] << 24) + 
			((UInt32)response[5] << 16) + 
			((UInt32)response[6] << 8) + 
			(UInt32)response[7]);
	else
		return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////
// getSignalMode
//////////////////////////////////////////////////////
static IOReturn getSignalMode(IOFireWireAVCLibUnitInterface **avc, UInt8 *mode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res;
    
    // build query Output Signal Mode command
    cmd[0] = kAVCStatusInquiryCommand;
    cmd[1] = IOAVCAddress(kAVCTapeRecorder, 0);
    cmd[2] = kAVCOutputSignalModeOpcode;
    cmd[3] = kAVCSignalModeDummyOperand;
    size = 4;
    res = (*avc)->AVCCommand(avc, cmd, 4, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
        *mode =  response[3];
		return res;
	}
	else
		return kIOReturnError;
}

////////////////////////////////////////////////////
// AVCDeviceStreamMessageProc
////////////////////////////////////////////////////
static void AVCDeviceStreamMessageProc(UInt32 msg, UInt32 param1, UInt32 param2, void *pRefCon)
{
	AVCDeviceStream *pStream = (AVCDeviceStream*) pRefCon;
	AVCDevice *pAVCDevice = pStream->pAVCDevice;

	switch (pStream->streamType)
	{
		case kStreamTypeDVTransmitter:
			if ((msg == kDVTransmitterAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;

				makeP2PInputConnection(pAVCDevice->deviceInterface, 
							pStream->devicePlugNum, 
							pStream->isochChannel);
				pStream->plugConnected = true;
			}
			else if ((msg == kDVTransmitterReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2PInputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->dvTransmitterMessageProc) 
				pStream->dvTransmitterMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		case kStreamTypeDVReceiver:
			if ((msg == kDVReceiverAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;

				makeP2POutputConnection(pAVCDevice->deviceInterface, 
						   pStream->devicePlugNum, 
						   pStream->isochChannel, 
						   pStream->isochSpeed);
				pStream->plugConnected = true;
			}
			else if ((msg == kDVReceiverReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2POutputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->dvReceiverMessageProc) 
				pStream->dvReceiverMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		case kStreamTypeMPEGTransmitter:
			if ((msg == kMpeg2TransmitterAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;

				makeP2PInputConnection(pAVCDevice->deviceInterface, 
							pStream->devicePlugNum, 
						   pStream->isochChannel);
				pStream->plugConnected = true;
			}
			else if ((msg == kMpeg2TransmitterReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2PInputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->mpegTransmitterMessageProc) 
				pStream->mpegTransmitterMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		case kStreamTypeMPEGReceiver:
			if ((msg == kMpeg2ReceiverAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;
				
				makeP2POutputConnection(pAVCDevice->deviceInterface, 
						   pStream->devicePlugNum, 
						   pStream->isochChannel, 
						   pStream->isochSpeed);
				pStream->plugConnected = true;
			}
			else if ((msg == kMpeg2ReceiverReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2POutputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->mpegReceiverMessageProc) 
				pStream->mpegReceiverMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		case kStreamTypeUniversalReceiver:	
			if ((msg == kUniversalReceiverAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;
				
				makeP2POutputConnection(pAVCDevice->deviceInterface, 
										pStream->devicePlugNum, 
										pStream->isochChannel, 
										pStream->isochSpeed);
				pStream->plugConnected = true;
			}
			else if ((msg == kUniversalReceiverReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2POutputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->universalReceiverMessageProc) 
				pStream->universalReceiverMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		case kStreamTypeUniversalTransmitter:
			if ((msg == kUniversalTransmitterAllocateIsochPort) && (pStream->doP2PConnection == true))
			{
				pStream->isochSpeed = (IOFWSpeed) param1;
				pStream->isochChannel = param2;
				
				makeP2PInputConnection(pAVCDevice->deviceInterface, 
									   pStream->devicePlugNum, 
									   pStream->isochChannel);
				pStream->plugConnected = true;
			}
			else if ((msg == kUniversalTransmitterReleaseIsochPort) && (pStream->doP2PConnection == true))
			{
				breakP2PInputConnection(pAVCDevice->deviceInterface, pStream->devicePlugNum);
				pStream->plugConnected = false;
			}
			if (pStream->universalTransmitterMessageProc) 
				pStream->universalTransmitterMessageProc(msg,param1,param2,pStream->pMessageProcRefCon);
			break;
			
		default:
			break;
	};
	
	return;
}

//////////////////////////////////////////////////////
// updateP2PCount
//////////////////////////////////////////////////////
static IOReturn updateP2PCount(IOFireWireLibDeviceRef interface, UInt32 plugAddr, SInt32 inc, bool failOnBusReset, UInt32 chan, IOFWSpeed speed)
{
    UInt32 plugVal, newVal;
	UInt32 curCount;
	UInt32 curChan;
	IOFWSpeed curSpeed;
    IOReturn res;
	FWAddress addr;
    io_object_t obj;
  
	addr.nodeID = 0;
	addr.addressHi = 0xffff;
	addr.addressLo = plugAddr;
	obj = (*interface)->GetDevice(interface);
	
    for(int i=0; i<4; i++)
	{
		// Read the register
		res = (*interface)->ReadQuadlet(interface, obj, &addr, &plugVal, false, 0);
		
		// Handle endianess issues with the read-quadlet payload
		plugVal = EndianU32_BtoN(plugVal);

		if(res != kIOReturnSuccess)
			return res;
		
		// Parse current plug value
		curCount = ((plugVal & kIOFWPCRP2PCount) >> 24);
		curChan = ((plugVal & kIOFWPCRChannel) >> 16);
		curSpeed = (IOFWSpeed)((plugVal & kIOFWPCROutputDataRate) >> 14);
		newVal = plugVal;
		
		// If requested, modify channel
		if (chan != 0xFFFFFFFF)
		{
			if ((curCount != 0) && (chan != curChan))
				return kIOReturnError;
			
			newVal &= ~kIOFWPCRChannel;
			newVal |= ((chan & 0x3F) << 16);
		}
		
		// If requested, modify speed
		if (speed != kFWSpeedInvalid)
		{
			if ((curCount != 0) && (speed != curSpeed))
				return kIOReturnError;
			
			newVal &= ~kIOFWPCROutputDataRate;
			newVal |= ((speed & 0x03) << 14);
		}
		
		// Modify P2P count
		newVal &= ~kIOFWPCRP2PCount;
		if (inc > 0)
		{
			if (curCount == 0x3F)
				return kIOReturnError;
			newVal |= ((curCount+1) << 24);
		}
		else
		{
			if (curCount == 0)
				return kIOReturnError;
			newVal |= ((curCount-1) << 24);
			
			// If breaking a connection, we should clear the broadcast connection bit at the same time
			newVal &= ~kIOFWPCRBroadcast;
		}
		
		res = (*interface)->CompareSwap(interface, obj, &addr, EndianU32_NtoB(plugVal), EndianU32_NtoB(newVal), false, 0);
        if(res == kIOReturnSuccess)
            break;
    }
    return res;
}

//////////////////////////////////////////////////////
// makeConnection
//////////////////////////////////////////////////////
static IOReturn makeConnection(IOFireWireLibDeviceRef interface, UInt32 addr, UInt32 chan, IOFWSpeed speed)
{
    return updateP2PCount(interface, addr, 1, false, chan, speed);
}

//////////////////////////////////////////////////////
// breakConnection
//////////////////////////////////////////////////////
static void breakConnection(IOFireWireLibDeviceRef interface, UInt32 addr)
{
	updateP2PCount(interface, addr, -1, false, 0xFFFFFFFF, kFWSpeedInvalid);
}

//////////////////////////////////////////////////////
// makeP2PInputConnection
//////////////////////////////////////////////////////
static IOReturn makeP2PInputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo, UInt32 chan)
{
    return makeConnection(interface, kPCRBaseAddress+0x84+4*plugNo, chan, kFWSpeedInvalid);
}

//////////////////////////////////////////////////////
// breakP2PInputConnection
//////////////////////////////////////////////////////
static IOReturn breakP2PInputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo)
{
    breakConnection(interface, kPCRBaseAddress+0x84+4*plugNo);
	return kIOReturnSuccess;
}

//////////////////////////////////////////////////////
// makeP2POutputConnection
//////////////////////////////////////////////////////
static IOReturn makeP2POutputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo, UInt32 chan, IOFWSpeed speed)
{
    return makeConnection(interface, kPCRBaseAddress+4+4*plugNo, chan, speed);
}

//////////////////////////////////////////////////////
// breakP2POutputConnection
//////////////////////////////////////////////////////
static IOReturn breakP2POutputConnection(IOFireWireLibDeviceRef interface, UInt32 plugNo)
{
    breakConnection(interface, kPCRBaseAddress+4+4*plugNo);
	return kIOReturnSuccess;
}

////////////////////////////////////////////////////
// getIsochPlugInfo
////////////////////////////////////////////////////
static IOReturn getIsochPlugInfo(IOFireWireAVCLibUnitInterface **avc, UInt32 *pNumInputPlugs, UInt32 *pNumOutputPlugs)
{
	// Local Vars
	IOReturn result = kIOReturnSuccess;
    UInt32 size;
    UInt8 cmd[8],response[8];

	// Pre-initialize the num isoch plugs to 0
	*pNumInputPlugs = 0;
	*pNumOutputPlugs = 0;
	
	// build AVC command command
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = kAVCUnitAddress;
	cmd[2] = kAVCPlugInfoOpcode;
	cmd[3] = 0x00;
	cmd[4] = 0xFF;
	cmd[5] = 0xFF;
	cmd[6] = 0xFF;
	cmd[7] = 0xFF;
	
	size = 8;
	result = (*avc)->AVCCommand(avc, cmd, 8, response, &size);
	if((result == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pNumInputPlugs = response[4];
		*pNumOutputPlugs = response[5];
	}

	return result;
}

} // namespace AVS