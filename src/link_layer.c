// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"
#include "utils.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source


volatile int alarmEnabled = FALSE;
volatile int alarmCount = 0;
volatile int uaReceived = FALSE;
volatile int STOP = FALSE;

int sendSupervisionFrame(LinkLayerRole role, unsigned char controlField);
int waitForSupervisionFrame(LinkLayerRole role, unsigned char expectedC, int timeoutSeconds);
int sendAndWaitResponse(LinkLayer connectionParameters, unsigned char commandC, unsigned char expectedReplyC);



////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    if (openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate) == -1) 
        return -1;

    if (connectionParameters.role == LlTx) {
        if (sendAndWaitResponse(connectionParameters, C_SET, C_UA) == -1) {
            printf("Failed to establish connection\n");
            closeSerialPort();
            return -1;
        }
        printf("Connection established (Transmitter)\n");
        return 0;
        
    } else if (connectionParameters.role == LlRx) {
        int extendedTimeout = connectionParameters.timeout * connectionParameters.nRetransmissions;
        if (waitForSupervisionFrame(LlRx, C_SET, extendedTimeout) == -1) {
            closeSerialPort();
            return -1;
        }
        
        if (sendSupervisionFrame(LlRx, C_UA) == -1) {
            closeSerialPort();
            return -1;
        }
        
        printf("Connection established (Receiver)\n");
        return 0;
    }

    return 0;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize, LinkLayer connectionParameters)
{

    if (connectionParameters.role == LlTx) {
        sendIframe();
    } else if (connectionParameters.role == LlRx) {
        //Send RR or REJ
       
    }

    return 0;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet, LinkLayer connectionParameters)
{
    if (connectionParameters.role == LlTx) {
        sendIframe();
    } else if (connectionParameters.role == LlRx) {
    
       
    }


    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(LinkLayer connectionParameters)
{
    if (connectionParameters.role == LlTx) {
        printf("Transmitter: Initiating disconnect...\n");
        
        if (sendAndWaitResponse(connectionParameters, C_DISC, C_DISC) == -1) {
            printf("Failed to receive DISC response\n");
            closeSerialPort();
            return -1;
        }
        
        if (sendSupervisionFrame(LlTx, C_UA) == -1) {
            closeSerialPort();
            return -1;
        }
        
        if (closeSerialPort() < 0) {
            perror("Error closing serial port");
            return -1;
        }
        printf("Transmitter: Connection closed successfully\n");
        
    } else if (connectionParameters.role == LlRx) {
        printf("Receiver: Waiting for disconnect...\n");
        
        if (waitForSupervisionFrame(LlRx, C_DISC, connectionParameters.timeout * connectionParameters.nRetransmissions) == -1) {
            printf("Timeout: Failed to receive DISC frame\n");
            closeSerialPort();
            return -1;
        }
        
        if (sendAndWaitResponse(connectionParameters, C_DISC, C_UA) == -1) {
            printf("Failed to receive UA after sending DISC\n");
            closeSerialPort();
            return -1;
        }
        
        if (closeSerialPort() < 0) {
            perror("Error closing serial port");
            return -1;
        }
        printf("Receiver: Connection closed successfully\n");
    }

    return 0;
}

void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;

    printf("Alarm #%d received\n", alarmCount);
}

int sendSupervisionFrame(LinkLayerRole role, unsigned char controlField)
{
    unsigned char sendA = (role == LlTx) ? A_T : A_R;
    
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = sendA;
    frame[2] = controlField;
    frame[3] = BCC1(sendA, controlField);
    frame[4] = FLAG;
    
    int bytes = writeBytesSerialPort(frame, sizeof(frame));
    if (bytes < 0) {
        perror("Error sending frame");
        return -1;
    }
    
    printf("Frame sent: A=0x%02X, C=0x%02X (%d bytes)\n", sendA, controlField, bytes);
    return 0;
}

