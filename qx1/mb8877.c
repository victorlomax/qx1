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

// ----------------------------------------------------------------------------
// Includes and defines
// ----------------------------------------------------------------------------

// #define _MICROFAT

#define DEBUG
#define SD_DEBUG

#include "mb8877.h"
#include "qx1.h"
#include "project.h"
#include "crc.h"
#include <SD.h>
#include <SdFat.h>

#include "../config.h"


// ----- Definition of interrupt names
#include <avr/io.h>
// ----- ISR interrupt service routine
#include <avr/interrupt.h>

#define FDC_FRM_MAXSECTOR	9
#define FDC_FRM_BLOCKSIZE	512

// ----- Events
#define EVENT_SEEK		0
#define EVENT_SEEKEND		1
#define EVENT_SEARCH		2
#define EVENT_TYPE4		3
#define EVENT_MULTI1		4
#define EVENT_MULTI2		5
#define EVENT_LOST		6

#define PORT_INPUT	0x00
#define PORT_OUTPUT	0xff

// ----------------------------------------------------------------------------
// Vars and consts
// ----------------------------------------------------------------------------

// ----- Delay table: 6msec, 12msec, 20msec, 30msec
static const int delays[4] = {600, 1200, 2000, 3000};

Sd2Card  card;
SdVolume volume;
SdFile   root;
File     diroot;
dir_t	 direntry;

// This variable will get data from the QX1 data bus
volatile unsigned char qx1bus;

int incomingByte = 0;   // for incoming serial data
int   ndisk=0;            // Number of virtual-disks on SD
int   lock=false;
int   side=0,
      track=0,
      sector=0;

char	filename[8];	// SD card filename

// ----------------------------------------------------------------------------
// DECLARE FUNCTION
// ----------------------------------------------------------------------------
void readsector();	

// ----------------------------------------------------------------------------
// DEBUG
// ----------------------------------------------------------------------------

#ifdef FDC_DEBUG
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
#endif

// ----------------------------------------------------------------------------
// Interrupt Management
// ----------------------------------------------------------------------------
volatile int Interrupt = false;	// When QX1 pin /RD goes low, Interrupt goes true

void CatchWriteInterrupt(INT0_vect) {
	// check the value again - since it takes some time to
	// activate the interrupt routine, we get a clear signal.
	qx1bus = digitalRead(sensePin);
	Interrupt=true;
}

// ----------------------------------------------------------------------------
// Arduino setup routine
// ----------------------------------------------------------------------------

void setup() {
	Serial.begin(9600);
	Serial.println("00 FDC init..");

	fdc.vector = FDC_SEEK_FORWARD;
	fdc.reg[TRACK] = fdc.reg[STATUS] = registers[CMD] = registers[SECTOR] = registers[DATA] = 0;
	fdc.disk = -1;
	fdc.track = fdc.side = fdc.cmdtype = 0;

	// ----- Set ports
	// Port D is the bus; input or output
	// Port C:0-1 drive the 74139 to manage the digital bus; always outputs
	// Port C:2-3 drive the interrupts; always outputs

	DDRD = PORT_INPUT;	// Set port D as input
	DDRC |= 0x0f;		// Set port C (0-3) as output
	qx1bus=0;		// no data on QX1 bus

	Serial.println("01 Card init..");

	pinMode(SD_CHIP_SELECT_PIN, OUTPUT);
	digitalWrite(SD_CHIP_SELECT_PIN, HIGH);   // Activate Pullup resistor

	scanSD();			// If SD card is present ...
	scanDirectory(fdc.disk);	// ... scan the directory
}

void loop()
{
	if (Serial.available() > 0) {
		// read the incoming byte:
		incomingByte = Serial.read();
		switch(incomingByte)
		{
  			case 'S': if(lock){++side&=2; Serial.print("SIDE=");Serial.println(side);} break;
    			case 'O': if(lock){Serial.println("OPEN");} break;
			case '+': if(!lock){Serial.println(">"); disk = scanDirectory(disk + 1);}
                                  else {track++; Serial.print("TRACK=");Serial.println(track);} break;
			case '-': if(!lock){Serial.println("<"); disk = scanDirectory(disk - 1);} break;
			case '0': if(!lock){Serial.println("<<"); disk = scanDirectory(0);} break;
			case '.': if(!lock){Serial.println(">>"); disk = scanDirectory(999);} break;
			case ' ': lock=!lock; break;
		}
	}
}

