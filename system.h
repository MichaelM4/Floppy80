
#ifndef _SYSTEM_H
#define _SYSTEM_H

#ifndef BYTE
typedef unsigned char BYTE;
#endif

#ifndef WORD
typedef unsigned short WORD;
#endif

#ifndef DWORD
typedef unsigned long DWORD;
#endif

// structures
//-----------------------------------------------------------------------------

// unions
//-----------------------------------------------------------------------------

// variables

// function definitions
//-----------------------------------------------------------------------------

DWORD GetCycDuration(DWORD dwStart, DWORD dwEnd);
void  StartStopWatch(void);
void  StopStopWatch(void);
float GetStopWatchDuration(void);

uint32_t CountDown(uint32_t nCount, uint32_t nAdjust);
uint32_t CountUp(uint32_t nCount, uint32_t nAdjust);

////////////////////////////////////////////////////////////////////////////////////

char* SkipBlanks(char* psz);
char* SkipToBlank(char* psz);
void  CopySectionName(char* pszSrc, char* pszDst, int nMaxLen);
char* CopyLabelName(char* pszSrc, char* pszDst, int nMaxLen);
void  CopyString(char* pszSrc, char* pszDst, int nMaxLen);
void  StrToUpper(char* psz);
char* stristr(char* psz, char* pszFind);
int   stricmp(char* psz1, char* psz2);
void  strcat_s(char* pszDst, int nDstSize, char* pszSrc);

void  SendTraceText(char* psz);

#endif

/* END OF FILE */
