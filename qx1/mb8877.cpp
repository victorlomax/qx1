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

#define TRUE 1
#define FALSE !TRUE

#include "mb8877.h"
#include "qx1.h"
#include "crc.h"
#include <SD.h>

#ifndef _H_SDCARD
#include "sdcard.h"
#endif

// ----- Definition of interrupt names

#include <avr/io.h>
// ----- ISR interrupt service routine
#include <avr/interrupt.h>

typedef unsigned int uint;
// ----------------------------------------------------------------------------
// Vars and consts
// ----------------------------------------------------------------------------

// ----- Delay table: 6msec, 12msec, 20msec, 30msec
static const int delays[4] = {600, 1200, 2000, 3000};



int incomingByte = 0;   // for incoming serial data
int   ndisk=0;            // Number of virtual-disks on SD
int   lock=false;
int   side=0,
      track=0,
      sector=0;

char	filename[13];	// SD card filename

extern volatile char qx1bus;
extern class MB8877 mb8877;
extern Sd2Card   card;
extern SdVolume  volume;
extern SdFile    root;
extern File      droot;    // Directory root
extern File      disk;    // Current virtual disk
extern dir_t     direntry;


// ----------------------------------------------------------------------------
// DECLARE FUNCTION
// ----------------------------------------------------------------------------
void readsector();	
void send_qx1(unsigned char);
void read_qx1();

// ----------------------------------------------------------------------------
// Interrupt Management
// ----------------------------------------------------------------------------
volatile int Interrupt = false;	// When QX1 pin /RD goes low, Interrupt goes true

ISR(INT0_vect) {
	// check the value again - since it takes some time to
	// activate the interrupt routine, we get a clear signal.
	qx1bus = digitalRead(INT0);
	Interrupt=true;
}