// ----------------------------------------------------------------------------
//	Scan the SD card and open the volume
//	Set fdc.reg[STATUS] to FDC_ST_NOTREADY if no card present
// ----------------------------------------------------------------------------
void scanSD()
{
	if (!card.init(SPI_FULL_SPEED, SD_CHIP_SELECT_PIN))
	{
		Serial.print("Init failed, error:");
		Serial.println(card.errorCode());
		fdc.reg[STATUS] = FDC_ST_NOTREADY;
		return;
	}

#ifdef SD_DEBUG
	Serial.print("\nCard type: ");
	switch(card.type()) {
		case SD_CARD_TYPE_SD1: Serial.println("SD1"); break;
		case SD_CARD_TYPE_SD2: Serial.println("SD2"); break;
		case SD_CARD_TYPE_SDHC: Serial.println("SDHC"); break;
		default: Serial.println("Unknown");
	}
#endif

	if (!volume.init(card)) {
#ifdef SD_DEBUG
		Serial.println("Could not find FAT16/FAT32 partition.\nMake sure you've formatted the card");
#endif
		fdc.reg[STATUS] = FDC_ST_NOTREADY;
		return;
	}

#ifdef SD_DEBUG
	// ----- Print the type and size of the first FAT-type volume
	Serial.print("\nVolume type is FAT");
	Serial.println(volume.fatType(), DEC);
	Serial.println();

	long volumesize;
	volumesize = volume.blocksPerCluster();    // clusters are collections of blocks
	volumesize *= volume.clusterCount();       // we'll have a lot of clusters
	volumesize *= 512;                            // SD card blocks are always 512 bytes
	Serial.print("Volume size (bytes): ");
	Serial.println(volumesize);
#endif
	root.openRoot(volume);
	fdc.reg[STATUS] = 0x00;
}

// ----------------------------------------------------------------------------
//	Scan the directory
// ----------------------------------------------------------------------------
int scanDirectory(int wanted) {
	dir_t	entry;
	int	i=-1;

	if (fdc.reg[STATUS] & FDC_ST_NOTREADY) return;	// Exit if no card

	root.rewind();
	while(root.readDir(&entry)>0) {
//                Serial.print("DBG Read: ");
//              	Serial.println((char*)entry.name);

		i=-1;
		if (!DIR_IS_FILE(&entry)) continue;
		if (entry.name[0] == DIR_NAME_FREE) break;
		if (entry.name[0] != 'D') continue;
		if (entry.name[1] != 'I') continue;
		if (entry.name[2] != 'S') continue;
		if (entry.name[3] != 'K') continue;
		if (entry.name[4] != '_') continue;
		if ((entry.name[5] < '0')||(entry.name[5] > '9')) continue;
		if ((entry.name[6] < '0')||(entry.name[6] > '9')) continue;
		if ((entry.name[7] < '0')||(entry.name[7] > '9')) continue;
		if (entry.name[8] != 'Q') continue;
		if (entry.name[9] != 'X') continue;
		if (entry.name[10] != '1') continue;

		i=(int)entry.name[5]-48;
		i=i*10+(int)entry.name[6]-48;
		i=i*10+(int)entry.name[7]-48;

		if (i >= wanted) break;
	}
	Serial.println("Fichier: ");
	Serial.println((char*)entry.name);
	if (entry.fileSize != 1556480)
	{
		Serial.print(" bad size: ");
		Serial.print(entry.fileSize, DEC);
		Serial.println(" != 1556480");
	}
	return i;
}

// ----------------------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------------------
MB8877::MB8877()
{
#ifdef FDC_DEBUG
	Serial.println("FDC Constructor");
#endif
	fdc.vector = FDC_SEEK_FORWARD;
	fdc.reg[TRACK] = fdc.reg[STATUS] = registers[CMD] = registers[SECTOR] = registers[DATA] = 0;
	fdc.disk = -1;
	fdc.track = fdc.side = fdc.cmdtype = 0;
}

// ----------------------------------------------------------------------------
// Destructor
// ----------------------------------------------------------------------------
MB8877::~MB8877(){}

