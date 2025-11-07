#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"

#include "fdc.pio.h"
#include "Defines.h"
#include "sd_core.h"
#include "fdc.h"
#include "system.h"

#if (ENABLE_TRACE_LOG == 1)
	void RecordBusHistory(DWORD dwBus, BYTE byData);
#endif

///////////////////////////////////////////////////////////////////////////////
// API documentions is located at
// https://raspberrypi.github.io/pico-sdk-doxygen/
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// to get memory usage of the compilied program enter
//
//     size build/fdc.elf
//
// in a bash terminal window
//
// Output is
// - text indicates code size (Read Only)
// - data is initialized data (R/W)
// - bss is uninitialize data (R/W)
///////////////////////////////////////////////////////////////////////////////

#define ENABLE_LED 1
#define HOST_SEQUENCE_COUNT 9

BYTE g_byHostSequence[HOST_SEQUENCE_COUNT] = {0x80, 0x7F, 0x81, 0xFE, 0x82, 0xFD, 0x83, 0xFC, 0x84};

//-----------------------------------------------------------------------------

pio_sm_config g_pio_config;
PIO           g_pio;
uint          g_sm, g_offset;

DWORD         g_dwPreviousTimeCount;
DWORD         g_dwRotationTime;
DWORD         g_dwIndexTime;
DWORD         g_dwResetTime;
BYTE          g_byMonitorReset;

BYTE          g_byMotorWasOn;
BYTE          g_byFlushTraceBuffer;

uint32_t      g_nTimeNow;
uint32_t      g_nPrevTime;

