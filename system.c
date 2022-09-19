
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/scb.h"

#include "Defines.h"
#include "datetime.h"
#include "sd_core.h"

uint32_t SystemCoreClock = 133000000;

//////////////////////////////////////////////////////////////////////////////////////////////////

const WORD wNormalYearDaysInMonth[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
const WORD wLeapYearDaysInMonth[12]   = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

///////////////////////////////////////////////////////////////////////////////////////////////////
// parameters: dwSeconds - number of seconds since midnight on January 1st, 1980
//
//             pdt - pointer to a CodedDateTime structure to receive the coded date/time
//                   - .year  - number of years since 1980.  A value of 43 relates to year 2023
//                   - .month - zero based index of the month (0 - 11)
//                   - .day   - zero based index of the day of the month (0 to one less than the number of days in the month)
//                   - .hour  - zero based index of the hour of the day (0 - 23)
//                   - .min   - zero based index of the minute in the hour (0 - 59)
//                   - .sec   - zero based index of the second in the minute (0 - 59)
//
// Note, works up to February, 28h 2100
//
void CodeDateTime(DWORD dwSeconds, CodedDateTime* pdt)
{
	WORD  wNumDays, wYear, wDayOfYear, wTimeBalance, wNumLeapYears, j;
	DWORD dwDayTime;

	wNumDays     = dwSeconds / SECONDS_IN_DAY;
	dwDayTime    = dwSeconds % SECONDS_IN_DAY;
	pdt->hour    = dwDayTime / SECONDS_IN_HOUR;
	wTimeBalance = dwDayTime % SECONDS_IN_HOUR;
	pdt->min     = wTimeBalance / 60;
	pdt->sec     = wTimeBalance % 60;

	wNumLeapYears = wNumDays / DAYS_IN_FOUR_YEARS;
	wNumDays      = wNumDays % DAYS_IN_FOUR_YEARS;

	wYear      = wNumDays / 365 + wNumLeapYears * 4;
	wDayOfYear = wNumDays % 365;
	pdt->year  = wYear;

	j = 11;
	
	while (wNormalYearDaysInMonth[j] > wDayOfYear)
	{
		j--;
	}

	pdt->month = j;												// coded 0..11
	pdt->day   = wDayOfYear - wNormalYearDaysInMonth[j];		// days start at 0
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// psz - points to the null terminated string in the date/time format (MM/DD/YYYY hh:mm:ss)
void ParseDateTime(char* psz, CodedDateTime* pdt)
{
	pdt->month = atoi(psz) - 1;
	pdt->day   = 0;
	pdt->year  = 0;
	pdt->hour  = 0;
	pdt->min   = 0;
	pdt->sec   = 0;
	
	// get day
	while ((*psz != 0) && (*psz != '/'))
	{
		++psz;
	}
	
	if (*psz != '/')
	{
		return;
	}
	
	++psz;

	pdt->day = atoi(psz) - 1;

	// get year
	while ((*psz != 0) && (*psz != '/'))
	{
		++psz;
	}
	
	if (*psz != '/')
	{
		return;
	}
	
	++psz;

	pdt->year = atoi(psz);
	
	if (pdt->year >= 80)
	{
		pdt->year += 1900;
	}
	else
	{
		pdt->year += 2000;
	}

	pdt->year -= 1980;
	
	// get hour
	while ((*psz != 0) && (*psz != ' '))
	{
		++psz;
	}
	
	if (*psz != ' ')
	{
		return;
	}
	
	++psz;

	pdt->hour = atoi(psz);

	// get minute
	while ((*psz != 0) && (*psz != ':'))
	{
		++psz;
	}
	
	if (*psz != ':')
	{
		return;
	}
	
	++psz;

	pdt->min = atoi(psz);

	// get second
	while ((*psz != 0) && (*psz != ':'))
	{
		++psz;
	}
	
	if (*psz != ':')
	{
		return;
	}
	
	++psz;

	pdt->sec = atoi(psz);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
unsigned long EncodeDateTime(CodedDateTime* pdt)
{
	unsigned long nTime;

	nTime  = pdt->year * SECONDS_IN_YEAR;			// start with seconds for the specified years
	nTime += ((pdt->year / 4) * SECONDS_IN_DAY);	// add a days seconds for each leap year

	if (((pdt->year + 1980) % 4) == 0) // is this a leap year
	{
		nTime += (wLeapYearDaysInMonth[pdt->month] * SECONDS_IN_DAY); // number of seconds to day 1 of the specified month (for a leap year)
	}
	else
	{
		nTime += (wNormalYearDaysInMonth[pdt->month] * SECONDS_IN_DAY); // number of seconds to day 1 of the specified month (for a non leap year)
	}

	nTime += (pdt->day * SECONDS_IN_DAY);    // number of second to start of this day of the month
	nTime += (pdt->hour * SECONDS_IN_HOUR);  // number of seconds to this hour
	nTime += (pdt->min * 60);                // number of seconds to this minute
	nTime += pdt->sec;                       // number of seconds to this second
	
	return nTime;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t CountDown(uint32_t nCount, uint32_t nAdjust)
{
	if (nCount > nAdjust)
	{
		nCount -= nAdjust;
	}
	else
	{
		nCount = 0;
	}

	return nCount;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t CountUp(uint32_t nCount, uint32_t nAdjust)
{
	if (nCount < (0xFFFFFFFF - nAdjust))
	{
		nCount += nAdjust;
	}

	return nCount;
}

////////////////////////////////////////////////////////////////////////////////////
char* SkipBlanks(char* psz)
{
	if (psz == NULL)
	{
		return NULL;
	}
	
	while ((*psz == ' ') && (*psz != 0))
	{
		++psz;
	}
	
	return psz;
}

////////////////////////////////////////////////////////////////////////////////////
char* SkipToBlank(char* psz)
{
	if (psz == NULL)
	{
		return NULL;
	}
	
	while ((*psz != ' ') && (*psz != 0))
	{
		++psz;
	}
	
	return psz;
}

////////////////////////////////////////////////////////////////////////////////////
void CopySectionName(char* pszSrc, char* pszDst, int nMaxLen)
{
	int i = 0;
	
	if (*pszSrc == '[')
	{
		++pszSrc;
	}
	
	while ((i < nMaxLen) && (*pszSrc != ']') && (*pszSrc != 0))
	{
		*pszDst = toupper(*pszSrc);
		++pszDst;
		++pszSrc;
		++i;
	}
	
	*pszDst = 0;
}

////////////////////////////////////////////////////////////////////////////////////
char* CopyLabelName(char* pszSrc, char* pszDst, int nMaxLen)
{
	int i = 0;
	
	while ((i < nMaxLen) && (*pszSrc != '=') && (*pszSrc != 0))
	{
		*pszDst = toupper(*pszSrc);
		++pszDst;
		++pszSrc;
		++i;
	}

	*pszDst = 0;

	if (*pszSrc == '=')
	{
		++pszSrc;
	}
	
	return pszSrc;
}

////////////////////////////////////////////////////////////////////////////////////
void CopyString(char* pszSrc, char* pszDst, int nMaxLen)
{
	int i;
	
	for (i = 0; i < nMaxLen; ++i)
	{
		if (*pszSrc != 0)
		{
			*pszDst = *pszSrc;
			++pszSrc;
		}
		else
		{
			*pszDst = 0;
		}

		++pszDst;
  	}
}

////////////////////////////////////////////////////////////////////////////////////
void StrToUpper(char* psz)
{
	while (*psz != 0)
	{
		*psz = toupper(*psz);
		++psz;
	}
}

////////////////////////////////////////////////////////////////////////////////////
char* stristr(char* psz, char* pszFind)
{
	char* psz1;
	char* psz2;

	while (*psz != 0)
	{
		if (tolower(*psz) == tolower(*pszFind))
		{
			psz1 = psz + 1;
			psz2 = pszFind + 1;

			while ((*psz1 != 0) && (*psz2 != 0) && (tolower(*psz1) == tolower(*psz2)))
			{
				++psz1;
				++psz2;
			}

			if (*psz2 == 0)
			{
				return psz;
			}
		}

		++psz;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// returns   0 if psz1 == psz2
//			-1 if psz1 < psz2
//           1 if psz1 > psz2
int stricmp(char* psz1, char* psz2)
{
	while ((tolower(*psz1) == tolower(*psz2)) && (*psz1 != 0) && (*psz2 != 0))
	{
		++psz1;
		++psz2;
	}

	if ((*psz1 == 0) && (*psz2 == 0))
	{
		return 0;
	}
	else if (*psz1 == 0)
	{
		return -1;
	}
	else if (*psz2 == 0)
	{
		return 1;
	}
	else if (tolower(*psz1) < tolower(*psz2))
	{
		return -1;
	}
	else if (tolower(*psz1) > tolower(*psz2))
	{
		return 1;
	}

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void strcat_s(char* pszDst, int nDstSize, char* pszSrc)
{
	int nLen = strlen(pszDst);

	pszDst += nLen;
	
	while ((nLen < nDstSize) && (*pszSrc != 0))
	{
		*pszDst = *pszSrc;
		++pszDst;
		++pszSrc;
		++nLen;
	}

	*pszDst = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DWORD GetCycDuration(DWORD dwEndCount, DWORD dwStartCount)
{
	if (dwStartCount >= dwEndCount)
	{
		return dwStartCount - dwEndCount;
	}
	else
	{
		return (0x01000000 - dwEndCount) + dwStartCount;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DWORD g_dwStopWatchStart;
DWORD g_dwStopWatchEnd;
float g_fStopWatchDuration;

void __not_in_flash_func(StartStopWatch)(void)
{
	g_dwStopWatchStart = systick_hw->cvr;
}

void __not_in_flash_func(StopStopWatch)(void)
{
	g_dwStopWatchEnd = systick_hw->cvr;
}

// returns the execution time in us
float GetStopWatchDuration(void)
{
	g_fStopWatchDuration = GetCycDuration(g_dwStopWatchEnd, g_dwStopWatchStart);
	
	g_fStopWatchDuration = g_fStopWatchDuration / (double) SystemCoreClock;
	
	return g_fStopWatchDuration;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void SendTraceText(char* psz)
{
	printf("%5d %s\r\n", time_us_32(), psz);
}

/* END OF FILE */
