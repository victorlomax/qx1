/*
	Yamaha QX1 floppy drive emulator

	Francois Basquin, 2014 mar 20

	Origin : mb8877a from RetroPC ver 2006.12.06 by Takeda.Toshiya
*/

#include "mb8877.h"
#include "../config.h"
// SD Card management
#include "SD.h"

#define FDC_FRM_MAXTRACK	160
#define FDC_FRM_MAXSECTOR	9
#define FDC_FRM_BLOCKSIZE	512

// Events
#define EVENT_SEEK		0
#define EVENT_SEEKEND		1
#define EVENT_SEARCH		2
#define EVENT_TYPE4		3
#define EVENT_MULTI1		4
#define EVENT_MULTI2		5
#define EVENT_LOST		6


// Delay table: 6msec, 12msec, 20msec, 30msec
static const int delays[4] = {6000, 12000, 20000, 30000};

#define CANCEL_EVENT(event) { \
	if(register_id[event] != -1) { \
		cancel_event(register_id[event]); \
		register_id[event] = -1; \
	} \
	if(event == EVENT_SEEK) { \
		now_seek = false; \
	} \
	if(event == EVENT_SEARCH) { \
		now_search = false; \
	} \
}
#define REGISTER_EVENT(event, wait) { \
	if(register_id[event] != -1) { \
		cancel_event(register_id[event]); \
		register_id[event] = -1; \
	} \
	register_event(this, (event << 8) | cmdtype, wait, false, &register_id[event]); \
	if(event == EVENT_SEEK) { \
		now_seek = after_seek = true; \
	} \
	if(event == EVENT_SEARCH) { \
		now_search = true; \
	} \
}

int	fd;		// SD card file descriptor
char	filename[8];	// SD card filename

void fdc_initialize()
{
  // ignore_crc = config.ignore_crc;
  dbaddr=0;
  seektrk = 0;
  fdc.vector = FDC_SEEK_FORWARD;
  indexcnt = sectorcnt = 0;
  registers[TRACK] = registers[STATUS] = registers[CMD] = registers[SECTOR] = registers[DATA] = registers[SIDE] = 0
  fdc.cmdtype = 0;
}

void mb8877_reset()
{
	fdc.track = 0;
	fdc.index = 0;
	fdc.access = false;

	for(int i = 0; i < 7; i++) {
		register_id[i] = -1;
	}
	now_search = now_seek = after_seek = false;
}

void MB8877::update_config()
{
	ignore_crc = config.ignore_crc;
}

// ----------------------------------------------------------------------------
// Write data to a given register
// parameters: addr=register, data=data to write
// returns: nothing
// ----------------------------------------------------------------------------
void MB8877::write_register(uchar addr, char data)
{
	switch(addr & 3) {
	case FDC_REG_CMD:			// command reg
		cmdreg = data;
		process_cmd();
		break;
	case FDC_REG_TRACK:			// track reg
		trkreg = data;
		if((status & FDC_ST_BUSY) && (fdc[drvreg].index == 0)) {
			// track reg is written after command starts
			if(cmdtype == FDC_CMD_RD_SEC || cmdtype == FDC_CMD_RD_MSEC || cmdtype == FDC_CMD_WR_SEC || cmdtype == FDC_CMD_WR_MSEC) {
				process_cmd();
			}
		}
		break;
	case FDC_REG_SECTOR:			// sector reg
		secreg = data;
		if((status & FDC_ST_BUSY) && (fdc[drvreg].index == 0)) {
			// sector reg is written after command starts
			if(cmdtype == FDC_CMD_RD_SEC || cmdtype == FDC_CMD_RD_MSEC || cmdtype == FDC_CMD_WR_SEC || cmdtype == FDC_CMD_WR_MSEC) {
				process_cmd();
			}
		}
		break;
	case FDC_REG_DATA:			// data reg
		datareg = data;
		if(motor && (status & FDC_ST_DRQ) && !now_search) {
			if(cmdtype == FDC_CMD_WR_SEC || cmdtype == FDC_CMD_WR_MSEC) {
				// write or multisector write
				if(fdc[drvreg].index < disk[drvreg]->sector_size) {
					if(!disk[drvreg]->write_protected) {
						disk[drvreg]->sector[fdc[drvreg].index] = datareg;
						// dm, ddm
						disk[drvreg]->deleted = (cmdreg & 1) ? 0x10 : 0;
					}
					else {
						status |= FDC_ST_WRITEFAULT;
						status &= ~FDC_ST_BUSY;
						status &= ~FDC_ST_DRQ;
						cmdtype = 0;
						set_irq(true);
					}
					fdc[drvreg].index++;
				}
				if(fdc[drvreg].index >= disk[drvreg]->sector_size) {
					if(cmdtype == FDC_CMD_WR_SEC) {
						// single sector
						status &= ~FDC_ST_BUSY;
						cmdtype = 0;
						set_irq(true);
					}
					else {
						// multisector
						REGISTER_EVENT(EVENT_MULTI1, 30);
						REGISTER_EVENT(EVENT_MULTI2, 60);
					}
					status &= ~FDC_ST_DRQ;
				}
				fdc[drvreg].access = true;
			}
			else if(cmdtype == FDC_CMD_WR_TRK) {
				// read track
				if(fdc[drvreg].index < disk[drvreg]->track_size) {
					if(!disk[drvreg]->write_protected) {
						disk[drvreg]->track[fdc[drvreg].index] = datareg;
					}
					else {
						status |= FDC_ST_WRITEFAULT;
						status &= ~FDC_ST_BUSY;
						status &= ~FDC_ST_DRQ;
						cmdtype = 0;
						set_irq(true);
					}
					fdc[drvreg].index++;
				}
				if(fdc[drvreg].index >= disk[drvreg]->track_size) {
					status &= ~FDC_ST_BUSY;
					status &= ~FDC_ST_DRQ;
					cmdtype = 0;
					set_irq(true);
				}
				fdc[drvreg].access = true;
			}
			if(!(status & FDC_ST_DRQ)) {
				set_drq(false);
			}
		}
		break;
	}
}

