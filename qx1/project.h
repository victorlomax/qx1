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

#ifndef _H_PROJECT
#define _H_PROJECT

uchar format_tracks, format_sectors;	// These are values provided by MPU

// Arduino Mini pins
#define PD0 RX
#define PD1 TX
#define PD2 2
#define PD3 3
#define PD4 4
#define PD5 5
#define PD6 6
#define PD7 7

#define	INT0 2

#define PC0 ADC0
#define PC1 ADC1
#define FDC_IRQ ADC2
#define FDC_DRQ ADC3

#define OUTPUT 0xff

// ------------------------------------------------
// Manage U4A (74LS139) via PORTC(0,1) to drive PORTD
//
//  [             PORTD                     ]  PORTC
//  -----+----+----+----+----+----+----+-----+-------------------------
//    DB7  DB6  DB5  DB4   -   R/W   E    -  | BUS_SELECT_LCD
//     -    -    -    -   <<    <   >    >>  | BUS_SELECT_KEYBOARD
//     -    -    -    -   A1   A0   /WR  /RD | BUS_SELECT_ADDRESS
//   DAL7 DAL6 DAL5 DAL4 DAL3 DAL2 DAL1 DAL0 | BUS_SELECT_DATA
// --------------------------------------------------------------------

#define BUS_SELECT_DATA	0x00
#define BUS_SELECT_ADDRESS	0x01
#define BUS_SELECT_KEYBOARD	0x02
#define BUS_SELECT_LCD	0x03

#endif

