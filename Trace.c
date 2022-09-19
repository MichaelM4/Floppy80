#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "Defines.h"
#include "system.h"
#include "fdc.h"

BYTE FdcGetCommandType(BYTE byCommand);
void InitTraceCapture(void);

////////////////////////////////////////////////////////////////////////////////////

typedef struct {
	BYTE byCommand;
	BYTE byData;
	BYTE bySector;
	BYTE byTrack;
} TraceCmdType;

TraceCmdType g_tcTrace;
FDC_BusType  g_btBus;

DWORD g_dwPrevBus;
int   g_nBusCount;
DWORD g_dwPrevCycCnt;
int   g_nFlushCount;
BYTE  g_byCaptureBusActivity;

file* g_fTraceFile;

#define BUS_TRACE_SIZE (1024*16)

FDC_BusType g_btBusTrace[BUS_TRACE_SIZE];
int g_nBusTraceIndex;

int  g_nSectorReadByteCount;
int  g_byForceInterruptCount;
BYTE g_byCaptureAll;

//-----------------------------------------------------------------------------
void InitBusTrace(void)
{
	g_dwPrevBus             = 0;
	g_nBusCount             = 0;
	g_nFlushCount           = 0;
	g_nBusTraceIndex        = 0;
	g_nSectorReadByteCount  = 0;
	g_byForceInterruptCount = 0;
	g_byCaptureAll          = 0;
	g_byCaptureBusActivity  = 0;

    InitTraceCapture();
}

//----------------------------------------------------------------------------
void StrCatBit(char* psz, int nVal)
{	
	if (nVal)
	{
		strcat(psz, "1 ");
	}
	else
	{
		strcat(psz, "0 ");
	}
}