// ----------------------------------------------------------------------------
// Read data from a given register
// parameters: addr=register
// returns: data read
// ----------------------------------------------------------------------------

uchar MB8877::read_register(uchar addr)
{
	uchar data;
	
	switch(addr & 3) {
	case FDC_REG_STATUS:
		// status reg
		if(cmdtype == FDC_CMD_TYPE4) {
			// now force interrupt
			if(!disk[drvreg]->inserted || !motor) {
				status = FDC_ST_NOTREADY;
			}
			else {
				// MZ-2500 RELICS invites STATUS = 0
				status = 0;
			}
			data = status;
		}
		else if(now_search) {
			// now sector search
			data = FDC_ST_BUSY;
		}
		else {
			// disk not inserted, motor stop
			if(!disk[drvreg]->inserted || !motor) {
				status |= FDC_ST_NOTREADY;
			}
			else {
				status &= ~FDC_ST_NOTREADY;
			}
			// write protected
			if(cmdtype == FDC_CMD_TYPE1 || cmdtype == FDC_CMD_WR_SEC || cmdtype == FDC_CMD_WR_MSEC || cmdtype == FDC_CMD_WR_TRK) {
				if(disk[drvreg]->inserted && disk[drvreg]->write_protected) {
					status |= FDC_ST_WRITEP;
				}
				else {
					status &= ~FDC_ST_WRITEP;
				}
			}
			else {
				status &= ~FDC_ST_WRITEP;
			}
			// track0, index hole
			if(cmdtype == FDC_CMD_TYPE1) {
				if(fdc[drvreg].track == 0) {
					status |= FDC_ST_TRACK00;
				}
				else {
					status &= ~FDC_ST_TRACK00;
				}
				if(!(status & FDC_ST_NOTREADY)) {
					if(indexcnt == 0) {
						status |= FDC_ST_INDEX;
					}
					else {
						status &= ~FDC_ST_INDEX;
					}
					if(++indexcnt >= ((disk[drvreg]->sector_num == 0) ? 16 : disk[drvreg]->sector_num)) {
						indexcnt = 0;
					}
				}
			}
			// show busy a moment
			data = status;
			if(cmdtype == FDC_CMD_TYPE1 && !now_seek) {
				status &= ~FDC_ST_BUSY;
			}
		}
		// reset irq/drq
		if(!(status & FDC_ST_DRQ)) {
			set_drq(false);
		}
		set_irq(false);
#ifdef _FDC_DEBUG_LOG
		// request cpu to output debug log
		if(d_cpu) {
			d_cpu->write_signal(SIG_CPU_DEBUG, 1, 1);
		}
		emu->out_debug(_T("FDC\tSTATUS=%2x\n"), val);
#endif
		return data;
	case FDC_REG_TRACK:
		// track reg
		return trkreg;
	case FDC_REG_SECTOR:
		// sector reg
#ifdef HAS_MB8876
		return (~secreg) & 0xff;
#else
		return secreg;
#endif
	case FDC_REG_DATA:
		// data reg
		if(motor && (status & FDC_ST_DRQ) && !now_search) {
			if(cmdtype == FDC_CMD_RD_SEC || cmdtype == FDC_CMD_RD_MSEC) {
				// read or multisector read
				if(fdc[drvreg].index < disk[drvreg]->sector_size) {
					datareg = disk[drvreg]->sector[fdc[drvreg].index];
					fdc[drvreg].index++;
				}
				if(fdc[drvreg].index >= disk[drvreg]->sector_size) {
					if(cmdtype == FDC_CMD_RD_SEC) {
						// single sector
#ifdef _FDC_DEBUG_LOG
						emu->out_debug(_T("FDC\tEND OF SECTOR\n"));
#endif
						status &= ~FDC_ST_BUSY;
						cmdtype = 0;
						set_irq(true);
					}
					else {
						// multisector
#ifdef _FDC_DEBUG_LOG
						emu->out_debug(_T("FDC\tEND OF SECTOR (SEARCH NEXT)\n"));
#endif
						REGISTER_EVENT(EVENT_MULTI1, 30);
						REGISTER_EVENT(EVENT_MULTI2, 60);
					}
					status &= ~FDC_ST_DRQ;
				}
				fdc[drvreg].access = true;
			}
			else if(cmdtype == FDC_CMD_RD_ADDR) {
				// read address
				if(fdc[drvreg].index < 6) {
					datareg = disk[drvreg]->id[fdc[drvreg].index];
					fdc[drvreg].index++;
				}
				if(fdc[drvreg].index >= 6) {
					status &= ~FDC_ST_BUSY;
					status &= ~FDC_ST_DRQ;
					cmdtype = 0;
					set_irq(true);
				}
				fdc[drvreg].access = true;
			}
			else if(cmdtype == FDC_CMD_RD_TRK) {
				// read track
				if(fdc[drvreg].index < disk[drvreg]->track_size) {
					datareg = disk[drvreg]->track[fdc[drvreg].index];
					fdc[drvreg].index++;
				}
				if(fdc[drvreg].index >= disk[drvreg]->track_size) {
#ifdef _FDC_DEBUG_LOG
					emu->out_debug(_T("FDC\tEND OF TRACK\n"));
#endif
					status &= ~FDC_ST_BUSY;
					status &= ~FDC_ST_DRQ;
					status |= FDC_ST_LOSTDATA;
					cmdtype = 0;
					set_irq(true);
				}
				fdc[drvreg].access = true;
			}
			if(!(status & FDC_ST_DRQ)) {
				set_drq(false);
			}
		}
#ifdef _FDC_DEBUG_LOG
		emu->out_debug(_T("FDC\tDATA=%2x\n"), datareg);
#endif
#ifdef HAS_MB8876
		return (~datareg) & 0xff;
#else
		return datareg;
#endif
	}
	return 0xff;
}

