# qx1
FDC Simulator for Yamaha QX1

Virtual Diskette File Format

// Track 00 to 79: 5 sector / track, 1024 (data) + 2 (crc) bytes / sector
0x00000 [ Block   0 (1024 bytes) ] = Side 0, Track 00, Sector 0
0x00400 CRC16 (2 bytes)
0x00402 [ Block   1 (1024 bytes) ] = Side 0, Track 00, Sector 1
0x00802 CRC16 (2 bytes)
0x00804 [ Block   2 (1024 bytes) ] = Side 0, Track 00, Sector 2
...
0x0140a [ Block   5 (1024 bytes) ] = Side 1, Track 00, Sector 0
0x0180a CRC16 (2 bytes)
0x0180c [ Block   6 (1024 bytes) ] = Side 1, Track 00, Sector 1
...
0xc823e [ Block 799 (1024 bytes) ] = Side 1, Track 79, Sector 4
0xc863e CRC16 (2 bytes)
0xc8640 [ Block 800 (512 bytes) ] = Side 0, Track 80, Sector 0
0xc8840 CRC16 (2 bytes)
// Track 80 to 135: 9 sector / track, 512 (data) + 2 (crc) bytes / sector
0xc8842 [ Block 801 (512 bytes) ] = Side 0, Track 80, Sector 1
...
[ Block 809 (512 bytes) ] = Side 1, Track 80, Sector 8
