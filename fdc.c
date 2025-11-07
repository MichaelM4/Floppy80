#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "Defines.h"
#include "system.h"
#include "sd_core.h"
#include "crc.h"
#include "datetime.h"
#include "fdc.h"
#include "ff.h"
#include "hardware/pio.h"
#include "util.h"

#include "pico/stdlib.h"

////////////////////////////////////////////////////////////////////////////////////
/*

For JV1 and JV3 format information see https://www.tim-mann.org/trs80/dskspec.html

*/
////////////////////////////////////////////////////////////////////////////////////
/*

DMK file format

Disk Header:

The first 16-bytes of the file is the header and defines the format of the virtual drive.

Byte	Description
0		Write Protect: 0xFF - drive is write protected; 0x00 - drive is not write protected;
1		Number of tracks
2&3		Track length = (Header[3] << 8) + Header[2];
4		Virtual disk options flags
		Bit-0: NA
		Bit-1: NA
		Bit-2: NA
		Bit-3: NA
		Bit-4: if set indicates it is a single sided diskette; if not set it is a double sided diskette;
		Bit-5: NA
		Bit-6: if set indicates it is a single density dikette; if not set it is a double density diskette;
		Bit-7: if set then the density of the disk it to be ignored.
5-11	Reserved for future use
12-15	0x00, 0x00, 0x00, 0x00 - for virtual diskette navive format
		0x12, 0x34, 0x56, 0x78 - if virtual disk is a REAL disk specification file

Track Data:

Following the header is the data for each track.  The size of each track (in bytes) is
specified by bytes 2 and 3 of the disk header.

Each track has a 128 (0x80) byte header which contains an offset to each IDAM in the track.
This is created during format and should NEVER require modification. The actual track data
follows this header and can be viewed with a hex editor showing the raw data on the track.
Modification should not be done as each IDAM and sector has a CRC, this is just like a real
disk, and modifying the sector data without updating the CRC value will cause CRC errors when
accessing the virtual disk within the emulator.  

Track header:

Each side of each track has a 128 (80H) byte header which contains an offset pointer to each
IDAM in the track. This allows a maximum of 64 sector IDAMs/track. This is more than twice
what an 8 inch disk would require and 3.5 times that of a normal TRS-80 5 inch DD disk. This
should more than enough for any protected disk also.

These IDAM pointers MUST adhere to the following rules.

    Each pointer is a 2 byte offset to the 0xFE byte of the IDAM.
	In double byte single density the pointer is to the first 0xFE.
	
    The offset includes the 128 byte header. For example, an IDAM 10h
	bytes into the track would have a pointer of 90h, 10h+80h=90h.
	
    The IDAM offsets MUST be in ascending order with no unused or bad pointers.
	
    If all the entries are not used the header is terminated with a 0x0000 entry.
	Unused entries must also be zero filled..
	
    Any IDAMs overwritten during a sector write command should have their entry
	removed from the header and all other pointer entries shifted to fill in.
	
    The IDAM pointers are created during the track write command (format). A completed
	track write MUST remove all previous IDAM pointers. A partial track write (aborted
	with the forced interrupt command) MUST have it's previous pointers that were not
	overwritten added to the new IDAM pointers.
	
    The pointer bytes are stored in reverse order (LSB/MSB).

Each IDAM pointer has two flags.
	Bit 15 is set if the sector is double density.
	Bit 14 is currently undefined.

These bits must be masked to get the actual sector offset. For example, an offset to an
IDAM at byte 0x90 would be 0x0090 if single density and 0x8090 if double density.

Track data:

The actual track data follows the header and can be viewed with a hex editor showing the
raw data on the track. If the virtual disk doesn't have bits 6 or 7 set of byte 4 of the
disk header then each single density data byte is written twice, this includes IDAMs and
CRCs (the CRCs are calculated as if only 1 byte was written however). The IDAM and sector
data each have CRCs, this is just like on a real disk. 
*/
////////////////////////////////////////////////////////////////////////////////////

extern file* g_fTraceFile;

char* g_pszVersion = {"1.1.0"};

FdcType    g_FDC;
DriveType  g_dtDives[MAX_DRIVES];
TrackType  g_tdTrack;
SectorType g_stSector;

uint64_t g_nMaxSeekTime;

DWORD    g_dwPrevTraceCycleCount = 0;

file*    g_fOpenFile;
DIR      g_dj;				// Directory object
FILINFO  g_fno;				// File information
char     g_szBootConfig[80];
BYTE     g_byBootConfigModified;

char     g_szFindFilter[80];

#define FIND_MAX_SIZE 100

FILINFO g_fiFindResults[FIND_MAX_SIZE];
int     g_nFindIndex;
int     g_nFindCount;

////////////////////////////////////////////////////////////////////////////////////
void FdcProcessConfigEntry(char szLabel[], char* psz)
{
	if (strcmp(szLabel, "DRIVE0") == 0)
	{
		CopyString(psz, g_dtDives[0].szFileName, sizeof(g_dtDives[0].szFileName)-2);
	}
	else if (strcmp(szLabel, "DRIVE1") == 0)
	{
		CopyString(psz, g_dtDives[1].szFileName, sizeof(g_dtDives[1].szFileName)-2);
	}
	else if (strcmp(szLabel, "DRIVE2") == 0)
	{
		CopyString(psz, g_dtDives[2].szFileName, sizeof(g_dtDives[2].szFileName)-2);
	}
	else if (strcmp(szLabel, "DRIVE3") == 0)
	{
		CopyString(psz, g_dtDives[3].szFileName, sizeof(g_dtDives[3].szFileName)-2);
	}
}

//-----------------------------------------------------------------------------
int FdcGetDriveIndex(int nDriveSel)
{
	if (nDriveSel & 0x01)
	{
		return 0;
	}
	else if (nDriveSel & 0x02)
	{
		return 1;
	}
	else if (nDriveSel & 0x04)
	{
		return 2;
	}
	else if (nDriveSel & 0x08)
	{
		return 3;
	}
	
	return -1;
}

//----------------------------------------------------------------------------
BYTE FdcGetCommandType(BYTE byCommand)
{
	BYTE byType;
	
	if (g_FDC.byDriveSel == 0x0F)
	{
		return 2;
	}

	switch (byCommand >> 4)
	{
		case 0: // Restore
			byType = 1;
			break;

		case 1: // Seek
			byType = 1;
			break;

		case 2: // Step (don't update track register)
		case 3: // Step (update track register)
			byType = 1;
			break;

		case 4: // Step In (don't update track register)
		case 5: // Step In (update track register)
			byType = 1;
			break;

		case 6: // Step Out (don't update track register)
		case 7: // Step Out (update track register)
			byType = 1;
			break;

		case 8: // Read Sector (single record)
		case 9: // Read Sector (multiple record)
			byType = 2;
			break;

		case 10: // Write Sector (single record)
		case 11: // Write Sector (multiple record)
			byType = 2;
			break;

		case 12: // Read Address
			byType = 3;
			break;

		case 13: // Force Interrupt
			byType = 4;
			break;

		case 14: // Read Track
			byType = 3;
			break;

		case 15: // Write Track
			byType = 3;
			break;
	}	
	
	return byType;
}

////////////////////////////////////////////////////////////////////////////////////
// For Command Type I and IV
//  S7 - 1 = drive is not ready
//  S6 - 1 = media is write protected
//  S5 - 1 = head is loaded and engaged
//  S4 - 1 = seek error
//  S3 - 1 = CRC error
//  S2 - 1 = head is positioned over track zero
//  S1 - 1 = index mark detected (set once per rotation)
//  S0 - 1 = busy, command in progress
//
// For Command Type II and III
//  S7 - 1 = drive is not ready
//  S6 - x = not used on read; 1 on write and media is write protected;
//  S5 - x = on read indicates record type code, 1 = Deleted Mark; 0 = Data Mark;
//           on write it is set if there is a write fault;
//  S4 - 1 = record not found (desired track, sector or side not found)
//  S3 - 1 = CRC error
//  S2 - 1 = lost data, indicates computer did not respond to a DRQ in one byte time
//  S1 - x = copy of the DRQ output
//       1 = DR is full on read operation or empty on write operation
//  S0 - 1 = busy, command in progress
//
BYTE FdcGetStatus(void)
{
	BYTE byStatus;
	int  nDrive;

	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	if ((nDrive < 0) || (g_dtDives[nDrive].f == NULL))
	{
		return 0x20;
	}
	
	if ((g_FDC.byCommandType == 1) || // Restore, Seek, Step, Step In, Step Out
		(g_FDC.byCommandType == 4))   // Force Interrupt
	{
		byStatus = 0;
		
		// S0 (BUSY)
		if (g_FDC.stStatus.byBusy)
		{
			byStatus |= F_BUSY;
		}
		
		// S1 (INDEX) default to 0
		if (g_FDC.stStatus.byIndex)
		{
			byStatus |= F_INDEX;
		}

		// S2 (TRACK 0) default to 0
		if (g_FDC.byTrack == 0)
		{
			byStatus |= F_TRACK0; // set TRACK 0 status flag
		}

		// S3 (CRC ERROR) default to 0
		if (g_FDC.stStatus.byCrcError)
		{
			byStatus |= F_CRCERR;
		}

		// S4 (SEEK ERROR) default to 0
		if (g_FDC.stStatus.bySeekError)
		{
			byStatus |= F_SEEKERR;
		}
		
		byStatus |= F_HEADLOAD;

		// S6 (PROTECTED) default to 0
		if (g_FDC.stStatus.byProtected || (g_dtDives[nDrive].nDriveFormat == eHFE))
		{
			byStatus |= 0x40;
		}
		
		// S7 (NOT READY) default to 0
		if (g_FDC.stStatus.byNotReady)
		{
			byStatus |= 0x80;
		}
	}
	else if ((g_FDC.byCommandType == 2) ||	// Read Sector, Write Sector
			 (g_FDC.byCommandType == 3))	// Read Address, Read Track, Write Track
	{
		byStatus = 0;
		
		// S0 (BUSY)
		if (g_FDC.stStatus.byBusy)
		{
			byStatus |= F_BUSY;
		}
	
		// S1 (DATA REQUEST)     default to 0
		if (g_FDC.stStatus.byDataRequest)
		{
			byStatus |= F_DRQ;
		}

		// S2 (LOST DATA)        default to 0
		if (g_FDC.stStatus.byDataLost)
		{
			byStatus |= F_LOSTDATA;
		}
		
		// S3 (CRC ERROR)        default to 0
		if (g_FDC.stStatus.byCrcError)
		{
			byStatus |= F_BADDATA;
		}
		
		// S4 (RECORD NOT FOUND) default to 0
		if (g_FDC.stStatus.byNotFound)
		{
			byStatus |= F_NOTFOUND;
		}
	
		// S5 (RECORD TYPE)      default to 0
		if (g_FDC.stStatus.byRecordType == 0xF8) // if 0xF8 (Deleted Data Mark) then set status as deleted data
		{
			byStatus |= F_DELETED;
		}
		
		// S6 (PROTECTED) default to 0
		if (g_FDC.stStatus.byProtected)
		{
			byStatus |= 0x40;
		}
	
		// S7 (NOT READY) default to 0
		if (g_FDC.stStatus.byNotReady)
		{
			byStatus |= 0x80;
		}
	}
	else // Force Interrupt
	{
		byStatus = 0;
	}
	
	return byStatus;
}

