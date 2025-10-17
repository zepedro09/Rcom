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

// Store connection parameters for llclose()
static LinkLayer connectionParams;


////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    // Store connection parameters for later use in llclose()
    connectionParams = connectionParameters;
    
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
    int result;

    if (connectionParams.role == LlTx)
    {
        // Transmitter: Send DISC, wait for DISC, send UA
        result = txDisconnect(connectionParams.timeout, connectionParams.nRetransmissions);
    }
    else if (connectionParams.role == LlRx)
    {
        // Receiver: Wait for DISC, send DISC, wait for UA
        result = rxDisconnect(connectionParams.timeout);
    }
    else
    {
        printf("Error: Invalid role\n");
        return -1;
    }

    if (result == -1)
    {
        printf("Error during disconnect sequence\n");
        closeSerialPort();
        return -1;
    }

    // Close the serial port
    if (closeSerialPort() < 0)
    {
        perror("Error closing serial port");
        return -1;
    }

    printf("Connection closed successfully\n");
    return 0;
}

// Helper function to receive DISC frame from peer
// For Tx: expects DISC with A=0x01, C=0x0B (from Rx)
// For Rx: expects DISC with A=0x03, C=0x0B (from Tx)
int receiveDISC(unsigned char expectedA)
{
    unsigned char byte;
    static int state = 0;

    const unsigned char FLAG = 0x7E;
    const unsigned char C = 0x0B; // DISC control field
    const unsigned char BCC = expectedA ^ C;

    int res = readByteSerialPort(&byte);
    if (res <= 0)
        return FALSE;

    switch (state)
    {
    case 0:
        if (byte == FLAG) state = 1;
        break;
    case 1:
        if (byte == expectedA) state = 2;
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
            printf("DISC frame received!\n");
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

// Transmitter side: Send DISC and wait for DISC response, then send UA
int txDisconnect(int timeoutSeconds, int maxRetries)
{
    unsigned char DISC_frame[5];
    const unsigned char FLAG = 0x7E;
    const unsigned char A_TX = 0x03; // Transmitter sends with A=0x03
    const unsigned char C_DISC = 0x0B;
    const unsigned char BCC = A_TX ^ C_DISC;

    DISC_frame[0] = FLAG;
    DISC_frame[1] = A_TX;
    DISC_frame[2] = C_DISC;
    DISC_frame[3] = BCC;
    DISC_frame[4] = FLAG;

    // Setup alarm handler
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    int retries = 0;
    int discReceived = FALSE;

    printf("Transmitter: Sending DISC frame...\n");

    while (retries < maxRetries && !discReceived)
    {
        retries++;
        
        // Send DISC frame
        int bytes = writeBytesSerialPort(DISC_frame, sizeof(DISC_frame));
        if (bytes < 0)
        {
            perror("Error sending DISC frame");
            return -1;
        }
        printf("DISC frame sent (%d bytes)\n", bytes);
        
        alarmEnabled = TRUE;
        alarm(timeoutSeconds);
        
        // Wait for DISC response from Receiver (A=0x01, C=0x0B)
        while (alarmEnabled && !discReceived)
        {
            discReceived = receiveDISC(0x01); // Expecting A=0x01 from Receiver
        }
        
        if (!discReceived && !alarmEnabled)
            printf("Timeout #%d - retransmitting DISC frame...\n", retries);
    }

    if (!discReceived)
    {
        printf("Failed to receive DISC response after %d attempts\n", maxRetries);
        return -1;
    }

    alarm(0); // Cancel alarm
    alarmEnabled = FALSE;

    // Send UA frame (A=0x01, C=0x07)
    unsigned char UA_frame[5];
    const unsigned char A_UA = 0x01;
    const unsigned char C_UA = 0x07;
    const unsigned char BCC_UA = A_UA ^ C_UA;

    UA_frame[0] = FLAG;
    UA_frame[1] = A_UA;
    UA_frame[2] = C_UA;
    UA_frame[3] = BCC_UA;
    UA_frame[4] = FLAG;

    int bytes = writeBytesSerialPort(UA_frame, sizeof(UA_frame));
    if (bytes < 0)
    {
        perror("Error sending UA frame");
        return -1;
    }
    printf("Transmitter: UA frame sent (%d bytes)\n", bytes);

    return 0;
}

// Receiver side: Wait for DISC, send DISC response, then wait for UA
int rxDisconnect(int timeoutSeconds)
{
    const unsigned char FLAG = 0x7E;
    
    // Setup alarm handler
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    printf("Receiver: Waiting for DISC frame...\n");

    // Wait for DISC from Transmitter (A=0x03, C=0x0B)
    alarmEnabled = TRUE;
    alarm(timeoutSeconds);
    
    int discReceived = FALSE;
    while (alarmEnabled && !discReceived)
    {
        discReceived = receiveDISC(0x03); // Expecting A=0x03 from Transmitter
    }

    if (!discReceived)
    {
        printf("Timeout: Failed to receive DISC frame from Transmitter\n");
        return -1;
    }

    alarm(0); // Cancel alarm

    // Send DISC response (A=0x01, C=0x0B)
    unsigned char DISC_frame[5];
    const unsigned char A_RX = 0x01;
    const unsigned char C_DISC = 0x0B;
    const unsigned char BCC = A_RX ^ C_DISC;

    DISC_frame[0] = FLAG;
    DISC_frame[1] = A_RX;
    DISC_frame[2] = C_DISC;
    DISC_frame[3] = BCC;
    DISC_frame[4] = FLAG;

    int bytes = writeBytesSerialPort(DISC_frame, sizeof(DISC_frame));
    if (bytes < 0)
    {
        perror("Error sending DISC frame");
        return -1;
    }
    printf("Receiver: DISC frame sent (%d bytes)\n", bytes);

    // Wait for UA from Transmitter (A=0x03, C=0x07)
    printf("Receiver: Waiting for UA frame...\n");
    
    alarmEnabled = TRUE;
    alarm(timeoutSeconds);
    
    int uaReceived = FALSE;
    int state = 0;
    const unsigned char A_UA = 0x03;
    const unsigned char C_UA = 0x07;
    const unsigned char BCC_UA = A_UA ^ C_UA;
    
    while (alarmEnabled && !uaReceived)
    {
        unsigned char byte;

        int res = readByteSerialPort(&byte);
        if (res <= 0)
            continue;

        switch (state)
        {
        case 0:
            if (byte == FLAG) state = 1;
            break;
        case 1:
            if (byte == A_UA) state = 2;
            else if (byte != FLAG) state = 0;
            break;
        case 2:
            if (byte == C_UA) state = 3;
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 3:
            if (byte == BCC_UA) state = 4;
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 4:
            if (byte == FLAG)
            {
                printf("Receiver: UA frame received!\n");
                uaReceived = TRUE;
            }
            else state = 0;
            break;
        default:
            state = 0;
            break;
        }
    }

    alarm(0);

    if (!uaReceived)
    {
        printf("Timeout: Failed to receive UA frame from Transmitter\n");
        return -1;
    }

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