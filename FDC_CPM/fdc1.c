
#include <stdio.h>
#include <z80regs.h>
#include <fcntl.h>
#include "fdc.h"

#option INLIB

/*******************************************************************/
ShowHelp()
{
	ClrScr();
	puts("FDC utility version 1.0.  Command line options:");
	/*puts("SET - set FDC date and time to the TRS-80 date and time."); */
	/*puts("GET - set TRS-80 date and time to the FDC date and time."); */
	puts("STA - get FDC status (firmware version, mounted disks, etc.).");
	puts("DIR - get a directory listing of the FDC SD-Card root folder.");
	puts("INI - select the default ini file.    FDC INI filename.ext n");
	puts("DMK - mount a disk image.             FDC DMK filename.ext n");
	puts("HFE - mount a disk image.             FDC HFE filename.ext n");
	puts("IMP - import a file from the SD-Card. FDC IMP filename.ext n");
	puts("EXP - export a file to the SD-card.   FDC EXP filename.ext:n");
	puts(" ");
	puts("      filename.ext - is the filname and extension.");
	puts("      n - is the drive number (0-3).");
}

/*******************************************************************/
main(argc, argv)
int   argc;
char* argv[];
{
	char c;
	int i;

	for (i = 0; i < argc; ++i)
	{
		strtoupr(argv[i]);
	}

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
/*
	if (strcmp(argv[1], "SET") == 0)
	{
		SetDT();
	}
	else if (strcmp(argv[1], "GET") == 0)
	{
		GetDT();
	}
	else
*/
	if (strcmp(argv[1], "STA") == 0)
	{
		GetStatus();
	}
	else if (strcmp(argv[1], "DIR") == 0)
	{
		GetDir();
	}
	else if (strcmp(argv[1], "INI") == 0)
	{
		if (argc < 3)
		{
			ListDiskImageFiles(".INI");
		}
		else
		{
			MountImage(argv[2], atoi(argv[3]));
		}

		GetStatus();
	}
	else if (strcmp(argv[1], "DMK") == 0)
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
	else if (strcmp(argv[1], "HFE") == 0)
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
		if (argc < 4)
		{
			puts("Insufficient command line arguments.");
		}
		else
		{
			GetFile(argv[2], atoi(argv[3]));
		}
	}
	else if (strcmp(argv[1], "EXP") == 0)
	{
		if (argc < 3)
		{
			puts("Insufficient command line arguments.");
		}
		else
		{
			PutFile(argv[2]);
		}
	}
	else
	{
		puts("Unrecognized command.");
	}

	return 0;
}
