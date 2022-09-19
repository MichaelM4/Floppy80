
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
//
// For HFE format see SDCard_HxC_Floppy_Emulator_HFE_file_format.pdf
//
////////////////////////////////////////////////////////////////////////////////////

BYTE g_byRawTrackData[MAX_TRACK_LEN*2];
int  g_nHfeSide;

// track modification address range. used to determine what sections of g_byRawTrackData
// should be written back to the disk image.
int  g_nHfeLowWriteAddress;
int  g_nHfeHighWriteAddress;

////////////////////////////////////////////////////////////////////////////////////
// [    512     ] [    512     ] [    512     ] [    512     ] 
// [Side0][Side1] [Side0][Side1] [Side0][Side1] [Side0][Side1]
// [ 256 ][ 256 ] [ 256 ][ 256 ] [ 256 ][ 256 ] [ 256 ][ 256 ]
BYTE __not_in_flash_func(GetHfeByte)(int nPos)
{
	//   =  (nPos / 256) * 512        +  nPos % 256
	nPos = ((nPos & 0xFFFFFF00) << 1) | (nPos & 0xFF);

	if (g_nHfeSide != 0)
	{
		nPos += 256;
	}

	return g_byRawTrackData[nPos];
}

////////////////////////////////////////////////////////////////////////////////////
// [    512     ] [    512     ] [    512     ] [    512     ] 
// [Side0][Side1] [Side0][Side1] [Side0][Side1] [Side0][Side1]
// [ 256 ][ 256 ] [ 256 ][ 256 ] [ 256 ][ 256 ] [ 256 ][ 256 ]
void __not_in_flash_func(PutHfeByte)(int nPos, BYTE by)
{
	//   =  (nPos / 256) * 512        +  nPos % 256
	nPos = ((nPos & 0xFFFFFF00) << 1) | (nPos & 0xFF);

	if (g_nHfeSide != 0)
	{
		nPos += 256;
	}

	// update track write pointers
	if (nPos > g_nHfeHighWriteAddress)
	{
		g_nHfeHighWriteAddress = nPos;
	}
	
	if (nPos < g_nHfeLowWriteAddress)
	{
		g_nHfeLowWriteAddress = nPos;
	}

	g_byRawTrackData[nPos] = by;
}

////////////////////////////////////////////////////////////////////////////////////
void write_next_mfm(int* pbitpos, unsigned short* pmfm)
{
	int off = (*pbitpos >> 3);
	int bit = (*pbitpos & 7);
	BYTE by = GetHfeByte(*pbitpos);
 
	unsigned char mask = (1 << bit);

	if (*pmfm & 0x8000)
	{
		by |= mask;
	}
	else
	{
		by &= ~mask;
	}

	PutHfeByte(*pbitpos, by);

	*pmfm <<= 1;
    
	++*pbitpos;
}

////////////////////////////////////////////////////////////////////////////////////
void write_next_fm(int* pbitpos, unsigned int* pfm)
{
	int off = (*pbitpos >> 3);
	int bit = (*pbitpos & 7);
 	BYTE by = GetHfeByte(*pbitpos);

	unsigned char mask = (1 << bit);

	if (*pfm & 0x80000000)
	{
		by |= mask;
	}
	else
	{
		by &= ~mask;
	}

	PutHfeByte(*pbitpos, by);

	*pfm <<= 1;

	++*pbitpos;
}

////////////////////////////////////////////////////////////////////////////////////
unsigned int mfm_encode(int prev_bit, int data, int skip_clock)
{
    unsigned int res = 0;
    int prev_zero = 0;

    if (!prev_bit)
	{
        prev_zero = 2;
    }

    for (int i = 7; i >= 0; i--)
    {
        res <<= 2;

        int bit = (data >> i) & 1;
        int clk = (((skip_clock >> i) & 1) << 1) ^ 2;

        if (bit)
		{
            res |= 1;           // always 01
            prev_zero = 0;
        }
		else
		{
            res |= (prev_zero & clk); // 00 unless previous zero then 10 (but skip_clock can override)
            prev_zero = 2;
        }
    }
    
    return res;
}

