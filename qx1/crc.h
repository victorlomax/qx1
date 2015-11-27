// ----------------------------------------------------------------------------
//	CCITT-CRC16 calculator
// ----------------------------------------------------------------------------
class CRC {
  public:
    CRC() { result.BYTE[0]=result.BYTE[1]=0xff; }
    void reset() { result.BYTE[0]=result.BYTE[1]=0xff; }
    unsigned char check(unsigned char c, unsigned char b) { return (c==result.BYTE[b])?true:false; }
    unsigned char msb() { return result.BYTE[1]; }
    unsigned char lsb() { return result.BYTE[0]; }
    void compute(unsigned char byte) 
    { 
	    unsigned char t = 8; 
	    result.WORD = result.WORD ^ byte << 8; 
      do 
	    { 
		    if (result.WORD & 0x8000) 
			    result.WORD = result.WORD << 1 ^ 0x1021; 
		    else 
			    result.WORD = result.WORD << 1; 
	    } while (--t); 
	  }
	  private:
      union {
        unsigned short WORD;
        unsigned char BYTE[2];
      } result;
};

