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
volatile int C_DISC = 0x0B;


////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    if (openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate) == -1) 
        return -1;

    if (connectionParameters.role == LlTx) {
        // Transmitter: send SET and wait for UA using the generic helper
        if (sendFrameAndWaitForResponse(LlTx, C_SET, C_UA, connectionParameters.timeout, connectionParameters.nRetransmissions, TRUE) == -1)
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
int llclose(LinkLayer connectionParameters)
{
    if(connectionParameters.role == LlTx) {
        // Transmitter: Send DISC, wait for DISC response, then send UA
        printf("Transmitter: Initiating disconnect...\n");
        
        // Step 1: Send DISC and wait for DISC response
        if (sendFrameAndWaitForResponse(LlTx, C_DISC, C_DISC, connectionParameters.timeout, connectionParameters.nRetransmissions, TRUE) == -1) {
            printf("Failed to complete DISC handshake\n");
            closeSerialPort();
            return -1;
        }
        
        // Step 2: Send UA frame (no retry, no wait for response)
        if (sendFrameAndWaitForResponse(LlTx, C_UA, 0, 0, 0, FALSE) == -1) {
            closeSerialPort();
            return -1;
        }
        
        // Step 3: Close serial port
        if (closeSerialPort() < 0) {
            perror("Error closing serial port");
            return -1;
        }
        printf("Transmitter: Connection closed successfully\n");
        
    } else if (connectionParameters.role == LlRx) {
        // Receiver: Wait for DISC, send DISC response, wait for UA
        printf("Receiver: Waiting for disconnect...\n");
        
        // Step 1: Wait for DISC (receive only - need custom approach)
        struct sigaction act = {0};
        act.sa_handler = &alarmHandler;
        if (sigaction(SIGALRM, &act, NULL) == -1) {
            perror("sigaction");
            closeSerialPort();
            return -1;
        }
        
        alarmEnabled = TRUE;
        alarm(connectionParameters.timeout);
        
        int discReceived = FALSE;
        int state = 0;
        
        printf("Receiver: Waiting for DISC frame...\n");
        
        while (alarmEnabled && !discReceived) {
            unsigned char byte;
            int res = readByteSerialPort(&byte);
            if (res <= 0) continue;
            
            switch (state) {
            case 0:
                if (byte == FLAG) state = 1;
                break;
            case 1:
                if (byte == A_T) state = 2;
                else if (byte != FLAG) state = 0;
                break;
            case 2:
                if (byte == C_DISC) state = 3;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 3:
                if (byte == BCC1(A_T, C_DISC)) state = 4;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 4:
                if (byte == FLAG) {
                    discReceived = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                    printf("DISC frame received from transmitter\n");
                } else state = 0;
                break;
            default:
                state = 0;
                break;
            }
        }
        
        if (!discReceived) {
            printf("Timeout: Failed to receive DISC frame\n");
            closeSerialPort();
            return -1;
        }
        
        // Step 2: Send DISC response and wait for UA
        if (sendFrameAndWaitForResponse(LlRx, C_DISC, C_UA, connectionParameters.timeout, 1, TRUE) == -1) {
            printf("Failed to receive UA after sending DISC\n");
            closeSerialPort();
            return -1;
        }
        
        // Step 3: Close serial port
        if (closeSerialPort() < 0) {
            perror("Error closing serial port");
            return -1;
        }
        printf("Receiver: Connection closed successfully\n");
    }

    return 0;
}

int receiveUA()
{
    unsigned char byte;
    static int state = 0;

    int res = readByteSerialPort(&byte);
    if (res <= 0)
        return FALSE;

    switch (state)
    {
    case 0:
        if (byte == FLAG) state = 1;
        break;
    case 1:
        if (byte == A_R) state = 2;
        else if (byte != FLAG) state = 0;
        break;
    case 2:
        if (byte == C_UA) state = 3;
        else if (byte == FLAG) state = 1;
        else state = 0;
        break;
    case 3:
        if (byte == BCC1(A_R, C_UA)) state = 4;
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

        SET_frame[0] = FLAG;
        SET_frame[1] = A_T;
        SET_frame[2] = C_SET;
        SET_frame[3] = BCC1(A_T, C_SET);
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

    UA[0] = FLAG;
    UA[1] = A_R;
    UA[2] = C_UA;
    UA[3] = BCC1(A_R, C_UA);
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
                if (byte == A_T) state = 2;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 2:
                if (byte == C_SET) state = 3;
                else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 3:
                if (byte == BCC1(A_T, C_SET)) state = 4;
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


/**
 * Generic function to send a supervision/unnumbered frame with retry mechanism
 * and wait for a specific response frame
 * 
 * @param role - LlTx or LlRx (determines A field values)
 * @param sendC - Control field of frame to send (e.g., 0x03 for SET, 0x0B for DISC)
 * @param expectedC - Control field expected in response (e.g., 0x07 for UA, 0x0B for DISC)
 * @param timeoutSeconds - Timeout in seconds for each attempt
 * @param maxRetries - Maximum number of retransmission attempts
 * @param waitForResponse - TRUE to wait for response, FALSE to just send once
 * @return 0 on success, -1 on failure
 */
int sendFrameAndWaitForResponse(LinkLayerRole role, unsigned char sendC, unsigned char expectedC, int timeoutSeconds, int maxRetries, int waitForResponse)
{
    unsigned char sendA, expectedA;
    
    // Determine A field values based on role
    if (role == LlTx) {
        sendA = A_T;      // Transmitter sends with A=0x03
        expectedA = A_R;  // Expects response with A=0x01
    } else { // LlRx
        sendA = A_R;      // Receiver sends with A=0x01
        expectedA = A_T;  // Expects response with A=0x03
    }
    
    // Build frame to send
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = sendA;
    frame[2] = sendC;
    frame[3] = BCC1(sendA, sendC);
    frame[4] = FLAG;
    
    // If not waiting for response, just send once and return
    if (!waitForResponse) {
        int bytes = writeBytesSerialPort(frame, sizeof(frame));
        if (bytes < 0) {
            perror("Error sending frame");
            return -1;
        }
        printf("Frame sent (%d bytes) - not waiting for response\n", bytes);
        return 0;
    }
    
    // Setup alarm handler (if not already configured)
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    
    int retries = 0;
    int responseReceived = FALSE;
    
    printf("Sending frame (A=0x%02X, C=0x%02X)...\n", sendA, sendC);
    
    while (retries < maxRetries && !responseReceived)
    {
        retries++;
        
        // Send frame
        int bytes = writeBytesSerialPort(frame, sizeof(frame));
        if (bytes < 0) {
            perror("Error sending frame");
            return -1;
        }
        printf("Frame sent (%d bytes, attempt #%d)\n", bytes, retries);
        
        // Start alarm
        alarmEnabled = TRUE;
        alarm(timeoutSeconds);
        
        // Wait for response using state machine
        int state = 0;
        unsigned char receivedA = 0, receivedC = 0, receivedBCC = 0;
        
        while (alarmEnabled && !responseReceived)
        {
            unsigned char byte;
            int res = readByteSerialPort(&byte);
            if (res <= 0) continue;
            
            switch (state)
            {
            case 0: // Wait for FLAG
                if (byte == FLAG) state = 1;
                break;
            case 1: // Wait for A
                if (byte == expectedA) {
                    receivedA = byte;
                    state = 2;
                } else if (byte != FLAG) state = 0;
                break;
            case 2: // Wait for C
                if (byte == expectedC) {
                    receivedC = byte;
                    state = 3;
                } else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 3: // Wait for BCC1
                receivedBCC = byte;
                if (receivedBCC == BCC1(receivedA, receivedC)) {
                    state = 4;
                } else if (byte == FLAG) state = 1;
                else state = 0;
                break;
            case 4: // Wait for closing FLAG
                if (byte == FLAG) {
                    printf("Response frame received (A=0x%02X, C=0x%02X)!\n", receivedA, receivedC);
                    responseReceived = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                } else state = 0;
                break;
            default:
                state = 0;
                break;
            }
        }
        
        if (!responseReceived && !alarmEnabled) {
            printf("Timeout #%d - retransmitting frame...\n", retries);
        }
    }
    
    if (!responseReceived) {
        printf("Failed to receive response after %d attempts\n", maxRetries);
        return -1;
    }
    
    return 0;
}