////////////////////////////////////////////////////////////////////////////////////
unsigned int fm_encode(int data, int clock)
{
	unsigned int fm = 0;

	for (int i = 0; i < 8; i++)
	{
		fm <<= 2;

		if (clock & 0x80)
		{
			fm |= FM_CLOCK;
		}

		clock <<= 1;

		fm <<= 2;

		if (data & 0x80)
		{
			fm |= FM_DATA_ONE;
		}

		data <<= 1;
	}

	return fm;
}

////////////////////////////////////////////////////////////////////////////////////
unsigned char mfm_write(int* pbitpos, unsigned char data, unsigned char skip, unsigned char prev)
{
	unsigned short mfm;

	mfm = mfm_encode(prev & 1, data, skip);
	
	for (int k = 0; k < 16; k++)
	{
		write_next_mfm(pbitpos, &mfm);
	}

	return data;
}

////////////////////////////////////////////////////////////////////////////////////
void fm_write(int* pbitpos, unsigned char data, unsigned char clock)
{
	unsigned int fm = fm_encode(data, clock);

	for (int k = 0; k < 32; k++)
	{
		write_next_fm(pbitpos, &fm);
	}
}

////////////////////////////////////////////////////////////////////////////////////
unsigned char __not_in_flash_func(sep_mfm)(unsigned short mfm)
{
    register int i;
    register char data;
    register unsigned short fm;

	data = 0;
    fm   = mfm << 1;

	for (i = 0; i < 8; i++)
	{
		data <<= 1;

        if (fm & 0x8000)
        {
            data |= 1;
        }

		fm <<= 2;
	}

	return data;
}

////////////////////////////////////////////////////////////////////////////////////
unsigned char __not_in_flash_func(read_byte_mfm)(int* pbitpos, unsigned short* pmfm)
{
    unsigned char data;
    register int off, bit;
    register unsigned short mfm;
    register unsigned char  by;

    data = sep_mfm(*pmfm);

    mfm = *pmfm;
    off = (*pbitpos >> 3);
	by  = GetHfeByte(off);
    bit = 1 << (*pbitpos & 7);

    for (int i = 0; i < 16; i++)
	{
        mfm <<= 1;

        if (by & bit)
        {
            mfm |= 1;
        }

        bit <<= 1;

        if (bit >= 0x100)
        {
            ++off;
			by  = GetHfeByte(off);
            bit = 1;
        }
    }
    
    *pbitpos += 16;
    *pmfm = mfm;
    return data;
}