// ----------------------------------------------------------------------------
//	Scan the SD card and open the volume
//	Set reg[STATUS] to FDC_ST_NOTREADY if no card present
// ----------------------------------------------------------------------------
void scanSD()
{
	if (!card.init(SPI_FULL_SPEED, SD_CHIP_SELECT_PIN))
	{
		Serial.print("Init failed, error:");
		Serial.println(card.errorCode());
		mb8877.reg[STATUS] = FDC_ST_NOTREADY;
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
		mb8877.reg[STATUS] = FDC_ST_NOTREADY;
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
	mb8877.reg[STATUS]=0x00;
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
	reg[TRACK] = reg[STATUS] = reg[CMD] = reg[SECTOR] = reg[DATA] = 0;
	fdc.disk = -1;
	fdc.track = fdc.side = fdc.cmdtype = 0;
}

// ----------------------------------------------------------------------------
// Destructor
// ----------------------------------------------------------------------------
MB8877::~MB8877(){}

// ----------------------------------------------------------------------------
// VDISK: build virtual disk name
// ----------------------------------------------------------------------------

void MB8877::vdisk()
{
#ifdef FDC_DEBUG
  Serial.println("vdisk");
#endif
  sprintf(filename,"DISK_%03d.QX1",fdc.disk);
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

	reg[STATUS] = 0x00;
	
// I3: issue an interrupt now. This bit can only be reset with another "Force interrupt" command.
// I2: issue an interrupt at the next index pulse.
// I1: issue an interrupt at the next ready to not-ready transition of the READY pin.
// I0: issue an interrupt at the next not-ready to ready transition of the READY pin.
// (If I0-I3 are 0: don't issue any interrupt, but still abort the current command).

// force interrupt if bit0-bit3 is high
//	if(cmdreg & 0x0f) digitalWrite(FDC_IRQ, HIGH);

	mb8877.vdisk();
	disk=SD.open(filename,2);

	// To simulate we've got the track number from the first sector encountered,
	// we compare the current track and the content of track register; if they
	// differ, we yield SEEKERR.

	if ((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[TRACK] != fdc.track))
	{
		reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		fdc.side = 0x00;
		reg[TRACK] = fdc.track = 0x00;
		reg[SECTOR] = 0x00;
		reg[STATUS] = FDC_ST_TRACK00|FDC_ST_INDEX|FDC_ST_HEADENG;
	}
}

// ----------------------------------------------------------------------------
// Type I command: SEEK
// ----------------------------------------------------------------------------
// reg[DATA] contains the track we want to reach
void MB8877::cmd_seek(char cmd)
{
#ifdef FDC_DEBUG
	display(" I  SEEK");
#endif
	fdc.cmdtype = cmd;			// Set command type
	fdc.vector = !(reg[DATA] > fdc.track);	// Determine seek vector

	reg[STATUS] = 0x00;

	if ((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[TRACK] != fdc.track))
	{
		reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
	}
	else
	{
		// Set track register
		reg[TRACK] = fdc.track = (reg[DATA]>FDC_TRACKS)?FDC_TRACKS:(reg[DATA] < 0)?0:reg[DATA];
		reg[STATUS] = (reg[TRACK] == 0) ? FDC_ST_HEADENG : FDC_ST_HEADENG|FDC_ST_TRACK00;
	}
}

// ----------------------------------------------------------------------------
// Type I command: STEP
// ----------------------------------------------------------------------------
void MB8877::cmd_step(bool track_update)
{
#ifdef FDC_DEBUG
	display(" I  STEP");
#endif
	if (fdc.vector)
		cmd_step(FDC_CMD_STEP_OUT, track_update);
	else 
	  cmd_step(FDC_CMD_STEP_IN, track_update);
}

// ----------------------------------------------------------------------------
// Type I command: STEP-IN/OUT
// ----------------------------------------------------------------------------
void MB8877::cmd_step(char cmd, bool track_update)
{
  fdc.cmdtype = cmd;    // Set command type
  reg[STATUS] = 0x00;

	if ((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[TRACK] != fdc.track))
	{
		reg[STATUS] = FDC_ST_HEADENG|FDC_ST_SEEKERR;
    return;
	}
 
	if(cmd==FDC_CMD_STEP_IN)
  {
#ifdef FDC_DEBUG
    display(" I  STEP_IN");
#endif
    fdc.vector = false;      // Reset seek vector
    fdc.track++;				// Next track
		if(fdc.track>FDC_TRACKS) fdc.track=FDC_TRACKS;
		if(track_update) reg[TRACK] = fdc.track;
		reg[STATUS] |= (reg[CMD] & FDC_FLAG_HEADLOAD)?FDC_ST_HEADENG:0;
 }
 else
 {
#ifdef FDC_DEBUG
    display(" I  STEP_OUT");
#endif
    fdc.vector = false;      // Reset seek vector
    fdc.track--;            // Previous track
    if(fdc.track<0) { fdc.track=0; reg[STATUS] = FDC_ST_TRACK00; }
    if(track_update) reg[TRACK] = fdc.track;
    reg[STATUS] |= (reg[CMD] & FDC_FLAG_HEADLOAD)?FDC_ST_HEADENG:0;
 }
}

// ----------------------------------------------------------------------------
// Type II command: READ-DATA
// ----------------------------------------------------------------------------
void MB8877::cmd_readdata(char cmd)
{
	CRC *crc = new CRC;
	int16_t	byte,		// the byte we'll read
		blocksize,	// # bytes to read / sector
		nsectors;	// # sectors to read
#ifdef FDC_DEBUG
	display(" II READ_DATA");
#endif

	reg[STATUS] = FDC_ST_BUSY|FDC_ST_RECNFND;		// Busy and no Record found yet

	// If CMDTYPE is not set, we must compare the side. If CMDTYPE is already set, the side comparison has already been done.
	if (fdc.cmdtype == 0)
	{
		if ((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[CMD] & 0x08) != fdc.side) return;
	}
	else
		fdc.cmdtype = cmd;

	// Calculate the number of sectors we will have to read
	nsectors = 1;
	if (fdc.cmdtype != FDC_CMD_RD_SEC) 
		nsectors = (fdc.track<80 ? FDC_SECTORS_0 : FDC_SECTORS_1) - reg[SECTOR];
	blocksize = (fdc.track<80) ? FDC_SIZE_SECTOR_0 : FDC_SIZE_SECTOR_1;

	// Try to set file cursor at the desired position.
	if (! disk.seek(locate()))  return;	// Exit with record not found status


/*PLUG HERE THE BEHAVIOR IF DATA ADDRESS MARK ON DISK (first byte) IS SET TO DELETE*/

	// Main loop: we'll read the sectors byte per byte,
	// transfer each byte to the Data register and generate a DRQ
	for(;reg[SECTOR] < reg[SECTOR]+nsectors; reg[SECTOR]++)
  {
		for(fdc.position=0; byte=disk.read()!=-1 && fdc.position < blocksize; fdc.position++)
		{
/*			if ((fdc.position==0) && (byte==0xF8))		// Deleted block
				reg[STATUS] &= FDC_ST_DELETED;*/
			reg[STATUS] &= ~FDC_ST_RECNFND;		// Reset RECNFND
      reg[DATA]=byte;
			if (fdc.cmdtype == FDC_CMD_RD_TRK) crc->compute(reg[DATA]);
			send_qx1(reg[DATA]);
		}
		if (byte==-1)						// End Of Data
			reg[STATUS] &= FDC_ST_RECNFND;		// Set RECNFND
		else
		{
			if (fdc.cmdtype == FDC_CMD_RD_TRK)		// We read to extra bytes (CRC)
			{
				if (! crc->check(disk.read(),1)) reg[STATUS] &= FDC_ST_CRCERR;	// MSB
				if (! crc->check(disk.read(),0)) reg[STATUS] &= FDC_ST_CRCERR;	// LSB
				send_qx1(crc->msb());
				send_qx1(crc->lsb());
			}
			crc->reset();
		}
  }
}

// ----------------------------------------------------------------------------
// Type II command: WRITE-DATA
// ----------------------------------------------------------------------------
void MB8877::cmd_writedata(char cmd)
{
  unsigned char BYTE,		// the byte to serve
		blocksize,	// # bytes / sector
		nsectors;	// # sectors to write
#ifdef FDC_DEBUG
	display(" II WRITE_DATA");
#endif

	// Calculate the number of sectors we will have to write
	nsectors = 1;
	if (fdc.cmdtype == FDC_CMD_WR_MSEC)
		nsectors = (fdc.track<80 ? FDC_SECTORS_0 : FDC_SECTORS_1) - reg[SECTOR];
	
	blocksize = (fdc.track<80) ? FDC_SIZE_SECTOR_0 : FDC_SIZE_SECTOR_1;

	reg[STATUS] = FDC_ST_BUSY|FDC_ST_HEADENG;

	// Make some comparison: is it the desired side ? Is reg[SECTOR] Ok ?

	if (((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[CMD] & 0x08) != fdc.side)
		|| (reg[SECTOR] > nsectors) )
	{
		reg[STATUS] |= FDC_ST_RECNFND;
		return;			// Exit with record not found status
	}

	// Main loop: we'll service the sector byte per byte.
	// transfer each byte to the Data register and generate a DRQ
	for(;reg[SECTOR] < reg[SECTOR]+nsectors; reg[SECTOR]++)
	{
		// Write the Data Address Mark on the first byte of the current sector
		BYTE = (reg[CMD] & FDC_FLAG_DAM) ? 0x01 : 0x00;
		if (disk.write(&BYTE,1)!=-1)	// Write error
		{
			reg[STATUS] |= FDC_ST_WRITEFAULT;
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
				reg[STATUS] |= FDC_ST_LOSTDATA;	// QX1 did not load DATA in time; exits
				return;
			}
			
			if (disk.write(&BYTE,1)!=-1)	// Write error
			{
				reg[STATUS] |= FDC_ST_WRITEFAULT;
				return;
			}
		} while (fdc.position++ < blocksize);
	}

	nsectors = (fdc.track<80 ? FDC_SECTORS_0 : FDC_SECTORS_1);
	if(reg[SECTOR]>nsectors) reg[SECTOR]=nsectors;
}

