
#include <stdio.h>
#include <z80regs.h>
#include <fcntl.h>
#include "fdc.h"

#option INLIB

/*******************************************************************/

char g_byHostSequence[HOST_SEQUENCE_COUNT] = {
	0x80, 0x7F, 0x81, 0xFE, 0x82, 0xFD, 0x83, 0xFC, 0x84
};

char g_byDetected;

/*******************************************************************/
DetectFdcHost()
{
	int i;

	g_byDetected = 0;

	/* send sequence to data register */
	for (i = 0; i < HOST_SEQUENCE_COUNT; ++i)
	{
		outport(0xF3, g_byHostSequence[i]); /* write data register */
	}

	/* read data register to see is we get the sequence back */
	for (i = 0; i < HOST_SEQUENCE_COUNT; ++i)
	{
		if (inport(0xF3) != g_byHostSequence[i])
		{
			return;
		}
	}

	g_byDetected = 1;
}

/*******************************************************************/
GetDate(pbyBuf)
char* pbyBuf;
{
	union REGS reg;

	/***************************************************************/
	/* @DATE = SVC-18 */

	reg.byteregs.a = 18;
	reg.wordregs.hl = pbyBuf;
	call(18, &reg);
}

/*******************************************************************/
GetTime(pbyBuf)
char* pbyBuf;
{
	union REGS reg;

	/***************************************************************/
	/* @TIME = SVC-19 */

	reg.byteregs.a = 19;
	reg.wordregs.hl = pbyBuf;
	call(19, &reg);
}

/*******************************************************************/
int WaitNotBusy()
{
	int i, j;

	j = 0;
	i = inport(0xF0); /* read status register */

	while ((i & 1) != 0) /* wait until not busy */
	{
		i = inport(0xF0); /* read status register */

		++j;

		if (j > 1000)
		{
			return -1;
		}
	}

	return 1;
}

/*******************************************************************/
int WaitDRQ()
{
	int i, j;

	j = 0;
	i = inport(0xF0); /* read status register */

	/* wait for DRQ to be set */
	while ((i & 2) == 0)
	{
		i = inport(0xF0); /* read status register */

		++j;

		if (j > 1000)
		{
			return -1;
		}
	}

	return 1;
}

/*******************************************************************/
int ReadByte()
{
	if (WaitNotBusy() < 0)
	{
		return -1;
	}

	if (WaitDRQ() < 0)
	{
		return -1;
	}

	return inport(0xF3); /* read data register */
}

/*******************************************************************/
int WriteByte(c)
char c;
{
	if (WaitNotBusy() < 0)
	{
		return -1;
	}

	if (WaitDRQ() < 0)
	{
		return -1;
	}

	outport(0xF3, c); /* write data register */

	return 1;
}

/*******************************************************************/
int ReadData(psz, nMaxLen)
char* psz;
int   nMaxLen;
{
	int i, nLen, b;

	*psz = 0;
	nLen = ReadByte(); /* use ReadByte() to wait for DRQ */

	if (nLen >= nMaxLen)
	{
		nLen = nMaxLen - 1;
	}

	for (i = 0; i < nLen; ++i)
	{
		b = inport(0xF3);
		*psz = b;
		++psz;
	}

	*psz = 0;

	return nLen;
}

/*******************************************************************/
int WriteData(psz, nLen)
char* psz;
int   nLen;
{
	int i;

	if (WriteByte(nLen) < 0)
	{
		return -1;
	}

	for (i = 0; i < nLen; ++i)
	{
		if (WriteByte(*psz) < 0)
		{
			return -1;
		}

		++psz;
	}

	return 1;
}

/*******************************************************************/
int ReadStatus(psz, nMaxLen)
char* psz;
int   nMaxLen;
{
	outport(0xF4, 0x0F); /* DRSEL = 0x0F */
	outport(0xF0, 0x01); /* read status command code */
	return ReadData(psz, nMaxLen);
}

/*******************************************************************/
/* nType =  0x02 - all files
*			0x80 - .DMK files
*			0x81 - .INI files */
int FindFirst(psz, nMaxLen, byType)
char* psz;
int   nMaxLen;
int   byType;
{
	outport(0xF4, 0x0F); /* DRVSEL = 0xF */
	outport(0xF0, byType); /* find first command code */
	return ReadData(psz, nMaxLen);
}

/*******************************************************************/
int FindNext(psz, nMaxLen)
char* psz;
int   nMaxLen;
{
	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x03); /* find next command code */
	return ReadData(psz, nMaxLen);
}