// ----------------------------------------------------------------------------
// Decode the received command
// ----------------------------------------------------------------------------
void	MB8877::decode_command()
{
#ifdef FDC_DEBUG
	Serial.println("FDC Decoder");
#endif
	fdc.reg[STATUS] = FDC_ST_BUSY;			// We are BUSY

	if (fdc.reg[STATUS] & FDC_ST_NOTREADY)		// Try again to open the directory
	{
		scanSD();
		scanDirectory(fdc.disk);
		if (fdc.reg[STATUS] & FDC_ST_NOTREADY) return;	// Still no SD
	}
	
	fdc.cmdtype = 0;	// Reset current command
	
	switch(fdc.reg[CMD] & 0xf0) {			// Decode which command to execute
	// type I
		case 0x00: cmd_restore(FDC_CMD_RESTORE); break;
		case 0x10: cmd_seek(FDC_CMD_SEEK); break;
		case 0x20: cmd_step(0); break;
		case 0x30: cmd_step(1); break;
		case 0x40: cmd_stepin(FDC_CMD_STEP_IN, 0); break;
		case 0x50: cmd_stepin(FDC_CMD_STEP_IN, 1); break;
		case 0x60: cmd_stepout(FDC_CMD_STEP_OUT, 0); break;
		case 0x70: cmd_stepout(FDC_CMD_STEP_OUT, 1); break;
	// type II
		case 0x80: cmd_readdata(FDC_CMD_RD_SEC); break;
		case 0x90: cmd_readdata(FDC_CMD_RD_MSEC); break;
		case 0xa0: cmd_writedata(FDC_CMD_WR_SEC); break;
		case 0xb0: cmd_writedata(FDC_CMD_WR_MSEC); break;
	// type III
		case 0xc0: cmd_readaddr(FDC_CMD_RD_ADDR); break;
		case 0xe0: cmd_readtrack(FDC_CMD_RD_TRK); break;
		case 0xf0: cmd_writetrack(FDC_CMD_WR_TRK); break;
	// type IV
		case 0xd0: cmd_forceint(FDC_CMD_TYPE1); break;
		default: break;
	}
	digitalWrite(FDC_IRQ, LOW);		// Generate interrupt, command completed
}

// ----------------------------------------------------------------------------
// Type I command: RESTORE
// ----------------------------------------------------------------------------
void MB8877::cmd_restore(int cmd)
{
#ifdef FDC_DEBUG
	display(" I  RESTORE");
#endif
	fdc.cmdtype = cmd;
	fdc.vector = FDC_SEEK_FORWARD;

	fdc.reg[STATUS] = 0x00;
	
// I3: issue an interrupt now. This bit can only be reset with another "Force interrupt" command.
// I2: issue an interrupt at the next index pulse.
// I1: issue an interrupt at the next ready to not-ready transition of the READY pin.
// I0: issue an interrupt at the next not-ready to ready transition of the READY pin.
// (If I0-I3 are 0: don't issue any interrupt, but still abort the current command).

	// force interrupt if bit0-bit3 is high
//	if(cmdreg & 0x0f) digitalWrite(FDC_IRQ, HIGH);

	sprintf(filename,"DISK_%03dQX1",fdc.disk);
	sdcard=sd.open(filename,2);

	// To simulate we've got the track number from the first sector encountered,
	// we compare the current track and the content of track register; if they
	// differ, we yield SEEKERR.

	if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[TRACK] != fdc.track))
	{
		fdc.reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		fdc.side = 0x00;
		fdc.reg[TRACK] = fdc.track = 0x00;
		fdc.reg[SECTOR] = 0x00;
		fdc.reg[STATUS] = FDC_ST_TRACK00|FDC_ST_INDEX|FDC_ST_HEADENG;
	}
}

// ----------------------------------------------------------------------------
// Type I command: SEEK
// ----------------------------------------------------------------------------
// fdc.reg[DATA] contains the track we want to reach
void MB8877::cmd_seek(int cmd)
{
#ifdef FDC_DEBUG
	display(" I  SEEK");
#endif
	fdc.cmdtype = cmd;			// Set command type
	fdc.vector = !(fdc.reg[DATA] > fdc.track);	// Determine seek vector

	fdc.reg[STATUS] = 0x00;

	if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[TRACK] != fdc.track))
	{
		fdc.reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		// Set track register
		fdc.reg[TRACK] = fdc.track = (fdc.reg[DATA]>FDC_TRACKS)?FDC_TRACKS:(fdc.reg[DATA] < 0)?0:fdc.reg[DATA];
		fdc.reg[STATUS] = (fdc.reg[TRACK] == 0) ? FDC_ST_HEADENG : FDC_ST_HEADENG|FDC_ST_TRACK00;
	}
}

