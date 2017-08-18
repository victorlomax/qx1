/*
	Yamaha QX1 floppy drive emulator

	Francois Basquin, 2014 mar 20

References
	Arduino Mini: http://arduino.cc/en/Main/ArduinoBoardMini
	SD library: http://www.roland-riegel.de/sd-reader/index.html
	MicroFAT: http://arduinonut.blogspot.ca/2008/04/ufat.html
	CRC: http://stackoverflow.com/questions/17196743/crc-ccitt-implementation
	mb8877a: from RetroPC ver 2006.12.06 by Takeda.Toshiya, http://homepage3.nifty.com/takeda-toshiya/
	Fujitsu MB8877a datasheet: map.grauw.nl/resources/disk/fujitsu_mb8876a.pdf
	Interrupts: https://thewanderingengineer.com/2014/08/11/arduino-pin-change-interrupts/
*/
#define FDC_DEBUG

#define PORT_INPUT  0x00
#define PORT_OUTPUT 0xff

unsigned char format_tracks, format_sectors;  // These are values provided by MPU


#include <avr/io.h>
#include <avr/interrupt.h>

#include "mb8877.h"
#include "sdcard.h"
/* #include <ewents.h> */
/*#include "mb8877.cpp"*/
/*#include "sdcard.cpp"*/
#ifndef _H_QX1
#include "qx1.h"
#endif

// This variable will get data from the QX1 data bus
volatile char qx1bus;

// ----------------------------------------------------------------------------
// Arduino setup routine
// ----------------------------------------------------------------------------

void setup() {
  Serial.begin(9600);
  Serial.println("00 FDC init..");

  // ----- Set ports
  // Port D is the bus; input or output
  // Port C:0-1 drive the 74139 to manage the digital bus; always outputs
  // Port C:2-3 drive the interrupts; always outputs

  DDRD = PORT_INPUT;  // Set port D as input
  DDRC |= 0x0f;       // Set port C (0-3) as output
  qx1bus=0;           // no data on QX1 bus

  Serial.println("01 Card init..");

  pinMode(SD_CHIP_SELECT_PIN, OUTPUT);
  digitalWrite(SD_CHIP_SELECT_PIN, HIGH);   // Activate Pullup resistor

  scanSD();                 // If SD card is present ...
  scanDirectory(1);  // ... scan the directory
}

void	bus_request()
{
	qx1bus = PORTD;
}

void	serve_request(int s, int r)
{
	detachInterrupt(digitalPinToInterrupt(2));
	detachInterrupt(digitalPinToInterrupt(3));

	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_DATA;

	if (s==0)
		mb8877.reg[r] = PORTD;
	else
		PORTD = mb8877.reg[r];

	digitalWrite(FDC_DRQ, HIGH);	// Data present
	digitalWrite(FDC_IRQ, HIGH);	// Command completed

	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_ADDRESS;

	attachInterrupt(digitalPinToInterrupt(2),bus_request, LOW);
	attachInterrupt(digitalPinToInterrupt(3),bus_request, LOW);
}

void loop()
{
  int lock=FALSE;
  int incomingByte; // DEBUG

	switch(qx1bus)
	{
		case 0x04: serve_request(0, STATUS); break;
		case 0x05: serve_request(0, TRACK); break;
		case 0x06: serve_request(0, SECTOR); break;
		case 0x07: serve_request(0, DATA); break;
		case 0x08: serve_request(1, COMMAND); break;
		case 0x09: serve_request(1, TRACK); break;
		case 0x0a: serve_request(1, SECTOR); break;
		case 0x0b: serve_request(1, DATA); break;
	}

// DEBUG ----
  if (Serial.available() > 0) {
    // read the incoming byte:
    incomingByte = Serial.read(); 
    switch(incomingByte)
    {
      case 'O': if(lock){Serial.println("OPEN");} break;
      case '>':
      case '+': if(!lock){Serial.println(">"); scanDirectory(1);} break;
      case '<':
      case '-': if(!lock){Serial.println("<"); scanDirectory(-1);} break;
      case '0': if(!lock){Serial.println("<<"); scanDirectory(0);} break;
      case '.': if(!lock){Serial.println(">>"); scanDirectory(999);} break;
      case ' ': lock=!lock; break;
    }
  }
// ---- DEBUG
}

void fdcdisplay(char *command)
{
  const char *cmdstr[0x10] = {
    "  I Seek track 0",
    "  I Seek",
    "  I Step",
    "  I Step",
    "  I Step In",
    "  I Step In",
    "  I Step Out",
    "  I Step Out",
    " II Read One Sector",
    " II Read Multiple Sector",
    " II Write One Sector",
    " II Write Multiple Sector",
    "III Read Address",
    " IV Force interrupt ",
    "III Read Track",
    "III Write Track"};

  Serial.print("Reg[CMD]="); Serial.println(mb8877.reg[CMD]);
  Serial.print("Reg[DATA]="); Serial.println(mb8877.reg[DATA]);
  Serial.print("Reg[TRACK]="); Serial.println(mb8877.reg[TRACK]);
  Serial.print("Reg[SECTOR]="); Serial.println(mb8877.reg[SECTOR]);
  Serial.println(cmdstr[mb8877.reg[CMD] >> 4]);

  Serial.println("Status register");
  Serial.print("      Not ready: "); Serial.println(!(mb8877.reg[STATUS] & 0x80) ? 'X' : ' ');
  Serial.print("Write protected: "); Serial.println(!(mb8877.reg[STATUS] & 0x40) ? 'X' : ' ');
  Serial.print("    Head loaded: "); Serial.println(!(mb8877.reg[STATUS] & 0x20) ? 'X' : ' ');
  Serial.print("     Seek error: "); Serial.println(!(mb8877.reg[STATUS] & 0x10) ? 'X' : ' ');
  Serial.print("      CRC error: "); Serial.println(!(mb8877.reg[STATUS] & 0x08) ? 'X' : ' ');
  Serial.print("        Track 0: "); Serial.println(!(mb8877.reg[STATUS] & 0x04) ? 'X' : ' ');
  Serial.print("     Index hole: "); Serial.println(!(mb8877.reg[STATUS] & 0x02) ? 'X' : ' ');
  Serial.print("           Busy: "); Serial.println(!(mb8877.reg[STATUS] & 0x01) ? 'X' : ' ');
 
  //Serial.println("Disk\tTRACK=%d DSK=%d SIDE=%d", fdc.track, fdc.disk, fdc.side);
}
