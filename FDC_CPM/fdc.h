
#define VKEY_CLR 31
#define VKEY_BRK 128

#define HOST_SEQUENCE_COUNT 9

extern char g_byDetected;

void DetectFdcHost();
void GetDate();
void GetTime();
void MountImage();
void GetDT();
void SetDT();
void GetStatus();
void GetDir();

int GetFile();
int PutFile();
int WaitNotBusy();
int WaitDRQ();
int ReadByte();
int WriteByte();
int ReadData();
int WriteData();
int ReadStatus();
int FindFirst();
int FindNext();

void ClrScr();
void strtoupr();

char* SkipBlanks();
char* SkipToBlank();

char SelectDiskImage();
void ListDiskImageFiles();
