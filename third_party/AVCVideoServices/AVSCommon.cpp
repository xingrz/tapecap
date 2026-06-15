/*
	File:		AVSCommon.cpp

 Synopsis: This is the sourcecode for the common routines used by all streaming objects


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

/////////////////////////////////////////////////////////////////////////////////////////
//
// GetFireWireLocalNodeInterface
//
// This function creates an interface for the local FireWire node user client.
//
// It returns the first local node it finds in the IORegistry.
//
/////////////////////////////////////////////////////////////////////////////////////////
IOReturn GetFireWireLocalNodeInterface(IOFireWireLibNubRef *fireWireLocalNodeInterface)
{
	IOReturn result = kIOReturnSuccess ;
    SInt32 theScore ;
    mach_port_t masterPort = 0 ;
    io_service_t theService = 0 ;
    io_iterator_t iterator	= 0 ;
    IOCFPlugInInterface **nodeCFPlugInInterface;
    IOFireWireLibNubRef nodeNubInterface;

	// Preinitialize the interface pointer to nil
	*fireWireLocalNodeInterface = nil;

	// Get the IO Kit master port
	result = IOMasterPort(MACH_PORT_NULL,&masterPort) ;
    if (result != kIOReturnSuccess)
        return result ;

    // Find the Local node in the IO registry
    result = IOServiceGetMatchingServices(masterPort,
                                          IOServiceMatching("IOFireWireLocalNode"),
                                          &iterator) ;

    // Make sure we found the local node
    if (!iterator)
        return result ;

	do
	{
		// Get the first item from the iterator
		theService = IOIteratorNext(iterator) ;
		if (!theService)
			break;
		
		// Add a user-interface plugin for the local node
		result = IOCreatePlugInInterfaceForService( theService,
													kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
													&nodeCFPlugInInterface, & theScore) ;
		if (result != kIOReturnSuccess)
			break;
		
		// Use the IUnknown interface to get the FireWireNub Interface
		// and return a pointer to it in the pointer passed into this function
		result = (*nodeCFPlugInInterface)->QueryInterface(nodeCFPlugInInterface,
														  CFUUIDGetUUIDBytes( kIOFireWireNubInterfaceID ),
														  (void**) &nodeNubInterface ) ;
		if ( result != S_OK )
			break;

	}while(0);
	
	// Destroy the nodeCFPlugInInterface
	if (nodeCFPlugInInterface)
		IODestroyPlugInInterface(nodeCFPlugInInterface) ;

	// Return a pointer to the newly created interface
	if (nodeNubInterface)
		*fireWireLocalNodeInterface = nodeNubInterface;

	// Release the iterator
	IOObjectRelease(iterator);
	
	return result;
}

/////////////////////////////////////////////////////////////////////////////////////////
//
// GetFireWireDeviceInterfaceFromExistingInterface
//
/////////////////////////////////////////////////////////////////////////////////////////
IOReturn GetFireWireDeviceInterfaceFromExistingInterface(IOFireWireLibDeviceRef existingDeviceInterface, 
														 IOFireWireLibDeviceRef *newDeviceInterface)
{
	IOReturn result = kIOReturnSuccess ;
    IOCFPlugInInterface **nodeCFPlugInInterface;
    IOFireWireLibNubRef nodeNubInterface;
    SInt32 theScore ;
	io_service_t theService = 0 ;

	// Preinitialize the interface pointer to nil
	*newDeviceInterface = nil;
	
	// Get the underlying service for the passed in device interface
	theService = (*existingDeviceInterface)->GetDevice(existingDeviceInterface);
	
    // Add a user-interface plugin for the local node
    result = IOCreatePlugInInterfaceForService( theService,
                                                kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
                                                &nodeCFPlugInInterface, & theScore) ;
    if (result != kIOReturnSuccess)
        return result ;
	
    // Use the IUnknown interface to get the FireWireNub Interface
    // and return a pointer to it in the pointer passed into this function
    result = (*nodeCFPlugInInterface)->QueryInterface(nodeCFPlugInInterface,
                                                      CFUUIDGetUUIDBytes( kIOFireWireNubInterfaceID ),
                                                      (void**) &nodeNubInterface ) ;
    if ( result != S_OK )
        return result ;
	
	// Destroy the nodeCFPlugInInterface
	IODestroyPlugInInterface(nodeCFPlugInInterface) ;
	
	// Return a pointer to the newly created interface
	*newDeviceInterface = nodeNubInterface;
	
	return result;
}


/////////////////////////////////////////////////////////////////////////////////////////
//
// GetAVCProtocolInterfaceWithAVCDevice
//
/////////////////////////////////////////////////////////////////////////////////////////
IOReturn GetAVCProtocolInterfaceWithAVCDevice(AVCDevice *pAVCDevice, 
											  IOFireWireAVCLibProtocolInterface ***avcProtocolInterface,
											  IOFireWireLibNubRef *fireWireNodeInterface)
{
	IOReturn result = kIOReturnSuccess ;
	IOFireWireAVCLibProtocolInterface **resultInterface;

	// Preinitialize the interface pointer to nil
	*avcProtocolInterface = nil;
	*fireWireNodeInterface = nil;
	
	// This can only work if the device is opened!
	if (!pAVCDevice->avcInterface)
		return kIOReturnNotOpen;

	// We get a node interface by duplicating the AVCDevice's deviceInterface
	result = GetFireWireDeviceInterfaceFromExistingInterface(pAVCDevice->deviceInterface, fireWireNodeInterface);
	if (result != kIOReturnSuccess)
		return kIOReturnError;
	
	// We get the AVC protocol interface from the AVCDevice's avcInterface 
	resultInterface = (IOFireWireAVCLibProtocolInterface**) (*(pAVCDevice->avcInterface))->getProtocolInterface(pAVCDevice->avcInterface,
																		  CFUUIDGetUUIDBytes(kIOFireWireAVCLibProtocolTypeID),
																		  CFUUIDGetUUIDBytes(kIOFireWireAVCLibProtocolInterfaceID));
	if(!resultInterface)
		result = kIOReturnError;
	else
	{
        result = (*resultInterface)->addCallbackDispatcherToRunLoop(resultInterface, CFRunLoopGetCurrent());
		if (result != kIOReturnSuccess)
		{
            (*resultInterface)->Release(resultInterface);
		}
		else
		{
			// Copy the new AVC protocol interface to the return value
			*avcProtocolInterface = resultInterface;
		}
	}

	return result;
}

/////////////////////////////////////////////////////////////////////////////////////////
//
// GetDefaultAVCProtocolInterface
//
/////////////////////////////////////////////////////////////////////////////////////////
IOReturn GetDefaultAVCProtocolInterface(IOFireWireAVCLibProtocolInterface ***avcProtocolInterface, IOFireWireLibNubRef *fireWireLocalNodeInterface)
{
	IOReturn result = kIOReturnSuccess ;
	IOFireWireAVCLibProtocolInterface **resultInterface = 0;
    IOCFPlugInInterface **nodeAVCCFPlugInInterface = 0;
	IOCFPlugInInterface **nodeCFPlugInInterface = 0;
	SInt32 theScore ;
    io_service_t theService = 0 ;
    mach_port_t masterPort = 0 ;
    io_iterator_t iterator	= 0 ;
	IOFireWireLibNubRef nodeNubInterface;
	
	// Preinitialize the interface pointer to nil
	*avcProtocolInterface = nil;
	*fireWireLocalNodeInterface = nil;
	
	// Get the IO Kit master port
	result = IOMasterPort(MACH_PORT_NULL,&masterPort) ;
    if (result != kIOReturnSuccess)
        return result ;
	
    // Find the Local node in the IO registry
    result = IOServiceGetMatchingServices(masterPort,
                                          IOServiceMatching("IOFireWireLocalNode"),
                                          &iterator) ;
	
    // Make sure we found the local node
    if (!iterator)
        return result ;

	do
	{
		theService = IOIteratorNext(iterator) ;
		if (!theService)
			break;
		
		// Add a user-interface plugin for the local node
		result = IOCreatePlugInInterfaceForService( theService,
													kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
													&nodeCFPlugInInterface, & theScore) ;
		if (result != kIOReturnSuccess)
			break;
		
		// Use the IUnknown interface to get the FireWireNub Interface
		// and return a pointer to it in the pointer passed into this function
		result = (*nodeCFPlugInInterface)->QueryInterface(nodeCFPlugInInterface,
														  CFUUIDGetUUIDBytes( kIOFireWireNubInterfaceID ),
														  (void**) &nodeNubInterface ) ;
		if ( result != S_OK )
			break;
		else
			*fireWireLocalNodeInterface = nodeNubInterface;
		
		// Add a user-interface plugin for the AVC Protocol (AVC Target services for the mac)
		result = IOCreatePlugInInterfaceForService( theService,
													kIOFireWireAVCLibProtocolTypeID, kIOCFPlugInInterfaceID,
													&nodeAVCCFPlugInInterface, & theScore) ;
		if (result != kIOReturnSuccess)
			break;
		
		// Use the IUnknown interface to get the AVC Protocol Interface
		// and return a pointer to it in the pointer passed into this function
		result = (*nodeAVCCFPlugInInterface)->QueryInterface(nodeAVCCFPlugInInterface,
															 CFUUIDGetUUIDBytes( kIOFireWireAVCLibProtocolInterfaceID ),
															 (void**) &resultInterface ) ;
		if ( result != S_OK )
			break;

	}while(0);
	
	if (result == kIOReturnSuccess)
	{
		if(!resultInterface)
			result = kIOReturnError;
		else
		{
			result = (*resultInterface)->addCallbackDispatcherToRunLoop(resultInterface, CFRunLoopGetCurrent());
			if (result != kIOReturnSuccess)
			{
				(*resultInterface)->Release(resultInterface);
			}
			else
			{
				// Copy the new AVC protocol interface to the return value
				*avcProtocolInterface = resultInterface;
			}
		}
	}
	
	// TODO
	//if (nodeAVCCFPlugInInterface)
	//	IODestroyPlugInInterface(nodeAVCCFPlugInInterface);
	
	if (nodeCFPlugInInterface)
		IODestroyPlugInInterface(nodeCFPlugInInterface) ;
	
	// Release the iterator
	IOObjectRelease(iterator);
	
	return result;
}

//////////////////////////////////////////////////////////////////////
// MakeCurrentThreadTimeContraintThread
//////////////////////////////////////////////////////////////////////
void MakeCurrentThreadTimeContraintThread(void)
{
	pthread_t currentPThread;
	
    double   mult;
    thread_time_constraint_policy_data_t constraints;
	
	currentPThread = pthread_self();
	
	// use mach_timebase_info to get abs to ns conversion parameters
	mach_timebase_info_data_t tTBI;
	mach_timebase_info(&tTBI);
	
    // Set thread to Real Time
	mult = ((double)tTBI.denom / (double)tTBI.numer) * 1000000;
	constraints.period = (uint32_t) (15*mult);
    constraints.computation = (uint32_t) (2*mult);
    constraints.constraint = (uint32_t) (30*mult);
    constraints.preemptible = TRUE;
	
	thread_policy_set(pthread_mach_thread_np(currentPThread),
					  THREAD_TIME_CONSTRAINT_POLICY,
					  (thread_policy_t)&constraints,
					  THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	
	return;
}

} // namespace AVS