////////////////////////////////////////////////////////////////////////////////////
//void __not_in_flash_func(LoadHfeTrack)(file* pFile, int nTrack, int nSide, HfeDriveType* pdisk, HfeTrackType* ptrack, BYTE* pbyTrackData, int nMaxLen)
void LoadHfeTrack(file* pFile, int nTrack, int nSide, HfeDriveType* pdisk, TrackType* ptrack, BYTE* pbyTrackData, int nMaxLen)
{
	UINT16 mfm;
	UINT   fm;
	BYTE   mark;
	int    bitpos, nFluxLen, nSector, nSectorSize;
	int    nReadTotal, nReadPos;

	g_nHfeLowWriteAddress  = 0x10000000;
	g_nHfeHighWriteAddress = 0;

	g_nHfeSide = nSide;
	nReadTotal = pdisk->trackLUT[nTrack].track_len;

	if (nReadTotal > sizeof(g_byRawTrackData))
	{
		return;
	}

	nReadPos = pdisk->trackLUT[nTrack].offset * 0x200;

   	FileSeek(pFile, nReadPos);
    FileRead(pFile, g_byRawTrackData, nReadTotal);

	bitpos   = 0;
	nFluxLen = pdisk->trackLUT[nTrack].track_len * 8;
	nSector  = 0;
	mfm      = 0;
	fm       = 0;

    if (nFluxLen > (sizeof(g_byRawTrackData)*8))
    {
       nFluxLen = sizeof(g_byRawTrackData)*8;
    }

	while ((bitpos < nFluxLen) && (nSector < MAX_SECTORS_PER_TRACK))
	{
	    fm <<= 1;

        if (GetHfeByte(bitpos >> 3) & (1 << (bitpos & 7)))
        {
            fm |= 1;
        }

        ++bitpos;

		mfm = (unsigned short)fm;		// Note: this is legitimate, same flux data

		// floppy has multiple A1 marks
		if (mfm == MFM_MARK_A1)
		{
			int mark_bitpos = bitpos;
			int mark_count  = 0;

			mark = read_byte_mfm(&bitpos, &mfm);

			while ((mark == 0xA1) && (mark_count < 3))
			{
				mark_count++;
				mark = read_byte_mfm(&bitpos, &mfm);
			}

			// IDAM sequence: 0xA1 0xA1 0xA1 0xFE; or
			//                0xF5 0xF5 0xF5 0xFE;
			// 
			// the following bytes are
			// - track number
			// - side number
			// - sector number
			// - sector size: number of data bytes the sector contains (log 2, minus seven), 0 => 128 bytes; 1 => 256 bytes; etc.
			// - next 2-bytes are the CRC (calculation starts with the three 0xA1/0xF5 bytes preceeding the 0xFE)
			// 
			// DAM sequence : 0xA1 0xA1 0xA1 0xF8; or
			//                0xA1 0xA1 0xA1 0xFB;
			// 
			// sector data starts after the 0xF8/0xFB byte
			//
			// pading values are present between the end of the IDAM bytes and the start of the DAM
			//

			if ((mark_count == 3) && (mark == 0xFE)) // then we found an IDAM sequence
			{
				BYTE* pby;
				int   nIDAM_BytePos, i;

				ptrack->nSectorIDAM_BitPos[nSector] = bitpos - 64;  // starting bit index of the first 0xA1
				nIDAM_BytePos = ptrack->nSectorIDAM_BitPos[nSector] / 16;

				// copy to raw track data buffer
				pby = pbyTrackData + nIDAM_BytePos;

				*pby = 0xA1; ++pby;
				*pby = 0xA1; ++pby;
				*pby = 0xA1; ++pby;
				*pby = 0xFE; ++pby;

				// next 6 bytes are
				// - track number
				// - side number
				// - sector number
				// - sector size
				// - CRC16 (high byte)
				// - CRC16 (low byte)

				for (i = 0; i < 6; ++i)
				{
					*pby = read_byte_mfm(&bitpos, &mfm);
					++pby;
				}

				ptrack->nSectorIDAM[nSector] = nIDAM_BytePos;

				nSectorSize = *(pby - 3);
			}
			else if ((mark_count == 3) && ((mark == 0xFB) || (mark == 0xF8))) // then we found a DAM
			{
				BYTE* pby;
				int   nSize = 128 << nSectorSize;
				int   nDAM_BytePos;

				ptrack->nSectorDAM_BitPos[nSector] = bitpos - 64;  // starting bit index of the first 0xA1
				nDAM_BytePos = ptrack->nSectorDAM_BitPos[nSector] / 16;

				if (nSize <= (nMaxLen-nDAM_BytePos))
				{
					pby = pbyTrackData + nDAM_BytePos;

					*pby = 0xA1; ++pby;
					*pby = 0xA1; ++pby;
					*pby = 0xA1; ++pby;
					*pby = mark; ++pby;

					for (int i = 0; i < nSize; ++i)
					{
						*pby = read_byte_mfm(&bitpos, &mfm);
						++pby;
					}

					*pby = read_byte_mfm(&bitpos, &mfm); ++pby; // high byte
					*pby = read_byte_mfm(&bitpos, &mfm); ++pby; // low byte
				}

				ptrack->nSectorDAM[nSector] = nDAM_BytePos;

				++nSector;
			}
			else
			{
				bitpos = mark_bitpos;
			}

			// Fix up the reading state (mostly, really should have 16 extra bits)
			fm = mfm;
		}
	}
}
