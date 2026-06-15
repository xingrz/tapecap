/*
	File:   TapeSubunitController.cpp
 
 Synopsis: This is the sourcecode for the TapeSubunitController Class
 
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
	
///////////////////////////////
// Constructor
///////////////////////////////
TapeSubunitController::TapeSubunitController(AVCDevice *pDevice, UInt8 subUnitID)
{
	pAVCDevice = pDevice;
	pAVCDeviceCommandInterface = nil;
	subunitTypeAndID = 0x20 + (subUnitID & 0x07);
}
	
///////////////////////////////
// Alternate Constructor
///////////////////////////////
TapeSubunitController::TapeSubunitController(AVCDeviceCommandInterface *pDeviceCommandInterface, UInt8 subUnitID)
{
	pAVCDevice = nil;
	pAVCDeviceCommandInterface = pDeviceCommandInterface;
	subunitTypeAndID = 0x20 + (subUnitID & 0x07);
}

///////////////////////////////
// Destructor
///////////////////////////////
TapeSubunitController::~TapeSubunitController()
{
	
}

///////////////////////////////
// Play 
///////////////////////////////
IOReturn TapeSubunitController::Play(UInt8 playMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapePlayOpcode;
	cmd[3] = playMode;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// Record
///////////////////////////////
IOReturn TapeSubunitController::Record(UInt8 recordMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeRecordOpcode;
	cmd[3] = recordMode;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// Wind
///////////////////////////////
IOReturn TapeSubunitController::Wind(UInt8 windMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeWindOpcode;
	cmd[3] = windMode;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// EjectTape
///////////////////////////////
IOReturn TapeSubunitController::EjectTape(void)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeLoadMediumOpcode;
	cmd[3] = 0x60;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetInputSignalMode
///////////////////////////////
IOReturn TapeSubunitController::GetInputSignalMode(UInt8 *pInputSignalMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeInputSignalModeOpcode;
	cmd[3] = 0xFF;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pInputSignalMode = response[3];
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// SetInputSignalMode
///////////////////////////////
IOReturn TapeSubunitController::SetInputSignalMode(UInt8 inputSignalMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeInputSignalModeOpcode;
	cmd[3] = inputSignalMode;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetOutputSignalMode
///////////////////////////////
IOReturn TapeSubunitController::GetOutputSignalMode(UInt8 *pOutputSignalMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeOutputSignalModeOpcode;
	cmd[3] = 0xFF;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pOutputSignalMode = response[3];
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// SetOutputSignalMode
///////////////////////////////
IOReturn TapeSubunitController::SetOutputSignalMode(UInt8 outputSignalMode)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeOutputSignalModeOpcode;
	cmd[3] = outputSignalMode;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetMediumInfo
///////////////////////////////
IOReturn TapeSubunitController::GetMediumInfo(UInt8 *pCassetteType, UInt8 *pTapeGradeAndWriteProtect)
{
    UInt32 size;
    UInt8 cmd[5],response[5];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeMediumInfoOpcode;
	cmd[3] = 0x7F;
	cmd[4] = 0x7F;
	size = 5;
	res = DoAVCCommand(cmd, 5, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pCassetteType = response[3];
		*pTapeGradeAndWriteProtect = response[4];
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetRelativeTimeCounter
///////////////////////////////
IOReturn TapeSubunitController::GetRelativeTimeCounter(UInt8 *pHour, UInt8 *pMinute, UInt8 *pSecond, UInt8 *pSignAndFrame)
{
    UInt32 size;
    UInt8 cmd[8],response[8];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeRelativeTimeCounterOpcode;
	cmd[3] = 0x71;
	cmd[4] = 0xFF;
	cmd[5] = 0xFF;
	cmd[6] = 0xFF;
	cmd[7] = 0xFF;
	size = 8;
	res = DoAVCCommand(cmd, 5, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pHour = response[7];
		*pMinute = response[6];
		*pSecond = response[5];
		*pSignAndFrame = response[4];
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetTransportState
///////////////////////////////
IOReturn TapeSubunitController::GetTransportState(UInt8 *pTransportMode, UInt8 *pTransportState, bool *pIsStable)
{
    UInt32 size;
    UInt8 cmd[4],response[4];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeTransportStateOpcode;
	cmd[3] = 0x7F;
	size = 4;
	res = DoAVCCommand(cmd, 4, response, &size);
	if ((res == kIOReturnSuccess) && ((response[0] == kAVCImplementedStatus) || (response[0] == kAVCInTransitionStatus)))
	{
		*pTransportMode = response[2];
		*pTransportState = response[3];
		*pIsStable = (response[0] == kAVCImplementedStatus) ? true : false;
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// GetTimeCode
///////////////////////////////
IOReturn TapeSubunitController::GetTimeCode(UInt8 *pHour, UInt8 *pMinute, UInt8 *pSecond, UInt8 *pFrame)
{
    UInt32 size;
    UInt8 cmd[8],response[8];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCStatusInquiryCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeTimeCodeOpcode;
	cmd[3] = 0x71;
	cmd[4] = 0xFF;
	cmd[5] = 0xFF;
	cmd[6] = 0xFF;
	cmd[7] = 0xFF;
	size = 8;
	res = DoAVCCommand(cmd, 8, response, &size);
	if ((res == kIOReturnSuccess) && (response[0] == kAVCImplementedStatus))
	{
		*pHour = response[7];
		*pMinute = response[6];
		*pSecond = response[5];
		*pFrame = response[4];
	}
	else
	{
		res = kIOReturnError;
	}
	
	return res;
}

///////////////////////////////
// SearchToTimeCode
///////////////////////////////
IOReturn TapeSubunitController::SearchToTimeCode(UInt8 hour, UInt8 minute, UInt8 second, UInt8 frame)
{
    UInt32 size;
    UInt8 cmd[8],response[8];
    IOReturn res = kIOReturnSuccess;
	
	cmd[0] = kAVCControlCommand;
	cmd[1] = subunitTypeAndID;
	cmd[2] = kAVCTapeTimeCodeOpcode;
	cmd[3] = 0x20;
	cmd[4] = frame;
	cmd[5] = second;
	cmd[6] = minute;
	cmd[7] = hour;
	size = 8;
	res = DoAVCCommand(cmd, 8, response, &size);
	if (!((res == kIOReturnSuccess) && (response[0] == kAVCAcceptedStatus)))
	{
		res = kIOReturnError;
	}
	
	return res;
}
	
///////////////////////////////
// DoAVCCommand
///////////////////////////////
IOReturn TapeSubunitController::DoAVCCommand(const UInt8 *command, UInt32 cmdLen, UInt8 *response, UInt32 *responseLen)
{
	if (pAVCDevice)
		return pAVCDevice->AVCCommand(command,cmdLen,response,responseLen);
	if (pAVCDeviceCommandInterface)
		return pAVCDeviceCommandInterface->AVCCommand(command,cmdLen,response,responseLen);
	else
		return kIOReturnNoDevice;
}

} // namespace AVS	