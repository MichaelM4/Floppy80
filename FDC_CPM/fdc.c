
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys.h>
#include <stdlib.h>
#include <conio.h>

/*******************************************************************/

#define HOST_SEQUENCE_COUNT 9

/*******************************************************************/

unsigned char g_byHostSequence[HOST_SEQUENCE_COUNT] = {
	0x80, 0x7F, 0x81, 0xFE, 0x82, 0xFD, 0x83, 0xFC, 0x84
};

char g_byDetected;

#define MAX_FILES 9
#define MAX_LEN   80

char g_szFiles[MAX_FILES][MAX_LEN];
int  g_nNumFiles;

/*******************************************************************/
void putstr(char* psz)
{
	while (*psz != 0)
	{
		putchar(*psz);

		if (*psz == 13)
		{
			putchar('\n');
		}

		++psz;
	}
}

/*******************************************************************/
void ClrScr()
{
	putchar('\x1A');
}

/*******************************************************************/
void strtoupr(char* psz)
{
	while (*psz != 0)
	{
		*psz = toupper(*psz);
		++psz;
	}
}

/*******************************************************************/
int stricmp(char* psz1, char* psz2)
{
	while ((*psz1 != 0) && (*psz2 != 0))
	{
		if (toupper(*psz1) < toupper(*psz2))
		{
			return -1;
		}
		else if (toupper(*psz1) > toupper(*psz2))
		{
			return 1;
		}

		++psz1;
		++psz2;
	}

	return 0;
}

/*******************************************************************/
char* SkipBlanks(char* psz)
{
	while ((*psz == ' ') && (*psz != 0))
	{
		++psz;
	}

	return psz;
}

/*******************************************************************/
char* SkipToBlank(char* psz)
{
	while ((*psz != ' ') && (*psz != 0))
	{
		++psz;
	}

	return psz;
}

/*******************************************************************/
int WaitNotBusy()
{
	int i, j;

	j = 0;
	i = inp(0xF0); /* read status register */

	while ((i & 1) != 0) /* wait until not busy */
	{
		i = inp(0xF0); /* read status register */

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
	i = inp(0xF0); /* read status register */

	/* wait for DRQ to be set */
	while ((i & 2) == 0)
	{
		i = inp(0xF0); /* read status register */

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

	return inp(0xF3); /* read data register */
}

/*******************************************************************/
int ReadData(char* psz, int nMaxLen)
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
		b = inp(0xF3);
		*psz = b;
		++psz;
	}

	*psz = 0;

	return nLen;
}

/*******************************************************************/
int WriteByte(char c)
{
	if (WaitNotBusy() < 0)
	{
		return -1;
	}

	if (WaitDRQ() < 0)
	{
		return -1;
	}

	outp(0xF3, c); /* write data register */

	return 1;
}

/*******************************************************************/
int WriteData(char* psz, int nLen)
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
/* nType =  0x02 - all files
*			0x80 - .DMK files
*			0x81 - .INI files */
int FindFirst(char* psz, int nMaxLen, int byType)
{
	outp(0xF4, 0x0F); /* DRVSEL = 0xF */
	outp(0xF0, byType); /* find first command code */
	return ReadData(psz, nMaxLen);
}