//-----------------------------------------------------------------------------
int FdcGetTrackOffset(int nDrive, int nSide, int nTrack)
{
	int nOffset;
	
	nOffset = (nTrack * g_dtDives[nDrive].dmk.byNumSides + nSide) * g_dtDives[nDrive].dmk.wTrackLength + 16;

	return nOffset;
}	

//-----------------------------------------------------------------------------
// calculates the index of the ID Address Mark for the specified physical sector.
//
// returns the index of the 0xFE byte in the sector byte sequence 0xA1, 0xA1, 0xA1, 0xFE
// in the g_tdTrack.byTrackData[] address
//
int FdcGetIDAM_Index(int nSector)
{
	BYTE* pby;
	WORD  wIDAM;
	int   nSectorOffset;

	// get IDAM pointer for the specified track
	pby = g_tdTrack.byTrackData + nSector * 2;

	// get IDAM value for the specified track
	wIDAM = (*(pby+1) << 8) + *pby;

	// get offset from start of track to specified sector
	nSectorOffset = (wIDAM & 0x3FFF);

	return nSectorOffset;
}

//-----------------------------------------------------------------------------
// determines the index of the Data Address Mark for the specified locical sector.
//
// returns the index of the 0xFE byte in the sector byte sequence 0xA1, 0xA1, 0xA1, 0xFE
// in the g_tdTrack.byTrackData[] address for the specified sector.
//
int FdcGetSectorIDAM_Offset(int nSide, int nTrack, int nSector)
{
	BYTE* pby;
	int   i, nOffset;

	for (i = 0; i < 0x80; ++i)
	{
		nOffset = FdcGetIDAM_Index(i);

		// bySectorData[nOffset-3] should be 0xA1
		// bySectorData[nOffset-2] should be 0xA1
		// bySectorData[nOffset-1] should be 0xA1
		// bySectorData[nOffset]   should be 0xFE
		// bySectorData[nOffset+1] is track address (should be the same as the nTrack parameter)
		// bySectorData[nOffset+2] side number		(should be the same as the nSide parameter)
		// bySectorData[nOffset+3] sector number    (should be the same as the nSector parameter)
		// bySectorData[nOffset+4] byte length (log 2, minus seven), 0 => 128 bytes; 1 => 256 bytes; etc.

		pby = g_tdTrack.byTrackData + nOffset;

		if ((*(pby+1) == nTrack) && (*(pby+2) == nSide) && (*(pby+3) == nSector))
		{
			return nOffset;
		}
	}

	return -1;
}

//-----------------------------------------------------------------------------
// returns TRUE if the byte sequence starting at pbt is one of the following
//					- 0xA1, 0xA1, 0xA1, 0xFB
//					- 0xA1, 0xA1, 0xA1, 0xF8
//		   FALSE is it is not
//
BYTE FdcIsDataStartPatern(BYTE* pby)
{
	if (*pby != 0xA1)
	{
		return FALSE;
	}
	
	++pby;
	
	if (*pby != 0xA1)
	{
		return FALSE;
	}
	
	++pby;
	
	if (*pby != 0xA1)
	{
		return FALSE;
	}
	
	++pby;
	
	if ((*pby == 0xFB) || (*pby == 0xF8))
	{
		return TRUE;
	}

	return FALSE;
}

//-----------------------------------------------------------------------------
int FdcGetDAM_Offset(TrackType* ptdTrack, int nSectorIDAM)
{
	int nDataOffset = nSectorIDAM;
	
	if (nSectorIDAM < 0)
	{
		return -1;
	}

	// locate the byte sequence 0xA1, 0xA1, 0xA1, 0xFB/0xF8
	while (nDataOffset < ptdTrack->nTrackSize)
	{
		if (FdcIsDataStartPatern(g_tdTrack.byTrackData+nDataOffset))
		{
			return nDataOffset;
		}
		else
		{
			++nDataOffset;
		}
	}
	
	return -1;
}

//-----------------------------------------------------------------------------
void FdcFillSectorOffset(TrackType* ptdTrack)
{
	int i;
	
	for (i = 0; i < 0x80; ++i)
	{
		ptdTrack->nSectorIDAM[i] = FdcGetSectorIDAM_Offset(ptdTrack->nSide, ptdTrack->nTrack, i);
		ptdTrack->nSectorDAM[i]  = FdcGetDAM_Offset(ptdTrack, ptdTrack->nSectorIDAM[i]);
	}
}

//-----------------------------------------------------------------------------
void FdcReadDmkTrack(int nDrive, int nSide, int nTrack)
{
	int nTrackOffset;
	
	g_tdTrack.nType = eDMK;

	// check if specified track is already in memory
	if ((g_tdTrack.nDrive == nDrive) && (g_tdTrack.nSide == nSide) && (g_tdTrack.nTrack == nTrack))
	{
		return;
	}

	if ((nDrive < 0) || (nDrive >= MAX_DRIVES) || (g_dtDives[nDrive].f == NULL))
	{
		return;
	}

	nTrackOffset = FdcGetTrackOffset(nDrive, nSide, nTrack);

	FileSeek(g_dtDives[nDrive].f, nTrackOffset);
	FileRead(g_dtDives[nDrive].f, g_tdTrack.byTrackData, g_dtDives[nDrive].dmk.wTrackLength);

	g_tdTrack.nDrive     = nDrive;
	g_tdTrack.nSide      = nSide;
	g_tdTrack.nTrack     = nTrack;
	g_tdTrack.nTrackSize = g_dtDives[nDrive].dmk.wTrackLength;

	FdcFillSectorOffset(&g_tdTrack);
}

//-----------------------------------------------------------------------------
void FdcReadHfeTrack(int nDrive, int nSide, int nTrack)
{
	int nTrackOffset;
	
	g_tdTrack.nType = eHFE;

	// check if specified track is already in memory
	if ((g_tdTrack.nDrive == nDrive) && (g_tdTrack.nSide == nSide) && (g_tdTrack.nTrack == nTrack))
	{
		return;
	}

	if ((nDrive < 0) || (nDrive >= MAX_DRIVES) || (g_dtDives[nDrive].f == NULL))
	{
		return;
	}

	LoadHfeTrack(g_dtDives[nDrive].f, nTrack, nSide, &g_dtDives[nDrive].hfe, &g_tdTrack, g_tdTrack.byTrackData, sizeof(g_tdTrack.byTrackData));

	g_tdTrack.nDrive = nDrive;
	g_tdTrack.nSide  = nSide;
	g_tdTrack.nTrack = nTrack;
}

//-----------------------------------------------------------------------------
void FdcReadTrack(int nDrive, int nSide, int nTrack)
{
	switch (g_dtDives[nDrive].nDriveFormat)
	{
		case eDMK:
			FdcReadDmkTrack(nDrive, nSide, nTrack);
			break;

		case eHFE:
			FdcReadHfeTrack(nDrive, nSide, nTrack);
			break;
	}
}

//-----------------------------------------------------------------------------
int FindSectorIndex(int nSector, TrackType* ptrack)
{
	int i;

	// locate sector
	for (i = 0; i < MAX_SECTORS_PER_TRACK; ++i)
	{
		if (ptrack->byTrackData[ptrack->nSectorIDAM[i]+6] == nSector)
		{
			return i;
		}
	}

	return 0;
}

//-----------------------------------------------------------------------------
WORD FdcGetDmkSectorCRC(int nDrive, int nDataOffset)
{
	WORD wCRC16;

	wCRC16  = g_tdTrack.byTrackData[nDataOffset+g_dtDives[nDrive].dmk.nSectorSize+4] << 8;
	wCRC16 += g_tdTrack.byTrackData[nDataOffset+g_dtDives[nDrive].dmk.nSectorSize+5];

	return wCRC16;
}

//-----------------------------------------------------------------------------
void FdcReadDmkSector(int nDriveSel, int nSide, int nTrack, int nSector)
{
	BYTE* pby;
	WORD  wCRC16;
	int   nDrive, nDataOffset;

	nDrive = FdcGetDriveIndex(nDriveSel);
	
	if (nDrive < 0)
	{
		return;
	}

	FdcReadTrack(nDrive, nSide, nTrack);

	g_tdTrack.nFileOffset = FdcGetTrackOffset(nDrive, nSide, nTrack);

	// get pointer to start of sector data
	pby = g_tdTrack.byTrackData + g_tdTrack.nSectorIDAM[nSector];

	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-3] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-2] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-1] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+0] should be 0xFE
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+1] is track address (should be the same as the nTrack parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+2] side number		 (should be the same as the nSide parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+3] sector number    (should be the same as the nSector parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+4] byte length (log 2, minus seven), 0 => 128 bytes; 1 => 256 bytes; etc.

	g_stSector.nSectorSize = 128 << *(pby+4);
	g_dtDives[nDrive].dmk.nSectorSize = g_stSector.nSectorSize;

	// g_FDC.byTrackData[g_FDC.nSectorOffset+5..6] CRC (calculation starts with the three 0xA1/0xF5 bytes preceeding the 0xFE)
	g_FDC.stStatus.byCrcError = 0;

	if (g_tdTrack.nSectorIDAM[nSector] <= 0)
	{
		g_stSector.nSectorDataOffset = 0; // then there is a problem and we will let the Z80 deal with it
		g_FDC.stStatus.byRecordType  = 0;
		g_FDC.stStatus.byNotFound    = 1;
		return;
	}

	wCRC16 = Calculate_CRC_CCITT(pby-3, 8);
	
	if (wCRC16 != ((g_tdTrack.byTrackData[g_tdTrack.nSectorIDAM[nSector]+5] << 8) + g_tdTrack.byTrackData[g_tdTrack.nSectorIDAM[nSector]+6]))
	{
		g_FDC.stStatus.byCrcError = 1;
	}
	
	nDataOffset = g_tdTrack.nSectorDAM[nSector];	// offset to first bytes of the sector data mark sequence (0xA1, 0xA1, 0xA1, 0xFB/0xF8)
													//  - 0xFB (regular data); or
													//  - 0xF8 (deleted data)
													// actual data starts after the 0xFB/0xF8 byte
	if (nDataOffset < 0)
	{
		g_stSector.nSectorDataOffset = 0; // then there is a problem and we will let the Z80 deal with it
		g_FDC.stStatus.byRecordType  = 0;
		g_FDC.stStatus.byNotFound    = 1;
		return;
	}

	// nDataOffset is the index of the first 0xA1 byte in the 0xA1, 0xA1, 0xA1, 0xFB/0xF8 sequence

	g_FDC.byRecordMark           = g_tdTrack.byTrackData[nDataOffset+3];
	g_stSector.nSectorDataOffset = nDataOffset + 4;
	g_FDC.stStatus.byNotFound    = 0;
	g_FDC.stStatus.byRecordType  = 0xFB;	// will get set to g_FDC.byRecordMark after a few status reads

	// perform a CRC on the sector data (including preceeding 4 bytes) and validate
	wCRC16 = Calculate_CRC_CCITT(&g_tdTrack.byTrackData[nDataOffset], g_dtDives[nDrive].dmk.nSectorSize+4);

	if (wCRC16 != FdcGetDmkSectorCRC(nDrive, nDataOffset))
	{
		g_FDC.stStatus.byCrcError = 1;
	}
}

