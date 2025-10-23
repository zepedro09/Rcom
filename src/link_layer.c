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
        // Transmitter: send SET, wait for UA (with retries)
        if (sendAndWaitResponse(connectionParameters, C_SET, C_UA) == -1) {
            printf("Failed to establish connection\n");
            closeSerialPort();
            return -1;
        }
        printf("Connection established (Transmitter)\n");
        return 0;
        
    } else if (connectionParameters.role == LlRx) {
        // Receiver: wait for SET (extended timeout for Tx retries), then send UA
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
    if (connectionParameters.role == LlTx) {
        printf("Transmitter: Initiating disconnect...\n");
        
        // Send DISC and wait for DISC response (with retries handled by orchestrator)
        if (sendAndWaitResponse(connectionParameters, C_DISC, C_DISC) == -1) {
            printf("Failed to receive DISC response\n");
            closeSerialPort();
            return -1;
        }
        
        // Send UA (no wait)
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
        
        // Wait for DISC
        if (waitForSupervisionFrame(LlRx, C_DISC, connectionParameters.timeout * connectionParameters.nRetransmissions) == -1) {
            printf("Timeout: Failed to receive DISC frame\n");
            closeSerialPort();
            return -1;
        }
        
        // Send DISC and wait for UA (with retries handled by orchestrator)
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

/**
 * Send a supervision/unnumbered frame (no waiting)
 * @param role - LlTx or LlRx (determines A field)
 * @param controlField - Control field to send (C_SET, C_DISC, C_UA, etc.)
 * @return 0 on success, -1 on failure
 */
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

/**
 * Send a command frame and wait for expected response with retry mechanism
 * @param connectionParameters - Connection config (includes role, timeout, nRetransmissions)
 * @param commandC - Control field to send (C_SET, C_DISC, etc.)
 * @param expectedReplyC - Expected control field in response (C_UA, C_DISC, etc.)
 * @return 0 on success, -1 on failure
 */
int sendAndWaitResponse(LinkLayer connectionParameters, unsigned char commandC, unsigned char expectedReplyC)
{
    int retries = 0;
    
    printf("Starting send-and-wait: command C=0x%02X, expecting reply C=0x%02X\n", commandC, expectedReplyC);
    
    while (retries < connectionParameters.nRetransmissions) {
        retries++;
        
        // 1. Send command frame
        if (sendSupervisionFrame(connectionParameters.role, commandC) == -1) {
            printf("Failed to send command frame (attempt %d/%d)\n", retries, connectionParameters.nRetransmissions);
            continue; // Try again
        }
        
        // 2. Wait for response (single attempt - no nested retries!)
        // Determine the role of the *responder* (opposite of sender)
        LinkLayerRole responderRole = (connectionParameters.role == LlTx) ? LlRx : LlTx;
        // Note: when we wait we must use the responder's role so the expected A field
        // is computed correctly inside waitForSupervisionFrame.
        if (waitForSupervisionFrame(responderRole, expectedReplyC, connectionParameters.timeout) == 0) {
            printf("Response received successfully!\n");
            return 0; // Success!
        }
        
        printf("No response - retry %d/%d\n", retries, connectionParameters.nRetransmissions);
    }
    
    printf("Failed after %d attempts\n", connectionParameters.nRetransmissions);
    return -1;
}

/**
 * Wait for a specific supervision/unnumbered frame (SINGLE ATTEMPT with timeout)
 * @param role - LlTx or LlRx (determines expected A field)
 * @param expectedC - Expected control field
 * @param timeoutSeconds - Timeout for this attempt
 * @return 0 on success, -1 on timeout
 */
int waitForSupervisionFrame(LinkLayerRole role, unsigned char expectedC, int timeoutSeconds)
{
    unsigned char expectedA;

    // Determine expected A depending on what control field we're waiting for.
    // SET is a command sent by Tx and uses A_T. UA is a reply sent by Rx and uses A_T.
    // DISC is a special case: when Rx waits for DISC (role == LlRx) it expects a DISC from Tx (A_T),
    // while when Tx waits for DISC (role == LlTx) it expects a DISC from Rx (A_R).
    if (expectedC == C_SET || expectedC == C_UA) {
        expectedA = A_T;
    } else if (expectedC == C_DISC) {
        expectedA = (role == LlRx) ? A_T : A_R;
    } else {
        // Default: assume A_T for now (will be refined for RR/REJ/I frames later)
        expectedA = A_T;
    }

    // Setup alarm handler
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
        case 0: // FLAG
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
        case 4: // closing FLAG
            if (byte == FLAG) {
                printf("Frame received (A=0x%02X, C=0x%02X)\n", receivedA, receivedC);
                alarm(0);
                alarmEnabled = FALSE;
                return 0; // Success!
            } 
            else state = 0;
            break;
        default:
            state = 0;
            break;
        }
    }

    printf("Timeout - no frame received\n");
    return -1; // Timeout
}