/*******************************************************************/
int FindNext(char* psz, int nMaxLen)
{
	outp(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outp(0xF0, 0x03); /* find next command code */
	return ReadData(psz, nMaxLen);
}

/*******************************************************************/
void MountImage(char* szFile, int nDrive)
{
	int  nLen;
	char szCmd[128];

	szCmd[0] = '0' + nDrive;
	szCmd[1] = ' ';
	szCmd[2] = 0;

	strcat(szCmd, szFile);
	nLen = strlen(szCmd);

	outp(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outp(0xF0, 0x04); /* mount image */
	WriteData(szCmd, nLen + 1);

	WaitNotBusy();
}

/*******************************************************************/
int SelectDiskImage(char* pszFilter)
{
	char c;
	int  nLine;

	if (g_nNumFiles <= 0)
	{
		return ' ';
	}

	if (stricmp(pszFilter, ".INI") == 0) /* the same */
	{
		printf("Press 1-%d to select the default ini file.\n", g_nNumFiles);
	}
	else
	{
		printf("Press 1-%d to select an image to mount.\n", g_nNumFiles);
	}

	if (g_nNumFiles >= MAX_FILES)
	{
		puts("Any other key to get next set of files.");
	}
	else
	{
		puts("Any other key to exit.");
	}

/*	puts("BREAK to exit."); */

	nLine = -1;

	while ((nLine < 0) || (nLine >= g_nNumFiles))
	{
		c = getch();

		if ((c < '1') || (c > (g_nNumFiles+'0')))
		{
			return c;
		}

		nLine = c - '1';
	}

	if ((nLine >= 0) && (nLine < g_nNumFiles))
	{
		if (stricmp(pszFilter, ".INI") == 0) /* the same */
		{
			MountImage(g_szFiles[nLine], 0);
			return -1;
		}

		puts("Specify drive to mount to (0-3)");

		c = getch();

		if ((c >= '0') && (c <= '3'))
		{
			MountImage(g_szFiles[nLine], c-'0');
			return -1;
		}
	}

	return c;
}

/*******************************************************************/
void ListDiskImageFiles(char* pszFilter)
{
	char* psz;
	int   nRet;
	int   i;
	char  szBuf[260];

	ClrScr();

	if (stricmp(pszFilter, ".INI") == 0)
	{
		nRet = FindFirst(szBuf, sizeof(szBuf) - 2, 0x80);
	}
	else if (stricmp(pszFilter, ".DMK") == 0)
	{
		nRet = FindFirst(szBuf, sizeof(szBuf) - 2, 0x81);
	}
	else if (stricmp(pszFilter, ".HFE") == 0)
	{
		nRet = FindFirst(szBuf, sizeof(szBuf) - 2, 0x82);
	}
	else
	{
		return;
	}

	g_nNumFiles = 0;

	for (i = 0; i < 10; ++i)
	{
		g_szFiles[i][0] = 0;
	}

	while ((nRet > 0) && (szBuf[0] != 0))
	{
		/* returned line is of the folmat "DD/MM/YYYY SIZE FileName.ext" */
		/* locate start of file name */
		psz = SkipBlanks(szBuf);
		psz = SkipToBlank(psz);
		psz = SkipBlanks(psz);

		/* here we are at the start of the file size */
		psz = SkipToBlank(psz);
		psz = SkipBlanks(psz);

		/* here we are at the file name */

		strcpy(g_szFiles[g_nNumFiles], psz);

		printf("%c %s\n", '1'+g_nNumFiles, psz);
		++g_nNumFiles;

		if (g_nNumFiles >= MAX_FILES)
		{
			if (SelectDiskImage(pszFilter) < 0)
			{
				return;
			}

			g_nNumFiles = 0;

			for (i = 0; i < 10; ++i)
			{
				g_szFiles[i][0] = 0;
			}

			ClrScr();
		}

		nRet = FindNext(szBuf, sizeof(szBuf) - 2);
	}

	SelectDiskImage(pszFilter);
}

/*******************************************************************/
int ReadStatus(char* psz, int nMaxLen)
{
	outp(0xF4, 0x0F); /* DRSEL = 0x0F */
	outp(0xF0, 0x01); /* read status command code */
	return ReadData(psz, nMaxLen);
}

/*******************************************************************/
void GetStatus()
{
	char szBuf[260];

	ClrScr();
	ReadStatus(szBuf, sizeof(szBuf) - 2);
	putstr(szBuf);
}

/*******************************************************************/
void DetectFdcHost()
{
	int i;

	g_byDetected = 0;

	/* send sequence to data register */
	for (i = 0; i < HOST_SEQUENCE_COUNT; ++i)
	{
		outp(0xF3, g_byHostSequence[i]);
	}

	/* read data register to see if we get the sequence back */
	for (i = 0; i < HOST_SEQUENCE_COUNT; ++i)
	{
		if (inp(0xF3) != g_byHostSequence[i])
		{
			return;
		}
	}

	g_byDetected = 1;
}

/*******************************************************************/
void GetDir()
{
	int  nRet;
	int  nLine;
	char szBuf[260];

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
			puts("Press any key to continue.");
			
			getchar();
			ClrScr();

			nLine = 0;
		}
	}
}