//-----------------------------------------------------------------------------
void FdcReadHfeSector(int nDriveSel, int nSide, int nTrack, int nSector)
{
	BYTE* pby;
	WORD  wCRC16;
	int   i, nDrive, nDataOffset;

	nDrive = FdcGetDriveIndex(nDriveSel);
	
	if (nDrive < 0)
	{
		return;
	}

	FdcReadTrack(nDrive, nSide, nTrack);

	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-3] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-2] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset-1] should be 0xA1 or 0xF5
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+0] should be 0xFE
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+1] is track address (should be the same as the nTrack parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+2] side number		 (should be the same as the nSide parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+3] sector number    (should be the same as the nSector parameter)
	// g_FDC.byTrackData[nSide][g_FDC.nTrackSectorOffset+4] byte length (log 2, minus seven), 0 => 128 bytes; 1 => 256 bytes; etc.
	// g_FDC.byTrackData[nSide][g_FDC.nSectorOffset+5..6] CRC (calculation starts with the three 0xA1/0xF5 bytes preceeding the 0xFE)
	g_FDC.stStatus.byCrcError = 0;

	i = FindSectorIndex(nSector, &g_tdTrack);

	g_FDC.byRecordMark           = g_tdTrack.byTrackData[g_tdTrack.nSectorDAM[i] + 3];
	g_stSector.nSectorDataOffset = g_tdTrack.nSectorDAM[FindSectorIndex(nSector, &g_tdTrack)] + 4;
	g_stSector.nSectorSize       = 128 << g_tdTrack.byTrackData[g_tdTrack.nSectorIDAM[i] + 7];
	g_FDC.stStatus.byNotFound    = 0;
	g_FDC.stStatus.byRecordType  = 0xFB;	// will get set to g_FDC.byRecordMark after a few status reads
}

