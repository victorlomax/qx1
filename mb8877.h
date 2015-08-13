/*
  Yamaha QX1 Floppy Emulator
*/

#ifndef _H_MB8877
#define _H_MB8877

#define FDC_DEBUG_LOG 1
#define drvreg 1

#ifndef uchar
#define unsigned char uchar
#endif

#ifndef uint
#define unsigned int uint
#endif

// MB8877 variables
#define FDC_ST_BUSY		0x01	// busy
#define FDC_ST_INDEX		0x02	// index hole
#define FDC_ST_DRQ		0x02	// data request
#define FDC_ST_TRACK00		0x04	// track0
#define FDC_ST_LOSTDATA		0x04	// data lost
#define FDC_ST_CRCERR		0x08	// crc error
#define FDC_ST_SEEKERR		0x10	// seek error
#define FDC_ST_RECNFND		0x10	// sector not found
#define FDC_ST_HEADENG		0x20	// head engage
#define FDC_ST_RECTYPE		0x20	// record type
#define FDC_ST_WRITEFAULT	0x20	// write fault
#define FDC_ST_WRITEP		0x40	// write protectdc
#define FDC_ST_NOTREADY		0x80	// media not inserted

// Command types
#define FDC_CMD_RESTORE		1
#define FDC_CMD_SEEK		1
#define FDC_CMD_STEP_IN		1
#define FDC_CMD_STEP_OUT	1
#define FDC_CMD_RD_SEC		2
#define FDC_CMD_RD_MSEC		3
#define FDC_CMD_WR_SEC		4
#define FDC_CMD_WR_MSEC		5
#define FDC_CMD_RD_ADDR		6
#define FDC_CMD_RD_TRK		7
#define FDC_CMD_WR_TRK		8
#define FDC_CMD_TYPE4		0x80

// Id of registers
#define STATUS		0
#define TRACK		1
#define SECTOR		2
#define DATA		3
#define CMD		4
#define SIDE		5
#define DISK		6

// Flags
#define FDC_FLAG_VERIFICATION	0x02
#define FDC_FLAG_HEADLOAD	0x04
#define FDC_FLAG_TRACKUPDATE	0x08
#define FDC_FLAG_MULTIRECORD	0x08

// Other
#define FDC_EXTRA_DELAY		15000
#define FDC_SEEK_FORWARD	true
#define	FDC_SEEK_BACKWARD	!FDC_SEEK_FORWARD

// These parameters are specific to the Yamaha QX1
#define FDC_DISKS		99	// Virtual disks per SD card
#define FDC_TRACKS		160	// Tracks per disk (from Canon MF-221 datasheet)
#define FDC_SECTORS		14	// Sectors per track
#define FDC_BLOCK_SIZE		256	// Size of a unit data block in double density
#define FDC_SECTOR_SIZE		258	// SECTOR_SIZE = BLOCK_SIZE + 2 bytes for CRC
#define FDC_TRACK_SIZE		6708	// TRACK_SIZE = SECTOR_SIZE * MAX_SECTOR

uchar format_tracks, format_sectors;	// These are values provided by MPU

typedef struct {
	uchar cmdtype,	// Command type
	seektrk;	// Current track
	bool	vector,		// Previous step direction
		seek;		// Seek selected
} fdc;

uchar registers[6],	// Registers
uint dbaddr;		// Data block address

/*
typedef struct {
	char	address_mark,
		track,
		side,
		sector,
		sector_len,
		crc[2];
} IBM3740_ID_RECORD;

typedef struct {
	char	address_mark,
		data[256],
		crc[2];
} IBM3740_DATA_RECORD;

typedef struct {
	char	gap0[95],
		index_mark,
		gap1[65];
	struct {
		IBM3740_ID_RECORD id_record;
		char	gap2[37];
		IBM3740_DATA_RECORD data_record;
		char	gap3[36];
	} data[FDC_SECTORS];
	char	gap4[598];
} IBM3740;

IBM3740	floppydisk[FDC_TRACKS];
*/

