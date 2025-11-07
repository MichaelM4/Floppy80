#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "Defines.h"
#include "file.h"
#include "sd_core.h"
#include "system.h"

//-----------------------------------------------------------------------------

file  g_fFiles[MAX_FILES];
FATFS g_FatFs;					/* FATFS work area (filesystem object) for logical drive */

//-----------------------------------------------------------------------------
void FileSystemInit(void)
{
	FRESULT fr;
    int     i;

	fr = f_mount(&g_FatFs, "", 0);

	for (i = 0; i < MAX_FILES; ++i)
	{
		g_fFiles[i].byIsOpen = FALSE;
	}
}

//-----------------------------------------------------------------------------
file* FileOpen(char* pszFileName, BYTE byMode)
{
	FRESULT fr;
	char    szTempPath[64];
	int     i;

	if (sd_byCardInialized == 0)
	{
		return NULL;
	}

	// find the first unused file buffer
	i = 0;

	while ((i < MAX_FILES) && (g_fFiles[i].byIsOpen))
	{
		++i;
	}

	if (i >= MAX_FILES)
	{
		return NULL;
	}

	if (strlen(pszFileName) > (sizeof(szTempPath)-3))
	{
		return NULL;
	}

	fr = f_open(&g_fFiles[i].f, pszFileName, byMode);

	if (fr == FR_OK)
	{
		g_fFiles[i].byIsOpen = TRUE;
		return &g_fFiles[i];
	}
	
	return NULL;
}

//-----------------------------------------------------------------------------
void FileClose(file* fp)
{
	int i;

	if ((fp == NULL) || (fp->byIsOpen == FALSE))
	{
		return;
	}

	f_close(&fp->f);
	fp->byIsOpen = FALSE;
}

//-----------------------------------------------------------------------------
BYTE FileIsOpen(file *fp)
{
	int i;

	if (fp == NULL)
	{
		return FALSE;
	}

	return fp->byIsOpen;
}

//-----------------------------------------------------------------------------
UINT32 FileRead(file* fp, BYTE* pby, UINT32 nSize)
{
	FRESULT fr;
	UINT    br;

	if ((fp == NULL) || (fp->byIsOpen == 0))
	{
		return 0;
	}

	fr = f_read(&fp->f, pby, nSize, &br);

	return br;
}

//-----------------------------------------------------------------------------
UINT32 FileWrite(file* fp, BYTE* pby, UINT32 nSize)
{
	FRESULT fr;
	UINT    bw;

	if (fp == NULL)
	{
		return 0;
	}

	fr = f_write(&fp->f, pby, nSize, &bw);

	return bw;
}

//-----------------------------------------------------------------------------
void FileSeek(file* fp, int nOffset)
{
	if (fp == NULL)
	{
		return;
	}

	f_lseek(&fp->f, nOffset);
}

//-----------------------------------------------------------------------------
void FileFlush(file* fp)
{
	f_sync(&fp->f);
}

//-----------------------------------------------------------------------------
void FileTruncate(file* fp)
{
	f_truncate(&fp->f);
}

//-----------------------------------------------------------------------------
BYTE IsEOF(file* fp)
{
	if (fp == NULL)
	{
		return TRUE;
	}

	return f_eof(&fp->f);
}

////////////////////////////////////////////////////////////////////////////////////
int FileReadLine(file* fp, char szLine[], int nMaxLen)
{
	char* psz;
	int   nLen = 0;
	
	if ((fp == NULL) || (IsEOF(fp)))
	{
		return -1;
	}

	szLine[0] = 0;
	f_gets(szLine, nMaxLen, &fp->f);						/* Get a string from the file */

	// remove CR
	psz = strchr(szLine, '\r');

	if (psz != NULL)
	{
		*psz = 0;
	}

	// remove LF
	psz = strchr(szLine, '\n');

	if (psz != NULL)
	{
		*psz = 0;
	}

	nLen = strlen(szLine);

	return nLen;
}

////////////////////////////////////////////////////////////////////////////////////
BYTE FileExists(char* pszFileName)
{
    FRESULT fr;  /* Return value */
    DIR     dj;  /* Directory object */
    FILINFO fno; /* File information */
	
    memset(&dj, 0, sizeof(dj));
    memset(&fno, 0, sizeof(fno));

    fr = f_findfirst(&dj, &fno, "0:", "*");

    if (FR_OK != fr)
    {
        return FALSE;
    }

    while ((fr == FR_OK) && (fno.fname[0] != 0)) /* Repeat until a file is found */
    {
        if ((fno.fattrib & AM_DIR) || (fno.fattrib & AM_SYS))
		{
			// pcAttrib = pcDirectory;
		}
		else if (stricmp(fno.fname, pszFileName) == 0)
		{
				return TRUE;
		}

        fr = f_findnext(&dj, &fno); /* Search for next item */
    }

    return FALSE;
}