//-----------------------------------------------------------------------------
void FdcReadSector(int nDriveSel, int nSide, int nTrack, int nSector)
{
	int nDrive;

	nDrive = FdcGetDriveIndex(nDriveSel);

	switch (g_dtDives[nDrive].nDriveFormat)
	{
		case eDMK:
			FdcReadDmkSector(nDriveSel, nSide, nTrack, nSector);
			break;

		case eHFE:
			FdcReadHfeSector(nDriveSel, nSide, nTrack, nSector);
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcMountDmkDrive(int nDrive)
{
	if (nDrive >= MAX_DRIVES)
	{
		return;
	}

	g_dtDives[nDrive].f = FileOpen(g_dtDives[nDrive].szFileName, FA_READ | FA_WRITE);

	if (g_dtDives[nDrive].f == NULL)
	{
		return;
	}

	g_dtDives[nDrive].nDriveFormat = eDMK;

	FileRead(g_dtDives[nDrive].f, g_dtDives[nDrive].dmk.byDmkDiskHeader, sizeof(g_dtDives[nDrive].dmk.byDmkDiskHeader));

	g_dtDives[nDrive].dmk.byWriteProtected = g_dtDives[nDrive].dmk.byDmkDiskHeader[0];
	g_dtDives[nDrive].byNumTracks          = g_dtDives[nDrive].dmk.byDmkDiskHeader[1];
	g_dtDives[nDrive].dmk.wTrackLength     = (g_dtDives[nDrive].dmk.byDmkDiskHeader[3] << 8) + g_dtDives[nDrive].dmk.byDmkDiskHeader[2];

	if (g_dtDives[nDrive].dmk.wTrackLength > MAX_TRACK_SIZE) // error (TODO: handle this gracefully)
	{
		g_dtDives[nDrive].dmk.wTrackLength = MAX_TRACK_SIZE - 1;
		return;
	}
	
	// determine number of sides for disk
	if ((g_dtDives[nDrive].dmk.byDmkDiskHeader[4] & 0x10) != 0)
	{
		g_dtDives[nDrive].dmk.byNumSides = 1;
	}
	else
	{
		g_dtDives[nDrive].dmk.byNumSides = 2;
	}

	// determine disk density
	if ((g_dtDives[nDrive].dmk.byDmkDiskHeader[4] & 0x40) != 0)
	{
		g_dtDives[nDrive].dmk.byDensity = eSD; // Single Density
	}
	else
	{
		g_dtDives[nDrive].dmk.byDensity = eDD; // Double Density
	}

	if ((g_dtDives[nDrive].dmk.byDmkDiskHeader[4] & 0x80) != 0) // then ignore denity setting and just use SD
	{
		g_dtDives[nDrive].dmk.byDensity = eSD; // Single Density
	}
	
	// bytes 0x05 - 0x0B are reserved
	
	// bytes 0x0B - 0x0F are zero for virtual disks; and 0x12345678 for real disks;

}

//-----------------------------------------------------------------------------
void FdcMountHfeDrive(int nDrive)
{
	if (nDrive >= MAX_DRIVES)
	{
		return;
	}

	g_dtDives[nDrive].f = FileOpen(g_dtDives[nDrive].szFileName, FA_READ | FA_WRITE);

	if (g_dtDives[nDrive].f == NULL)
	{
		return;
	}

	g_dtDives[nDrive].nDriveFormat = eHFE;

    FileRead(g_dtDives[nDrive].f, (BYTE*)&g_dtDives[nDrive].hfe.header, sizeof(g_dtDives[nDrive].hfe.header));
    FileSeek(g_dtDives[nDrive].f, g_dtDives[nDrive].hfe.header.track_list_offset*0x200);
    FileRead(g_dtDives[nDrive].f, (BYTE*)&g_dtDives[nDrive].hfe.trackLUT, sizeof(g_dtDives[nDrive].hfe.trackLUT));

	g_dtDives[nDrive].byNumTracks = g_dtDives[nDrive].hfe.header.number_of_tracks;
}

//-----------------------------------------------------------------------------
void FdcMountDrive(int nDrive)
{
	g_dtDives[nDrive].nDriveFormat = eUnknown;

	if (stristr(g_dtDives[nDrive].szFileName, ".dmk") != NULL)
	{
		FdcMountDmkDrive(nDrive);
	}
	else if (stristr(g_dtDives[nDrive].szFileName, ".hfe") != NULL)
	{
		FdcMountHfeDrive(nDrive);
	}
}

//-----------------------------------------------------------------------------
void FdcSaveBootCfg(char* pszIniFile)
{
	file* f;

	f = FileOpen("boot.cfg", FA_WRITE | FA_CREATE_ALWAYS);
	
	if (f == NULL)
	{
		return;
	}

	g_byBootConfigModified = TRUE;

	strcpy(g_szBootConfig, pszIniFile);
	FileWrite(f, pszIniFile, strlen(pszIniFile));
	FileClose(f);
}

//-----------------------------------------------------------------------------
void FdcLoadIni(void)
{
	file* f;
	char  szLine[256];
	char  szSection[16];
	char  szLabel[128];
	char* psz;
	int   nLen;

	g_byBootConfigModified = FALSE;

	// read the default ini file to load on init
	f = FileOpen("boot.cfg", FA_READ);
	
	if (f == NULL)
	{
		return;
	}

	// open the ini file specified in boot.cfg
	nLen = FileReadLine(f, (BYTE*)g_szBootConfig, sizeof(g_szBootConfig)-2);
	FileClose(f);

	f = FileOpen(g_szBootConfig, FA_READ);
	
	if (f == NULL)
	{
		return;
	}
	
	nLen = FileReadLine(f, (BYTE*)szLine, 126);
	
	while (nLen >= 0)
	{
		psz = SkipBlanks(szLine);
		
		if (*psz == '[')
		{
			CopySectionName(psz, szSection, sizeof(szSection)-1);
		}
		else if ((*psz != 0) && (*psz != ';')) // blank line or a comment line
		{
			StrToUpper(psz);
			psz = CopyLabelName(psz, szLabel, sizeof(szLabel)-1);
			FdcProcessConfigEntry(szLabel, psz);
		}

		nLen = FileReadLine(f, (BYTE*)szLine, 126);
	}
	
	FileClose(f);
}

//-----------------------------------------------------------------------------
void FdcInit(void)
{
	int i;

	memset(&g_FDC, 0, sizeof(g_FDC));
	g_FDC.stStatus.byBusy = 1;

	g_tdTrack.nDrive = -1;
	g_tdTrack.nSide  = -1;
	g_tdTrack.nTrack = -1;

	for (i = 0; i < MAX_DRIVES; ++i)
	{
		memset(&g_dtDives[i], 0, sizeof(DriveType));
	}

	FdcLoadIni();

	for (i = 0; i < MAX_DRIVES; ++i)
	{
		if (g_dtDives[i].szFileName[0] != 0)
		{
			FdcMountDrive(i);
		}
	}

	g_fOpenFile = NULL;
	
	g_FDC.byCommandReceived = 0;
	g_FDC.byCommandReg = 255;
	g_FDC.byCurCommand = 255;

	g_FDC.bySdCardPresent = sd_byCardInialized;

	g_nMaxSeekTime = 0;
}

//-----------------------------------------------------------------------------
void FdcReleaseCommandWait(void)
{
	DWORD dwBus;
	int   nCount;
	
	if (g_FDC.byReleaseWait == 0)
	{
		return;
	}

	g_FDC.byReleaseWait = 0;
	g_FDC.byWaitOutput  = 0;	// when 1 => wait line is being held low;

	// re-enable automatic WAIT generation
	ReleaseWait(); // allow pio state machine to resume
}

//-----------------------------------------------------------------------------
void FdcReleaseWait(void)
{
	DWORD dwBus;
	int   nCount;

	g_FDC.dwWaitTimeoutCount = 0;
	
	if (g_FDC.byWaitOutput)
	{
		g_FDC.byWaitOutput = 0;

		// re-enable automatic WAIT generation
		ReleaseWait(); // allow pio state machine to resume
	}
}

//-----------------------------------------------------------------------------
void FdcGenerateIntr(void)
{
	BYTE byNmiMaskReg = g_FDC.byNmiMaskReg;

	g_FDC.byNmiStatusReg = 0x7F; // inverted state of all bits low except INTRQ

	FdcReleaseWait();

	if ((byNmiMaskReg & 0x80) != 0)	// configured to generate NMI output
	{
		g_FDC.stStatus.byIntrRequest = 1;
		gpio_put(NMI_PIN, 1);
		gpio_put(NMI_PIN, 0);
	}
}

//-----------------------------------------------------------------------------
void FdcGenerateDRQ(void)
{
	g_FDC.stStatus.byDataRequest = 1;
	FdcReleaseWait();
}	

//-----------------------------------------------------------------------------
void FdcCloseAllFiles(void)
{
	int i;
	
	for (i = 0; i < MAX_DRIVES; ++i)
	{
		if (g_dtDives[i].f != NULL)
		{
			FileClose(g_dtDives[i].f);
			g_dtDives[i].f = NULL;
		}

		memset(&g_dtDives[i], 0, sizeof(DriveType));
	}
	
	if (g_fOpenFile != NULL)
	{
		FileClose(g_fOpenFile);
		g_fOpenFile = NULL;
	}
}

//-----------------------------------------------------------------------------
void FdcReset(void)
{
	FdcCloseAllFiles();
	FdcInit();
	FdcReleaseWait();
}

//-----------------------------------------------------------------------------
// Command code 0 0 0 0 h V r1 r0
//
// h = 1 - load head at begining; 0 - unload head at begining;
// V = 1 - verify on destination track
// r1/r0 - steppeing motor rate
//
void FdcProcessRestoreCommand(void)
{
	int nDrive;
	int nSide = 0;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.byTrack = 255;
	g_FDC.byCommandType = 1;
	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	FdcReadTrack(nDrive, nSide, 0);
	FdcReleaseCommandWait();

	g_FDC.stStatus.byBusy = 0; // clear busy flag
	g_FDC.byTrack         = 0;
	FdcGenerateIntr();
}

//-----------------------------------------------------------------------------
int GetStepRate(BYTE byCommandReg)
{
	int nStepRate = 3;

	switch (byCommandReg & 0x03)
	{
		case 0:
			nStepRate = 3;
			break;

		case 1:
			nStepRate = 6;
			break;

		case 2:
			nStepRate = 10;
			break;

		case 3:
			nStepRate = 15;
			break;
	}

	return nStepRate;
}

//-----------------------------------------------------------------------------
// Command code 0 0 0 1 h V r1 r0
//
// h = 1 - load head at begining; 0 - unload head at begining;
// V = 1 - verify on destination track
// r1/r0 - steppeing motor rate
//
// seek to the track specified in the data register
//
void FdcProcessSeekCommand(void)
{
	uint64_t nStart, nEnd, nDiff;
	int nTimeOut;
	int nDrive;
	int nStepRate;
	int nSide = 0;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.byCommandType = 1;
	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);
	
	if (nDrive != g_tdTrack.nDrive)
	{
		g_tdTrack.nDrive = -1;
	}

	if (g_FDC.byData >= g_dtDives[nDrive].byNumTracks)
	{
		g_FDC.stStatus.bySeekError = 1;
		g_FDC.stStatus.byBusy      = 0; // clear busy flag
		FdcReleaseCommandWait();
		FdcGenerateIntr();
		return;
	}

	nStepRate = GetStepRate(g_FDC.byCommandReg);

	if (g_FDC.byTrack > g_FDC.byData)
	{
		nTimeOut = nStepRate * (g_FDC.byTrack - g_FDC.byData);
	}
	else
	{
		nTimeOut = nStepRate * (g_FDC.byData - g_FDC.byTrack);
	}

	nStart = time_us_64();
	g_nWaitTime  = time_us_64() + (nTimeOut * 1000);

	FdcReadTrack(nDrive, nSide, g_FDC.byData);
	FdcReleaseCommandWait();

	nEnd  = time_us_64();
	nDiff = nEnd - nStart; // 0x41747 => 268103us (before optimization)

	if (nDiff > g_nMaxSeekTime)
	{
		 g_nMaxSeekTime = nDiff;
	}

//	while (time_us_64() < g_nWaitTime);

	g_FDC.byTrack = g_FDC.byData;
	g_FDC.stStatus.bySeekError = 0;
	g_FDC.stStatus.byBusy      = 0; // clear busy flag
	FdcGenerateIntr();
}

//-----------------------------------------------------------------------------
// Command code 0 0 1 u h V r1 r0
//
// u = 1 - update track register; 0 - do not update track register;
// h = 1 - load head at begining; 0 - unload head at begining;
// V = 1 - verify on destination track
// r1/r0 - steppeing motor rate
//
void FdcProcessStepCommand(void)
{
	int nDrive;
	int nStepRate;
	int nSide = 0;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.byCommandType = 1;
	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	if ((g_FDC.byCurCommand & 0x04) != 0) // perform verification
	{
		// TODO: peform what ever is needed
	}
	
	if ((g_FDC.byCurCommand & 0x10) != 0) // update flag set, then update track register
	{
		if ((g_FDC.nStepDir > 0) && (g_FDC.byTrack < 255))
		{
			++g_FDC.byTrack;
		}
		else if ((g_FDC.nStepDir < 0) && (g_FDC.byTrack > 0))
		{
			--g_FDC.byTrack;
		}
	}

	nStepRate   = GetStepRate(g_FDC.byCommandReg);
	g_nWaitTime = time_us_64() + (nStepRate * 1000);

	FdcReadTrack(nDrive, nSide, g_FDC.byTrack);
	FdcReleaseCommandWait();

	while (time_us_64() < g_nWaitTime);

	g_FDC.stStatus.bySeekError = 0;
	g_FDC.stStatus.byBusy = 0; // clear busy flag

	FdcGenerateIntr();
}

//-----------------------------------------------------------------------------
// Command code 0 1 0 u h V r1 r0
//
// u = 1 - update track register; 0 - do not update track register;
// h = 1 - load head at begining; 0 - unload head at begining;
// V = 1 - verify on destination track
// r1/r0 - steppeing motor rate
//
void FdcProcessStepInCommand(void)
{
	BYTE byData;
	int  nDrive;
	int  nStepRate;
	int  nSide = 0;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.byCommandType = 1;
	g_FDC.nStepDir = 1;

	if ((g_FDC.byCurCommand & 0x04) != 0) // perform verification
	{
		// TODO: peform what ever is needed
	}

	byData = g_FDC.byTrack;

	if (byData < 255)
	{
		++byData;
	}
	
	if ((g_FDC.byCurCommand & 0x10) != 0) // is u == 1 then update track register
	{
		g_FDC.byTrack = byData;
	}

	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);
	
	if (nDrive != g_tdTrack.nDrive)
	{
		g_tdTrack.nDrive = -1;
	}

	nStepRate   = GetStepRate(g_FDC.byCommandReg);
	g_nWaitTime = time_us_64() + (nStepRate * 1000);

	FdcReadTrack(nDrive, nSide, byData);
	FdcReleaseCommandWait();

	while (time_us_64() < g_nWaitTime);

	g_FDC.byTrack = byData;
	g_FDC.stStatus.bySeekError = 0;
	g_FDC.stStatus.byBusy = 0; // clear busy flag
	
	FdcGenerateIntr();
}

//-----------------------------------------------------------------------------
// Command code 0 1 1 u h V r1 r0
//
// u = 1 - update track register; 0 - do not update track register;
// h = 1 - load head at begining; 0 - unload head at begining;
// V = 1 - verify on destination track
// r1/r0 - steppeing motor rate
//
void FdcProcessStepOutCommand(void)
{
	BYTE byData;
	int  nDrive;
	int  nStepRate;
	int  nSide = 0;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.byCommandType = 1;
	g_FDC.nStepDir = -1;

	if ((g_FDC.byCurCommand & 0x04) != 0) // perform verification
	{
		// TODO: peform what ever is needed
	}

	byData = g_FDC.byTrack;

	if (byData > 0)
	{
		--byData;
	}
	
	if ((g_FDC.byCurCommand & 0x10) != 0) // is u == 1 then update track register
	{
		g_FDC.byTrack = byData;
	}

	nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);
	
	if (nDrive != g_tdTrack.nDrive)
	{
		g_tdTrack.nDrive = -1;
	}

	nStepRate   = GetStepRate(g_FDC.byCommandReg);
	g_nWaitTime = time_us_64() + (nStepRate * 1000);

	FdcReadTrack(nDrive, nSide, byData);
	FdcReleaseCommandWait();

	while (time_us_64() < g_nWaitTime);

	FdcGenerateIntr();
}

//-----------------------------------------------------------------------------
// Command code 1 0 0 m F2 E F1 0
//
// m  = 0 - single record read; 1 - multiple record read;
// F2 = 0 - compare for side 0; 1 - compare for side 1;
// E  = 0 - no delay; 1 - 15 ms delay;
// F1 = 0 - disable side select compare; 1 - enable side select compare;
//
void FdcProcessReadSectorCommand(void)
{
	int nSide  = 0;
	int nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	g_FDC.byCommandType = 2;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	FdcReadSector(g_FDC.byDriveSel, nSide, g_FDC.byTrack, g_FDC.bySector);
	FdcReleaseCommandWait();

	if (g_FDC.stStatus.byNotFound)
	{
		g_FDC.stStatus.byBusy = 0;
		return;
	}		
	
	g_FDC.nReadStatusCount       = 0;
	g_FDC.dwStateCounter         = 1000;
	g_FDC.stStatus.byDataRequest = 0;

	// number of byte to be transfered to the computer before
	// setting the Data Address Mark status bit (1 if Deleted Data)
	g_tdTrack.nReadSize     = g_stSector.nSectorSize;
	g_tdTrack.pbyReadPtr    = g_tdTrack.byTrackData + g_stSector.nSectorDataOffset;
	g_tdTrack.nReadCount    = g_tdTrack.nReadSize;
	g_FDC.nProcessFunction  = psReadSector;
	g_FDC.nServiceState     = 0;
	g_FDC.nDataRegReadCount = 0;

	// Note: computer now reads the data register for each of the sector data bytes
	//       once the last data byte is transfered status bit-5 is set if the
	//       Data Address Mark corresponds to a Deleted Data Mark.
	//
	//       Actual data transfer in handle in the FdcServiceRead() function.
}

