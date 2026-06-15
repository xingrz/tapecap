 /*
  File:		AVSShared.h
  
  Synopsis: This is the header for the constants and structs that are shared between
            the AVCVideoServices.framework, and the FWAVC.framework
            Note that stuff in here is outside the AVCVideoServices "AVS" namespace!
  
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
 
#ifndef __AVCVIDEOSERVICES_AVSSHARED__
#define __AVCVIDEOSERVICES_AVSSHARED__
 
//////////////////////////////////////////////////////////////////
// Constants
//////////////////////////////////////////////////////////////////
enum
{
	// constants for default parameters when creating a TS demuxer
	kFWAVCTSDemuxerMaxVideoPESSizeDefault = 1048576,
	kFWAVCTSDemuxerMaxAudioPESSizeDefault = 65536,
	kFWAVCTSDemuxerDefaultProgramSelection = 0x01,
	kFWAVCTSDemuxerDefaultVideoPESBufferCount = 5,
	kFWAVCTSDemuxerDefaultAudioPESBufferCount	= 5,
	
	// constants for message sent to client in TS demuxer PES Callback
	kFWAVCTSDemuxerPESReceived = 0,
	kFWAVCTSDemuxerPacketError = 1,
	kFWAVCTSDemuxerDiscontinuity = 2,
	kFWAVCTSDemuxerIllegalAdaptationFieldCode = 3,
	kFWAVCTSDemuxerBadAdaptationFieldLength = 4,
	kFWAVCTSDemuxerPESLargerThanAllocatedBuffer = 5,
	kFWAVCTSDemuxerFlushedPESBuffer = 6,
	kFWAVCTSDemuxerRescanningForPSI = 7,
	
	// constants for ts demuxer PES packet stream types
	kFWAVCTSDemuxerStreamTypeVideo = 0,
	kFWAVCTSDemuxerStreamTypeAudio = 1,
	
	// constants for default parameters when creating a DV framer
	kFWAVCDVFramerDefaultFrameCount = 5,

	// constants for message sent to client in DV framer frame callback
	kFWAVCDVFramerFrameReceivedSuccessfully = 0,
	kFWAVCDVFramerPartialFrameReceived = 1,
	kFWAVCDVFramerCorruptFrameReceived = 2,
	kFWAVCDVFramerNoFrameBufferAvailable = 3,
	kFWAVCDVFramerNoMemoryForFrameBuffer = 4,
	
	// constant for the client to force use of default parameter value when creating a device stream
	kFWAVCDeviceStreamDefaultParameter	= 0,
	
	// constants for default parameters when creating a universal receiver
	kFWAVCNumUniversalReceiveSegments	= 3,
	kFWAVCCyclesPerUniversalReceiveSegment = 2000,
	kFWAVCUniversalReceiverDefaultBufferSize  = 2048,

	// constants for default parameters when creating a universal transmitter
	kFWAVCNumUniversalTransmitSegments = 3,
	kFWAVCCyclesPerUniversalTransmitSegment = 1000,
	kFWAVCUniversalTransmitterClientBufferSize = 4096,
	kFWAVCUniversalTransmitterIrmAllocationMaxPacketSize = 4096,
	kFWAUniversalTransmitterNumStartupCycleMatchBits = 13,
	
	// constants for default parameters when creating a MPEG2 receiver
	kFWAVCNumMPEG2ReceiveSegments	= 3,
	kFWAVCCyclesPerMPEG2ReceiveSegment = 2000,
	kFWAVCMaxNumMPEG2ReceivePacketsPerCycle = 5,
	
	// constants for default parameters when creating a MPEG2 transmitter
	kFWAVCCyclesPerMPEG2TransmitSegment = 1500,
	kFWAVCNumMPEG2TransmitSegments = 3,
	kFWAVCNumTSPacketsPerMPEG2TransmitCycle = 3,
	kFWAVCMPEG2TransmitTSPacketQueueSizeInPackets = 2048,

	// constants for default parameters when creating a DV transmitter
	kFWAVCCyclesPerDVTransmitSegment = 600,
	kFWAVCNumDVTransmitSegments = 3,
	kFWAVCDVTransmitNumFrameBuffers = 4,
	
	// constants for default parameters when creating a DV receiver
	kFWAVCCyclesPerDVReceiveSegment = 800,
	kFWAVCNumDVReceiveSegments	= 3,
	kFWAVCDVReceiveNumFrameBuffers = 4,
	
	// constants for message sent to client in DV receiver frame-received callback
	kFWAVCDVReceiverFrameReceivedSuccessfully = 0,
	kFWAVCDVReceiverFrameDropped = 1,
	kFWAVCDVReceiverFrameCorrupted = 2,
	kFWAVCDVReceiverFrameWrongMode = 3,
	
	// constants for messages sent to client in streamming objects's message callback
	kFWAVCStreamAllocateIsochPort = 0,
	kFWAVCStreamReleaseIsochPort = 1,
	kFWAVCStreamDCLOverrun = 2,
	kFWAVCStreamTimeStampAdjust = 3,		// Only for transmitters
	kFWAVCStreamReceivedBadPacket = 4,		// Only for receivers
	kFWAVCStreamPreparePacketFetcher = 5,	// Only for transmitters
	kFWAVCStreamBadBufferRange = 6,			// Only for universal transmitter
	kFWAVCStreamDCLOverrunAutoRestartFailed = 7,
	
	// constants for supported DV modes
	kFWAVCDVMode_SDL_625_50 = 0x84,
	kFWAVCDVMode_SDL_525_60 = 0x04,
	kFWAVCDVMode_SD_625_50 = 0x80,
	kFWAVCDVMode_SD_525_60 = 0x00,
	kFWAVCDVMode_DVCPro25_625_50 = 0xF8,
	kFWAVCDVMode_DVCPro25_525_60 = 0x78,
	kFWAVCDVMode_DVCPro50_625_50 = 0xF4,
	kFWAVCDVMode_DVCPro50_525_60 = 0x74,
	kFWAVCDVMode_HD_1250_50 = 0x88,
	kFWAVCDVMode_HD_1125_60 = 0x08,
	kFWAVCDVMode_DVCPro100_50 = 0xF0,
	kFWAVCDVMode_DVCPro100_60 =0x70,
	
	// constants for the CIP packet parser
	kFWAVC61883Fmt_DV = 0x00,
	kFWAVC61883Fmt_BT601 = 0x01,
	kFWAVC61883Fmt_AudioMidi = 0x10,
	kFWAVC61883Fmt_MPEG2TS = 0x20,
	kFWAVC61883Fmt_Invalid = 0xff,
	kFWAVCMaxSourcePacketsPerCIPPacket = 32
};

//////////////////////////////////////////////////////////////////
// For UniversalReceiver's "structured-data" callback method,
// a struct to hold all info regarding a single cycle's received data. 
//////////////////////////////////////////////////////////////////
typedef struct FWAVCUniversalReceiveCycleDataStruct
{
	void *pRefCon; 
	UInt32 payloadLength;
	UInt8 *pPayload; 
	UInt32 isochHeader;
	UInt32 fireWireTimeStamp;
	UInt64 nanoSecondsTimeStamp;
	void *pExpansionData;	// Currently Unused
}FWAVCUniversalReceiveCycleData;

//////////////////////////////////////////////////////////////////
// For MPEG2Receiver's "structured-data" callback method,
// a struct to hold all info regarding a single cycle's received data. 
//////////////////////////////////////////////////////////////////
typedef struct FWAVCMPEGReceiveCycleDataStruct
{
	UInt32 tsPacketCount;
	UInt32 *pBuf[kFWAVCMaxNumMPEG2ReceivePacketsPerCycle]; 
	void *pRefCon; 
	UInt32 isochHeader;
	UInt32 cipHeader0;
	UInt32 cipHeader1;
	UInt32 fireWireTimeStamp;
	UInt64 nanoSecondsTimeStamp;
	void *pExpansionData;	// Currently Unused
}FWAVCMPEGReceiveCycleData;

//////////////////////////////////////////////////////////////////
// For the CIP packet parser, 
// a struct to contain the parsed info
//////////////////////////////////////////////////////////////////
typedef struct FWAVCCIPPacketParserInfoStruct
{
	// The stream format
	UInt8 fmt;
	
	// The format-dependent-field
	// This will either be an 8-bit value (for streams with SYT), 
	// or a 24-bit value (for no-SYT streams)
	UInt32 fdf;
	
	// The CIP-header SYT field
	UInt16 syt;
	
	// The size of source packets in this CIP packet
	UInt32 sourcePacketSize;
	
	// The number of source packets in this CIP packet
	UInt8 numSourcePackets;
	
	// An array of pointers to the source packets in this CIP packet
	UInt8 *pSourcePacket[kFWAVCMaxSourcePacketsPerCIPPacket];
	
	// The dvMode is the same as the 8-bit FDF
	UInt8 dvMode;
}FWAVCCIPPacketParserInfo;

//////////////////////////////////////////////////////////////////
// Structure for HDV2 VAux parameters
//////////////////////////////////////////////////////////////////
typedef struct FWAVCTSDemuxerHDV2VideoFramePackStruct
{
	UInt8 keyWord;
	UInt8 length;
	UInt32 extendedTrackNumber;
	UInt8 numVideoFrames;
	UInt8 dataH;
	UInt16 vbvDelay;
	UInt8 headerSize;
	UInt64 dts;
	Boolean progressive;
	Boolean topFieldFirst;
	Boolean repeatFirstField;
	UInt8 sourceFrameRate;
	UInt8 searchDataMode;
	UInt16 horizontalSize;
	UInt16 verticalSize;
	UInt8 aspectRatio;
	UInt8 frameRate;
	UInt32 bitRate;
	UInt16 vbvBufferSize;
	UInt8 mpegProfile;
	UInt8 mpegLevel;
	UInt8 videoFormat;
	UInt8 chroma;
	UInt8 gopN;
	UInt8 gopM;
	Boolean packDataEnable0;
	Boolean packDataEnable1;
	Boolean packDataEnable2;
	Boolean bf;
	Boolean ttc_df;
	UInt8 tc_frames;
	UInt8 tc_seconds;
	UInt8 tc_minutes;
	UInt8 tc_hours;
	Boolean ds;
	Boolean tm;
	UInt8 recDate_timeZone;
	UInt8 recDate_day;
	UInt8 recDate_month;
	UInt8 recDate_week;
	UInt8 recDate_year;
	UInt8 recTime_frames;
	UInt8 recTime_seconds;
	UInt8 recTime_minutes;
	UInt8 recTime_hours;
	UInt8 copyGenerationManagementSystem;
	Boolean rec_st;
	Boolean atn_bf;
	UInt8 extendedDVPack1[5];
	UInt8 extendedDVPack2[5];
	UInt8 extendedDVPack3[5];
	
	UInt8 *pVAuxDataBytes;
	UInt32 vAuxDataLen;
	
	UInt32 TSDemuxerHDV2VideoFramePackStructureVersion;
	
	Boolean hasMakerCodeAndImagingFrameRate;
	UInt8 makerCode;
	UInt8 imagingFrameRate;
}FWAVCTSDemuxerHDV2VideoFramePack;

//////////////////////////////////////////////////////////////////
// Structure for HDV1 Pack-data parameters
//////////////////////////////////////////////////////////////////
typedef struct FWAVCTSDemuxerHDV1PackDataStruct
{
	UInt32 idString;
	
	Boolean seamlessPlayBackPoint;
	Boolean has_2_3_pullDown;
	Boolean pullDownRepetition;
	
	Boolean hasMPEGSourcePack;
	UInt16 serviceID;
	UInt8 sourceCode;
	Boolean is50HzSystem;
	UInt8 sType;
	UInt8 tunerCategory;
	
	Boolean hasMPEGSourceControlPack;
	UInt8 cgms;
	UInt8 tph;
	Boolean tpl;
	UInt8 ss;
	Boolean recST;
	UInt8 recMode;
	Boolean mr;
	Boolean isHD;
	UInt8 audMode;
	UInt8 maxBitRate;
	Boolean recEnd;
	UInt8 genreCategory;
	
	Boolean hasRecDatePack;
	UInt8 recTimeZone;
	UInt8 recDay;
	UInt8 recWeek;
	UInt8 recMonth;
	UInt8 recYear;
	
	Boolean hasRecTimePack;
	UInt8 recFrames;
	UInt8 recSeconds;
	UInt8 recMinutes;
	UInt8 recHours;
	
	Boolean hasStreamPack;
	UInt8 streamType;
	UInt16 elementaryPID;
	UInt8 pidType;
	
	Boolean hasTitleTimeCodePack;
	UInt8 ttcFrames;
	UInt8 ttcSeconds;
	UInt8 ttcMinutes;
	UInt8 ttcHours;
	Boolean s1;
	Boolean s2;
	Boolean s3;
	Boolean s4;
	Boolean s5;
	Boolean s6;
	
	Boolean hasBinaryGroupPack;
	UInt8 binaryGroup1;
	UInt8 binaryGroup2;
	UInt8 binaryGroup3;
	UInt8 binaryGroup4;
	UInt8 binaryGroup5;
	UInt8 binaryGroup6;
	UInt8 binaryGroup7;
	UInt8 binaryGroup8;
	
	UInt8 *pPackDataBytes;
	UInt32 packDataLen;
}FWAVCTSDemuxerHDV1PackData;

 
#endif // __AVCVIDEOSERVICES_AVSSHARED__

