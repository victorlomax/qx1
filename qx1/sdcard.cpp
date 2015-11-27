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

typedef unsigned int uint8_t;

Sd2Card   card;
SdVolume  volume;
//SdFile    root;
File      root;    // Directory root
File      disk;    // Current virtual disk
dir_t     direntry;


// ----------------------------------------------------------------------------
//  Scan the directory
// ----------------------------------------------------------------------------
int scanDirectory(int wanted) {
  dir_t entry;
  int i=-1;

  if (mb8877.reg[STATUS] & FDC_ST_NOTREADY) return FALSE; // Exit if no card

  root.rewind();
  while(root.readDir(&entry)>0) {
//                Serial.print("DBG Read: ");
//                Serial.println((char*)entry.name);

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
    if (entry.name[8] != '.') continue;
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