// ----------------------------------------------------------------------------
// Type III command: READ-ADDRESS
// ----------------------------------------------------------------------------
void MB8877::cmd_readaddr(char cmd)
{
	CRC *crc = new CRC;
#ifdef FDC_DEBUG
	display("III READ_ADDR");
#endif
  fdc.cmdtype = cmd;

	reg[STATUS] |= FDC_ST_BUSY|FDC_ST_HEADENG;

	// Compute CRC
	crc->compute(reg[TRACK]);
	crc->compute(fdc.side);
	crc->compute(reg[SECTOR]);

	// 0x02=512 bytes/sector, 0x03=1024 bytes/sector
	crc->compute(reg[TRACK]<80 ? 0x03 : 0x02);

	// Send data :
	send_qx1(reg[TRACK]);			// 1- Track Address
	send_qx1(fdc.side);				// 2- Side number
	send_qx1(reg[SECTOR]);			// 3- Sector Address
	send_qx1((reg[TRACK]<80 ? 0x03 : 0x02));	// 4- Sector length
	send_qx1(crc->msb());				// 5- CRC1
	send_qx1(crc->lsb());				// 6- CRC2
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

void MB8877::cmd_readtrack(char cmd)
{
	uint	i;

#ifdef FDC_DEBUG
	display("III READ_TRACK");
#endif

	// type-3 read track
	 fdc.cmdtype = cmd;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	reg[STATUS] = FDC_ST_BUSY | FDC_ST_RECNFND;

	// If side Compare flag is set, compare the current and desired sides
	// and exits if they differ.
	if ((reg[CMD] & FDC_FLAG_VERIFICATION) && (reg[CMD] & 0x08) != fdc.side) return;

	// Try to set file cursor at the desired position.
	if (! disk.seek(locate()) ) return;	// Exit with record not found status

	// Send ID RECORD
	for(i=0; i<80; i++) send_qx1(0x4e);	// 000-079: (G) GAP 0
	for(i=0; i<12; i++) send_qx1(0x00);	// 080-091: (G) SYNC
	for(i=0; i<3; i++) send_qx1(0xc2);	// 092-(G) Index address mark
	send_qx1(0xfc);				// (G) Index address mark
	for(i=0; i<50; i++) send_qx1(0x4e);	// (G) GAP 1
	for(i=0; i<12; i++) send_qx1(0x00);	// (G) SYNC
	// Send DATA
	if(reg[TRACK]<80)
	{
		reg[SECTOR]=0+fdc.side*5; cmd_readdata(FDC_CMD_RD_TRK);
		reg[SECTOR]=3+fdc.side*5; cmd_readdata(FDC_CMD_RD_TRK);
		reg[SECTOR]=1+fdc.side*5; cmd_readdata(FDC_CMD_RD_TRK);
		reg[SECTOR]=4+fdc.side*5; cmd_readdata(FDC_CMD_RD_TRK);
		reg[SECTOR]=2+fdc.side*5; cmd_readdata(FDC_CMD_RD_TRK);
	}
	else
		for(i=0; i<9; i++)
			reg[SECTOR]=0+fdc.side*10; cmd_readdata(FDC_CMD_RD_TRK);
	for(i=0; i<22; i++) send_qx1(0x4e);	// (G) GAP 2
}

void MB8877::cmd_writetrack(char cmd)
{
#ifdef FDC_DEBUG
	display("III WRITE_TRACK");
#endif
	// type-3 write track
	fdc.cmdtype = cmd;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	reg[STATUS] = FDC_ST_BUSY;
}

// ----------------------------------------------------------------------------
// Type IV command: FORCE-INTERRUPT
// ----------------------------------------------------------------------------

void MB8877::cmd_forceint(char cmd)
{
#ifdef FDC_DEBUG
	display(" IV FORCE_INT");
#endif
	if(fdc.cmdtype == 0 || fdc.cmdtype == 4) {
		fdc.cmdtype = cmd;
		fdc.control ^= (reg[CMD] & 0x0f);
	}
	reg[STATUS] &= ~FDC_ST_BUSY;
	disk.close();
	
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
		offset += (fdc.track-1) * (unsigned int)FDC_SIZE_TRACK_0;	// # tracks below 80
		switch(reg[SECTOR])
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
		offset += 80 * (unsigned int)FDC_SIZE_TRACK_0;		// 80 tracks of FDC_TRACK_SIZE_0
		offset += (fdc.track-81) * (unsigned int)FDC_SIZE_TRACK_1;	// tracks above 80
		offset += reg[SECTOR] * FDC_SIZE_SECTOR_1;
	}
	return offset;
}

