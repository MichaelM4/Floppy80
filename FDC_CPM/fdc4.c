
#include <stdio.h>
#include <z80regs.h>
#include <fcntl.h>
#include "fdc.h"

#option INLIB

#define MAX_FILES 9
#define MAX_LEN   80

char g_szFiles[MAX_FILES][MAX_LEN];
int  g_nNumFiles;

/*******************************************************************/
ClrScr()
{
	puts("\x1c\x1f\x0f"); /* home, clear, Cursor Off, CR */
}

/*******************************************************************/
strtoupr(psz)
char* psz;
{
	while (*psz != 0)
	{
		*psz = toupper(*psz);
		++psz;
	}
}

/*******************************************************************/
char* SkipBlanks(psz)
char* psz;
{
	while ((*psz == ' ') && (*psz != 0))
	{
		++psz;
	}

	return psz;
}

/*******************************************************************/
char* SkipToBlank(psz)
char* psz;
{
	while ((*psz != ' ') && (*psz != 0))
	{
		++psz;
	}

	return psz;
}

/*******************************************************************/
char SelectDiskImage(pszFilter)
char* pszFilter;
{
	char c;
	int  nLine;

	if (g_nNumFiles <= 0)
	{
		return ' ';
	}

	if (strcmp(pszFilter, ".INI") == 0) /* the same */
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
		c = toupper(getchar());

		if (c == VKEY_BRK)
		{
			return c;
		}

		if ((c < '1') || (c > (g_nNumFiles+'0')))
		{
			return c;
		}

		nLine = c - '1';
	}

	if ((nLine >= 0) && (nLine < g_nNumFiles))
	{
		if (strcmp(pszFilter, ".INI") == 0) /* the same */
		{
			MountImage(g_szFiles[nLine], 0);
			return VKEY_BRK;
		}

		puts("Specify drive to mount to (0-3)");

		c = getchar();

		if (c == VKEY_BRK)
		{
			return c;
		}

		if ((c >= '0') && (c <= '3'))
		{
			MountImage(g_szFiles[nLine], c-'0');
			return VKEY_BRK;
		}
	}

	return c;
}

/*******************************************************************/
ListDiskImageFiles(pszFilter)
char* pszFilter;
{
	char* psz;
	char  szBuf[260];
	char  c;
	int   nRet;
	int   i;

	ClrScr();

	if (strcmp(pszFilter, ".INI") == 0)
	{
		nRet = FindFirst(szBuf, sizeof(szBuf) - 2, 0x80);
	}
	else if (strcmp(pszFilter, ".DMK") == 0)
	{
		nRet = FindFirst(szBuf, sizeof(szBuf) - 2, 0x81);
	}
	else if (strcmp(pszFilter, ".HFE") == 0)
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
			c = SelectDiskImage(pszFilter);

			if (c == VKEY_BRK)
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
