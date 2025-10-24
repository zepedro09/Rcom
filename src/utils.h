#ifndef _UTILS_H_
#define _UTILS_H_


#define FLAG 0x7E 

#define A_T  0x03
#define A_R  0x01 

#define C_SET   0x03  
#define C_DISC  0x0B 
#define C_UA    0x07 

#define C_RR_0  0x05  
#define C_RR_1  0x85  
#define C_REJ_0 0x01  
#define C_REJ_1 0x81 


#define C_I_0   0x00  
#define C_I_1   0x80  

#define ESC      0x7D  
#define ESC_FLAG 0x5E  
#define ESC_ESC  0x5D  

#define BCC1(a, c) ((a) ^ (c))



#endif // _UTILS_H_