// ------------------------------------------------
// FDC section
// ------------------------------------------------



// Go one step ahead or forward
void fdc_cmd_step(int _step)
{
  fdc.cmdtype = FDC_CMD_TYPE1;
  fdc.statreg = FDC_ST_HEADENG | FDC_ST_BUSY;
  fdc.vector = _step;
  fdc.trkreg = (fdc.trkreg > FDC_TRACKS) ? FDC_TRACKS : (fdc.trkreg < 0) ? 0 : fdc.trkreg+fdc.vector;
  dbaddr=fdc.trkreg*FDC_TRACK_SIZE;
  fdc_raiseinterrupt();
}

// Read data
void fdc_cmd_readdata()
{
  fdc.cmdtype = (fdc.cmdreg & FDC_FLAG_MULTIRECORD) ? FDC_CMD_RD_MSEC : FDC_CMD_RD_SEC;
  if(fdc.cmdreg & FDC_FLAG_VERIFICATION) {
    fdc.statreg = search_sector(fdc[fdc.drvreg].track, ((fdc.cmdreg & 8) ? 1 : 0), fdc.secreg, true);
  } else {
    fdc.statreg = search_sector(fdc[fdc.drvreg].track, fdc.sidereg, fdc.secreg, false);
  }
  if(!(fdc_statreg & FDC_ST_RECNFND)) fdc_statreg |= FDC_ST_BUSY;
}

void fdc_cmd_writedata()
{
  fdc_cmdtype = (fdc_cmdreg & 0x10) ? FDC_CMD_WR_MSEC : FDC_CMD_WR_SEC;
  if(fdc_cmdreg & 2) {
    fdc_statreg = search_sector(fdc[fdc_drvreg].track, ((fdc_cmdreg & 8) ? 1 : 0), fdc_secreg, true);
  } else {
    fdc_statreg = search_sector(fdc[fdc_drvreg].track, fdc_sidereg, fdc_secreg, false);
  }
  fdc_statreg &= ~FDC_ST_RECTYPE;
  if(!(fdc_statreg & FDC_ST_RECNFND)) fdc_statreg |= FDC_ST_BUSY;
}
void fdc_cmd_readaddr() {; }
void fdc_cmd_readtrack() {
	
}
void fdc_cmd_writetrack() {
	
}
void fdc_cmd_forceint() {; }

void fdc_cmd() {
#ifdef FDC_DEBUG_LOG
  const char *cmdstr[0x10] = {
    "Rest", "Seek", "Step", "Step",
    "StpI", "StpI", "StpO", "StpO",
    "RDat", "RDat", "RDat", "WDat",
    "RAdd", "Int ", "RTrk", "WTrk"};
  lcd.setCursor(19, 0);
  lcd.print(cmdstr[fdc.cmdreg >> 4]);
  // emu->out_debug(_T("FDC\tCMD=%2xh (%s) DATA=%2xh DRV=%d TRK=%3d SIDE=%d SEC=%2d\n"), cmdreg, cmdstr[cmdreg >> 4], datareg, drvreg, trkreg, sidereg, secreg);
#endif
  switch(fdc.cmdreg & 0xf0) {
	// type-1
	case 0x00: fdc_cmd_restore(); break;
	case 0x10: fdc_cmd_seek(); break;
	case 0x20:
	case 0x30: fdc_cmd_step(fdc_vector); break;
	case 0x40:
	case 0x50: fdc_cmd_step(-1); break;
	case 0x60:
	case 0x70: fdc_cmd_step(1); break;
	// type-2
	case 0x80:
	case 0x90: fdc_cmd_readdata(); break;
	case 0xa0:
	case 0xb0: fdc_cmd_writedata(); break;
	// type-3
	case 0xc0: fdc_cmd_readaddr(); break;
	case 0xe0: fdc_cmd_readtrack(); break;
	case 0xf0: fdc_cmd_writetrack(); break;
	// type-4
	case 0xd0: fdc_cmd_forceint(); break;
	default: break;
  }
}
#endif
