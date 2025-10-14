// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <signal.h>

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source


volatile int alarmEnabled = FALSE;
volatile int alarmCount = 0;
volatile int uaReceived = FALSE;
volatile int STOP = FALSE;


////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    if (openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate) == -1) 
        return -1;

    if (connectionParameters.role == LlTx) {
        if (sendSetAndWaitForUa(connectionParameters.timeout, connectionParameters.nRetransmissions) == -1)
            return -1;
    } else if (connectionParameters.role == LlRx) {
        if (waitForSetsendUA(connectionParameters.timeout) == -1)
            return -1;
    }

    return 0;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose()
{
    // TODO: Implement this function

    return 0;
}

int receiveUA()
{
    unsigned char byte;
    static int state = 0;

    unsigned char FLAG = 0x7E;
    unsigned char A = 0x01; // receiver->transmitter direction
    unsigned char C = 0x07; // UA control field
    unsigned char BCC = A ^ C;

    int res = readByteSerialPort(&byte);
    if (res <= 0)
        return FALSE;

    switch (state)
    {
    case 0:
        if (byte == FLAG) state = 1;
        break;
    case 1:
        if (byte == A) state = 2;
        else if (byte != FLAG) state = 0;
        break;
    case 2:
        if (byte == C) state = 3;
        else if (byte == FLAG) state = 1;
        else state = 0;
        break;
    case 3:
        if (byte == BCC) state = 4;
        else if (byte == FLAG) state = 1;
        else state = 0;
        break;
    case 4:
        if (byte == FLAG)
        {
            printf("UA frame received!\n");
            state = 0;
            return TRUE;
        }
        else state = 0;
        break;
    default:
        state = 0;
        break;
    }

    return FALSE;
}

void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;

    printf("Alarm #%d received\n", alarmCount);
}

int sendSetAndWaitForUa(int timeoutSeconds, int maxRetries) {
    unsigned char SET_frame[5];
        const unsigned char FLAG = 0x7E;
        const unsigned char A = 0x03;    
        const unsigned char C = 0x03;    
        const unsigned char BCC = A ^ C; 

        SET_frame[0] = FLAG;
        SET_frame[1] = A;
        SET_frame[2] = C;
        SET_frame[3] = BCC;
        SET_frame[4] = FLAG;
        struct sigaction act = {0};
        act.sa_handler = &alarmHandler;
        if (sigaction(SIGALRM, &act, NULL) == -1)
        {
            perror("sigaction");
            exit(1);
        }

        printf("Alarm configured\n");

        
        int retries = 0;

        while (retries < maxRetries && !uaReceived)
        {
            retries++;
            
            int bytes = writeBytesSerialPort(SET_frame, sizeof(SET_frame));
            printf("%d bytes written to serial port\n", bytes);
            alarmEnabled = TRUE;
            alarm(timeoutSeconds);
            while (alarmEnabled && !uaReceived)
            {
                uaReceived = receiveUA();
            }
            if (!uaReceived && !alarmEnabled)
            printf("Timeout #%d - retransmitting SET frame...\n", retries);

        
        }
        if (uaReceived)
        {
            printf("UA frame successfully received!\n");
            alarm(0);
            alarmEnabled = FALSE;
            return 0;
        }
        else{
            printf("Failed to receive UA frame after 3 attempts\n");
            return -1;
        }

        // Wait until all bytes have been written to the serial port
        sleep(1);
}

int waitForSetsendUA(int timeoutSeconds) {
    unsigned char UA[5];
    const unsigned char FLAG = 0x7E;
    const unsigned char A = 0x01;
    const unsigned char C = 0x07;
    const unsigned char BC = A ^ C;
    const unsigned char S_A = 0x03;
    const unsigned char S_C = 0x03;
    const unsigned char S_BC = S_A ^ S_C;

    UA[0] = FLAG;
    UA[1] = A;
    UA[2] = C;
    UA[3] = BC;
    UA[4] = FLAG;

    int state = 0;

    // Setup alarm signal handler
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    // Start alarm
    alarmEnabled = 1;
    alarm(timeoutSeconds);

    while (alarmEnabled) {
        unsigned char byte;
        int bytes = readByteSerialPort(&byte);
        if (bytes < 0) {
            perror("Error reading from serial port");
            return -1;
        }
        if (bytes == 0) {
            // No data available; just continue waiting
            continue;
        }

        printf("Byte received: 0x%02X\n", byte);

        switch (state) {
            case 0:
                if (byte == FLAG) state = 1;
                break;
            case 1:
                if (byte == S_A) state = 2;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 2:
                if (byte == S_C) state = 3;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 3:
                if (byte == S_BC) state = 4;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 4:
                if (byte == FLAG) {
                    int send = writeBytesSerialPort(UA, sizeof(UA));
                    if (send < 0) {
                        perror("Error sending UA frame");
                        return -1;
                    }
                    printf("Sent UA frame.\n");
                    alarm(0);        // Cancel the alarm
                    alarmEnabled = 0; // Stop the loop
                    return 0;
                } else {
                    state = 0;
                }
                break;
            default:
                state = 0;
                break;
        }
    }

    // Timeout reached without receiving SET frame properly
    printf("Timeout reached while waiting for SET frame.\n");
    return -1;
}