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

#ifndef _H_SDCARD
#define _H_SDCARD

int scanDirectory(int);

#endif
