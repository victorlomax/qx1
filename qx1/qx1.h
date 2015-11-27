#include <events.h>

/* ==========================================
  Yamaha QX1 Floppy Emulator with Arduino Mini

	Francois Basquin

-- History
2015 Jan 28: v2.0	Restart working after crash

-- References
* Arduino Mini: http://arduino.cc/en/Main/ArduinoBoardMini
* SD library: http://www.roland-riegel.de/sd-reader/index.html
* MicroFAT: http://arduinonut.blogspot.ca/2008/04/ufat.html
* mb8877a: from RetroPC ver 2006.12.06 by Takeda.Toshiya, http://homepage3.nifty.com/takeda-toshiya/?
========================================== */

#ifndef _H_QX1
#define _H_QX1

// These parameters are specific to the Yamaha QX1
//	Track format=ISOIBM_MFM_ENCODING
//	2 sides, 160 tracks/side (775072 bytes/side)
//	The first byte on each sector is Data Address Mark (i.e. not a data byte)
//	zone 0 (track 0 to 39):   819200 bytes (5 sectors/track * 2 sides, 1024 bytes/sector = 10240 bytes/track)
//	zone 1 (track 40 to 79): 728064 bytes (9 sectors/track * 2 sides, 512 bytes/sector = 9216 bytes/track)
//	zone CRC: 2444 bytes (2 bytes / sector)
//	1550144 bytes/disk

#define FDC_DISKS		99	// Virtual disks per SD card
#define FDC_TRACKS		80	// Tracks per disk (from Canon MF-221 datasheet)
#define FDC_SIDE		775072	// Bytes per side
#define FDC_SIZE_TRACK_0	10240	// Bytes per track, zone 0
#define FDC_SIZE_TRACK_1	9216	// Bytes per track, zone 1
#define FDC_SIZE_SECTOR_0	1024	// Bytes per sector, zone 0
#define FDC_SIZE_SECTOR_1	512	// Bytes per sector, zone 1
#define FDC_SECTORS_0		5	// Sectors per track, zone 0
#define FDC_SECTORS_1		9	// Sectors per track, zone 1

#define FDC_FRM_MAXSECTOR  9
#define FDC_FRM_BLOCKSIZE 512

// ------------------------------------------------
// Manage U4A (74LS139) via PORTC(0,1) to drive PORTD
//
//  [  PORTC  ][             PORTD                     ]
//  [-C1-+-C0-][----+----+----+----+----+----+----+----]
//  | 1     1 ||DB7  DB6  DB5  DB4   -   R/W   E    -  | BUS_SELECT_LCD
//  | 1     0 || -    -    -    -   <<    <   >    >>  | BUS_SELECT_KEYBOARD
//  | 0     1 || -    -    -    -   A1   A0   /WR  /RD | BUS_SELECT_ADDRESS
//  | 0     0 ||DAL7 DAL6 DAL5 DAL4 DAL3 DAL2 DAL1 DAL0| BUS_SELECT_DATA
// --------------------------------------------------------------------

#define BUS_SELECT_DATA 0x00
#define BUS_SELECT_ADDRESS  0x01
#define BUS_SELECT_KEYBOARD 0x02
#define BUS_SELECT_LCD  0x03

// Arduino Mini pins
#define PD0 RX
#define PD1 TX
#define PD2 2
#define PD3 3
#define PD4 4
#define PD5 5
#define PD6 6
#define PD7 7

#define ADC0  14
#define ADC1  15
#define ADC2  16
#define ADC3  17

#define PC0 ADC0
#define PC1 ADC1

#define FDC_IRQ ADC2
#define FDC_DRQ ADC3

#endif
