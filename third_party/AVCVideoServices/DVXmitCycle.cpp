/*
	File:		DVXmitCycle.cpp

    Synopsis: This is the implementation file for the DVXmitCycle class, a support
	class for the DVTransmitter class.
 
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

namespace AVS
{

DVXmitCycle::DVXmitCycle(UInt32 bufSize,
							   UInt8 *pBuffer,
							   Boolean callbackEnabled,
							   DCLCallCommandProcPtr callBackProc,
							   IOFireWireLibDCLCommandPoolRef commandPool,
							   DCLCommandStruct *pPrevDCL,
							   DCLCommandStruct **ppLastDCL,
							   UInt32 *pTimeStamp,
							   DVTransmitter *pDVTransmitter)
{
    DCLCommandStruct *dcl;

	bufferSize = bufSize;
    pBuf = (UInt8*) pBuffer;
	pTransmitter = pDVTransmitter;

	// Create CIP Only DCL
    dcl = (*commandPool)->AllocateLabelDCL( commandPool, pPrevDCL ) ;
    pCIPOnlyXfrLabel = dcl;
	
	dcl = (*commandPool)->AllocateSendPacketStartDCL( commandPool, dcl, pBuf, kDVXmitCIPOnlySize ) ;
	
    if (callbackEnabled == true)
    {
		// Get the timestamp
		dcl = (*commandPool)->AllocatePtrTimeStampDCL(commandPool, dcl, pTimeStamp);
		timeStampUpdateDCLListCIPOnly = dcl;
		dcl = (*commandPool)->AllocateUpdateDCLListDCL(commandPool, dcl, &timeStampUpdateDCLListCIPOnly, 1);
		
		// Do a callback
		dcl = (*commandPool)->AllocateCallProcDCL(commandPool, dcl, callBackProc, (DCLCallProcDataType) this);
    }
    dcl = (*commandPool)->AllocateJumpDCL( commandPool, dcl, nil ) ;
    pCIPOnlyXfrJump = dcl;
	
	// Create Full Xfr DCL
    dcl = (*commandPool)->AllocateLabelDCL( commandPool, dcl ) ;
    pFullXfrLabel = dcl;

	dcl = (*commandPool)->AllocateSendPacketStartDCL( commandPool, dcl, pBuf, bufferSize ) ;

    if (callbackEnabled == true)
    {
		// Get the timestamp
		dcl = (*commandPool)->AllocatePtrTimeStampDCL(commandPool, dcl, pTimeStamp);
		timeStampUpdateDCLListFull = dcl;
		dcl = (*commandPool)->AllocateUpdateDCLListDCL(commandPool, dcl, &timeStampUpdateDCLListFull, 1);

		// Do a callback
		dcl = (*commandPool)->AllocateCallProcDCL(commandPool, dcl, callBackProc, (DCLCallProcDataType) this);
    }
    dcl = (*commandPool)->AllocateJumpDCL( commandPool, dcl, nil ) ;
    pFullXfrJump = dcl;

    // Pass the caller back a pointer to the last DCL created
    *ppLastDCL = dcl;

    return;
}

// Desctructor
DVXmitCycle::~DVXmitCycle(void)
{
    return;
}

// UpdateJumpTarget Function
void DVXmitCycle::UpdateJumpTarget(XmitCycleMode nextCycleMode,
									  IOFireWireLibLocalIsochPortRef outPort)
{
	if ((CycleMode == CycleModeFull) && (FullXfrJumpNextMode != nextCycleMode))
	{
		// We need to modify this jump target
		(*outPort)->ModifyJumpDCL(outPort,
							(DCLJump*) pFullXfrJump,
							(nextCycleMode == CycleModeFull) ?
							(DCLLabel*) pNext->pFullXfrLabel :
							(DCLLabel*) pNext->pCIPOnlyXfrLabel);
		FullXfrJumpNextMode = nextCycleMode;
	}
	else if ((CycleMode == CycleModeCIPOnly) && (CIPOnlyJumpNextMode != nextCycleMode))
	{
		// We need to modify this jump target
		(*outPort)->ModifyJumpDCL(outPort,
							(DCLJump*) pCIPOnlyXfrJump,
							(nextCycleMode == CycleModeFull) ?
							(DCLLabel*) pNext->pFullXfrLabel :
							(DCLLabel*) pNext->pCIPOnlyXfrLabel);
		CIPOnlyJumpNextMode = nextCycleMode;
	}

	return;
}

} // namespace AVS
