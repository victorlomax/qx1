
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
*/
#define FDC_DEBUG

#define PORT_INPUT  0x00
#define PORT_OUTPUT 0xff

#include <qx1.h>

unsigned char format_tracks, format_sectors;  // These are values provided by MPU

#include <mb8877.h>
#include <events.h>
#include <mb8877.cpp>
#include "sdcard.cpp"

// This variable will get data from the QX1 data bus
volatile char qx1bus;

void display(char *command)
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

  Serial.println("%s\tRegisters\tCMD=%2xh (%s) DATA=%2xh TRK=%3d SEC=%2d", command,
    mb8877.reg[CMD],
    cmdstr[mb8877.reg[CMD] >> 4],
    mb8877.reg[DATA],
    mb8877.reg[TRACK],
    mb8877.reg[SECTOR]);

  Serial.println("Status register");
  Serial.println("[%c] Not ready", !(mb8877.reg[STATUS] & 0x80) ? 'X' : ' ');
  Serial.println("[%c] Write protected", !(mb8877.reg[STATUS] & 0x40) ? 'X' : ' ');
  Serial.println("[%c] Head loaded", !(mb8877.reg[STATUS] & 0x20) ? 'X' : ' ');
  Serial.println("[%c] Seek error", !(mb8877.reg[STATUS] & 0x10) ? 'X' : ' ');
  Serial.println("[%c] CRC error", !(mb8877.reg[STATUS] & 0x08) ? 'X' : ' ');
  Serial.println("[%c] Track 0", !(mb8877.reg[STATUS] & 0x04) ? 'X' : ' ');
  Serial.println("[%c] Index hole", !(mb8877.reg[STATUS] & 0x02) ? 'X' : ' ');
  Serial.println("[%c] Busy", !(mb8877.reg[STATUS] & 0x01) ? 'X' : ' ');
 
  //Serial.println("Disk\tTRACK=%d DSK=%d SIDE=%d", fdc.track, fdc.disk, fdc.side);
}

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
  DDRC |= 0x0f;   // Set port C (0-3) as output
  qx1bus=0;   // no data on QX1 bus

  Serial.println("01 Card init..");

  pinMode(SD_CHIP_SELECT_PIN, OUTPUT);
  digitalWrite(SD_CHIP_SELECT_PIN, HIGH);   // Activate Pullup resistor

  scanSD();     // If SD card is present ...
  scanDirectory(fdc.disk);  // ... scan the directory
}

void loop()
{
  int lock=FALSE;
#ifdef DEBUG
  int incomingByte;
#endif

// - Prepare interrupts; on falling edge, we'll call getNewCommand
  attachInterrupt(0, getNewCommand, FALLING);

// Global Enable INT0 interrupt
  GICR |= ( 1 < < INT0);
// Signal change triggers interrupt
  MCUCR |= ( 1 << ISC00);
  MCUCR |= ( 0 << ISC01);

  digitalWrite(FDC_DRQ, HIGH);  // Reset DRQ interrupt
  digitalWrite(FDC_IRQ, HIGH);

  PORTC &= 0xf0;
  PORTC |= BUS_SELECT_ADDRESS;

  if (Serial.available() > 0) {
    // read the incoming byte:
    incomingByte = Serial.read();
    switch(incomingByte)
    {
      case 's':
      case 'S': if(lock){++side&=2; Serial.print("SIDE=");Serial.println(side);} break;
      case 'O': if(lock){Serial.println("OPEN");} break;
      case '>':
      case '+': if(!lock){Serial.println(">"); scanDirectory(1);}
                                  else {track++; Serial.print("TRACK=");Serial.println(track);} break;
      case '<':
      case '-': if(!lock){Serial.println("<"); scanDirectory(-1);} break;
      case '0': if(!lock){Serial.println("<<"); scanDirectory(0);} break;
      case '.': if(!lock){Serial.println(">>"); scanDirectory(999);} break;
      case ' ': lock=!lock; break;
    }
  }
}