// ----------------------------------------------------------------------------
// Type I command: STEP
// ----------------------------------------------------------------------------
void MB8877::cmd_step(byte track_update)
{
	byte	inc;
#ifdef FDC_DEBUG
	display(" I  STEP");
#endif
	if (fdc.vector)
	{
		inc=-1;
		cmd_stepout(FDC_CMD_STEP_OUT, track_update) else cmd_stepin(FDC_CMD_STEP_IN, track_update);
}

// ----------------------------------------------------------------------------
// Type I command: STEP-IN
// ----------------------------------------------------------------------------
void MB8877::cmd_stepin(int cmd, byte track_update)
{
#ifdef FDC_DEBUG
	display(" I  STEP_IN");
#endif
	fdc.cmdtype = cmd;		// Set command type

	fdc.vector = false;			// Reset seek vector
	fdc.reg[STATUS] = 0x00;

	if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[TRACK] != fdc.track))
	{
		fdc.reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		fdc.track++;				// Next track
		if(fdc.track>FDC_TRACKS) fdc.track=FDC_TRACKS;
		if(track_update) fdc.reg[TRACK] = fdc.track;
		fdc.reg[STATUS] |= (fdc.reg[CMD] & FDC_FLAG_HEADLOAD)?FDC_ST_HEADENG:0;
	}
}

// ----------------------------------------------------------------------------
// Type I command: STEP-OUT
// ----------------------------------------------------------------------------
void MB8877::cmd_stepout(int cmd, byte track_update)
{
#ifdef FDC_DEBUG
	display(" I  STEP_OUT");
#endif
	fdc.cmdtype = cmd;		// Set command type

	fdc.vector = true;			// Set seek vector
	fdc.reg[STATUS] = 0x00;

	if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[TRACK] != fdc.track))
	{
		fdc.reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		fdc.track--;			// Previous track
		if(fdc.track<0) { fdc.track=0; fdc.reg[STATUS] = FDC_ST_TRACK0; }
		if(track_update) fdc.reg[TRACK] = fdc.track;
		fdc.reg[STATUS] |= (fdc.reg[CMD] & FDC_FLAG_HEADLOAD)?FDC_ST_HEADENG:0;
	}
}

// ----------------------------------------------------------------------------
// Type II command: READ-DATA
// ----------------------------------------------------------------------------
void MB8877::cmd_readdata(int cmd)
{
	CRC crc = new CRC;
	int16_t	byte,		// the byte we'll read
		blocksize,	// # bytes to read / sector
		nsectors;	// # sectors to read
#ifdef FDC_DEBUG
	display(" II READ_DATA");
#endif

	fdc.reg[STATUS] = FDC_ST_BUSY|FDC_ST_RECNFND;		// Busy and no Record found yet

	// If CMDTYPE is not set, we must compare the side. If CMDTYPE is already set, the side comparison has already been done.
	if (fdc.cmdtype == 0)
	{
		if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[CMD] & 0x08) != fdc.side)) return;
	}
	else
		fdc.cmdtype = cmd;

	// Calculate the number of sectors we will have to read
	nsectors = 1;
	if (fdc.cmdtype != FDC_CMD_RD_SEC) 
		nsectors = (fdc.track<80 ? FDC_SECTOR_0 : FDC_SECTOR_1) - fdc.reg[SECTOR];
	blocksize = (fdc.track<80) ? FDC_SIZE_SECTOR_0 : FDC_SIZE_SECTOR_1;

	// Try to set file cursor at the desired position.
	if (! sd.seekSet(locate()) ) return;	// Exit with record not found status


PLUG HERE THE BEHAVIOR IF DATA ADDRESS MARK ON DISK (first byte) IS SET TO DELETE

	// Main loop: we'll read the sectors byte per byte,
	// transfer each byte to the Data register and generate a DRQ
	for(;fdc.reg[SECTOR] < fdc.reg[SECTOR]+nsectors; fdc.reg[SECTOR]++)
		for(fdc.position=0; byte=sd.read())!=-1 && fdc.position < blocksize; fdc.position++)
		{
			if ((fdc.position==0) && (byte==0xF8))		// Deleted block
				fdc.reg[STATUS] &= FDC_ST_DELETED;
			fdc.reg[STATUS] &= ~FDC_ST_RECNFND;		// Reset RECNFND
			if (fdc.cmdtype == FDC_CMD_RD_TRK) crc.compute(fdc.reg[DATA]);
			send_qx1(fdc.reg[DATA]);
		}
		if (byte==-1)						// End Of Data
			fdc.reg[STATUS] &= FDC_ST_RECNFND;		// Set RECNFND
		else
		{
			if (fdc.cmdtype == FDC_CMD_RD_TRK)		// We read to extra bytes (CRC)
			{
				if (! crc.check(sd.read(),1)) fdc.reg[STATUS] &= FDC_CRC_ERROR;	// MSB
				if (! crc.check(sd.read(),0)) fdc.reg[STATUS] &= FDC_CRC_ERROR;	// LSB
				send_qx1(crc.msb());
				send_qx1(crc.lsb());
			}
			crc.reset();
		}
}