// ----------------------------------------------------------------------------
// Decode the received command
// ----------------------------------------------------------------------------
void  MB8877::decode_command()
{
#ifdef FDC_DEBUG
  Serial.println("FDC Decoder");
#endif
  reg[STATUS] = FDC_ST_BUSY;      // We are BUSY

  if (reg[STATUS] & FDC_ST_NOTREADY)    // Try again to open the directory
  {
    scanSD();
    scanDirectory(fdc.disk);
    if (reg[STATUS] & FDC_ST_NOTREADY) return;  // Still no SD
  }
  
  fdc.cmdtype = 0;  // Reset current command
  
  switch(reg[CMD] & 0xf0) {     // Decode which command to execute
  // type I
    case 0x00: cmd_restore(FDC_CMD_RESTORE); break;
    case 0x10: cmd_seek(FDC_CMD_SEEK); break;
    case 0x20: cmd_step(0); break;
    case 0x30: cmd_step(1); break;
    case 0x40: cmd_step(FDC_CMD_STEP_IN, 0); break;
    case 0x50: cmd_step(FDC_CMD_STEP_IN, 1); break;
    case 0x60: cmd_step(FDC_CMD_STEP_OUT, 0); break;
    case 0x70: cmd_step(FDC_CMD_STEP_OUT, 1); break;
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
    case 0xd0: cmd_forceint(FDC_CMD_TYPE4); break;
    default: break;
  }
  digitalWrite(FDC_IRQ, LOW);   // Generate interrupt, command completed
}

