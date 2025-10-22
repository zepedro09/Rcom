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

// Forward declarations for internal helpers
int waitForSupervisionFrame(LinkLayerRole role, unsigned char expectedC, int timeoutSeconds, int maxRetries);



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
        // Receiver: wait for SET, then send UA (send-only)
        if (waitForSupervisionFrame(LlRx, C_SET, connectionParameters.timeout, connectionParameters.nRetransmissions) == -1)
            return -1;
        if (sendFrameAndWaitForResponse(LlRx, C_UA, 0, 0, 0, FALSE) == -1)
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
        
        // Step 1: Wait for DISC (receive-only helper)
        if (waitForSupervisionFrame(LlRx, C_DISC, connectionParameters.timeout, connectionParameters.nRetransmissions) == -1) {
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

// Waits for a valid supervision/unnumbered frame with a specific control field
// Uses a timeout and retry mechanism similar to the send-and-wait helper
int waitForSupervisionFrame(LinkLayerRole role, unsigned char expectedC, int timeoutSeconds, int maxRetries)
{
    unsigned char expectedA = (role == LlRx) ? A_T : A_R;

    // Setup alarm handler
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    int retries = 0;
    int frameReceived = FALSE;

    printf("Waiting for frame (A=0x%02X, C=0x%02X)...\n", expectedA, expectedC);

    while (retries < maxRetries && !frameReceived)
    {
        retries++;
        alarmEnabled = TRUE;
        alarm(timeoutSeconds);

        int state = 0;
        unsigned char receivedA = 0, receivedC = 0, receivedBCC = 0;
        
        while (alarmEnabled && !frameReceived)
        {
            unsigned char byte;
            int res = readByteSerialPort(&byte);
            if (res <= 0) continue;

            switch (state)
            {
            case 0: // FLAG
                if (byte == FLAG) state = 1;
                break;
            case 1: // A
                if (byte == expectedA) { receivedA = byte; state = 2; }
                else if (byte != FLAG) state = 0;
                break;
            case 2: // C
                if (byte == expectedC) { receivedC = byte; state = 3; }
                else if (byte == FLAG) state = 1; else state = 0;
                break;
            case 3: // BCC1
                receivedBCC = byte;
                if (receivedBCC == BCC1(receivedA, receivedC)) state = 4;
                else if (byte == FLAG) state = 1; else state = 0;
                break;
            case 4: // closing FLAG
                if (byte == FLAG) {
                    printf("Expected frame received (A=0x%02X, C=0x%02X).\n", receivedA, receivedC);
                    frameReceived = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                } else state = 0;
                break;
            default:
                state = 0;
                break;
            }
        }

        if (!frameReceived && !alarmEnabled) {
            printf("Timeout #%d - still waiting for expected frame...\n", retries);
        }
    }

    return frameReceived ? 0 : -1;
}


// Removed waitForSetsendUA in favor of generic waitForSupervisionFrame + send-only UA


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