void MB8877::write_dma_io8(u32 addr, u32 data)
{
	write_register(FDC_REG_DATA, data);
}

u32 MB8877::read_dma_io8(u32 addr)
{
	return read_register(FDC_REG_DATA);
}

void MB8877::write_signal(int id, u32 data, u32 mask)
{
	if(id == SIG_MB8877_DRIVEREG) {
		drvreg = data & DRIVE_MASK;
	}
	else if(id == SIG_MB8877_SIDEREG) {
		sidereg = (data & mask) ? 1 : 0;
	}
	else if(id == SIG_MB8877_MOTOR) {
		motor = ((data & mask) != 0);
	}
}

u32 MB8877::read_signal(int ch)
{
	// get access status
	u32 stat = 0;
	for(int i = 0; i < MAX_DRIVE; i++) {
		if(fdc[i].access) {
			stat |= 1 << i;
		}
		fdc[i].access = false;
	}
	return stat;
}

void mb8877_event_callback(int event_id, int err)
{
	int event = event_id >> 8;
	int cmd = event_id & 0xff;
	register_id[event] = -1;
	
	// cancel event if the command is finished or other command is executed
	if(cmd != cmdtype) {
		if(event == EVENT_SEEK) {
			now_seek = false;
		}
		else if(event == EVENT_SEARCH) {
			now_search = false;
		}
		return;
	}
	
	switch(event) {
	case EVENT_SEEK:
		if(seektrk > fdc[drvreg].track) {
			fdc[drvreg].track++;
		}
		else if(seektrk < fdc[drvreg].track) {
			fdc[drvreg].track--;
		}
		if(cmdreg & 0x10) {
			trkreg = fdc[drvreg].track;
		}
		else if((cmdreg & 0xf0) == 0) {
			trkreg--;
		}
		if(seektrk == fdc[drvreg].track) {
			// auto update
			if((cmdreg & 0x10) || ((cmdreg & 0xf0) == 0)) {
				trkreg = fdc[drvreg].track;
			}
			if((cmdreg & 0xf0) == 0) {
				datareg = 0;
			}
			status |= search_track();
			now_seek = false;
			set_irq(true);
		}
		else {
			REGISTER_EVENT(EVENT_SEEK, delay[cmdreg & 3] + err);
		}
		break;
	case EVENT_SEEKEND:
		if(seektrk == fdc[drvreg].track) {
			// auto update
			if((cmdreg & 0x10) || ((cmdreg & 0xf0) == 0)) {
				trkreg = fdc[drvreg].track;
			}
			if((cmdreg & 0xf0) == 0) {
				datareg = 0;
			}
			status |= search_track();
			now_seek = false;
			CANCEL_EVENT(EVENT_SEEK);
			set_irq(true);
		}
		break;
	case EVENT_SEARCH:
		now_search = false;
		// start dma
		if(!(status & FDC_ST_RECNFND)) {
			status |= FDC_ST_DRQ;
			set_drq(true);
		}
		break;
	case EVENT_TYPE4:
		cmdtype = FDC_CMD_TYPE4;
		break;
	case EVENT_MULTI1:
		secreg++;
		break;
	case EVENT_MULTI2:
		if(cmdtype == FDC_CMD_RD_MSEC) {
			cmd_readdata();
		}
		else if(cmdtype == FDC_CMD_WR_MSEC) {
			cmd_writedata();
		}
		break;
	case EVENT_LOST:
		if(status & FDC_ST_BUSY) {
			status |= FDC_ST_LOSTDATA;
			status &= ~FDC_ST_BUSY;
			//status &= ~FDC_ST_DRQ;
			set_irq(true);
			//set_drq(false);
		}
		break;
	}
}

