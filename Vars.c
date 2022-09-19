//-----------------------------------------------------------------------------
// Vars.c
//-----------------------------------------------------------------------------
//
// MK40DN512VLQ10 Kinetis MCU with 512 KB flash and 128 KB SRAM
//

#include <string.h>
#include <stdio.h>

#include "Defines.h"
#include "system.h"
#include "fdc.h"

// Note: g_ (among other) prefix is used to denote global variables

//-----------------------------------------------------------------------------
// counter for real time clock (RTC)

DWORD    g_dwForegroundRtc = 0;
DWORD    g_dwBackgroundRtc;
uint64_t g_nWaitTime;
uint64_t g_nTimeStart;
uint64_t g_nTimeEnd;
uint64_t g_nTimeDiff;

///////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////////////
void InitVars(void)
{
	g_dwRotationTime  = 200000;	// 200ms
	g_dwIndexTime     = (g_dwRotationTime * 5) / 360;	// 5 degrees for index
	g_dwResetTime     = 1000;	// 1ms
	g_byMonitorReset  = FALSE;

	memset(&g_FDC, 0, sizeof(g_FDC));
	g_FDC.stStatus.byBusy = 1;
}

/* END OF LINE */