//-----------------------------------------------------------------------------
// Command code 1 0 0 m F2 E F1 a0
//
// m  = 0 - single record read; 1 - multiple record read;
// F2 = 0 - compare for side 0; 1 - compare for side 1;
// E  = 0 - no delay; 1 - 15 ms delay;
// F1 = 0 - disable side select compare; 1 - enable side select compare;
// a0 = 0 - 0xFB (Data Mark); 1 - 0xF8 (Deleted Data Mark);
//
void FdcProcessWriteSectorCommand(void)
{
	int nSide = 0;
	int nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	g_FDC.byCommandType = 2;

	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	g_FDC.stStatus.byRecordType = 0xFB;

	g_FDC.nReadStatusCount = 0;

	// read specified sector so that it can be modified
	FdcReadSector(g_FDC.byDriveSel, nSide, g_FDC.byTrack, g_FDC.bySector);

	g_FDC.stStatus.byDataRequest = 0;
	g_stSector.nSector           = g_FDC.bySector;
	g_stSector.nSectorSize       = g_dtDives[nDrive].dmk.nSectorSize;
	g_tdTrack.nFileOffset        = FdcGetTrackOffset(nDrive, nSide, g_FDC.byTrack);
	g_tdTrack.pbyWritePtr        = g_tdTrack.byTrackData + g_stSector.nSectorDataOffset;
	g_tdTrack.nWriteCount        = g_stSector.nSectorSize;
	g_tdTrack.nWriteSize         = g_stSector.nSectorSize;	// number of byte to be transfered to the computer before
															// setting the Data Address Mark status bit (1 if Deleted Data)
	g_FDC.nProcessFunction       = psWriteSector;
	g_FDC.nServiceState          = 0;

	if ((g_FDC.byCurCommand & 0x01) == 0)
	{
		g_stSector.bySectorDataAddressMark = 0xFB;
	}
	else
	{
		g_stSector.bySectorDataAddressMark = 0xF8;
	}

	// Note: computer now writes the data register for each of the sector data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceWrite() function.
}

//-----------------------------------------------------------------------------
// Command code 1 1 0 0 0 E 0 0
//
// E = 1 - 15ms delay; 0 - no 15ms delay;
//
void FdcProcessReadAddressCommand(void)
{
	g_FDC.byCommandType = 3;
	
	// send the first ID field of the current track to the computer
	
	// Byte 1 : Track Address
	// Byte 2 : Side Number
	// Byte 3 : Sector Address
	// Byte 4 : Sector Length
	// Byte 5 : CRC1
	// Byte 6 : CRC2

	g_tdTrack.pbyReadPtr = &g_tdTrack.byTrackData[FdcGetIDAM_Index(0) + 1];
	g_tdTrack.nReadSize  = 6;
	g_tdTrack.nReadCount = 6;

	g_FDC.nReadStatusCount       = 0;
	g_FDC.dwStateCounter         = 1000;
	g_FDC.stStatus.byDataRequest = 0;

	// number of byte to be transfered to the computer before
	// setting the Data Address Mark status bit (1 if Deleted Data)
	g_FDC.nProcessFunction  = psReadSector;
	g_FDC.nServiceState     = 0;
	g_FDC.nDataRegReadCount = 0;

	// Note: CRC should be checked during transfer to the computer

}

//-----------------------------------------------------------------------------
// Command code 1 1 0 1 I3 I2 I1 I0
//
// Interrupt Condition flasg (Bits 3-1)
// ------------------------------------
// I0 = 1, Not-Ready to Ready Transition
// I1 = 1, Ready to Not-Ready Transition
// I2 = 1, Index Pulse
// I3 = 1, Immediate Interrupt
// I3-I0 = 0, Terminate with no Interrupt
//
void FdcProcessForceInterruptCommand(void)
{
	g_FDC.byCommandType  = 4;
	g_tdTrack.nReadSize  = 0;
	g_tdTrack.nReadCount = 0;
	g_tdTrack.nWriteSize = 0;
	g_FDC.byIntrEnable   = g_FDC.byCurCommand & 0x0F;
	memset(&g_FDC.stStatus, 0, sizeof(g_FDC.stStatus));
}

//-----------------------------------------------------------------------------
// Command code 1 1 1 0 0 E 0 0
//
// E = 1 - 15ms delay; 0 - no 15ms delay;
//
void FdcProcessReadTrackCommand(void)
{
	g_FDC.byCommandType = 3;
}

//-----------------------------------------------------------------------------
// Command code 1 1 1 1 0 E 0 0
//
// E = 1 - 15ms delay; 0 - no 15ms delay;
//
void FdcProcessWriteTrackCommand(void)
{
	int nSide = 0;

	g_FDC.byCommandType = 3;
	
	if ((g_FDC.byDriveSel & 0x10) != 0)
	{
		nSide = 1;
	}

	memset(g_tdTrack.byTrackData+0x80, 0, sizeof(g_tdTrack.byTrackData)-0x80);
	
	g_tdTrack.nDrive       = FdcGetDriveIndex(g_FDC.byDriveSel);
	g_tdTrack.nSide        = nSide;
	g_tdTrack.nTrack       = g_FDC.byTrack;
	g_tdTrack.pbyWritePtr  = g_tdTrack.byTrackData + 0x80;
	g_tdTrack.nWriteSize   = g_dtDives[g_tdTrack.nDrive].dmk.wTrackLength;
	g_tdTrack.nWriteCount  = g_tdTrack.nWriteSize;
	g_FDC.nProcessFunction = psWriteTrack;
	g_FDC.nServiceState    = 0;
}

//-----------------------------------------------------------------------------
void FdcProcessReadStatus(void)
{
	char szBuf[64];
	int  i;
	
	g_FDC.byCommandType = 2;

	g_FDC.nReadStatusCount  = 0;
	g_FDC.dwStateCounter    = 100000;
	g_FDC.nProcessFunction  = psSendData;
	g_FDC.nServiceState     = 0;
	g_FDC.stStatus.byDataRequest = 0;

	g_FDC.byTransferBuffer[0] = 0;
	g_FDC.byTransferBuffer[1] = 0;
	
	strcpy((char*)(g_FDC.byTransferBuffer+1), "Pico FDC Version ");
	strcat((char*)(g_FDC.byTransferBuffer+1), g_pszVersion);
	strcat((char*)(g_FDC.byTransferBuffer+1), "\r");
	strcat((char*)(g_FDC.byTransferBuffer+1), "BootIni=");
	strcat((char*)(g_FDC.byTransferBuffer+1), g_szBootConfig);
	strcat((char*)(g_FDC.byTransferBuffer+1), "\r");

	if (g_byBootConfigModified)
	{
		file* f;
		char  szLine[256];
		char  szSection[16];
		char  szLabel[128];
		char* psz;
		int   nLen, nLeft, nRead;

		f = FileOpen(g_szBootConfig, FA_READ);
		
		if (f == NULL)
		{
			strcat((char*)(g_FDC.byTransferBuffer+1), "Unable to open specified ini file");
		}
		else
		{
			nLen = FileReadLine(f, (BYTE*)szLine, nRead);
			
			while (nLen >= 0)
			{
				if (nLen > 2)
				{
					strcat_s((char*)(g_FDC.byTransferBuffer+1),  sizeof(g_FDC.byTransferBuffer)-2, szLine);
					strcat_s((char*)(g_FDC.byTransferBuffer+1),  sizeof(g_FDC.byTransferBuffer)-2, "\r");
				}

				nLen = FileReadLine(f, (BYTE*)szLine, 126);
			}
			
			FileClose(f);
		}
	}
	else
	{
		for (i = 0; i < MAX_DRIVES; ++i)
		{
			sprintf(szBuf, "%d: ", i);
			strcat((char*)(g_FDC.byTransferBuffer+1), szBuf);
			strcat((char*)(g_FDC.byTransferBuffer+1), g_dtDives[i].szFileName);
			strcat((char*)(g_FDC.byTransferBuffer+1), "\r");
		}
	}

	g_FDC.nTransferSize          = strlen((char*)(g_FDC.byTransferBuffer+1)) + 2;
	g_FDC.byTransferBuffer[0]    = g_FDC.nTransferSize;
	g_FDC.nTrasferIndex          = 0;
	g_FDC.stStatus.byDataRequest = 1;
	g_FDC.stStatus.byBusy        = 0;

	// Actual data transfer in handle in the FdcServiceSendData() function.
}

//-----------------------------------------------------------------------------
int FdcFileListCmp(const void * a, const void * b)
{
	FILINFO* f1 = (FILINFO*) a;
	FILINFO* f2 = (FILINFO*) b;

	return stricmp(f1->fname, f2->fname);
}