// ----------------------------------------------------------------------------
// Type II command: WRITE-DATA
// ----------------------------------------------------------------------------
void MB8877::cmd_writedata(int cmd)
{
	int16_t	byte,		// the byte to serve
		blocksize,	// # bytes / sector
		nsectors;	// # sectors to write
#ifdef FDC_DEBUG
	display(" II WRITE_DATA");
#endif

	// Calculate the number of sectors we will have to write
	nsectors = 1;
	if (fdc.cmdtype == FDC_CMD_WR_MSEC)
		nsectors = (fdc.track<80 ? FDC_SECTOR_0 : FDC_SECTOR_1) - fdc.reg[SECTOR];
	
	blocksize = (fdc.track<80) ? FDC_SIZE_SECTOR_0 : FDC_SIZE_SECTOR_1;

	fdc.reg[STATUS] = FDC_ST_BUSY|FDC_ST_HEADENG;

	// Make some comparison: is it the desired side ? Is reg[SECTOR] Ok ?

	if (((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[CMD] & 0x08) != fdc.side)
		|| (fdc.reg[SECTOR] > nsectors) )
	{
		fdc.reg[STATUS] |= FDC_ST_RECNFND;
		return;			// Exit with record not found status
	}

	// Main loop: we'll service the sector byte per byte.
	// transfer each byte to the Data register and generate a DRQ
	for(;fdc.reg[SECTOR] < fdc.reg[SECTOR]+nsectors; fdc.reg[SECTOR]++)
	{
		// Write the Data Address Mark on the first byte of the current sector
		byte = (fdc.reg[CMD] & FDC_FLAG_DAM) ? 0x01 : 0x00;
		if (sd.write((const void*)&byte,1))!=-1)	// Write error
		{
			fdc.reg[STATUS] |= FDC_ST_WRITEFAULT;
			return;
		}
		fdc.position=0;
		do
		{
			qx1bus = 0xff;
			PORTC &= 0xf0;
			PORTC |= BUS_SELECT_DATA;			// Prepare bus to get data

			attachInterrupt(0, read_qx1, FALLING);	// Prepare interrupt
			digitalWrite(FDC_DRQ, LOW);			// Fire DRQ Interrupt
			__asm__("nop\n\t""nop\n\t");			// Waits ~ 125 nsec
			if(qx1bus == 0xff)
			{
				fdc.reg[STATUS] |= FDC_ST_LOSTDATA;	// QX1 did not load DATA in time; exits
				return;
			}
			
			if (sd.write((const void*)&byte,1))!=-1)	// Write error
			{
				fdc.reg[STATUS] |= FDC_ST_WRITEFAULT;
				return;
			}
		} until (fdc.position++ > blocksize);
	}

	nsectors = (fdc.track<80 ? FDC_SECTOR_0 : FDC_SECTOR_1);
	if(fdc.reg[SECTOR]>nsectors) fdc.reg[SECTOR]=nsectors;
}

// ----------------------------------------------------------------------------
// Type III command: READ-ADDRESS
// ----------------------------------------------------------------------------
void MB8877::cmd_readaddr()
{
	CRC crc = new CRC;
#ifdef FDC_DEBUG
	display("III READ_ADDR");
#endif

	fdc.cmdtype = FDC_CMD_RD_ADDR;

	fdc.reg[STATUS] |= FDC_ST_BUSY|FDC_ST_HEADENG;

	// Compute CRC
	crc.compute(fdc.reg[TRACK]);
	crc.compute(fdc.side);
	crc.compute(fdc.reg[SECTOR]);

	// 0x02=512 bytes/sector, 0x03=1024 bytes/sector
	crc.compute(fdc.reg[TRACK]<80 ? 0x03 : 0x02));

	// Send data :
	send_qx1(fdc.reg[TRACK]);			// 1- Track Address
	send_qx1(fdc.side);				// 2- Side number
	send_qx1(fdc.reg[SECTOR]);			// 3- Sector Address
	send_qx1((fdc.reg[TRACK]<80 ? 0x03 : 0x02));	// 4- Sector length
	send_qx1(crc.msb());				// 5- CRC1
	send_qx1(crc.lsb());				// 6- CRC2
}

