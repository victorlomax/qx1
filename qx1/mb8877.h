/* ==========================================
  Yamaha QX1 Floppy Emulator with Arduino Mini

	Francois Basquin

-- History
2015 Jan 28: v2.0	Restart working after crash
2015 Nov 18: v2.1	Split this file into several

-- References
* Arduino Mini: http://arduino.cc/en/Main/ArduinoBoardMini
* SD library: http://www.roland-riegel.de/sd-reader/index.html
* MicroFAT: http://arduinonut.blogspot.ca/2008/04/ufat.html
* mb8877a: from RetroPC ver 2006.12.06 by Takeda.Toshiya, http://homepage3.nifty.com/takeda-toshiya/?
========================================== */

#ifndef _H_MB8877
#define _H_MB8877

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

// Interrupt masks
#define FDC_INT_NR2R		0x01	// Not used
#define FDC_INT_R2NR		0x02	// Not used
#define FDC_INT_PULSE		0x04
#define FDC_INT_NOW		0x08

// Id of registers
#define STATUS		0
#define TRACK		1
#define SECTOR		2
#define DATA		3
#define CMD		4

// Flags
#define FDC_FLAG_DAM		0x00
#define FDC_FLAG_VERIFICATION	0x02
#define FDC_FLAG_HEADLOAD	0x04
#define FDC_FLAG_TRACKUPDATE	0x08
#define FDC_FLAG_MULTIRECORD	0x08

// Other
#define FDC_EXTRA_DELAY		15000
#define FDC_SEEK_FORWARD	true
#define	FDC_SEEK_BACKWARD	!FDC_SEEK_FORWARD

/* FDC emulation control:
Bit 7  6  5  4  3  2  1  0
    V  S  -  -  I3 I2 I1 I0
    
  V = vector (previous step direction)
  S = seek (seek is selected)
  I3-0 = interrupt request mask, according to the following
      I3: issue an interrupt now. This bit can only be reset with another "Force interrupt" command.
      I2: issue an interrupt at the next index pulse.
      I1: issue an interrupt at the next ready to not-ready transition of the READY pin.
      I0: issue an interrupt at the next not-ready to ready transition of the READY pin.
  (If I0-I3 are 0: don't issue any interrupt, but still abort the current command). 
 */
      
class  MB8877 {
  struct {
    char control,  
      cmdtype,  // Command type
      track;    // Current track (might be != reg[TRACK])
    unsigned int  position, // Current position on sector
      side,    // Current side
      disk;   // Current disk
    bool  vector,   // Previous step direction
      seek;   // Seek selected
  } fdc;
  public:
    MB8877();
    ~MB8877();
    char reg[5];
    void  decode_command();
    long  locate(void);
    void  vdisk(void);
    void  cmd_restore(int);
    void  cmd_seek(char);
    void  cmd_step(bool);
    void  cmd_step(char, bool);
    void  cmd_readdata(char);
    void  cmd_writedata(char);
    void  cmd_readaddr(char);
    void  cmd_readtrack(char);
    void  cmd_writetrack(char);
    void  cmd_forceint(char);
  private:
} mb8877;

/* ------------------------------------------------
	FDC section
------------------------------------------------
Type	Command         b7 b6 b5 b4 b3 b2 b1 b0
   I	Restore         0  0  0  0  h  V  r1 r0
   I	Seek            0  0  0  1  h  V  r1 r0
   I	Step            0  0  1  T  h  V  r1 r0
   I	Step-In         0  1  0  T  h  V  r1 r0
   I	Step-Out        0  1  1  T  h  V  r1 r0
  II	Read Sector     1  0  0  m  S  E  C  0
  II	Write Sector    1  0  1  m  S  E  C  a0
 III	Read Address    1  1  0  0  0  E  0  0
 III	Read Track      1  1  1  0  0  E  0  0
 III	Write Track     1  1  1  1  0  E  0  0        
  IV	Force Interrupt 1  1  0  1  i3 i2 i1 i0 	

    r1 r0  Stepping Motor Rate
       V      Track Number Verify Flag (0: no verify, 1: verify on dest track)
       h      Head Load Flag (1: load head at beginning, 0: unload head)
       T      Track Update Flag (0: no update, 1: update Track Register)
       a0     Data Address Mark (0: FB, 1: F8 (deleted DAM))
       C      Side Compare Flag (0: disable side compare, 1: enable side comp)
       E      15 ms delay (0: no 15ms delay, 1: 15 ms delay)
       S      Side Compare Flag (0: compare for side 0, 1: compare for side 1)
       m      Multiple Record Flag (0: single record, 1: multiple records)
           i3 i2 i1 i0    Interrupt Condition Flags
              i3-i0 = 0 Terminate with no interrupt (INTRQ)
                    i3 = 1 Immediate interrupt, requires a reset
                    i2 = 1 Index pulse
                    i1 = 1 Ready to not ready transition
                    i0 = 1 Not ready to ready transition
------------------------------------------------ */


// ----- Events
#define EVENT_SEEK    0
#define EVENT_SEEKEND   1
#define EVENT_SEARCH    2
#define EVENT_TYPE4   3
#define EVENT_MULTI1    4
#define EVENT_MULTI2    5
#define EVENT_LOST    6

void scanSD();

#endif

