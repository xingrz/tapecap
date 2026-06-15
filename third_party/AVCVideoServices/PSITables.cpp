/*
	File:		PSITables.cpp

    Synopsis: This is the implementation file for the PSITables class, a support class for
	the MPEG2Transmitter class.
 
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

#define VERBOSE_PSI_TABLES 1

//////////////////////////////////////////////////////
// Constructor
//////////////////////////////////////////////////////
PSITables::PSITables(StringLogger *stringLogger)
{
	patVersion = 0xFF;			// 0xFF is an invalid Version meaning no PAT yet
	primaryPMTVersion = 0xFF; 	// 0xFF is an invalid Version meaning no PMT yet
	primaryProgramPmtPid = 0;	// 0 means none available here!
	pcrPID = 0;

	primaryProgramVideoPid = kReservedPid;
	primaryProgramAudioPid = kReservedPid;
	
	primaryProgramVideoStreamType = 0; // Reserved stream type 
	primaryProgramAudioStreamType = 0; // Reserved stream type 
	
	logger = stringLogger;
	
	primaryProgramDescriptorsLen = 0;
	pPrimaryProgramDescriptors = NULL;
	
	primaryVideoESDescriptorsLen = 0;
	pPrimaryVideoESDescriptors = NULL;
	
	primaryAudioESDescriptorsLen = 0;
	pPrimaryAudioESDescriptors = NULL;
	
	selectedProgramIndex = 1;	// A one here means the first program in the PAT!
}

//////////////////////////////////////////////////////
// Destructor
//////////////////////////////////////////////////////
PSITables::~PSITables(void)
{
	if (pPrimaryProgramDescriptors)
		delete [] pPrimaryProgramDescriptors;
	if (pPrimaryVideoESDescriptors)
		delete [] pPrimaryVideoESDescriptors;
	if (pPrimaryAudioESDescriptors)
		delete [] pPrimaryAudioESDescriptors;
}

//////////////////////////////////////////////////////
// setPATProgramIndex
//////////////////////////////////////////////////////
void PSITables::selectProgram(UInt32 patProgramIndex)
{
	selectedProgramIndex = patProgramIndex;
}

//////////////////////////////////////////////////////
// extractTableDataFromPacket
//////////////////////////////////////////////////////
void PSITables::extractTableDataFromPacket(TSPacket *pTSPacket)
{
	UInt8 *pTable;
	UInt32 sectionLength;
	UInt32 numProgs;
	unsigned int progNum;
	unsigned int pmtPid;
	unsigned int i;
	unsigned int tableVersion;
	unsigned int sectionNum;
	unsigned int programInfoLen;
	unsigned int esPID;
	unsigned int esInfoLen;
	unsigned int primaryProgramIndex = (selectedProgramIndex > 0) ? selectedProgramIndex-1 : 0;  // Here we convert from one base index to zero base index!!!!!!!!

	// TODO: This needs work to support multiple section tables
	// and to support multiple packet per section tables.
	// Curently it only examines the first packet of the first
	// section of any PAT or PMT table.

	// First see if this is a PAT section
	if (pTSPacket->pid == 0)
	{
		// If error indicator set, bail
		if (pTSPacket->pPacket[1] & 0x80)
			return;
			
		// Move passed the pointer field
		pTable = pTSPacket->pPayload;
		pTable += (*pTable + 1);	// Move to the start of the table

		if (pTSPacket->pPacket[1] & 0x40)	// Is the payload_unit_start_indicator set?
		{
			sectionLength = (( (UInt32) pTable[1] & 0x03) << 8) + pTable[2];
			numProgs = ((sectionLength - 9) / 4);
			tableVersion = ((pTable[5] & 0x3E) > 1);
			sectionNum = pTable[6];

			// Parse this table only if it's a newer version then what we have, or if we haven't identified both the video and audio PIDs
			if ((sectionNum == 0) && ((patVersion != tableVersion) || (primaryProgramVideoPid == kReservedPid) || (primaryProgramAudioPid == kReservedPid)))
			{

#ifdef VERBOSE_PSI_TABLES
				if (logger)
				{
					logger->log("\n===========================\n");
					logger->log("PID 0 = PAT\n");
					logger->log(" Table ID:       %d\n",pTable[0]);
					logger->log(" Version Num:    %d\n",tableVersion);
					logger->log(" Section:        %d\n",sectionNum);
					logger->log(" Last Section:   %d\n",pTable[7]);
					logger->log(" Section Length: %ld\n",sectionLength);
					logger->log(" Num Programs:   %ld\n",numProgs);
				}
#endif
				// Parse the remainder of this table
				pTable += 8;
				
				// If the primaryProgramIndex is greater than the number of programs
				// in the PAT, we'll just look for the first program instead.
				if (primaryProgramIndex >= numProgs)
					primaryProgramIndex = 0;

				for (i=0;i<numProgs;i++)
				{
					progNum = (((unsigned int)(pTable[0]) << 8) + pTable[1]);
					pmtPid =  (((unsigned int)(pTable[2] & 0x1F) << 8) + pTable[3]);

#ifdef VERBOSE_PSI_TABLES
					if (logger)
					{
						logger->log("    Program: 0x%04X  Program_Map_PID: 0x%04X \n",progNum,pmtPid);
					}
#endif
					if (i == primaryProgramIndex)
					{
						// If the program number of the program at the current index
						// is zero, move on to the next index. Program 0 is
						// reserved as the "network program".
						if (progNum == 0)
							primaryProgramIndex++;
						else
							primaryProgramPmtPid = pmtPid;
					}
					pTable += 4;
				}

				// Update the patVersion var
				patVersion = tableVersion;

				// Invalidate our PMT so that we will process the next one
				primaryPMTVersion = 0xFF;
			}
		}
	}

	// Else, see if this is a PMT section for the primary program
	else if (pTSPacket->pid == primaryProgramPmtPid)
	{
		// If error indicator set, bail
		if (pTSPacket->pPacket[1] & 0x80)
			return;
		
		// Move passed the pointer field
		pTable = pTSPacket->pPayload;
		pTable += (*pTable + 1);	// Move to the start of the table

		if (pTSPacket->pPacket[1] & 0x40)	// Is the payload_unit_start_indicator set?
		{
			sectionLength = (( (UInt32) pTable[1] & 0x03) << 8) + pTable[2];
			pcrPID = (((unsigned int)(pTable[8] & 0x1F) << 8) + pTable[9]);
			tableVersion = ((pTable[5] & 0x3E) > 1);
			sectionNum = pTable[6];

			// Parse this table only if it's a newer version then what we have, or if we haven't identified both the video and audio PIDs
			if ((sectionNum == 0) && ((primaryPMTVersion != tableVersion) || (primaryProgramVideoPid == kReservedPid) || (primaryProgramAudioPid == kReservedPid)))
			{
				
				programInfoLen = (((unsigned int)(pTable[10] & 0x0F) << 8) + pTable[11]);

#ifdef VERBOSE_PSI_TABLES
				if (logger)
				{
					logger->log("\n===========================\n");
					logger->log("PID 0x%04X = PMT\n",primaryProgramPmtPid);
					logger->log(" Table ID:         %d\n",pTable[0]);
					logger->log(" Version Num:      %d\n",tableVersion);
					logger->log(" Program Num:      %d\n",((unsigned int)(pTable[3] << 8) + pTable[4]));
					logger->log(" Section:          %d\n",sectionNum);
					logger->log(" Last Section:     %d\n",pTable[7]);
					logger->log(" Section Length:   %ld\n",sectionLength);
					logger->log(" PCR PID:          0x%04X\n",pcrPID);
				}
#endif

				// Reset the audio/video PIDs
				primaryProgramVideoPid = kReservedPid;
				primaryProgramAudioPid = kReservedPid;
				
				// Delete any existing program descriptors
				if (pPrimaryProgramDescriptors)
				{
					delete [] pPrimaryProgramDescriptors;
					pPrimaryProgramDescriptors = NULL;
					primaryProgramDescriptorsLen = 0;
				}
				
				// Save a copy of the program descriptors, if they exist
				if (programInfoLen != 0)
				{
					pPrimaryProgramDescriptors = new unsigned char[programInfoLen];
					if (pPrimaryProgramDescriptors)
					{
						// Copy the descriptors
						memcpy(pPrimaryProgramDescriptors,&pTable[12],programInfoLen);
						primaryProgramDescriptorsLen = programInfoLen;
					}
				}
				
				// Parse the remainder of this table
				pTable += (12+programInfoLen);
				for (i=0;i<2;i++)
				{
					esPID = (((unsigned int)(pTable[1] & 0x1F) << 8) + pTable[2]);
					esInfoLen = (((unsigned int)(pTable[3] & 0x0F) << 8) + pTable[4]);
					
#ifdef VERBOSE_PSI_TABLES
					if (logger)
					{
						logger->log("    Stream Type: 0x%04X  PID: 0x%04X\n",pTable[0],esPID);
					}
#endif

					// See if this is the video PID we're interested in
					if ((primaryProgramVideoPid == kReservedPid) && ((pTable[0] == 0x02) || (pTable[0] == 0x1B)))
					{
						primaryProgramVideoPid = esPID;
						primaryProgramVideoStreamType = pTable[0];
#ifdef VERBOSE_PSI_TABLES
						if (logger)
						{						
							logger->log("      Video Stream PID: 0x%04X\n",primaryProgramVideoPid);
						}
#endif
						// Get rid of any other video descriptors
						if (pPrimaryVideoESDescriptors)
						{
							delete [] pPrimaryVideoESDescriptors;
							pPrimaryVideoESDescriptors = NULL;
							primaryVideoESDescriptorsLen = 0;
						}
						
						// Save a copy of the Video ES descriptors, if they exist
						if (esInfoLen != 0)
						{
							pPrimaryVideoESDescriptors = new unsigned char[esInfoLen];
							if (pPrimaryVideoESDescriptors)
							{
								// Copy the descriptors
								memcpy(pPrimaryVideoESDescriptors,&pTable[5],esInfoLen);
								primaryVideoESDescriptorsLen = esInfoLen;
							}
						}
					}
						
					// See if this is the audio PID we're interested in
					if ((primaryProgramAudioPid == kReservedPid) &&
		 ((pTable[0] == 0x03) || (pTable[0] == 0x04) || (pTable[0] == 0x81)))
					{
						primaryProgramAudioPid = esPID;
						primaryProgramAudioStreamType = pTable[0];
#ifdef VERBOSE_PSI_TABLES
						if (logger)
						{						
							logger->log("      Audio Stream PID: 0x%04X\n",primaryProgramAudioPid);
						}
#endif
						// Get rid of any other audio descriptors
						if (pPrimaryAudioESDescriptors)
						{
							delete [] pPrimaryAudioESDescriptors;
							pPrimaryAudioESDescriptors = NULL;
							primaryAudioESDescriptorsLen = 0;
						}
						
						// Save a copy of the Audio ES descriptors, if they exist
						if (esInfoLen != 0)
						{
							pPrimaryAudioESDescriptors = new unsigned char[esInfoLen];
							if (pPrimaryAudioESDescriptors)
							{
								// Copy the descriptors
								memcpy(pPrimaryAudioESDescriptors,&pTable[5],esInfoLen);
								primaryAudioESDescriptorsLen = esInfoLen;
							}
						}
					}

					pTable += (5+esInfoLen);
				};

				// Update the primaryPMTVersion var
				primaryPMTVersion = tableVersion;
			}
		}
	}

	return;
}

//////////////////////////////////////////////////////
// ResetPSITables
//////////////////////////////////////////////////////
void PSITables::ResetPSITables(void)
{
	patVersion = 0xFF;
	primaryPMTVersion = 0xFF;
	primaryProgramPmtPid = 0;
	pcrPID = 0;
	primaryProgramVideoPid = kReservedPid;
	primaryProgramAudioPid = kReservedPid;
	
	primaryProgramVideoStreamType = 0; // Reserved stream type 
	primaryProgramAudioStreamType = 0; // Reserved stream type 

	if (pPrimaryProgramDescriptors)
	{
		delete [] pPrimaryProgramDescriptors;
		primaryProgramDescriptorsLen = 0;
		pPrimaryProgramDescriptors = NULL;
	}

	if (pPrimaryVideoESDescriptors)
	{
		delete [] pPrimaryVideoESDescriptors;
		primaryVideoESDescriptorsLen = 0;
		pPrimaryVideoESDescriptors = NULL;
	}
	
	if (pPrimaryAudioESDescriptors)
	{
		delete [] pPrimaryAudioESDescriptors;
		primaryAudioESDescriptorsLen = 0;
		pPrimaryAudioESDescriptors = NULL;
	}
}

} // namespace AVS