// ----------------------------------------------------------------------------
// command
// ----------------------------------------------------------------------------

void mb8877_process_cmd(int delay)
{
	registers[STATUS] = FDC_ST_HEADENG | FDC_ST_BUSY;	// Initial status	
	wait(delays[delay]);			// Wait to simulate execution
	registers[STATUS] = FDC_ST_HEADENG;			// Set result status

}

void fdc_process_cmd()
{
#ifdef _FDC_DEBUG_LOG
	static const _TCHAR *cmdstr[0x10] = {
		_T("RESTORE "),	_T("SEEK    "),	_T("STEP    "),	_T("STEP    "),
		_T("STEP IN "),	_T("STEP IN "),	_T("STEP OUT"),	_T("STEP OUT"),
		_T("RD DATA "),	_T("RD DATA "),	_T("RD DATA "),	_T("WR DATA "),
		_T("RD ADDR "),	_T("FORCEINT"),	_T("RD TRACK"),	_T("WR TRACK")
	};
	emu->out_debug(_T("FDC\tCMD=%2xh (%s) DATA=%2xh DRV=%d TRK=%3d SIDE=%d SEC=%2d\n"), cmdreg, cmdstr[cmdreg >> 4], datareg, drvreg, trkreg, sidereg, secreg);
#endif
	while true
	{
		CANCEL_EVENT(EVENT_TYPE4);
		set_irq(false);
	
		status = FDC_ST_HEADENG|FDC_ST_BUSY;	// We are BUSY
		wait(delay[(registers[CMD] & 0x03)]);		// Wait to simulate execution

		switch(registers[CMD] & 0xf0) {			// Decode which command to execute
		// type I
			case 0x00: cmd_restore(); break;
			case 0x10: cmd_seek(); break;
			case 0x20:
			case 0x30: cmd_step(); break;
			case 0x40:
			case 0x50: cmd_stepin(); break;
			case 0x60:
			case 0x70: cmd_stepout(); break;
		// type II
			case 0x80:
			case 0x90: cmd_readdata(); break;
			case 0xa0:
			case 0xb0: cmd_writedata(); break;
		// type III
			case 0xc0: cmd_readaddr(); break;
			case 0xe0: cmd_readtrack(); break;
			case 0xf0: cmd_writetrack(); break;
		// type IV
			case 0xd0: cmd_forceint(); break;
			default: break;
		}
	}
}