/*******************************************************************/
/* szFile - null terminated file name string.                      */
/* nDrive - the index (0 - 7) of the drive to copy the file too.   */
int GetFile(char* szFile)
{
	FILE* f;
	int   nLen, nSize, i;
	char  szBuf[256];

	f = fopen(szFile, "w+b");

	if (f == NULL)
	{
		printf("Unable to create file: %s", szFile);
		return;
	}

	printf("Importing file %s", szFile);

	i = 0;

	if (szFile[1] == ':')
	{
		i = 2;
	}

	/* build command string */
	strcpy(szBuf, szFile+i);
	strcat(szBuf, ", r");
	nLen = strlen(szBuf);

	/* open file for reading */
	outp(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outp(0xF0, 0x05); /* open file */

	if (WriteData(szBuf, nLen + 1) < 0)
	{
		fclose(f);
		return -1;
	}

	nSize = 0;

	while (nLen > 0)
	{
		if (WaitNotBusy() < 0)
		{
			fclose(f);
			return -1;
		}

		/* read data */
		outp(0xF4, 0x0F); /* DRVSEL = 0x0F */
		outp(0xF0, 0x06); /* read bytes from file */
		nLen = ReadData(szBuf, sizeof(szBuf));

		if (nLen > 0)
		{
			if (fwrite(szBuf, 1, nLen, f) != nLen)
			{
				puts("Error writing to file.");
				fclose(f);
				return;
			}
		}

		nSize += nLen;
		putchar('.');
	}

	fclose(f);

	if (WaitNotBusy() < 0)
	{
		return -1;
	}

	/* close file (remote and local) */
	outp(0xF4, 0x0F); /* DRVSEL = 0x0F */
	outp(0xF0, 0x08); /* close file */

	return 1;
}

/*******************************************************************/
void ShowHelp()
{
	ClrScr();
	puts("FDC utility version 1.0.2  Command line options:");
	/*puts("SET - set FDC date and time to the TRS-80 date and time."); */
	/*puts("GET - set TRS-80 date and time to the FDC date and time."); */
	puts("STA - get FDC status (firmware version, mounted disks, etc.).");
	puts("DIR - get a directory listing of the FDC SD-Card root folder.");
	puts("INI - select the default ini file.    FDC INI filename.ext");
	puts("DMK - mount a disk image.             FDC DMK filename.ext n");
	puts("HFE - mount a disk image.             FDC HFE filename.ext n");
	puts("IMP - import a file from the SD-Card. FDC IMP x:filename.ext");
/*	puts("EXP - export a file to the SD-card.   FDC EXP filename.ext x"); */
	puts(" ");
	puts("      filename.ext - is the filname and extension.");
	puts("      n - is the drive number (0-3).");
	puts("      x - is a drive designator (a, b, c, etc).");
}

/*******************************************************************/
int main(argc, argv)
char ** argv;
{
	char c;

	if (argc < 2)
	{
		ShowHelp();
		return 0;
	}

	DetectFdcHost();

	if (g_byDetected == 0)
	{
		puts("FDC host not detected.");
		return 0;
	}

	if (stricmp((char*)argv[1], "STA") == 0)
	{
		GetStatus();
	}
	else if (stricmp(argv[1], "DIR") == 0)
	{
		GetDir();
	}
	else if (stricmp(argv[1], "INI") == 0)
	{
		if (argc < 3)
		{
			ListDiskImageFiles(".INI");
		}
		else
		{
			MountImage(argv[2], atol(argv[3]));
		}

		GetStatus();
		puts("Press reset to continue.");
		c = '1';
		while (c == '1');
	}
	else if (stricmp(argv[1], "DMK") == 0)
	{
		if (argc < 3)
		{
			ListDiskImageFiles(".DMK");
		}
		else
		{
			MountImage(argv[2], atoi(argv[3]));
		}

		GetStatus();
	}
	else if (stricmp(argv[1], "HFE") == 0)
	{
		if (argc < 3)
		{
			ListDiskImageFiles(".HFE");
		}
		else
		{
			MountImage(argv[2], atoi(argv[3]));
		}

		GetStatus();
	}
	else if (strcmp(argv[1], "IMP") == 0)
	{
		if (argc < 3)
		{
			puts("Insufficient command line arguments.");
		}
		else
		{
			GetFile(argv[2]);
		}
	}
	else
	{
		puts("Unrecognized command.");
	}

	return 0;
}