// ----------------------------------------------------------------------------
// Type III command: READ-TRACK
// ----------------------------------------------------------------------------
//	The QX1 expects to read the track byte per byte, including gaps, address
//	marks and CRC. Because we only store data on the SD card, we have to
//	generate some expected bytes.
//	(G) represents generated bytes
//	(R) represents register bytes
//	(D) represents actual data bytes.

void MB8877::cmd_readtrack()
{
	uint	i;

#ifdef FDC_DEBUG
	display("III READ_TRACK");
#endif

	// type-3 read track
	fdc.cmdtype = FDC_CMD_RD_TRK;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	status = FDC_ST_BUSY | FDC_ST_RECNFND;

	// If side Compare flag is set, compare the current and desired sides
	// and exits if they differ.
	if ((fdc.reg[CMD] & FDC_FLAG_VERIFICATION) && (fdc.reg[CMD] & 0x08) != fdc.side)) return;

	// Try to set file cursor at the desired position.
	if (! sd.seekSet(locate()) ) return;	// Exit with record not found status

	// Send ID RECORD
	for(i=0; i<80; i++) send_qx1(0x4e);	// 000-079: (G) GAP 0
	for(i=0; i<12; i++) send_qx1(0x00);	// 080-091: (G) SYNC
	for(i=0; i<3; i++) send_qx1(0xc2);	// 092-(G) Index address mark
	send_qx1(0xfc);				// (G) Index address mark
	for(i=0; i<50; i++) send_qx1(0x4e);	// (G) GAP 1
	for(i=0; i<12; i++) send_qx1(0x00);	// (G) SYNC
	// Send DATA
	if(fdc.reg[TRACK]<80)
	{
		fdc.reg[SECTOR]=0+fdc.side*5; cmd_readsector(FDC_CMD_RD_TRK);
		fdc.reg[SECTOR]=3+fdc.side*5; cmd_readsector(FDC_CMD_RD_TRK);
		fdc.reg[SECTOR]=1+fdc.side*5; cmd_readsector(FDC_CMD_RD_TRK);
		fdc.reg[SECTOR]=4+fdc.side*5; cmd_readsector(FDC_CMD_RD_TRK);
		fdc.reg[SECTOR]=2+fdc.side*5; cmd_readsector(FDC_CMD_RD_TRK);
	}
	else
		for(i=0; i<9; i++)
			fdc.reg[SECTOR]=0+fdc.side*10; cmd_readsector(FDC_CMD_RD_TRK);
	for(i=0; i<22; i++) send_qx1(0x4e);	// (G) GAP 2
}

void MB8877::cmd_writetrack()
{
#ifdef FDC_DEBUG
	display("III WRITE_TRACK");
#endif
	// type-3 write track
	cmdtype = FDC_CMD_WR_TRK;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	status = FDC_ST_BUSY;
	
	disk[drvreg]->track_size = 0x1800;
	fdc[drvreg].index = 0;
	
	int time = GET_SEARCH_TIME;
}

// ----------------------------------------------------------------------------
// Type IV command: FORCE-INTERRUPT
// ----------------------------------------------------------------------------

void MB8877::cmd_forceint()
{
#ifdef FDC_DEBUG
	display(" IV FORCE_INT");
#endif
	if(fdc.cmdtype == 0 || fdc.cmdtype == 4) {
		status = 0;
		cmdtype = FDC_CMD_TYPE1;
		fdc.control ^= (cmdreg & 0x0f);
	}
	fdc.reg[STATUS] &= ~FDC_ST_BUSY;
	sd.close(sdcard);
	
	if(fdc.control & FDC_INT_NOW) digitalWrite(FDC_IRQ, HIGH);
}

// ----------------------------------------------------------------------------
// media handler
// ----------------------------------------------------------------------------
//	Locate calculate the offset to reach the right sector given the side,
//	track and sector number.
//	On the QX1, the density differs if the track is below or above track 80.
// 	Moreover, for track below 80, sectors are interlaced and appears in
//	the following order after index hole:
//
//	Track 00-79
//	Side 0: [ 0 | 3 | 1 | 4 | 2 ]
//	Side 1: [ 5 | 8 | 6 | 9 | 7 ]
//	
//	Track 80-159
//	Side 0: [ 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 ]
//	Side 1: [ 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 ]