void fdc_raiseinterrupt()
{
}

// ----------------------------------------------------------------------------
// Type I command: RESTORE
// ----------------------------------------------------------------------------
void cmd_restore()
{
	fdc.cmdtype = FDC_CMD_TYPE1;
	fdc.vector = FDC_SEEK_FORWARD;

  	registers[TRACK] = 0x00;
  	registers[SIDE] = 0x00;
	registers[STATUS] = FDC_ST_HEADENG | FDC_ST_BUSY;
}

// ----------------------------------------------------------------------------
// Type I command: SEEK
// ----------------------------------------------------------------------------
// fdc_datareg contains the track we want to reach
void cmd_seek()
{
	fdc.cmdtype = FDC_CMD_SEEK;			// Set command type
	fdc.vector = !(registers[DATA] > registers[TRACK]);		// Determine seek vector

	registers[TRACK] = (registers[DATA]>FDC_FRM_MAXTRACK)?FDC_FRM_MAXTRACK:(registers[DATA] < 0)?0:registers[DATA];	// Set track register

	dbaddr = registers[TRACK]*FDC_TRACK_SIZE;
	fdc_raiseinterrupt();
	registers[STATUS] = (registers[TRACK] == 0) ? FDC_ST_HEADENG : FDC_ST_HEADENG|FDC_ST_TRACK00;
}

// ----------------------------------------------------------------------------
// Type I command: STEP
// ----------------------------------------------------------------------------
void cmd_step()
{
	if (fdc.vector) cmd_stepout() else cmd_stepin();
}

// ----------------------------------------------------------------------------
// Type I command: STEP-IN
// ----------------------------------------------------------------------------
void cmd_stepin()
{
	fdc.cmdtype = FDC_CMD_STEP_IN;		// Set command type

	fdc.vector = false;			// Reset seek vector
	registers[TRACK] = (registers[TRACK]<FDC_FRM_MAXTRACK) ? registers[TRACK]+1 : FDC_FRM_MAXTRACK;
	dbaddr = registers[TRACK]*FDC_TRACK_SIZE;
	fdc_raiseinterrupt();
	registers[STATUS] = FDC_ST_HEADENG;
}

// ----------------------------------------------------------------------------
// Type I command: STEP-OUT
// ----------------------------------------------------------------------------
void cmd_stepout()
{
	fdc.cmdtype = FDC_CMD_STEP_OUT;		// Set command type

	fdc.vector = true;			// Set seek vector
	registers[TRACK] = (registers[TRACK]>0) ? registers[TRACK]-1 : 0;
	dbaddr = registers[TRACK]*FDC_TRACK_SIZE;
	fdc_raiseinterrupt();
	registers[STATUS] = (registers[TRACK] == 0) ? FDC_ST_HEADENG : FDC_ST_HEADENG|FDC_ST_TRACK00;
}

// wait 70msec to read/write data just after seek command is done
#define GET_SEARCH_TIME (after_seek ? (after_seek = false, 70000) : 200)

