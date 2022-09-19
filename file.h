#ifndef __FILE_C_
#define __FILE_C_

#ifdef __cplusplus
extern "C" {
#endif

#include "ff.h"

#define MAX_FILES 6

typedef struct {
    BYTE byIsOpen;
    FIL  f;
} file;

//-----------------------------------------------------------------------------
void   FileSystemInit(void);
file*  FileOpen(char* pszFileName, BYTE byMode);
void   FileClose(file* fp);
BYTE   FileIsOpen(file *fp);
UINT32 FileRead(file* fp, BYTE* pby, UINT32 nSize);
UINT32 FileWrite(file* fp, BYTE* pby, UINT32 nSize);
void   FileSeek(file* fp, int nOffset);
void   FileFlush(file* fp);
void   FileTruncate(file* fp);
int    FileReadLine(file* fp, char szLine[], int nMaxLen);

BYTE   IsEOF(file* fp);
BYTE   FileExists(char* pszFileName);

#ifdef __cplusplus
}
#endif

#endif


