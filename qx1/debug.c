
#include <events.h>

#include <SoftwareSerial.h>

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
		fdc.reg[CMD],
		cmdstr[fdc.reg[CMD] >> 4],
		fdc.reg[DATA],
		fdc.reg[TRACK],
		fdc.reg[SECTOR]);

	Serial.println("Status register");
	Serial.println("[%c] Not ready", !(fdc.reg[STATUS] & 0x80) ? 'X' : ' ');
	Serial.println("[%c] Write protected", !(fdc.reg[STATUS] & 0x40) ? 'X' : ' ');
	Serial.println("[%c] Head loaded", !(fdc.reg[STATUS] & 0x20) ? 'X' : ' ');
	Serial.println("[%c] Seek error", !(fdc.reg[STATUS] & 0x10) ? 'X' : ' ');
	Serial.println("[%c] CRC error", !(fdc.reg[STATUS] & 0x08) ? 'X' : ' ');
	Serial.println("[%c] Track 0", !(fdc.reg[STATUS] & 0x04) ? 'X' : ' ');
	Serial.println("[%c] Index hole", !(fdc.reg[STATUS] & 0x02) ? 'X' : ' ');
	Serial.println("[%c] Busy", !(fdc.reg[STATUS] & 0x01) ? 'X' : ' ');
 
	Serial.println("Disk\tTRACK=%d DSK=%d SIDE=%d", fdc.track, fdc.disk, fdc.side);
}