//-----------------------------------------------------------------------------
void FdcProcessFindFirst(char* pszFilter)
{
    FRESULT fr;  // Return value
	
	g_FDC.byCommandType = 2;

	g_nFindIndex = 0;
	g_nFindCount = 0;

	strcpy((char*)(g_FDC.byTransferBuffer+1), "too soon");
	g_FDC.byTransferBuffer[0] = strlen((char*)(g_FDC.byTransferBuffer+1)) + 2;

    memset(&g_dj, 0, sizeof(g_dj));
    memset(&g_fno, 0, sizeof(g_fno));
	memset(g_fiFindResults, 0, sizeof(g_fiFindResults));

	strcpy(g_szFindFilter, pszFilter);

    fr = f_findfirst(&g_dj, &g_fno, "0:", "*");

    if (FR_OK != fr)
	{
		strcpy((char*)(g_FDC.byTransferBuffer+1), "No matching file found.");
		g_FDC.byTransferBuffer[0] = strlen((char*)(g_FDC.byTransferBuffer+1));
		g_FDC.stStatus.byDataRequest = 0;
		g_FDC.stStatus.byBusy        = 0;
        return;
    }

	while ((fr == FR_OK) && (g_fno.fname[0] != 0) && (g_nFindCount < FIND_MAX_SIZE))
	{
		if ((g_fno.fattrib & AM_DIR) || (g_fno.fattrib & AM_SYS))
		{
			// pcAttrib = pcDirectory;
		}
		else
		{
			if ((g_szFindFilter[0] == '*') || (stristr(g_fno.fname, g_szFindFilter) != NULL))
			{
				memcpy(&g_fiFindResults[g_nFindCount], &g_fno, sizeof(FILINFO));
				++g_nFindCount;
			}
		}

		if (g_fno.fname[0] != 0)
		{
			fr = f_findnext(&g_dj, &g_fno); /* Search for next item */
		}
	}

	f_closedir(&g_dj);

	if (g_nFindCount > 0)
	{
		qsort(g_fiFindResults, g_nFindCount, sizeof(FILINFO), FdcFileListCmp);

		sprintf((char*)(g_FDC.byTransferBuffer+1), "%2d/%02d/%d %7d %s",
				((g_fiFindResults[g_nFindIndex].fdate >> 5) & 0xF) + 1,
				(g_fiFindResults[g_nFindIndex].fdate & 0xF) + 1,
				(g_fiFindResults[g_nFindIndex].fdate >> 9) + 1980,
				g_fiFindResults[g_nFindIndex].fsize,
				(char*)g_fiFindResults[g_nFindIndex].fname);

		++g_nFindIndex;
	}

	g_FDC.nTransferSize       = strlen((char*)(g_FDC.byTransferBuffer+1)) + 2;
	g_FDC.byTransferBuffer[0] = g_FDC.nTransferSize;
	g_FDC.nTrasferIndex       = 0;
	
	g_FDC.nReadStatusCount       = 0;
	g_FDC.dwStateCounter         = 100000;
	g_FDC.nProcessFunction       = psSendData;
	g_FDC.nServiceState          = 0;
	g_FDC.stStatus.byDataRequest = 1;
	g_FDC.stStatus.byBusy        = 0;

	// Actual data transfer in handle in the FdcServiceSendData() function.
}

//-----------------------------------------------------------------------------
void FdcProcessFindNext(void)
{
    FRESULT fr = FR_OK;  /* Return value */
	BYTE    bFound = FALSE;
	
	g_FDC.byCommandType = 2;

	if (g_nFindIndex < g_nFindCount)
	{
		sprintf((char*)(g_FDC.byTransferBuffer+1), "%2d/%02d/%d %7d %s",
				((g_fiFindResults[g_nFindIndex].fdate >> 5) & 0xF) + 1,
				(g_fiFindResults[g_nFindIndex].fdate & 0xF) + 1,
				(g_fiFindResults[g_nFindIndex].fdate >> 9) + 1980,
				g_fiFindResults[g_nFindIndex].fsize,
				(char*)g_fiFindResults[g_nFindIndex].fname);

		++g_nFindIndex;
	}
	else
	{
		g_FDC.byTransferBuffer[1] = 0;
	}
	
	g_FDC.nTransferSize       = strlen((char*)(g_FDC.byTransferBuffer+1)) + 2;
	g_FDC.byTransferBuffer[0] = g_FDC.nTransferSize;
	g_FDC.nTrasferIndex       = 0;
	
	g_FDC.nReadStatusCount       = 0;
	g_FDC.dwStateCounter         = 100000;
	g_FDC.nProcessFunction       = psSendData;
	g_FDC.nServiceState          = 0;
	g_FDC.stStatus.byDataRequest = 1;
	g_FDC.stStatus.byBusy        = 0;

	// Actual data transfer in handle in the FdcServiceSendData() function.
}

//-----------------------------------------------------------------------------
void FdcProcessMount(void)
{
	g_FDC.byCommandType          = 2;
	g_FDC.nReadStatusCount       = 0;
	g_FDC.stStatus.byDataRequest = 0;
	g_FDC.nProcessFunction       = psMountImage;
	g_FDC.nServiceState          = 0;

	// Note: computer now writes the data register for each of the command data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceMountImage() function.

}

//-----------------------------------------------------------------------------
void FdcProcessOpenFile(void)
{
	g_FDC.byCommandType          = 2;
	g_FDC.nReadStatusCount       = 0;
	g_FDC.stStatus.byDataRequest = 0;
	g_FDC.nProcessFunction       = psOpenFile;
	g_FDC.nServiceState          = 0;

	// Note: computer now writes the data register for each of the command data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceOpenFile() function.
	
}

//-----------------------------------------------------------------------------
void FdcProcessReadFile(void)
{
	g_FDC.byCommandType = 2;

	g_FDC.byTransferBuffer[0] = FileRead(g_fOpenFile, g_FDC.byTransferBuffer+1, 250);
	g_FDC.nTransferSize       = g_FDC.byTransferBuffer[0] + 1;
	g_FDC.nTrasferIndex       = 0;

	g_FDC.nReadStatusCount       = 0;
	g_FDC.dwStateCounter         = 100000;
	g_FDC.nProcessFunction       = psSendData;
	g_FDC.nServiceState          = 0;
	g_FDC.stStatus.byDataRequest = 1;
	g_FDC.stStatus.byBusy        = 0;

	// Actual data transfer in handle in the FdcServiceSendData() function.
}

//-----------------------------------------------------------------------------
void FdcProcessWriteFile(void)
{
	g_FDC.byCommandType          = 2;
	g_FDC.nReadStatusCount       = 0;
	g_FDC.stStatus.byDataRequest = 0;
	g_FDC.nProcessFunction       = psWriteFile;
	g_FDC.nServiceState          = 0;

	// Note: computer now writes the data register for each of the command data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceWriteFile() function.

}

//-----------------------------------------------------------------------------
void FdcProcessCloseFile(void)
{
	if (g_fOpenFile != NULL)
	{
		FileClose(g_fOpenFile);
		g_fOpenFile = NULL;
	}

	g_FDC.stStatus.byBusy  = 0; // clear busy flag
}

//-----------------------------------------------------------------------------
void FdcProcessSetTime(void)
{
	g_FDC.byCommandType          = 2;
	g_FDC.nReadStatusCount       = 0;
	g_FDC.stStatus.byDataRequest = 0;
	g_FDC.nProcessFunction       = psSetTime;
	g_FDC.nServiceState          = 0;

	// Note: computer now writes the data register for each of the command data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceSetTime() function.
	
}

//-----------------------------------------------------------------------------
void FdcProcessGetTime(void)
{
	CodedDateTime pdt;
	
	g_FDC.byCommandType          = 2;
	g_FDC.nReadStatusCount       = 100000;
	g_FDC.nProcessFunction       = psSendData;
	g_FDC.nServiceState          = 0;
	g_FDC.stStatus.byDataRequest = 1;
	g_FDC.stStatus.byBusy        = 0;

	CodeDateTime(g_dwForegroundRtc, &pdt);

	sprintf((char*)g_FDC.byTransferBuffer+1, "%02d/%02d/%02d %02d:%02d:%02d", pdt.month+1, pdt.day+1, (pdt.year+1980) % 100, pdt.hour, pdt.min, pdt.sec);

	g_FDC.byTransferBuffer[0] = strlen((char*)g_FDC.byTransferBuffer+1);
	g_FDC.nTransferSize       = g_FDC.byTransferBuffer[0] + 2;
	g_FDC.nTrasferIndex       = 0;
	
	// Note: computer now writes the data register for each of the command data bytes.
	//
	//       Actual data transfer is handled in the FdcServiceSendData() function.
	
}

