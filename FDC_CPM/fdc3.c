
#include <stdio.h>
#include <z80regs.h>
#include <fcntl.h>
#include "fdc.h"

#option INLIB

/*******************************************************************/
MountImage(szFile, nDrive)
char* szFile;
int   nDrive;
{
	char szCmd[128];
	int  nLen;

	szCmd[0] = '0' + nDrive;
	szCmd[1] = ' ';
	szCmd[2] = 0;

	strcat(szCmd, szFile);
	nLen = strlen(szCmd);

	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x04); /* mount image */
	WriteData(szCmd, nLen + 1);

	puts(szCmd);

	WaitNotBusy();
}

/*******************************************************************/
/* szFile - null terminated file name string.                      */
/* nDrive - the index (0 - 7) of the drive to copy the file too.   */
int GetFile(szFile, nDrive)
char* szFile;
int   nDrive;
{
	char  szBuf[256];
	int   nLen, nDst, nSize, x, y;

	sprintf(szBuf, "%s:%d", szFile, nDrive);
	nDst = open(szBuf, O_WRONLY | O_CREAT);

	if (nDst < 0)
	{
		printf("Unable to create file: %s", szBuf);
		return;
	}

	printf("Importing file %s to %s, ", szFile, szBuf);

	y = (x = curpos()) >> 8;    /* get curpos; calc row */
	x &= 0x0ff;                 /* calc column */

	/* build command string */
	strcpy(szBuf, szFile);
	strcat(szBuf, ", r");
	nLen = strlen(szBuf);

	/* open file for reading */
	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x05); /* open file */

	if (WriteData(szBuf, nLen + 1) < 0)
	{
		close(nDst);
		return -1;
	}

	nSize = 0;

	while (nLen > 0)
	{
		if (WaitNotBusy() < 0)
		{
			close(nDst);
			return -1;
		}

		/* read data */
		outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
		outport(0xF0, 0x06); /* read bytes from file */
		nLen = ReadData(szBuf, sizeof(szBuf));

		if (nLen > 0)
		{
			if (write(nDst, szBuf, nLen) != nLen)
			{
				puts("Error writing to file.");
				close(nDst);
				return;
			}
		}

		nSize += nLen;
		cursor(x, y);
		printf("%d", nSize);
	}

	close(nDst);

	if (WaitNotBusy() < 0)
	{
		return -1;
	}

	/* close file (remote and local) */
	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x08); /* close file */

	return 1;
}

/*******************************************************************/
int PutFile(szFile)
char* szFile;
{
	char szBuf[512];
	char *psz;
	int  i, x, y, nOffset, nSegment;
	int  nLen, nSrc, nRead, nSize;

	nSrc = open(szFile, O_RDONLY);

	if (nSrc < 0)
	{
		printf("Unable to open file: %s", szFile);
		return;
	}

	printf("Exporting file %s, ", szFile);

	y  = (x = curpos()) >> 8;    /* get curpos; calc row */
	x &= 0x0ff;                  /* calc column */

	/* strip of drive designator */
	psz = strchr(szFile, ':');

	if ((psz != NULL) && (*psz == ':'))
	{
		*psz = 0;
	}

	/* build command string */
	strcpy(szBuf, szFile);
	strcat(szBuf, ", w");
	nLen = strlen(szBuf);

	/* open file for writing */
	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x05); /* open file */
	WriteData(szBuf, nLen + 1);

	nLen  = sizeof(szBuf);
	nRead = read(nSrc, szBuf, nLen);
	nSize = 0;

	while (nRead > 0)
	{
		if (WaitNotBusy() < 0)
		{
			puts("Wait Not Busy error 1");
			close(nSrc);
			return -1;
		}

		nSize   += nRead;
		nOffset  = 0;
		nSegment = 128;

		while (nRead > 0)
		{
			if (nSegment > nRead)
			{
				nSegment = nRead;
			}

			/* write data */
			outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
			outport(0xF0, 0x07); /* write bytes to file */
			WriteData(szBuf+nOffset, nSegment);

			nRead   -= nSegment;
			nOffset += nSegment;
		}

		cursor(x, y);
		printf("%d", nSize);

		nRead = read(nSrc, szBuf, nLen);
	}

	close(nSrc);

	if (WaitNotBusy() < 0)
	{
		puts("Wait Not Busy error 2");
		return -1;
	}

	/* close file (remote and local) */
	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x08); /* close file */

	return 1;
}

/*******************************************************************/
/* GET - set the FDC date and time to the TRS-80 date and time.    */
GetDT()
{
	char szBuf[260];
	char szCmd[64];
	char *psz;

	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x0A); /* get time */
	ReadData(szBuf, sizeof(szBuf)-2);

	psz = strchr(szBuf, ' ');

	if ((psz == NULL) || (*psz != ' '))
	{
		return;
	}

	*psz = 0;
	strcpy(szCmd, "DATE ");
	strcat(szCmd, szBuf);
	puts(szCmd);
	system(szCmd);

	++psz;
	strcpy(szCmd, "TIME ");
	strcat(szCmd, psz);
	puts(szCmd);
	system(szCmd);
}

/*******************************************************************/
/* SET - set the TRS-80 date and time to the FDC date and time.    */
SetDT()
{
	char szCmd[128];
	char szBuf[16];
	int  nLen;

	GetDate(szBuf);
	szBuf[8] = 0;
	strcpy(szCmd, szBuf);

	strcat(szCmd, " ");

	GetTime(szBuf);
	szBuf[8] = 0;
	strcat(szCmd, szBuf);

	puts(szCmd);

	outport(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outport(0xF0, 0x09); /* set time command */
	WriteData(szCmd, nLen + 1);
}

/*******************************************************************/
GetStatus()
{
	char szBuf[260];

	ReadStatus(szBuf, sizeof(szBuf) - 2);
	puts(szBuf);
}

/*******************************************************************/
GetDir()
{
	char szBuf[260];
	char c;
	int  nRet;
	int  nLine;

	ClrScr();

	nRet  = FindFirst(szBuf, sizeof(szBuf) - 2, 0x02);
	nLine = 0;

	while ((nRet > 0) && (szBuf[0] != 0))
	{
		puts(szBuf);
		nRet = FindNext(szBuf, sizeof(szBuf) - 2);
		++nLine;

		if (nLine > 13)
		{
			puts("Press any key to continue. BREAK to exit.");
			
			c = getchar();
			ClrScr();

			if (c == VKEY_BRK)
			{
				return;
			}

			nLine = 0;
		}
	}
}
