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

#include "sdcard.h"
#include "qx1.h"

extern class MB8877 mb8877;

// ----------------------------------------------------------------------------
//  Scan the directory
// ----------------------------------------------------------------------------
int scanDirectory(int wanted) {
   int i=-1;
   char _filename[32];
   File entry;

  if (mb8877.reg[STATUS] & FDC_ST_NOTREADY) return FALSE; // Exit if no card

  droot.rewindDirectory();
  while(true)
  {
    entry =  droot.openNextFile();
    if (! entry) return false;

    if (! entry.isDirectory())
    {
      i=-1;
      strcpy(_filename,entry.name());
      if (_filename[0] != 'D') continue;
      if (_filename[1] != 'I') continue;
      if (_filename[2] != 'S') continue;
      if (_filename[3] != 'K') continue;
      if (_filename[4] != '_') continue;
      if ((_filename[5] < '0')||(_filename[5] > '9')) continue;
      if ((_filename[6] < '0')||(_filename[6] > '9')) continue;
      if ((_filename[7] < '0')||(_filename[7] > '9')) continue;
      if (_filename[8] != '.') continue;
      if (_filename[8] != 'Q') continue;
      if (_filename[9] != 'X') continue;
      if (_filename[10] != '1') continue;

      i=_filename[5]-48;
      i=i*10+_filename[6]-48;
      i=i*10+_filename[7]-48;

      if (i >= wanted) break;
    }
  }
  Serial.println("Fichier: ");
  Serial.println(_filename);
  if (entry.size() != 1556480)
  {
    Serial.print(" bad size: ");
    Serial.print(entry.size(), DEC);
    Serial.println(" != 1556480");
  }
  return i;
}