int sendAndWaitResponse(LinkLayer connectionParameters, unsigned char commandC, unsigned char expectedReplyC)
{
    int retries = 0;
    
    printf("Starting send-and-wait: command C=0x%02X, expecting reply C=0x%02X\n", commandC, expectedReplyC);
    
    while (retries < connectionParameters.nRetransmissions) {
        retries++;

        if (sendSupervisionFrame(connectionParameters.role, commandC) == -1) {
            printf("Failed to send command frame (attempt %d/%d)\n", retries, connectionParameters.nRetransmissions);
            continue;
        }
        LinkLayerRole responderRole = (connectionParameters.role == LlTx) ? LlRx : LlTx;

        if (waitForSupervisionFrame(responderRole, expectedReplyC, connectionParameters.timeout) == 0) {
            printf("Response received successfully!\n");
            return 0; 
        }
        
        printf("No response - retry %d/%d\n", retries, connectionParameters.nRetransmissions);
    }
    
    printf("Failed after %d attempts\n", connectionParameters.nRetransmissions);
    return -1;
}

int waitForSupervisionFrame(LinkLayerRole role, unsigned char expectedC, int timeoutSeconds)
{
    unsigned char expectedA;
    expectedA = (role == LlRx) ? A_T : A_R;

    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    alarmEnabled = TRUE;
    alarm(timeoutSeconds);

    int state = 0;
    unsigned char receivedA = 0, receivedC = 0, receivedBCC = 0;
    
    printf("Waiting for frame (A=0x%02X, C=0x%02X) with %ds timeout...\n", expectedA, expectedC, timeoutSeconds);
    
    while (alarmEnabled)
    {
        unsigned char byte;
        int res = readByteSerialPort(&byte);
        if (res <= 0) continue;

        switch (state)
        {
        case 0: // Flag
            if (byte == FLAG) state = 1;
            break;
        case 1: // A
            if (byte == expectedA) { receivedA = byte; state = 2; }
            else if (byte != FLAG) state = 0;
            break;
        case 2: // C
            if (byte == expectedC) { receivedC = byte; state = 3; }
            else if (byte == FLAG) state = 1; 
            else state = 0;
            break;
        case 3: // BCC1
            receivedBCC = byte;
            if (receivedBCC == BCC1(receivedA, receivedC)) state = 4;
            else if (byte == FLAG) state = 1; 
            else state = 0;
            break;
        case 4: //Flag
            if (byte == FLAG) {
                printf("Frame received (A=0x%02X, C=0x%02X)\n", receivedA, receivedC);
                alarm(0);
                alarmEnabled = FALSE;
                return 0;
            } 
            else state = 0;
            break;
        default:
            state = 0;
            break;
        }
    }

    printf("Timeout - no frame received\n");
    return -1; 
}


int sendIFrame(unsigned char *data, int datasize, int seqNumber){

    unsigned char tmp[datasize + 1];
    memcpy(tmp, data, datasize);
    tmp[datasize] = (unsigned char) createBCC2(data, datasize);

    int max_stuffed = 2 * (datasize + 1) + 1;
    unsigned char *stuffed = malloc(max_stuffed);
    if (!stuffed) return -1;

    int stuffedsize = stuffBytes(tmp, datasize + 1, stuffed);
    int frame_size = stuffedsize + 5;
    int controlField;

    controlField = (seqNumber == 0) ? C_I_0 : C_I_1;

    unsigned char frame[frame_size];

    frame[0] = FLAG;
    frame[1] = A_T;
    frame[2] = controlField;
    frame[3] = BCC1(A_T, controlField);
    memcpy(&frame[4], stuffed, stuffedsize);
    frame[frame_size - 1] = FLAG;

    int bytes = writeBytesSerialPort(frame, sizeof(frame));

    if (bytes < 0) {
        perror("Error sending frame");
        return -1;
    }
    
    printf("Frame sent: A=0x%02X, C=0x%02X (%d bytes)\n", sendA, controlField, bytes);
    return 0;


}