// ----------------------------------------------------------------------------
// Send a byte to the QX1 via DAL
// ----------------------------------------------------------------------------

void send_qx1(unsigned char byte)
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
		mb8877.reg[STATUS] |= FDC_ST_LOSTDATA;	// QX1 did not load DATA in time
	else
		mb8877.reg[STATUS] &= ~FDC_ST_LOSTDATA;	// QX1 got DATA in time
}

// ----------------------------------------------------------------------------
// We've got an interrupt. We scan /RD, /WR, A0 and A1 to determine what is
// requested.
//
//    X   X   X   X  A1  A0 /WR /RD         MPU wants to ...
//  ---+---+---+---+---+---+---+---+------+---------------------------
//   -   -   -   -   0   0   0   1 | 0x01 | write to reg[CMD]
//   -   -   -   -   0   0   1   0 | 0x02 | read reg[STATUS]
//   -   -   -   -   0   1   0   1 | 0x05 | write to reg[TRACK]
//   -   -   -   -   0   1   1   0 | 0x06 | read reg[TRACK]
//   -   -   -   -   1   0   0   1 | 0x09 | write to reg[SECTOR]
//   -   -   -   -   1   0   1   0 | 0x0a | read reg[SECTOR]
//   -   -   -   -   1   1   0   1 | 0x0d | write to reg[DATA]
//   -   -   -   -   1   1   1   0 | 0x0e | read reg[DATA]
//
// All other values are impossible to occur.
// ----------------------------------------------------------------------------

void read_qx1() {
	qx1bus=(PORTD & 0x0f); 		// Get value
	PORTC &= 0xf0;
	PORTC |= BUS_SELECT_DATA;	// Prepare bus to get data

  switch(qx1bus)
  {
    // QX1 MPU wants to write to a register; we get the value from PORTD
    case 0x01: mb8877.reg[CMD] = PORTD; mb8877.decode_command(); break;
    case 0x05: mb8877.reg[TRACK] = PORTD; break;
    case 0x09: mb8877.reg[SECTOR] = PORTD; break;
    case 0x0d: mb8877.reg[DATA] = PORTD; break;

    // QX1 MPU wants to read from a register; we serve the value on PORTD
    case 0x02: PORTD = mb8877.reg[STATUS]; break;
    case 0x06: PORTD = mb8877.reg[TRACK]; break;
    case 0x0a: PORTD = mb8877.reg[SECTOR]; break;
    case 0x0e: PORTD = mb8877.reg[DATA]; break;
  }
}