// ----------------------------------------------------------------------------
// Type II command: READ-DATA
// ----------------------------------------------------------------------------
void cmd_readdata()
{
	char	filename[8];

	fdc.cmdtype = (registers[CMD] & 0x10) ? FDC_CMD_RD_MSEC : FDC_CMD_RD_SEC;

	registers[STATUS] |= FDC_ST_BUSY;		// Set Busy flag
	wait(delay[(cmdreg & 3)] + ((registers[CMD] & 0x04) ? FDC_EXTRA_DELAY : 0));		// Wait to simulate execution
	registers[STATUS] &= ~FDC_ST_BUSY;		// Reset Busy flag

	if(registers[CMD] & 2) {	// Side Compare flag is set
		if ((registers[CMD] & 0x08) != registers[SIDE]) registers[STATUS] |= FDC_ST_RECNFND;
	}
	else {
		registers[SIDE] = (registers[CMD] & 0x08) ? 1 : 0;
		sdcard=sd.open(filename,2);
		sd.seek(sdcard,locate());
// ------/
   
		if (!SD.begin(4)) {
			Serial.println("initialization failed!");
			return;
		}
		Serial.println("initialization done.");
  
		fd = SD.open("00.DSK", FILE_WRITE);
  
  // if the file opened okay, write to it:
  if (myFile) {
    Serial.print("Writing to test.txt...");
    fd.println("testing 1, 2, 3.");
    // close the file:
    fd.close();
    Serial.println("done.");
  } else {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }
  
  // re-open the file for reading:
  myFile = SD.open("test.txt");
  if (myFile) {
    Serial.println("test.txt:");
    
    // read from the file until there's nothing else in it:
    while (myFile.available()) {
        Serial.write(myFile.read());
    }
    // close the file:
    myFile.close();
  } else {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }

// ******-
		while(sd.read(
		block_addr = trkreg * (FDC_FRM_BLOCKSIZE * secreg) + sidereg * FDC_FRM_MAXTRACK * FDC_FRM_MAXSECTOR * FDC_FRM_BLOCKSIZE;

		status = search_sector(fdc[drvreg].track, sidereg, secreg, false);
	}
	if(!(status & FDC_ST_RECNFND)) {
//		status |= FDC_ST_DRQ | FDC_ST_BUSY;
		registers[STATUS] |= FDC_ST_BUSY;
	}


	
	int time = GET_SEARCH_TIME;
	REGISTER_EVENT(EVENT_SEARCH, time);
	CANCEL_EVENT(EVENT_LOST);
	if(!(status & FDC_ST_RECNFND)) {
		REGISTER_EVENT(EVENT_LOST, time + 30000);
	}
}

// ----------------------------------------------------------------------------
// Type II command: WRITE-DATA
// ----------------------------------------------------------------------------
void MB8877::cmd_writedata()
{
	cmdtype = (cmdreg & 0x10) ? FDC_CMD_WR_MSEC : FDC_CMD_WR_SEC;
	if(cmdreg & 2) {
		status = search_sector(fdc[drvreg].track, ((cmdreg & 8) ? 1 : 0), secreg, true);
	}
	else {
		status = search_sector(fdc[drvreg].track, sidereg, secreg, false);
	}
	status &= ~FDC_ST_RECTYPE;
	if(!(status & FDC_ST_RECNFND)) {
//		status |= FDC_ST_DRQ | FDC_ST_BUSY;
		status |= FDC_ST_BUSY;
	}
	
	int time = GET_SEARCH_TIME;
	REGISTER_EVENT(EVENT_SEARCH, time);
	CANCEL_EVENT(EVENT_LOST);
	if(!(status & FDC_ST_RECNFND)) {
		REGISTER_EVENT(EVENT_LOST, time + 30000);
	}
}

void MB8877::cmd_readaddr()
{
	// type-3 read address
	cmdtype = FDC_CMD_RD_ADDR;
	status = search_addr();
	if(!(status & FDC_ST_RECNFND)) {
//		status |= FDC_ST_DRQ | FDC_ST_BUSY;
		status |= FDC_ST_BUSY;
	}
	
	int time = GET_SEARCH_TIME;
	REGISTER_EVENT(EVENT_SEARCH, time);
	CANCEL_EVENT(EVENT_LOST);
	if(!(status & FDC_ST_RECNFND)) {
		REGISTER_EVENT(EVENT_LOST, time + 10000);
	}
}

void MB8877::cmd_readtrack()
{
	// type-3 read track
	cmdtype = FDC_CMD_RD_TRK;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	status = FDC_ST_BUSY;
	
	if(!make_track()) {
		// create dummy track
		for(int i = 0; i < 0x1800; i++) {
			disk[drvreg]->track[i] = rand();
		}
		disk[drvreg]->track_size = 0x1800;
	}
	fdc[drvreg].index = 0;
	
	int time = GET_SEARCH_TIME;
	REGISTER_EVENT(EVENT_SEARCH, time);
	REGISTER_EVENT(EVENT_LOST, time + 150000);
}

void MB8877::cmd_writetrack()
{
	// type-3 write track
	cmdtype = FDC_CMD_WR_TRK;
//	status = FDC_ST_DRQ | FDC_ST_BUSY;
	status = FDC_ST_BUSY;
	
	disk[drvreg]->track_size = 0x1800;
	fdc[drvreg].index = 0;
	
	int time = GET_SEARCH_TIME;
	REGISTER_EVENT(EVENT_SEARCH, time);
	REGISTER_EVENT(EVENT_LOST, time + 150000);
}

// ----------------------------------------------------------------------------
// Type IV command: FORCE-INTERRUPT
// ----------------------------------------------------------------------------

void cmd_forceint()
{
	if(cmdtype == 0 || cmdtype == 4) {
		status = 0;
		cmdtype = FDC_CMD_TYPE1;
	}
	registers[STATUS] &= ~FDC_ST_BUSY;
#endif
	
	// force interrupt if bit0-bit3 is high
	if(cmdreg & 0x0f) {
		set_irq(true);
	}
	CANCEL_EVENT(EVENT_SEEK);
	CANCEL_EVENT(EVENT_SEEKEND);
	CANCEL_EVENT(EVENT_SEARCH);
	CANCEL_EVENT(EVENT_TYPE4);
	CANCEL_EVENT(EVENT_MULTI1);
	CANCEL_EVENT(EVENT_MULTI2);
	CANCEL_EVENT(EVENT_LOST);
	REGISTER_EVENT(EVENT_TYPE4, 100);
}

// ----------------------------------------------------------------------------
// media handler
// ----------------------------------------------------------------------------

long	locate()
{
	return (long)(registers[TRACK] * (FDC_FRM_BLOCKSIZE * registers[SECTOR]) + registers[SIDE] * FDC_FRM_MAXTRACK * FDC_FRM_MAXSECTOR * FDC_FRM_BLOCKSIZE);
}

u8 MB8877::search_track()
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

u8 MB8877::search_sector(int trk, int side, int sct, bool compare)
{
	// get track
	if(!disk[drvreg]->get_track(trk, side)) {
		set_irq(true);
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
	set_irq(true);
	return FDC_ST_RECNFND;
}

u8 MB8877::search_addr()
{
	int trk = fdc[drvreg].track;
	
	// get track
	if(!disk[drvreg]->get_track(trk, sidereg)) {
		set_irq(true);
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
	set_irq(true);
	return FDC_ST_RECNFND;
}

bool MB8877::make_track()
{
	int trk = fdc[drvreg].track;
	
	return disk[drvreg]->make_track(trk, sidereg);
}

// ----------------------------------------------------------------------------
// irq / drq
// ----------------------------------------------------------------------------

void MB8877::set_irq(bool val)
{
	write_signals(&outputs_irq, val ? 0xffffffff : 0);
}

void MB8877::set_drq(bool val)
{
	write_signals(&outputs_drq, val ? 0xffffffff : 0);
}

// ----------------------------------------------------------------------------
// user interface
// ----------------------------------------------------------------------------

void MB8877::open_disk(int drv, _TCHAR path[], int offset)
{
	if(drv < MAX_DRIVE) {
		disk[drv]->open(path, offset);
	}
}

void MB8877::close_disk(int drv)
{
	if(drv < MAX_DRIVE) {
		disk[drv]->close();
		cmdtype = 0;
	}
}

bool MB8877::disk_inserted(int drv)
{
	if(drv < MAX_DRIVE) {
		return disk[drv]->inserted;
	}
	return false;
}

void MB8877::set_drive_type(int drv, u8 type)
{
	if(drv < MAX_DRIVE) {
		disk[drv]->drive_type = type;
	}
}

u8 MB8877::get_drive_type(int drv)
{
	if(drv < MAX_DRIVE) {
		return disk[drv]->drive_type;
	}
	return DRIVE_TYPE_UNK;
}

u8 MB8877::fdc_status()
{
	// for each virtual machines
#if defined(_FMR50) || defined(_FMR60)
	return disk[drvreg]->inserted ? 2 : 0;
#else
	return 0;
#endif
}