//----------------------------------------------------------------------------
void StrCatCommand(char* psz, DWORD dwVal)
{
	char szBuf[128];
	
	if ((dwVal & 0xF0)	== 0) // 0000xxxx
	{
		strcat(psz, "CMD: Restore");
	}
	else if ((dwVal & 0xF0) == 0x10) // 0001xxxx
	{
		sprintf(szBuf, "CMD: SEEK 0x%02X, From 0x%02X", g_tcTrace.byData, g_tcTrace.byTrack);
		g_tcTrace.byTrack = g_tcTrace.byData;
		strcat(psz, szBuf);
	}
	else if ((dwVal & 0xF0) == 0x20) // 0010xxxx
	{
		strcat(psz, "CMD: Step, Do Not Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x30) // 0011xxxx
	{
		strcat(psz, "CMD: Step, Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x40) // 0100xxxx
	{
		strcat(psz, "CMD: Step In, Do Not Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x50) // 0101xxxx
	{
		strcat(psz, "CMD: Step In, Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x60) // 0110xxxx
	{
		strcat(psz, "CMD: Step Out, Do Not Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x70) // 0111xxxx
	{
		strcat(psz, "CMD: Step Out, Update Track Register");
	}
	else if ((dwVal & 0xF0) == 0x80) // 1000xxxx
	{
		sprintf(szBuf, "CMD: RSEC: 0x%02X TRK: 0x%02X", g_tcTrace.bySector, g_tcTrace.byTrack);
		strcat(psz, szBuf);
		g_nSectorReadByteCount = 0;
	}
	else if ((dwVal & 0xF0) == 0x90) // 1001xxxx
	{
		strcat(psz, "CMD: RSEC: Multiple Record");
	}
	else if ((dwVal & 0xF0) == 0xA0) // 1010xxxx
	{
		sprintf(szBuf, "CMD: WSEC: 0x%02X TRK: 0x%02X", g_tcTrace.bySector, g_tcTrace.byTrack);
		strcat(psz, szBuf);
	}
	else if ((dwVal & 0xF0) == 0xB0) // 1011xxxx
	{
		strcat(psz, "CMD: WSEC: Multiple Record");
	}
	else if ((dwVal & 0xF0) == 0xC0) // 1100xxxx
	{
		strcat(psz, "CMD: Read Address");
	}
	else if ((dwVal & 0xF0) == 0xD0) // 1101xxxx
	{
		++g_byForceInterruptCount;
//		strcat(psz, "CMD: Force Interrupt");
		sprintf(szBuf, "CMD: Force Interrupt %d", g_byForceInterruptCount);
		strcat(psz, szBuf);
	}
	else if ((dwVal & 0xF0) == 0xE0) // 1110xxxx
	{
		sprintf(szBuf, "CMD: RTRK: 0x%02X", g_tcTrace.byTrack);
		strcat(psz, szBuf);
	}
	else if ((dwVal & 0xF0) == 0xF0) // 1110xxxx
	{
		sprintf(szBuf, "CMD: WTRK: 0x%02X", g_tcTrace.byTrack);
		strcat(psz, szBuf);
	}
}

//----------------------------------------------------------------------------
void StrCatDesc(char* psz, FDC_BusType* pEntry)
{
	char szBuf[128];
	BYTE byType;
	
	if (pEntry->Bus.b.DISK_IN_RE == 0)
	{
		switch (pEntry->Bus.b.A0 | (pEntry->Bus.b.A1 << 1))
		{
			case 0:
				strcat(psz, "RD Status: ");
				
				if (pEntry->Bus.b.DATA & 0x01)
				{
					strcat(psz, "S0:Busy, ");
				}
				
				byType = FdcGetCommandType(g_tcTrace.byCommand);
				
				if (pEntry->Bus.b.DATA & 0x02)
				{
					if ((byType == 2) || (byType == 3))
					{
						strcat(psz, "S1:DRQ, ");
					}
					else
					{
						strcat(psz, "S1:Index, ");
					}
				}

				if (pEntry->Bus.b.DATA & 0x04)
				{
					if ((byType == 2) || (byType == 3))
					{
						strcat(psz, "S2:Lost Data, ");
					}
					else
					{
						strcat(psz, "S2:Track 0, ");
					}
				}

				if (pEntry->Bus.b.DATA & 0x08)
				{
					strcat(psz, "S3:CRC Error, ");
				}

				if (pEntry->Bus.b.DATA & 0x10)
				{
					if ((byType == 2) || (byType == 3))
					{
						strcat(psz, "S4:Rec Not Found, ");
					}
					else
					{
						strcat(psz, "S4:Seek Err, ");
					}
				}

				if (pEntry->Bus.b.DATA & 0x20)
				{
					if ((byType == 2) || (byType == 3))
					{
						strcat(psz, "S5:Deleted, ");
					}
					else
					{
						strcat(psz, "S5:Head Loaded, ");
					}
				}

				if (pEntry->Bus.b.DATA & 0x40)
				{
					strcat(psz, "S6:Protected, ");
				}

				if (pEntry->Bus.b.DATA & 0x80)
				{
					strcat(psz, "S0:Not Ready, ");
				}

				strcat(psz, "\t");
				break;

			case 1:
				strcat(psz, "RD Track: ");
				break;

			case 2:
				strcat(psz, "RD Sector: ");
				break;

			case 3:
				if (pEntry->Bus.b.DATA < 32)
				{
					sprintf(szBuf, "RD Data: 0x%02X - . (%d)", pEntry->Bus.b.DATA, g_nSectorReadByteCount);
				}
				else
				{
					sprintf(szBuf, "RD Data: 0x%02X - %c (%d)", pEntry->Bus.b.DATA, pEntry->Bus.b.DATA, g_nSectorReadByteCount);
				}
				
				++g_nSectorReadByteCount;
				strcat(psz, szBuf);
				break;
		}
	}
	else if (pEntry->Bus.b.DISK_OUT_WE == 0)
	{
		switch (pEntry->Bus.b.A0 | (pEntry->Bus.b.A1 << 1))
		{
			case 0:
				g_tcTrace.byCommand = pEntry->Bus.b.DATA;
				StrCatCommand(psz, pEntry->Bus.b.DATA);
				break;

			case 1:
				g_tcTrace.byTrack = pEntry->Bus.b.DATA;
				sprintf(szBuf, "WR Track: 0x%02X", pEntry->Bus.b.DATA);
				strcat(psz, szBuf);
				break;

			case 2:
				g_tcTrace.bySector = pEntry->Bus.b.DATA;
				sprintf(szBuf, "WR Sector: 0x%02X", pEntry->Bus.b.DATA);
				strcat(psz, szBuf);
				break;

			case 3:
				g_tcTrace.byData = pEntry->Bus.b.DATA;
			
				if (pEntry->Bus.b.DATA < 32)
				{
					sprintf(szBuf, "WR Data: 0x%02X", pEntry->Bus.b.DATA);
				}
				else
				{
					sprintf(szBuf, "WR Data: 0x%02X, %c", pEntry->Bus.b.DATA, pEntry->Bus.b.DATA);
				}
				
				strcat(psz, szBuf);
				break;
		}
	}
	else
	{
//		if (pEntry->Bus.b.NMI == 0) // NMI
//		{
//			strcat(psz, "NMI ");
//		}
	
//		if (pEntry->Bus.b.RESET == 0) // RESET
//		{
//			strcat(psz, "RESET ");
//		}

		if (pEntry->Bus.b.WRNMI == 0) // WR_NMI
		{
			strcat(psz, "WR NMI MASK");
		}

		if (pEntry->Bus.b.RDNMI == 0) // RD_NMI
		{
			strcat(psz, "RD NMI STATUS");
		}

		if (pEntry->Bus.b.DVRSEL == 0) // DRV_SEL
		{
			if ((pEntry->Bus.b.DATA & 0x00001) != 0) // D0=1
			{
				strcat(psz, "WR DRV_SEL1, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00002) != 0) // D1=1
			{
				strcat(psz, "WR DRV_SEL2, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00004) != 0) // D2=1
			{
				strcat(psz, "WR DRV_SEL3, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00008) != 0) // D3=1
			{
				strcat(psz, "WR DRV_SEL4, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00010) != 0) // D4=1
			{
				strcat(psz, "Side 1, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00020) != 0) // D5=1
			{
				strcat(psz, "Precomp, ");
			}

			if ((pEntry->Bus.b.DATA & 0x00040) != 0) // D6=1
			{
				strcat(psz, "Wait, ");
			}
			
			if ((pEntry->Bus.b.DATA & 0x00080) != 0) // D7=1
			{
				strcat(psz, "MFM Mode");
			}
		}
	}
}

//----------------------------------------------------------------------------
void GetTraceHeader(char* psz)
{
//	strcpy(psz, "Count\tTime\tD0-7\tA0 A1 RD WR WRNMI RDNMI DRVSEL NMI RST WAIT\n");
	strcpy(psz, "Count\tTime\tD0-7\tA0 A1 RD WR WRNMI RDNMI DRVSEL\n");
//	printf("Count\tTime\tD0-7\n");
}

//----------------------------------------------------------------------------
void InitTraceCapture(void)
{
	char szBuf[80];

	GetTraceHeader(szBuf);
	printf(szBuf);
}

//----------------------------------------------------------------------------
void DecodeBusData(FDC_BusType* pHistory, char* psz, int nType)
{
	if (nType == 0)
	{
		sprintf(psz, "%3d\t%6d\t%02X\t", pHistory->Bus.b.Count, pHistory->dwTicks, (int) pHistory->Bus.b.DATA);
//		sprintf(psz, "%3d\t%02X\t", pHistory->Bus.b.Count, (int) pHistory->Bus.b.DATA);
		StrCatBit(psz, pHistory->Bus.b.A0);
		StrCatBit(psz, pHistory->Bus.b.A1);
		StrCatBit(psz, pHistory->Bus.b.DISK_IN_RE);
		StrCatBit(psz, pHistory->Bus.b.DISK_OUT_WE);
		StrCatBit(psz, pHistory->Bus.b.WRNMI);
		StrCatBit(psz, pHistory->Bus.b.RDNMI);
		StrCatBit(psz, pHistory->Bus.b.DVRSEL);
//		StrCatBit(psz, pHistory->Bus.b.NMI);
//		StrCatBit(psz, pHistory->Bus.b.RESET);
//		StrCatBit(psz, pHistory->Bus.b.WAIT);
		StrCatDesc(psz, pHistory);
		strcat(psz, "\r\n");
	}
	else
	{
//		sprintf(psz, "%d %08X; ", pHistory->dwTicks / 100, pHistory->Bus.dw);
		sprintf(psz, "%d %08X; ", pHistory->Bus.dw);
	}
}

//-----------------------------------------------------------------------------
DWORD CalcTicks(DWORD dwPrev, DWORD dwNow)
{
	DWORD dw;

	if (dwNow > dwPrev)
	{
		dw = (0x00FFFFFF - dwNow) + dwPrev;
	}
	else
	{
		dw = dwPrev - dwNow;
	}

	return dw;
}

//-----------------------------------------------------------------------------
BYTE TraceIgnore(DWORD dwBus)
{
	FDC_BusType* pEntry;
	FDC_BusType  btBus;

	if (g_byCaptureAll)
	{
		return FALSE;
	}
	
	btBus.Bus.dw = dwBus;
	pEntry = &btBus;

	if (pEntry->Bus.b.DISK_IN_RE == 0)
	{
		switch (pEntry->Bus.b.A0 | (pEntry->Bus.b.A1 << 1))
		{
			case 0: // RD Status:
//				return FALSE;
				break;

			case 1: // RD Track:
				break;

			case 2: // RD Sector:
				break;

			case 3: // RD Data:
				break;
		}
	}
	else if (pEntry->Bus.b.DISK_OUT_WE == 0)
	{
		switch (pEntry->Bus.b.A0 | (pEntry->Bus.b.A1 << 1))
		{
			case 0: // WR Command:
				if ((pEntry->Bus.b.DATA & 0xF0) == 0x80) // 1000xxxx - CMD: RSEC:
				{
					if ((g_FDC.byTrack == 0x12) && (g_FDC.bySector == 0x02))
					{
						g_byCaptureAll = 1;
					}
				}

				return FALSE;

			case 1: // WR Track
				return FALSE;

			case 2: // WR Sector
				return FALSE;

			case 3: // WR Data
				return FALSE;
				break;
		}
	}
	else
	{
		if (pEntry->Bus.b.WRNMI == 0) // WR_NMI
		{
//			return FALSE;
		}

		if (pEntry->Bus.b.RDNMI == 0) // RD_NMI
		{
//			return FALSE;
		}

		if (pEntry->Bus.b.DVRSEL == 0) // DRV_SEL
		{
//			return FALSE;
		}
	}

	return TRUE;
}

//-----------------------------------------------------------------------------
void RecordBusHistory(DWORD dwBus, BYTE byData)
{
	char szLine[256];
	int  nFormat = 0; // 0 => verbose; 1 => coded;

	if (g_byCaptureBusActivity == 0)
	{
		return;
	}

//	if (g_FDC.stStatus.byIntrRequest) // NMI (GPIO13)
//	{
//		dwBus &= ~(0x01 << NMI_PIN);
//	}
//	else
//	{
//		dwBus |= (0x01 << NMI_PIN);
//	}
	
//	if (g_FDC.byWaitOutput) // WAIT (GPIO6)
//	{
//		dwBus &= ~(0x01 << WAIT_PIN);
//	}
//	else
//	{
//		dwBus |= (0x01 << WAIT_PIN);
//	}

    // adjust raw bus value to align with the FDC_BusBitsType structure
    dwBus = dwBus >> 6;
 	dwBus = (dwBus & 0xFFFE01FF) + (byData << 9);

	if (TraceIgnore(dwBus))
	{
		return;
	}

	if ((dwBus != g_dwPrevBus) || (g_nBusCount > 255))
	{
		DWORD dw = time_us_32();

		g_btBus.Bus.dw      = g_dwPrevBus;
		g_btBus.dwTicks     = dw - g_dwPrevCycCnt; //CalcTicks(g_dwPrevCycCnt, systick_hw->cvr);
		g_btBus.Bus.b.Count = g_nBusCount;

		if (g_nBusTraceIndex < BUS_TRACE_SIZE)
		{
			g_btBusTrace[g_nBusTraceIndex] = g_btBus;
			++g_nBusTraceIndex;
		}

		if (nFormat == 1)
		{
			++g_nFlushCount;

			if (g_nFlushCount >= 32)
			{
				g_nFlushCount = 0;
				printf("\r\n");
			}
		}

		g_dwPrevCycCnt = dw; //systick_hw->cvr;

		g_dwPrevBus = dwBus;
		g_nBusCount = 1;
	}
	else
	{
		++g_nBusCount;
	}
}

void FlushTraceBuffer(void)
{
	char szBuf[512];
	int  i;

	g_fTraceFile = FileOpen("trace.txt", FA_WRITE | FA_CREATE_ALWAYS);

	if (g_fTraceFile == NULL)
	{
		return;
	}

	FileSeek(g_fTraceFile, 0);
	FileTruncate(g_fTraceFile);

	GetTraceHeader(szBuf);
	FileWrite(g_fTraceFile, szBuf, strlen(szBuf));

	for (i = 0; i < g_nBusTraceIndex; ++i)
	{
		DecodeBusData(&g_btBusTrace[i], szBuf, 0);
		FileWrite(g_fTraceFile, szBuf, strlen(szBuf));
	}

	FileClose(g_fTraceFile);
}