//-----------------------------------------------------------------------------
void DetectHostSequnce(BYTE byData)
{
	if ((g_FDC.nWrHostSequence < HOST_SEQUENCE_COUNT) && (g_byHostSequence[g_FDC.nWrHostSequence] == byData))
	{
		++g_FDC.nWrHostSequence;
	}
	else
	{
		g_FDC.nWrHostSequence = 0;
		g_FDC.nRdHostSequence = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// restart pio code to release WAIT state
void ReleaseWait(void)
{
	// push pin direction mask into tx fifo (this way the pio state machine does not need to generate it)
	pio_sm_put(g_pio, g_sm, ~GPIO_IN_MASK >> WAIT_PIN);

	// restart the pio state machine at its first instruction (the address for which is g_offset)
	// set x, g_offset
	//   [3 bit opcode]      = 111
	//   [5 bits = delay]    = 00000
	//   [3 bits detination] = 001 = x
	//   [5 bits data]       = g_offset
	//   -------------------------------
	//   1110  0000 0010 0000 + [offset]
	pio_sm_exec(g_pio, g_sm, 0xE020 + g_offset);

	// mov pc, x
	//   [3 bit opcode]      = 101
	//   [5 bits = delay]    = 00000
	//   [3 bits detination] = 101 = pc
	//   [2 bits Op]         = 00  = None
	//   [3 bits Source]     = 001 = x
	//   -------------------------------
	//   1010  0000 1010 0001
	pio_sm_exec(g_pio, g_sm, 0xA0A1);
}

//----------------------------------------------------------------------------
void GetCommandText(char* psz, BYTE byCmd)
{
	char szBuf[128];
	
	*psz = 0;

	if ((byCmd & 0xF0) == 0) // 0000xxxx
	{
		strcpy(psz, "CMD: Restore");
	}
	else if ((byCmd & 0xF0) == 0x10) // 0001xxxx
	{
		sprintf(psz, "CMD: SEEK 0x%02X, From 0x%02X", g_FDC.byData, g_FDC.byTrack);
	}
	else if ((byCmd & 0xF0) == 0x20) // 0010xxxx
	{
		strcpy(psz, "CMD: Step, Do Not Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x30) // 0011xxxx
	{
		strcpy(psz, "CMD: Step, Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x40) // 0100xxxx
	{
		strcpy(psz, "CMD: Step In, Do Not Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x50) // 0101xxxx
	{
		strcpy(psz, "CMD: Step In, Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x60) // 0110xxxx
	{
		strcpy(psz, "CMD: Step Out, Do Not Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x70) // 0111xxxx
	{
		strcpy(psz, "CMD: Step Out, Update Track Register");
	}
	else if ((byCmd & 0xF0) == 0x80) // 1000xxxx
	{
		sprintf(psz, "CMD: RSEC: 0x%02X TRK: 0x%02X", g_FDC.bySector, g_FDC.byTrack);
	}
	else if ((byCmd & 0xF0) == 0x90) // 1001xxxx
	{
		strcpy(psz, "CMD: RSEC: Multiple Record");
	}
	else if ((byCmd & 0xF0) == 0xA0) // 1010xxxx
	{
		sprintf(psz, "CMD: WSEC: 0x%02X TRK: 0x%02X", g_FDC.bySector, g_FDC.byTrack);
	}
	else if ((byCmd & 0xF0) == 0xB0) // 1011xxxx
	{
		strcpy(psz, "CMD: WSEC: Multiple Record");
	}
	else if ((byCmd & 0xF0) == 0xC0) // 1100xxxx
	{
		strcpy(psz, "CMD: Read Address");
	}
	else if ((byCmd & 0xF0) == 0xD0) // 1101xxxx
	{
		sprintf(psz, "CMD: Force Interrupt (0x%02X)", byCmd);
	}
	else if ((byCmd & 0xF0) == 0xE0) // 1110xxxx
	{
		sprintf(psz, "CMD: RTRK: 0x%02X", g_FDC.byTrack);
	}
	else if ((byCmd & 0xF0) == 0xF0) // 1110xxxx
	{
		sprintf(psz, "CMD: WTRK: 0x%02X", g_FDC.byTrack);
	}
	else
	{
		strcpy(psz, "CMD: Unknown");
	}
}

//-----------------------------------------------------------------------------
void __not_in_flash_func(fdc_isr)(void)
{
	DWORD dwBus;
	WORD  wReg, wCom;
	BYTE  byData;
	BYTE  byReleaseWait;
	BYTE  byReqCount;
	char  szBuf[128];

	dwBus  = gpio_get_all();
	byData = (dwBus >> D0_PIN) & 0xFF;

	byReleaseWait = 1;
	byReqCount    = 0;

	// INTR_MASK (DISKIN, DISKOUT, WRNMI, RDNMI and DRVSEL bits 1)
	// if ((dwBus & INTR_MASK) == INTR_MASK) then DRVSEL, RDNMI, WRNMI, DISKOUT and DISKIN are all high

	if ((dwBus & DISKIN_MASK) == 0) // the DISKIN is low (READ from FDC), put data on bus
	{
		++byReqCount;

		// make all pins outputs
		gpio_set_dir_out_masked(DATA_BUS_MASK);

		wReg = (dwBus >> A0_PIN) & 0x03;

		switch (wReg)
		{
			case 0:
				byData = FdcGetStatus();
			
				++g_FDC.nReadStatusCount;

				if (g_FDC.stStatus.byIntrRequest)
				{
					g_FDC.byNmiStatusReg = 0xFF; // inverted state of all bits low except INTRQ
					g_FDC.stStatus.byIntrRequest = 0;
				}

				g_FDC.nWrHostSequence = 0;
				break;

			case 1:
				byData = g_FDC.byTrack;
				g_FDC.nWrHostSequence = 0;
				break;

			case 2:
				byData = g_FDC.bySector;
				g_FDC.nWrHostSequence = 0;
				break;

			case 3:
				if ((g_FDC.byDriveSel == 0x0F) && (g_FDC.nProcessFunction == psSendData))
				{
					byData = g_FDC.byTransferBuffer[g_FDC.nTrasferIndex];
					++g_FDC.nTrasferIndex;
					g_FDC.dwStateCounter = 10000;

					if (g_FDC.nTrasferIndex >= g_FDC.nTransferSize)
					{
						g_FDC.nProcessFunction = psIdle;
						g_FDC.stStatus.byDataRequest = 0;

						if (g_FDC.byBackupDriveSel != 0)
						{
							g_FDC.byDriveSel = g_FDC.byBackupDriveSel;
							g_FDC.byBackupDriveSel = 0;
						}
					}
				}
				else
				{
					byData = g_FDC.byData;
					g_FDC.stStatus.byDataRequest = 0;
					++g_FDC.nDataRegReadCount;

					if (g_FDC.nWrHostSequence == HOST_SEQUENCE_COUNT)
					{
						byData = g_byHostSequence[g_FDC.nRdHostSequence];
						++g_FDC.nRdHostSequence;
					}
				}

				break;
		}

		gpio_put_masked(DATA_BUS_MASK, byData << D0_PIN);

		#if (ENABLE_TRACE_LOG == 1)
			RecordBusHistory(dwBus, byData);
		#endif
	}

	if ((dwBus & RDNMI_MASK) == 0) // the RDNMI is low (read NMI latch from FDC), put data on bus
	{
		++byReqCount;

		// make all pins outputs
		gpio_set_dir_out_masked(DATA_BUS_MASK);

		byData = g_FDC.byNmiStatusReg;

		gpio_put_masked(DATA_BUS_MASK, byData << D0_PIN);

		#if (ENABLE_TRACE_LOG == 1)
			RecordBusHistory(dwBus, byData);
		#endif
	}
	
	if ((dwBus & DISKOUT_MASK) == 0) // DISKOUT (WRITE to FDC)
	{
		++byReqCount;

		wReg = (dwBus >> A0_PIN) & 0x03;

		switch (wReg)
		{
			case 0: // address 0xF0/240, command register
				g_FDC.byCommandReg    = byData;
				g_FDC.byCommandType   = FdcGetCommandType(byData);
				g_FDC.byNmiStatusReg  = 0xFF;
				g_FDC.nWrHostSequence = 0;

				if (g_FDC.stStatus.byIntrRequest)
				{
					g_FDC.byNmiStatusReg = 0xFF; // inverted state of all bits low except INTRQ
					g_FDC.stStatus.byIntrRequest = 0;
				}

				memset(&g_FDC.stStatus, 0, sizeof(g_FDC.stStatus));

				if (g_FDC.byDriveSel == 0x0F)
				{
					g_FDC.byCommandReceived      = 1;
					g_FDC.stStatus.byBusy        = 1;
					g_FDC.stStatus.byDataRequest = 0;
				}
				else
				{
					wCom = g_FDC.byCommandReg & 0xF0;

					if (wCom == 0xD0) // 0xD0 is Force Interrupt command
					{
						g_nWaitTime             = time_us_64();
						g_FDC.byCommandType     = 4;
						g_tdTrack.nReadSize     = 0;
						g_tdTrack.nReadCount    = 0;
						g_tdTrack.nWriteSize    = 0;
						g_FDC.byCurCommand      = g_FDC.byCommandReg;
						g_FDC.byIntrEnable      = g_FDC.byCurCommand & 0x0F;
						g_FDC.nProcessFunction  = psIdle;
						g_FDC.byCommandReceived = 0;
						memset(&g_FDC.stStatus, 0, sizeof(g_FDC.stStatus));
					}
					else
					{
						g_FDC.byCommandReceived = 1;
						g_FDC.stStatus.byBusy   = 1;
					}
				}

				break;

			case 1: // address 0xF1/241, track register
				g_FDC.byTrack = byData;
				g_FDC.nWrHostSequence = 0;
				break;

			case 2: // address 0xF2/242, sector register
				g_FDC.bySector        = byData;
				g_FDC.nWrHostSequence = 0;
				break;

			case 3: // address 0xF3/243, data register
				g_FDC.byData = byData;
				g_FDC.stStatus.byDataRequest = 0;
				DetectHostSequnce(byData);
				break;
		}

		#if (ENABLE_TRACE_LOG == 1)
			RecordBusHistory(dwBus, byData);
		#endif
	}

	if ((dwBus & WRNMI_MASK) == 0) // WRNMI (WRITE to FDC)
	{
		++byReqCount;

		g_FDC.byNmiMaskReg = byData;

		if (byData & 0x80)
		{
			g_FDC.byNmiMaskReg = byData;
		}
		else
		{
			g_FDC.stStatus.byIntrRequest = 0;
		}

		#if (ENABLE_TRACE_LOG == 1)
			RecordBusHistory(dwBus, byData);
		#endif
	}

	if ((dwBus & DRVSEL_MASK) == 0) // DRVSEL (WRITE to FDC)
	{
		++byReqCount;

		if (((byData & 0x0F) == 0x0F) && // host drive select?
			(g_FDC.byBackupDriveSel == 0))
		{
			g_FDC.byBackupDriveSel = g_FDC.byDriveSel;
		}

		g_FDC.byDriveSel = byData;

		++g_FDC.nDrvSelWriteCount;

		// do not allow a wait output if WAITTIMEOUT, INTRQ or DRQ are active
		if (g_FDC.stStatus.byIntrRequest == 0)
		{
			// in "real" hardware the wait is activated on rising edge of the DRV_SEL input
			// here it is already activated by the PIO code on every FDC read/write operation
			if ((byData & 0x40) != 0) // activate WAIT
			{
				g_FDC.dwWaitTimeoutCount = 2000;
				g_FDC.byWaitOutput = 1;
				byReleaseWait = 0;
			}
		}

		g_FDC.dwMotorOnTimer = 2000000;

		if (ENABLE_LED)
		{
			gpio_put(RED_LED_PIN, 0);
		}

		#if (ENABLE_TRACE_LOG == 1)
			RecordBusHistory(dwBus, byData);
		#endif
	}

	if (byReqCount > 1)
	{
		++byReqCount;
	}

	if (byReleaseWait)
	{
		// re-enable automatic WAIT generation
		ReleaseWait(); // allow pio state machine to resume
	}

    irq_clear(PIO0_IRQ_0);
    pio_interrupt_clear(g_pio, 0);
}

///////////////////////////////////////////////////////////////////////////////
// GPIO	NET 	IN	OUT
//-----------------------------------------------------------------------------
// 0	LED			*	(Motor ON LED)
// 1	OE			*	(BUS level translator Output Enable)
// 2	CLK			*	(SD-Card)
// 3	CMD			*	(SD-Card)
//
// 4	DAT0	*		(SD-Card)
// 5	DAT3		*	(SD-Card)
// 6	WAIT		*	(Wait control, high pulls wait low)
// 7	DRVSEL	*		(write to the Drive Select Register)
//
// 8	RDNMI	*		(read the NMI register)
// 9	WRNMI	*		(write to the NMI register)
// 10	DISKOUT	*		(write to FDC, register identified by A0, A1)
// 11	DISKIN	*		(read from FDC, register identified by A0, A1)
//
// 12	RESET	*		(pulled low by reset)
// 13	NMI 		*	(NMI control, high pulls NMI low)
// 14	DIR			*	(set the direction of the bus level translator)
// 15	D0		*	*	(bit 0 of the data bus)
//
// 16	D1		*	*	(bit 1 of the data bus)
// 17	D2		*	*	(bit 2 of the data bus)
// 18	D3		*	*	(bit 3 of the data bus)
// 19	D4		*	*	(bit 4 of the data bus)
//
// 20	D5		*	*	(bit 5 of the data bus)
// 21	D6		*	*	(bit 6 of the data bus)
// 22	D7		*	*	(bit 7 of the data bus)
// 23	-
//
// 24	-
// 25	-
// 26	-
// 27	A0		*		(bit 0 of address/regsister select)
//
// 28	A1		*		(bit 1 of address/regsister select)
// 29	-
// 30	-
// 31	-
//-----------------------------------------------------------------------------
void InitGPIO(void)
{
    // configure pins for input
    gpio_init_mask(GPIO_IN_MASK);

    gpio_init(RESET_PIN);
    gpio_set_dir(RESET_PIN, GPIO_IN);
    gpio_set_pulls(RESET_PIN, true, false);

    gpio_init(A0_PIN);
    gpio_set_dir(A0_PIN, GPIO_IN);

    gpio_init(A1_PIN);
    gpio_set_dir(A1_PIN, GPIO_IN);

    gpio_init(D0_PIN);
    gpio_set_dir(D0_PIN, GPIO_IN);

    gpio_init(D1_PIN);
    gpio_set_dir(D1_PIN, GPIO_IN);

    gpio_init(D2_PIN);
    gpio_set_dir(D2_PIN, GPIO_IN);

    gpio_init(D3_PIN);
    gpio_set_dir(D3_PIN, GPIO_IN);

    gpio_init(D4_PIN);
    gpio_set_dir(D4_PIN, GPIO_IN);

    gpio_init(D5_PIN);
    gpio_set_dir(D5_PIN, GPIO_IN);

    gpio_init(D6_PIN);
    gpio_set_dir(D6_PIN, GPIO_IN);

    gpio_init(D7_PIN);
    gpio_set_dir(D7_PIN, GPIO_IN);

    // configure pins for output
    gpio_init(NMI_PIN);
    gpio_set_dir(NMI_PIN, GPIO_OUT);
    gpio_put(NMI_PIN, 0);

	if (ENABLE_LED)
	{
		gpio_init(RED_LED_PIN);
		gpio_set_dir(RED_LED_PIN, GPIO_OUT);
		gpio_put(RED_LED_PIN, 0);
	}

    gpio_init(OE_PIN);
    gpio_set_dir(OE_PIN, GPIO_OUT);
    gpio_put(OE_PIN, 0);

    gpio_init(WAIT_PIN);
    gpio_set_dir(WAIT_PIN, GPIO_OUT);
    gpio_put(WAIT_PIN, 1); // activate wait until FDC initialization is complete

    gpio_init(DIR_PIN);
    gpio_set_dir(DIR_PIN, GPIO_OUT);
    gpio_put(DIR_PIN, 1); // Z80 to FDC
}

///////////////////////////////////////////////////////////////////////////////
void UpdateCounters(void)
{
	uint32_t nDiff;

	g_nTimeNow  = time_us_32();

	if (g_nTimeNow >= g_nPrevTime)
	{
		nDiff = g_nTimeNow - g_nPrevTime;
	}
	else
	{
		nDiff = 0x100000000 - g_nPrevTime + g_nTimeNow;
	}

	g_nPrevTime = g_nTimeNow;

	if (g_FDC.dwWaitTimeoutCount > 0)
	{
		g_FDC.dwWaitTimeoutCount = CountDown(g_FDC.dwWaitTimeoutCount, nDiff);
		
		if (g_FDC.dwWaitTimeoutCount == 0) // release wait line
		{
			FdcReleaseWait();
		}
	}

	if (g_FDC.dwMotorOnTimer != 0)
	{
		if (ENABLE_LED)
		{
			gpio_put(RED_LED_PIN, 0);
		}
		
		g_byMotorWasOn = 1;

		g_FDC.dwMotorOnTimer  = CountDown(g_FDC.dwMotorOnTimer, nDiff);
		g_FDC.dwRotationCount = CountUp(g_FDC.dwRotationCount, nDiff);

		// (g_dwTimerFrequency / 5) = count to make one full rotation of the diskette (200 ms at 300 RPM)
		if (g_FDC.dwRotationCount >= g_dwRotationTime)
		{
			g_FDC.dwRotationCount -= g_dwRotationTime;
		}

		if (g_FDC.dwRotationCount < g_dwIndexTime)
		{
			g_FDC.stStatus.byIndex = 1;
		}
		else
		{
			g_FDC.stStatus.byIndex = 0;
		}
	}
	else
	{
		if (ENABLE_LED)
		{
			gpio_put(RED_LED_PIN, 1);
		}

		if (g_byMotorWasOn)
		{
			g_byMotorWasOn = 0;
		}
	}

	if (gpio_get(RESET_PIN) == 0)
	{
		if (g_byMonitorReset)
		{
			g_FDC.dwResetCount = CountUp(g_FDC.dwResetCount, nDiff);

			if (g_FDC.dwResetCount >= g_dwResetTime) // ~ 1ms duration
			{
				g_FDC.byResetFDC = 1;
			}
		}
	}
	else
	{
		g_FDC.dwResetCount = 0;
		g_byMonitorReset   = TRUE;
	}

	if (g_FDC.dwStateCounter > 0)
	{
		g_FDC.dwStateCounter = CountDown(g_FDC.dwStateCounter, nDiff);
	}

	if (get_cd())		// 0 => card removed; 1 => card inserted;
	{
		if (g_dwSdCardPresenceCount < g_dwSdCardMaxPresenceCount)
		{
			g_dwSdCardPresenceCount = CountUp(g_dwSdCardPresenceCount, nDiff);
		}
	}
	else
	{
		g_dwSdCardPresenceCount = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
int main()
{
	int i;

    stdio_init_all();

    systick_hw->csr = 0x5;
    systick_hw->rvr = 0x00FFFFFF;

	g_byMotorWasOn = 0;
	g_byFlushTraceBuffer = 0;
	g_nTimeNow  = time_us_32();
	g_nPrevTime = g_nTimeNow;

    InitGPIO();

	if (ENABLE_LED)
	{
		gpio_put(RED_LED_PIN, 1);
		gpio_put(RED_LED_PIN, 0);
	}

    InitVars();

    g_pio    = pio0;
    g_sm     = pio_claim_unused_sm(g_pio, true);
    g_offset = pio_add_program(g_pio, &fdc_program);

    fdc_program_init(g_pio, g_sm, g_offset, WAIT_PIN, DRVSEL_PIN, DIR_PIN, &g_pio_config);

    // Start running the PIO program in the state machine
    pio_sm_set_enabled(g_pio, g_sm, true);

    irq_set_exclusive_handler(PIO0_IRQ_0, fdc_isr);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(g_pio, pis_interrupt0, true);

	// push pin direction mask into tx fifo
	pio_sm_put(g_pio, g_sm, ~GPIO_IN_MASK >> WAIT_PIN);

   	SDHC_Init();
    FileSystemInit();
 	FdcInit();

	#if (ENABLE_TRACE_LOG == 1)
		InitBusTrace();
	#endif

    gpio_put(WAIT_PIN, 0); // release wait
	
    while (true)
    {
		UpdateCounters();
        FdcServiceStateMachine();

		#if (ENABLE_TRACE_LOG == 1)
			if (g_byFlushTraceBuffer)
			{
				FlushTraceBuffer();
				g_byFlushTraceBuffer = 0;
			}
		#endif
    }   
}