long	MB8877::locate()
{
	long	offset=fdc.side * FDC_SIDE;	// Side offset

	if(fdc.track<80)
	{
		offset += (fdc.track-1) * FDC_SIZE_TRACK_0;	// # tracks below 80
		switch(fdc.reg[SECTOR])
		{
			case 0:	
			case 5: break;
			case 3:	
			case 8: offset+=FDC_SIZE_SECTOR_0;	break;
			case 1:	
			case 6: offset+=2*FDC_SIZE_SECTOR_0;	break;
			case 4:	
			case 9: offset+=3*FDC_SIZE_SECTOR_0;	break;
			case 2:	
			case 7: offset+=4*FDC_SIZE_SECTOR_0;	break;
		}
	}
	else
	{
		// Number of tracks in each zone, minus the current track
		offset += 80 * FDC_SIZE_TRACK_0;		// 80 tracks of FDC_TRACK_SIZE_0
		offset += (fdc.track-81) * FDC_SIZE_TRACK_1;	// tracks above 80
		offset += fdc.reg[SECTOR] * FDC_SIZE_SECTOR_1;
	}
	return offset;
}

uchar mb8877_search_track()
{
	int trk = fdc[drvreg].track;
	
	if(!disk[drvreg]->get_track(trk, sidereg)) {
		return FDC_ST_SEEKERR;
	}
	
	// verify track number
	if(!(cmdreg & 4)) {
		return 0;
	}
	for(int i = 0; i < disk[drvreg]->sector_num; i++) {
		if(disk[drvreg]->verify[i] == trkreg) {
			return 0;
		}
	}
	return FDC_ST_SEEKERR;
}

uchar mb8877_search_sector(int trk, int side, int sct, bool compare)
{
	// get track
	if(!disk[drvreg]->get_track(trk, side)) {
		digitalWrite(FDC_IRQ, HIGH);
		return FDC_ST_RECNFND;
	}
	
	// first scanned sector
	int sector_num = disk[drvreg]->sector_num;
	if(sectorcnt >= sector_num) {
		sectorcnt = 0;
	}
	
	// scan sectors
	for(int i = 0; i < sector_num; i++) {
		// get sector
		int index = sectorcnt + i;
		if(index >= sector_num) {
			index -= sector_num;
		}
		disk[drvreg]->get_sector(trk, side, index);
		
		// check id
		if(disk[drvreg]->id[2] != sct) {
			continue;
		}
		// check density
		if(disk[drvreg]->density) {
			continue;
		}
		
		// sector found
		sectorcnt = index + 1;
		if(sectorcnt >= sector_num) {
			sectorcnt -= sector_num;
		}
		fdc[drvreg].index = 0;
		return (disk[drvreg]->deleted ? FDC_ST_RECTYPE : 0) | ((disk[drvreg]->status && !ignore_crc) ? FDC_ST_CRCERR : 0);
	}

	// sector not found
	disk[drvreg]->sector_size = 0;
	digitalWrite(FDC_IRQ, HIGH);
	return FDC_ST_RECNFND;
}

uchar mb8877_search_addr()
{
	int trk = fdc[drvreg].track;
	
	// get track
	if(!disk[drvreg]->get_track(trk, sidereg)) {
		digitalWrite(FDC_IRQ, HIGH);
		return FDC_ST_RECNFND;
	}
	
	// get sector
	if(sectorcnt >= disk[drvreg]->sector_num) {
		sectorcnt = 0;
	}
	if(disk[drvreg]->get_sector(trk, sidereg, sectorcnt)) {
		sectorcnt++;
		
		fdc[drvreg].index = 0;
		secreg = disk[drvreg]->id[0];
		return (disk[drvreg]->status && !ignore_crc) ? FDC_ST_CRCERR : 0;
	}
	
	// sector not found
	disk[drvreg]->sector_size = 0;
	digitalWrite(FDC_IRQ, HIGH);
	return FDC_ST_RECNFND;
}

bool mb8877_make_track()
{
	int trk = fdc[drvreg].track;
	
	return disk[drvreg]->make_track(trk, sidereg);
}

// ----------------------------------------------------------------------------
// Send a byte to the QX1 via DAL
// ----------------------------------------------------------------------------

