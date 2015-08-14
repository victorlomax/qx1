// ----------------------------------------------------------------------------
//	CCITT-CRC16 calculator
// ----------------------------------------------------------------------------
class CRC {
  public:
    CRC() { result[0]=result[1]=0xff; }
    uchar msb() { return result[1]; }
    uchar lsb() { return result[0]; }
    void compute(uchar byte) 
    { 
	    uchar t = 8; 
	    result = result ^ byte << 8; 
      do 
	    { 
		    if (result & 0x8000) 
			    result = result << 1 ^ 0x1021; 
		    else 
			    result = result << 1; 
	    } while (--t); 
	  }
	  private:
	    uchar result[2];
}
