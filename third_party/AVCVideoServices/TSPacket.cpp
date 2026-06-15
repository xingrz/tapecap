/*
	File:		TSPacket.cpp

    Synopsis: This is the implementation file for the TSPacket class, a support class for
	the MPEG2Transmitter class.
 
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

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
TSPacket::TSPacket(unsigned char *pPacketBuf)
{
    pPacket = pPacketBuf;
    parsePacket();
}

//////////////////////////////////////////////////////
// update
//////////////////////////////////////////////////////
void TSPacket::update(unsigned char *pPacketBuf)
{
    pPacket = pPacketBuf;
    parsePacket();
}

//////////////////////////////////////////////////////
// parsePacket
//////////////////////////////////////////////////////
void TSPacket::parsePacket(void)
{
    hasPCR = false;
	hasDataRateChange = false;
	hasPacketFetchError = false;
	hasAdaptationPrivateData = false;

    // Extract the PID
    pid = (((unsigned int)(pPacket[1] & 0x1F) << 8) + pPacket[2]);

    // Extract PCR from packet if it exists
    if (pPacket[3] & 0x20) // See if this packet has a adaptation field
	{
        // This packet has an adaption field. See if it has a PCR
        if ((pPacket[5] & 0x10) >> 4)
        {
            hasPCR = true;

            // The pcr is extracted into 3 parts
            pcr_base_high = ((pPacket[6] & 0x80) >> 7);

            pcr_base_low = ( ((pPacket[6] & 0x7f) << 25) +
							 (pPacket[7] << 17) +
							 (pPacket[8] << 9) +
							 (pPacket[9] << 1) +
							 ((pPacket[10] & 0x80) >> 7));

            pcr_ext = ((unsigned int)(pPacket[10] & 0x1) << 8) + pPacket[11];
			
			// Calculate the PCR
			pcr = ((((UInt64)pcr_base_high << 32LL) + pcr_base_low)*300) + pcr_ext;

            // Calculate the full PCR timestamp
			pcrTime = ((1.0/27000000.0) * pcr);
            //pcrTime =  ((1.0/27000000.0) * ((pcr_base_high * 4294967296.0 * 300.0) + (pcr_base_low * 300.0) + pcr_ext));
        }
		
		// See if this packet has private data in the adaptation header
        if (pPacket[5] & 0x02)
		{
			hasAdaptationPrivateData = true;
			pAdaptationPrivateData = &pPacket[6];
			if (pPacket[5] & 0x10)
				pAdaptationPrivateData += 6;	// Bump past PCR
			if (pPacket[5] & 0x08)
				pAdaptationPrivateData += 6;	// Bump past OPCR
			if (pPacket[5] & 0x04)
				pAdaptationPrivateData += 1;	// Bump past Splice-Countdown
			adaptationPrivateDataLen = *pAdaptationPrivateData;
			pAdaptationPrivateData += 1;	// Bump past Private Data Length
		}
		else
		{
			hasAdaptationPrivateData = false;
			adaptationPrivateDataLen = 0;
			pAdaptationPrivateData = nil;
		}

        // Set the payload pointer
        pPayload = &pPacket[5] + pPacket[4];
    }
    else
    {
        // Set the payload pointer
        pPayload = &pPacket[4];
    }
}

} // namespace AVS
