// Protocol constants and definitions for Link Layer

#ifndef _UTILS_H_
#define _UTILS_H_

// Frame delimiters
#define FLAG 0x7E  // 01111110 - Frame boundary delimiter

// Address field values
#define A_T  0x03  // 00000011 - Transmitter commands / Receiver responses
#define A_R  0x01  // 00000001 - Receiver commands / Transmitter responses

// Control field values - Unnumbered frames
#define C_SET   0x03  // 00000011 - Set up connection
#define C_DISC  0x0B  // 00001011 - Disconnect
#define C_UA    0x07  // 00000111 - Unnumbered Acknowledgment

// Control field values - Supervisory frames (R = N(r) bit)
#define C_RR_0  0x05  // 00000101 - Receiver Ready, N(r) = 0
#define C_RR_1  0x85  // 10000101 - Receiver Ready, N(r) = 1
#define C_REJ_0 0x01  +
#define C_REJ_1 0x81 


#define C_I_0   0x00  
#define C_I_1   0x80  

#define ESC      0x7D  
#define ESC_FLAG 0x5E  
#define ESC_ESC  0x5D  

#define BCC1(a, c) ((a) ^ (c))



#endif // _UTILS_H_
