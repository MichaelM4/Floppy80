########################################################################

Floppy 80 for the TRS-80 Model 3/4 NGA

The Floppy80 emulates up to four floppy drives.

Configuration of the Floppy80 is performed with the placement of
files on the SD-Card inserted in its card reader.
The files are as follows:

boot.cfg
- specified the defaul ini file to load at reset of the Floppy80
  when the floppy 80 boots or is reset it reads the contents of
  the boot.cfg to determine the default configuration ini file.

ini files
- specifies the disk images to load after reset.

  ini options
  - Drive0 - specified the image to load for drive :0
  - Drive1 - specified the image to load for drive :1
  - Drive2 - specified the image to load for drive :2
  - Drive3 - specified the image to load for drive :3

dmk files
- these are virtual disk images with a specific file format
  that allows them to be generated and used with a number
  of existing programs and simulators.

hfe files
- these are virtual disk images with a specific file format
  that allows them to be generated and used with a number
  of existing programs and simulators.

########################################################################

FDC utility
- Is a utility to interact with the Floppy80 from within the
  TRS-80 Model 3/4 operating environment.  Versions of FDC exist
  for the following operating systems

  - CPM
  - LDOS
  - TRSDOS

  FDC.COM is used with the CP/M OS and
  FDC/CMD is used with the rest.

  Usage

  FDC OPT PARM1:drive

  Where:
  - OPT is one of the following
    - STA - returns the status of the Floppy80.
    - INI - switches between tyhe differnt ini file on the SD-Card.
    - DMK - allows the mounting of DMK disk images in the root folder
            of the SD-Card for a specified drive (0, 1, 2 or 3).
    - IMP - imports a file from the root folder of the SD-Card
            into one of the mounted disk images (0, 1, 2 or 3).

  FDC STA
  - Displays the contects to the ini file specified by boot.cfg

  FDC INI filename.exe
  - filename.exe is optional.  If not specified and list of ini files
    on the SD-Card will be displayed allow you to select the one to
    write to boot.cfg.  When specified FDC will bypass the file
    selection process.

  FDC DMK filename.exe n
  - filename.exe:n is optional.  When specified it will mount
    the dmk file names by filename.exe into the drive specified
    by n.  For example

    FDC DMK LDOS-DATA.DMK 2

    will mount the dmk file LDOS-DATA.DMK into drive :2

    If filename.exe:n is not specfied a list of dmk files
    will be listed allowing you to select on and the drive
    to mount it into.

  FDC IMP filename.exe:n
  - imports the specified file from the root folder of the
    FAT32 formated SD-Card to the disk image indicated by n.

########################################################################

Building Source
- Floppy80 firmware
  - Uses VSCode and the Pi Pico extension.

- FDC TRS (utility for the TRSDOS related utility)
  
  - zmac fdc.asm

- FDC CP/M (utility for the CP/M related utility)
  1. Import the source file onto a working CP/M diskette.
  
       D:\TRS-80\TRSUtils\trswrite cpm-work2.dmk -o fdc.c
     
  2. Run TRS80GP loading the CP/M disk images.
     
       start trs80gp -m4 -vs -frehd -frehd_dir source -turbo -mem 128 -d0 M4-CPM-MONTEZUMA-v22.dmk -d1 cpm-hc-disk.dmk -d2 cpm-work1.dmk -d3 cpm-work2.dmk
     
  3. Compile the C source using the Hi-Tech C compilier.

     b:
     c d:fdc.c
  
 