void send_qx1(uchar byte)
{
	DDRD = 0xff;					// Set PORT D as output

	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_DATA;			// Select the bus

	PORTD = byte;					// Write the byte

	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_ADDRESS;			// Latch the bus

	DDRD = 0x00;					// Set PORT D as input

	digitalWrite(INT0, HIGH); 			// Activate internal pullup resistor
	attachInterrupt(0, read_qx1, FALLING);		// Declare interrupt routine
	digitalWrite(FDC_DRQ, LOW);			// Fire DRQ Interrupt
	__asm__("nop\n\t""nop\n\t");			// Waits ~ 125 nsec
	if(qx1bus == 0xff)
		fdc.reg[STATUS] |= FDC_ST_LOSTDATA;	// QX1 did not load DATA in time
	else
		fdc.reg[STATUS] &= ~FDC_ST_LOSTDATA;	// QX1 got DATA in time
}

// ----------------------------------------------------------------------------
// We've got an interrupt. We scan /RD, /WR, A0 and A1 to determine what is
// requested.
//
//    X   X   X   X  A1  A0 /WR /RD         MPU wants to ...
//  ---+---+---+---+---+---+---+---+------+---------------------------
//   -   -   -   -   0   0   0   1 | 0x01 | write to fdc.reg[CMD]
//   -   -   -   -   0   0   1   0 | 0x02 | read fdc.reg[STATUS]
//   -   -   -   -   0   1   0   1 | 0x05 | write to fdc.reg[TRACK]
//   -   -   -   -   0   1   1   0 | 0x06 | read fdc.reg[TRACK]
//   -   -   -   -   1   0   0   1 | 0x09 | write to fdc.reg[SECTOR]
//   -   -   -   -   1   0   1   0 | 0x0a | read fdc.reg[SECTOR]
//   -   -   -   -   1   1   0   1 | 0x0d | write to fdc.reg[DATA]
//   -   -   -   -   1   1   1   0 | 0x0e | read fdc.reg[DATA]
//
// All other values are impossible to occur.
// ----------------------------------------------------------------------------

void read_qx1() {
	qx1bus=(PORTD & 0x0f); 		// Get value
	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_DATA;	// Prepare bus to get data
}

// ----------------------------------------------------------------------------
// Main loop
// ----------------------------------------------------------------------------

void loop()
{
#ifdef DEBUG
	int incomingByte;
#endif
	digitalWrite(FDC_DRQ, HIGH);	// Reset DRQ interrupt
	digitalWrite(FDC_IRQ, HIGH);

	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_ADDRESS;

// - Prepare interrupts; on falling edge, we'll call getNewCommand
	attachInterrupt(0, getNewCommand, FALLING);

	// Global Enable INT0 interrupt
	GICR |= ( 1 < < INT0);
	// Signal change triggers interrupt
	MCUCR |= ( 1 << ISC00);
	MCUCR |= ( 0 << ISC01);

#ifdef DEBUG
	if (Serial.available() > 0) {
		// read the incoming byte:
		incomingByte = Serial.read();
		switch(incomingByte)
		{
  			case 'r': 
  			case 'R': if(lock){++side&=2; Serial.print("SIDE=");Serial.println(side);} break;
    			case 'O': if(lock){Serial.println("OPEN");} break;
			case '+': if(!lock){Serial.println(">"); disk = scanDirectory(disk + 1);}
                                  else {track++; Serial.print("TRACK=");Serial.println(track);} break;
			case '-': if(!lock){Serial.println("<"); disk = scanDirectory(disk - 1);} break;
			case '0': if(!lock){Serial.println("<<"); disk = scanDirectory(0);} break;
			case '.': if(!lock){Serial.println(">>"); disk = scanDirectory(999);} break;
			case ' ': lock=!lock; break;
		}
	}
#endif

// We now wait for a command
	switch(qx1bus)
	{
		// QX1 MPU wants to write in a register; we get the value from PORTD
		case 0x01: fdc.reg[CMD] = PORTD; decode_command(); break;
		case 0x05: fdc.reg[TRACK] = PORTD; break;
		case 0x09: fdc.reg[SECTOR] = PORTD; break;
		case 0x0d: fdc.reg[DATA] = PORTD; break;

		// QX1 MPU wants to read from a register; we serve the value on PORTD
		case 0x02: PORTD = fdc.reg[STATUS]; break;
		case 0x06: PORTD = fdc.reg[TRACK]; break;
		case 0x0a: PORTD = fdc.reg[SECTOR]; break;
		case 0x0e: PORTD = fdc.reg[DATA]; break;
	}
} 