int recieveIFrame(LinkLayerRole role, int timeoutSeconds, unsigned char *dest, int destSize, int seqNumber)
{
    unsigned char expectedA, expectedC;
    expectedA =  A_T;
    expectedC = (seqNumber == 0) ? C_I_0 : C_I_1;

    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    alarmEnabled = TRUE;
    alarm(timeoutSeconds);

    int state = 0;
    unsigned char receivedA = 0, receivedC = 0, receivedBCC = 0;

    int tmpSize = destSize * 2 + 16;
    unsigned char *tmp = malloc(tmpSize);
    int tmpLen = 0;
    
    
    printf("Waiting for frame (A=0x%02X, C=0x%02X) with %ds timeout...\n", expectedA, expectedC, timeoutSeconds);
    
    while (alarmEnabled)
    {
        unsigned char byte;
        int res = readByteSerialPort(&byte);
        if (res <= 0) continue;

        switch (state)
        {
        case 0: // Flag
            if (byte == FLAG) state = 1;
            break;
        case 1: // A
            if (byte == expectedA) { receivedA = byte; state = 2; }
            else if (byte != FLAG) state = 0;
            break;
        case 2: // C
            if (byte == expectedC) {
                receivedC = byte;
                state = 3; 
                }
            else if (byte == FLAG) state = 1; 
            else state = 0;
            break;
        case 3: // BCC1
            receivedBCC = byte;
            if (receivedBCC == BCC1(receivedA, receivedC)) state = 4;
            else if (byte == FLAG) state = 1; 
            else state = 0;
            break;
        case 4: //Flag, D and BCC2
            if (byte == FLAG) {
                destSize = destuffBytes(tmp, tmpLen, dest);
                free(tmp);
                int bcc2 = createBCC2(dest, destSize -1);
                if(bcc2 != dest[destSize -1]){
                    printf("BCC2 error\n");
                    return -2;
                }

                alarm(0);
                alarmEnabled = FALSE;
                return seqNumber;
            }
            else{
                if(tmpLen + 1 > tmp){
                    free(tmp);
                    printf("Temporary buffer overflow\n");
                    return -1;
                }
                tmp[tmpLen++] = byte;
            }
            break;
        default:
            state = 0;
            break;
        }
    }

    printf("Timeout - no frame received\n");
    return -1; 
}

int createBCC2 (const unsigned char *data, int dataSize){
    int bcc2 = 0x00;

    for(int i =0; i < dataSize; i++){
        bcc2 ^= data[i];
    }

    return bcc2;
}


int replaceByte(unsigned char byte, unsigned char *res){
    if(!res) return -1;

    if(byte == FLAG){
        res[0] = ESC;
        res[1] = ESC_FLAG;
        return 2;
    }
    else if(byte == ESC){
        res[0] = ESC;
        res[1] = ESC_ESC;
        return 2;
    }
    else{
        res[0] = byte;
        return 1;
    }
    
    
}


int stuffBytes(unsigned char *data, int dataSize, unsigned char *dest){
    int max_size = 2 * dataSize + 1;

    if (!data || !dest || dataSize < 0) return -1;

    int dest_size = 0;
    for(int i =0; i < dataSize; i++){
        unsigned char temp = data[i];
        int size_added = replaceByte(temp, &dest[dest_size]);

        if(size_added == -1) return -1;
        if(dest_size + size_added > max_size) return -1;

        dest_size += size_added;
        
    }
    return dest_size;
}

int destuffBytes(unsigned char *data, int dataSize, unsigned char *dest){
    int min_size = dataSize/2 -1;

    if (!data || !dest || dataSize < 0) return -1;

    int dest_size = 0;

    for(int i =0; i < dataSize; i++){
        if(data[i] == ESC){
            if (i + 1 >= dataSize) return -1;
            if(data[i + 1] == ESC_FLAG) dest[dest_size++] = FLAG;
            else if(data[i + 1] == ESC_ESC) dest[dest_size++] = ESC;
            else return -1; 
            i++;
        }
        else dest[dest_size++] = data[i];
    }
    return dest_size;

}