//-----------------------------------------------------------------------------
void FdcProcessCommand(void)
{
	g_FDC.nServiceState     = 0;
	g_FDC.nProcessFunction  = psIdle;
	g_FDC.byCurCommand      = g_FDC.byCommandReg;
	g_FDC.byCommandReceived = 0;

	if (g_FDC.byDriveSel == 0x0F) // special request to this host processor
	{
		switch (g_FDC.byCurCommand)
		{
			case 1: // read firmware version
				FdcProcessReadStatus();
				break;
			
			case 2: // find first file
				FdcProcessFindFirst("*");
				break;
			
			case 3: // find next file
				FdcProcessFindNext();
				break;
			
			case 4: // Mount ini, dmk or hfe file
				FdcProcessMount();
				break;
			
			case 5: // Open file
				FdcProcessOpenFile();
				break;
			
			case 6: // read file block
				FdcProcessReadFile();
				break;
			
			case 7: // write file block
				FdcProcessWriteFile();
				break;

			case 8: // close file
				FdcProcessCloseFile();
				break;
			
			case 9: // set time
				FdcProcessSetTime();
				break;
			
			case 10: // get time
				FdcProcessGetTime();
				break;

			case 0x80:
				FdcProcessFindFirst(".INI");
				break;

			case 0x81:
				FdcProcessFindFirst(".DMK");
				break;

			case 0x82:
				FdcProcessFindFirst(".HFE");
				break;
		}
		
		FdcReleaseCommandWait();
		return;
	}
	
	switch (g_FDC.byCurCommand >> 4)
	{
		case 0: // Restore									(Type 1 Command)
			FdcProcessRestoreCommand();
			break;

		case 1: // Seek										(Type 1 Command)
			FdcProcessSeekCommand();
			break;

		case 2: // Step (don't update track register)		(Type 1 Command)
		case 3: // Step (update track register)				(Type 1 Command)
			FdcProcessStepCommand();
			break;

		case 4: // Step In (don't update track register)	(Type 1 Command)
		case 5: // Step In (update track register)			(Type 1 Command)
			FdcProcessStepInCommand();
			break;

		case 6: // Step Out (don't update track register)	(Type 1 Command)
		case 7: // Step Out (update track register)			(Type 1 Command)
			FdcProcessStepOutCommand();
			break;

		case 8: // Read Sector (single record)				(Type 2 Command)
		case 9: // Read Sector (multiple record)			(Type 2 Command)
			FdcProcessReadSectorCommand();
			break;

		case 10: // Write Sector (single record)			(Type 2 Command)
		case 11: // Write Sector (multiple record)			(Type 2 Command)
			FdcProcessWriteSectorCommand();
			break;

		case 12: // Read Address							(Type 3 Command)
			FdcProcessReadAddressCommand();
			break;

		case 13: // Force Interrupt							(Type 4 Command)
			FdcProcessForceInterruptCommand();
			break;

		case 14: // Read Track								(Type 3 Command)
			FdcProcessReadTrackCommand();
			break;

		case 15: // Write Track								(Type 3 Command)
			FdcProcessWriteTrackCommand();
			break;

		default:
			FdcReleaseCommandWait();
			memset(&g_FDC.stStatus, 0, sizeof(g_FDC.stStatus));
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceReadSector(void)
{
	switch (g_FDC.nServiceState)
	{
		case 0:
			g_FDC.nReadStatusCount = 0;
			++g_FDC.nServiceState;
			break;

		case 1: // wait for some nyumber of status reads before moving on
			g_FDC.stStatus.byRecordType = g_FDC.byRecordMark;
			g_FDC.nReadStatusCount      = 0;
			++g_FDC.nServiceState;
			break;

		case 2: // wait for 5 reads with the record mark provide in the read
			++g_FDC.nServiceState;
			g_FDC.dwStateCounter = 100;
			break;

		case 3: // wait for data byte to be read (if available)
			if (g_FDC.stStatus.byDataRequest != 0)	// wait for byte to be read by Z80
			{
				if (g_FDC.dwStateCounter == 0)		// don't wait forever
				{
					g_FDC.stStatus.byDataRequest = 0;
					g_FDC.stStatus.byDataLost    = 1;
				}
				
				break;
			}

			g_FDC.dwStateCounter = 100;
			++g_FDC.nServiceState;
			break;

		case 4:
			// if WAIT is not active then wait for DRVSEL write operation, but don't wait forever
			if ((g_tdTrack.nReadCount > 0) && (g_FDC.byWaitOutput == 0) && (g_FDC.dwStateCounter != 0))
			{
				break;
			}

			g_FDC.byData = *g_tdTrack.pbyReadPtr;
			g_FDC.dwStateCounter = 100;

			if (g_tdTrack.nReadCount > 0)
			{
				++g_tdTrack.pbyReadPtr;
				--g_tdTrack.nReadCount;

				if (g_tdTrack.nReadCount == 0)
				{
					g_FDC.nDataRegReadCount = 0;
				}

				FdcGenerateDRQ();
				--g_FDC.nServiceState;
				break;
			}

			g_FDC.nDrvSelWriteCount = 0;
			g_FDC.nReadStatusCount  = 0;
			g_FDC.dwStateCounter    = 20;
			++g_FDC.nServiceState;
			break;

		case 5:
			if (g_FDC.dwStateCounter != 0) // don't wait forever
			{
				break;
			}

			FdcGenerateIntr();

			g_FDC.stStatus.byBusy  = 0; // clear busy flag
			g_FDC.nProcessFunction = psIdle;
			++g_FDC.nServiceState;
			break;
	}
}

//-----------------------------------------------------------------------------
void WriteDmkSectorData(int nSector)
{
	int  nDataOffset;

	if ((g_tdTrack.nDrive < 0) || (g_tdTrack.nDrive >= MAX_DRIVES))
	{
		return;
	}

	if (g_dtDives[g_tdTrack.nDrive].f == NULL)
	{
		return;
	}

	// TODO: check to see if disk image is read only

	nDataOffset = g_tdTrack.nSectorDAM[nSector];

	if (nDataOffset < 0)
	{
		return;
	}
	
	FileSeek(g_dtDives[g_tdTrack.nDrive].f, g_tdTrack.nFileOffset+nDataOffset);
	FileWrite(g_dtDives[g_tdTrack.nDrive].f, g_tdTrack.byTrackData+nDataOffset, g_stSector.nSectorSize+6);
	FileFlush(g_dtDives[g_tdTrack.nDrive].f);
}

//-----------------------------------------------------------------------------
void WriteSectorData(int nSector)
{
	int nDrive = FdcGetDriveIndex(g_FDC.byDriveSel);

	switch (g_dtDives[nDrive].nDriveFormat)
	{
		case eDMK:
			WriteDmkSectorData(nSector);
			break;

		case eHFE:
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcGenerateSectorCRC(int nSector, int nSectorSize)
{
	WORD wCRC16;
	int  nDataOffset;

	// now locate the 0xA1, 0xA1, 0xA1, 0xFB sequence that marks the start of sector data

	nDataOffset = g_tdTrack.nSectorDAM[nSector];
	
	if (nDataOffset < 0)
	{
		return;
	}

	// CRC consists of the 0xA1, 0xA1, 0xA1, 0xFB sequence and the sector data
	wCRC16 = Calculate_CRC_CCITT(&g_tdTrack.byTrackData[nDataOffset], nSectorSize+4);
		
	g_tdTrack.byTrackData[nDataOffset+nSectorSize+4] = wCRC16 >> 8;
	g_tdTrack.byTrackData[nDataOffset+nSectorSize+5] = wCRC16 & 0xFF;
}

//-----------------------------------------------------------------------------
void FdcUpdateDataAddressMark(int nSector, int nSectorSize)
{
	int  nDataOffset, i;

	// get offset of the 0xA1, 0xA1, 0xA1, 0xFB sequence that marks the start of sector data

	nDataOffset = g_tdTrack.nSectorDAM[nSector];

	if (nDataOffset < 0)
	{
		return;
	}

	// nDataOffset is the index of the first 0xA1 byte in the 0xA1, 0xA1, 0xA1, 0xFB sequence

	// update sector data mark (0xFB/0xF8)

	for (i = 0; i < 4; ++i)
	{
		if (g_tdTrack.byTrackData[nDataOffset+i] != 0xA1)
		{
			g_tdTrack.byTrackData[nDataOffset+i] = g_stSector.bySectorDataAddressMark;
			i = 4;
		}
	}
}

//-----------------------------------------------------------------------------
void FdcServiceWriteSector(void)
{
	switch (g_FDC.nServiceState)
	{
		case 0:
			if ((g_FDC.nReadStatusCount < 25) && (g_FDC.dwStateCounter != 0))
			{
				break;
			}

			// indicate to the Z80 that we are ready for the first data byte
			FdcGenerateDRQ();
			++g_FDC.nServiceState;
			break;
		
		case 1:
			if (g_FDC.stStatus.byDataRequest != 0) // wait for byte to be placed in the data register by the Z80
			{
				break;
			}

			g_FDC.dwStateCounter = 1000;
			++g_FDC.nServiceState;
			break;

		case 2:
			if ((g_FDC.byWaitOutput == 0) && (g_FDC.dwStateCounter != 0)) // don't wait forever
			{
				break;
			}

			*g_tdTrack.pbyWritePtr = g_FDC.byData;

			if (g_tdTrack.nWriteCount > 0)
			{
				++g_tdTrack.pbyWritePtr;
				--g_tdTrack.nWriteCount;

				if (g_tdTrack.nWriteCount == 0)
				{
					++g_FDC.nServiceState;
					break;
				}
				else if (g_tdTrack.nWriteCount == 1)
				{
					g_FDC.byHoldWaitOnDataWrite = 1; // request that wait be held after last data write in the write sequnce
				}

				--g_FDC.nServiceState;
				FdcGenerateDRQ();
				break;
			}

			g_FDC.dwStateCounter = 200;
			++g_FDC.nServiceState;
			break;

		case 3:
			FdcUpdateDataAddressMark(g_stSector.nSector, g_stSector.nSectorSize);
			
			// perform a CRC on the sector data (including preceeding 4 bytes) and update sector CRC value
			FdcGenerateSectorCRC(g_stSector.nSector, g_stSector.nSectorSize);
			
			// flush sector to SD-Card
			WriteSectorData(g_stSector.nSector);
		
			FdcReleaseWait();
		
			++g_FDC.nServiceState;
			g_FDC.dwStateCounter = 200;
			break;
		
		case 4:
			if (g_FDC.dwStateCounter != 0)
			{
				break;
			}

			FdcGenerateIntr();

			g_FDC.stStatus.byBusy  = 0; // clear busy flag
			g_FDC.nServiceState    = 0;
			g_FDC.nProcessFunction = psIdle;
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcProcessTrackData(TrackType* ptdTrack)
{
	BYTE* pbyCrcStart = ptdTrack->byTrackData;
	BYTE* pby = ptdTrack->byTrackData;
	WORD  wCRC16;
	int   i;

	for (i = 0; i < ptdTrack->nTrackSize; ++i)
	{
		switch (*pby)
		{
			case 0xF5:
				pbyCrcStart = pby;
				*pby = 0xA1;
				break;
			
			case 0xF6:
				*pby = 0xC2;
				break;

			case 0xF7:
				wCRC16 = Calculate_CRC_CCITT(pbyCrcStart-2, pby-pbyCrcStart+2);
				*pby = wCRC16 >> 8;
				++pby;
				++i;
				*pby = wCRC16 & 0xFF;
				break;
		}
		
		++pby;
	}
}

//-----------------------------------------------------------------------------
void FdcBuildIdamTable(TrackType* ptdTrack)
{
	BYTE* pbyTrackData = ptdTrack->byTrackData;
	BYTE  byDensity    = g_dtDives[ptdTrack->nDrive].dmk.byDensity;
	BYTE  byFound;
	int   nIndex, nIDAM;
	int   nTrackSize = ptdTrack->nTrackSize;

	// reset IDAM table to 0's
	memset(pbyTrackData, 0, 0x80);

	// search track data for sectors (start at first byte after the last IDAM index)
	nIndex = 128;
	nIDAM  = 0;

	while (nIndex < nTrackSize)
	{
		byFound = 0;

		while ((byFound == 0) && (nIndex < nTrackSize))
		{
			if ((pbyTrackData[nIndex] == 0xA1) && (pbyTrackData[nIndex+1] == 0xA1) && (pbyTrackData[nIndex+2] == 0xA1) && (pbyTrackData[nIndex+3] == 0xFE))
			{
				byFound = 1;
			}
			else
			{
				++nIndex;
			}
		}

		if (byFound) // then we found the byte sequence 0xA1, 0xA1, 0xA1, 0xFE
		{
			// at this point nIndex contains the location of the first 0xA1 byte

			// advance to the 0xFE byte. 
			nIndex += 3; // The IDAM pointer is the offset from the start of track data to the 0xFE of the associated sector.

			*(pbyTrackData + nIDAM * 2)     = nIndex & 0xFF;
			*(pbyTrackData + nIDAM * 2 + 1) = nIndex >> 8;

			if (byDensity == eDD) // Double Density
			{
				*(pbyTrackData + nIDAM * 2 + 1) |= 0x80;
			}

			++nIDAM;
		}
	}
}

//-----------------------------------------------------------------------------
void FdcWriteDmkTrack(TrackType* ptdTrack)
{
	if ((ptdTrack->nDrive < 0) || (ptdTrack->nDrive >= MAX_DRIVES))
	{
		return;
	}

	if (g_dtDives[ptdTrack->nDrive].f == NULL)
	{
		return;
	}

	ptdTrack->nFileOffset = FdcGetTrackOffset(ptdTrack->nDrive, ptdTrack->nSide, ptdTrack->nTrack);

	FileSeek(g_dtDives[ptdTrack->nDrive].f, ptdTrack->nFileOffset);
	FileWrite(g_dtDives[ptdTrack->nDrive].f, ptdTrack->byTrackData, ptdTrack->nTrackSize);
	FileFlush(g_dtDives[ptdTrack->nDrive].f);
}

//-----------------------------------------------------------------------------
void FdcWriteTrack(TrackType* ptdTrack)
{
	switch (ptdTrack->nType)
	{
		case eDMK:
			FdcWriteDmkTrack(ptdTrack);
			break;

		case eHFE:
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceWriteTrack(void)
{
	switch (g_FDC.nServiceState)
	{
		case 0:
			if ((g_FDC.nReadStatusCount < 25) && (g_FDC.dwStateCounter != 0))
			{
				break;
			}

			// indicate to the Z80 that we are ready for the first data byte
			FdcGenerateDRQ();
			++g_FDC.nServiceState;
			break;
		
		case 1:
			if (g_FDC.stStatus.byDataRequest != 0) // wait for byte to be placed in the data register by the Z80
			{
				break;
			}

			g_FDC.dwStateCounter = 1000;
			++g_FDC.nServiceState;
			break;

		case 2:
			if ((g_FDC.byWaitOutput == 0) && (g_FDC.dwStateCounter != 0)) // don't wait forever
			{
				break;
			}

			*g_tdTrack.pbyWritePtr = g_FDC.byData;

			if (g_tdTrack.nWriteCount > 0)
			{
				++g_tdTrack.pbyWritePtr;
				--g_tdTrack.nWriteCount;

				if (g_tdTrack.nWriteCount == 0)
				{
					++g_FDC.nServiceState;
					break;
				}
				else if (g_tdTrack.nWriteCount == 1)
				{
					g_FDC.byHoldWaitOnDataWrite = 1; // request that wait be held after last data write in the write sequnce
				}

				--g_FDC.nServiceState;
				FdcGenerateDRQ();
				break;
			}

			g_FDC.dwStateCounter = 200;
			++g_FDC.nServiceState;
			break;

		case 3:
			FdcProcessTrackData(&g_tdTrack);	// scan track data to generate CRC values
			FdcBuildIdamTable(&g_tdTrack);		// scan track data to build the IDAM table
			FdcFillSectorOffset(&g_tdTrack);

			// flush track to SD-Card
			FdcWriteTrack(&g_tdTrack);
			FdcReleaseWait();
		
			g_FDC.dwStateCounter = 200;
			++g_FDC.nServiceState;
			break;

		case 4:
			if (g_FDC.dwStateCounter != 0)
			{
				break;
			}

			FdcGenerateIntr();

			g_FDC.stStatus.byBusy  = 0; // clear busy flag
			g_FDC.nServiceState    = 0;
			g_FDC.nProcessFunction = psIdle;
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceMountImage(void)
{
	static int nIndex;
	static int nSize;
	char* psz;
	int   nDrive;

	switch (g_FDC.nServiceState)
	{
		case 0:
			g_FDC.dwStateCounter         = 100000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 1: // first byte received is the size of the data to be received
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			nSize  = g_FDC.byData;
			nIndex = 0;
			
			g_FDC.dwStateCounter         = 100000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 2: // now request each data byte
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			g_FDC.byTransferBuffer[nIndex] = g_FDC.byData;
			++nIndex;
			
			if (nIndex < nSize) // request next byte
			{
				g_FDC.stStatus.byDataRequest = 1;
			}
			else // mount specified imgae onto specified drive
			{
				g_FDC.nProcessFunction = psIdle;
				
				// locate the drive number
				psz = SkipBlanks((char*)g_FDC.byTransferBuffer);
				nDrive = atoi(psz);

				psz = SkipToBlank((char*)psz);

				if (*psz != ' ')
				{
					break;
				}

				psz = SkipBlanks((char*)psz);

				if ((nDrive < 0) || (nDrive >= MAX_DRIVES))
				{
					break;
				}
				
				// if test if it is an ini file
				if (stristr(g_FDC.byTransferBuffer, ".ini"))
				{
					FdcSaveBootCfg((char*)psz);
				}
				else if (FileExists((char*)psz))
				{
					strcpy(g_dtDives[nDrive].szFileName, (char*)psz);
					FileClose(g_dtDives[nDrive].f);
					g_dtDives[nDrive].f = NULL;
					FdcMountDrive(nDrive);
				}
			}
			
			g_FDC.dwStateCounter  = 100000;
			g_FDC.stStatus.byBusy = 0;
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceOpenFile(void)
{
	static int nIndex;
	static int nSize;
	BYTE  byMode;
	char* psz;

	switch (g_FDC.nServiceState)
	{
		case 0:
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 1: // first byte received is the size of the data to be received
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			nSize  = g_FDC.byData;
			nIndex = 0;
			
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 2: // now request each data byte
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			g_FDC.byTransferBuffer[nIndex] = g_FDC.byData;
			++nIndex;
			
			if (nIndex < nSize) // request next byte
			{
				g_FDC.stStatus.byDataRequest = 1;
			}
			else // mount specified imgae onto specified drive
			{
				g_FDC.nProcessFunction = psIdle;

				// locate the mode to open the file
				psz = (char*)g_FDC.byTransferBuffer;
				nIndex = 0;
				byMode = 0;

				while ((*psz != 0) && (*psz != ',') && (nIndex < nSize))
				{
					if (*psz == '/') // replace / with .
					{
						*psz = '.';
					}
					
					if (*psz == ':') // strip off drive specification
					{
						*psz = 0;
					}
					
					++psz;
					++nIndex;
				}
				
				if (*psz != ',')
				{
					break;
				}

				*psz = 0;
				++psz;
				
				while (*psz != 0)
				{
					if (*psz == 'r')
					{
						byMode |= FA_READ;
					}
					else if (*psz == 'w')
					{
						byMode |= FA_WRITE;
					}

					++psz;
				}
				
				if (g_fOpenFile != NULL)
				{
					FileClose(g_fOpenFile);
				}
				
				g_fOpenFile = FileOpen((char*)g_FDC.byTransferBuffer, byMode);
			}
			
			g_FDC.dwStateCounter  = 10000;
			g_FDC.stStatus.byBusy = 0;
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceWriteFile(void)
{
	static int nIndex;
	static int nSize;

	switch (g_FDC.nServiceState)
	{
		case 0:
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 1: // first byte received is the size of the data to be received
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			nSize  = g_FDC.byData;
			nIndex = 0;
			
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 2: // now request each data byte
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			g_FDC.byTransferBuffer[nIndex] = g_FDC.byData;
			++nIndex;
			
			if (nIndex < nSize) // request next byte
			{
				g_FDC.stStatus.byDataRequest = 1;
			}
			else // write received data to previously opened file
			{
				g_FDC.nProcessFunction = psIdle;
				
				if (g_fOpenFile != NULL)
				{
					FileWrite(g_fOpenFile, g_FDC.byTransferBuffer, nSize);
				}
			}
			
			g_FDC.dwStateCounter  = 10000;
			g_FDC.stStatus.byBusy = 0;
			break;
	}
}

//-----------------------------------------------------------------------------
void FdcServiceSetTime(void)
{
	static int nIndex;
	static int nSize;
	CodedDateTime dt;
	
	switch (g_FDC.nServiceState)
	{
		case 0:
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 1: // first byte received is the size of the data to be received
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			nSize  = g_FDC.byData;
			nIndex = 0;
			
			g_FDC.dwStateCounter         = 10000;
			g_FDC.stStatus.byDataRequest = 1;
			g_FDC.stStatus.byBusy        = 0;
			++g_FDC.nServiceState;
			break;

		case 2: // now request each data byte
			if (g_FDC.dwStateCounter == 0) // don't wait forever
			{
				g_FDC.nProcessFunction = psIdle;
				break;
			}

			if (g_FDC.stStatus.byDataRequest != 0)
			{
				break;
			}
			
			g_FDC.byTransferBuffer[nIndex] = g_FDC.byData;
			++nIndex;
			
			if (nIndex < nSize) // request next byte
			{
				g_FDC.stStatus.byDataRequest = 1;
			}
			else // set the RTC based on the string in g_FDC.g_byTransferBuffer
			{
				ParseDateTime((char*)g_FDC.byTransferBuffer, &dt);
				g_dwBackgroundRtc = EncodeDateTime(&dt);
				g_FDC.nProcessFunction = psIdle;
			}
			
			g_FDC.dwStateCounter  = 10000;
			g_FDC.stStatus.byBusy = 0;
			break;
	}
}

//-----------------------------------------------------------------------------
// primary data transfer is handled in fdc_isr()
void FdcServiceSendData(void)
{
	if (g_FDC.dwStateCounter != 0) // don't wait forever
	{
		return;
	}

	g_FDC.stStatus.byDataRequest = 0;
	g_FDC.stStatus.byBusy        = 0;
	g_FDC.nProcessFunction       = psIdle;
}

//-----------------------------------------------------------------------------
void FdcServiceStateMachine(void)
{
	// test is we have has a reset pulse of at least 0.5ms
	if (g_FDC.byResetFDC)
	{
		FdcCloseAllFiles();
		system_reset();
		return;
	}

	TestSdCardInsertion();

	if (g_FDC.bySdCardPresent != sd_byCardInialized)
	{
		if (g_FDC.bySdCardPresent) // card has been removed
		{
			FdcCloseAllFiles();
		}
		else // card has been inserted and initialized
		{
			FdcInit();
		}

		g_FDC.bySdCardPresent = sd_byCardInialized;
	}

	// check if we have a command to process
	if (g_FDC.byCommandReceived != 0)
	{
		FdcProcessCommand();
		return;
	}

	switch (g_FDC.nProcessFunction)
	{
		case psIdle:
			break;
		
		case psReadSector:
			FdcServiceReadSector();
			break;
		
		case psWriteSector:
			FdcServiceWriteSector();
			break;
		
		case psWriteTrack:
			FdcServiceWriteTrack();
			break;
		
		case psSendData:
			FdcServiceSendData();
			break;
		
		case psMountImage:
			FdcServiceMountImage();
			break;
		
		case psOpenFile:
			FdcServiceOpenFile();
			break;
		
		case psWriteFile:
			FdcServiceWriteFile();
			break;
		
		case psSetTime:
			FdcServiceSetTime();
			break;
	}
}
