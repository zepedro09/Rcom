// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source


volatile int alarmEnabled = FALSE;
volatile int alarmCount = 0;
volatile int uaReceived = FALSE;
volatile int STOP = FALSE;

// Store connection parameters for llclose()
static LinkLayer connectionParams;

// Sequence number for Stop & Wait ARQ
static unsigned char Ns = 0; // Transmitter sequence number (0 or 1)
static unsigned char expectedNs = 0; // Receiver expected sequence number (0 or 1)


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
// HELPER FUNCTIONS FOR BYTE STUFFING
////////////////////////////////////////////////

// Byte stuffing: Replace 0x7E with 0x7D 0x5E and 0x7D with 0x7D 0x5D
// Returns the number of bytes after stuffing
int stuffBytes(const unsigned char *input, int len, unsigned char *output)
{
    int outputLen = 0;
    
    for (int i = 0; i < len; i++)
    {
        if (input[i] == 0x7E)
        {
            output[outputLen++] = 0x7D;
            output[outputLen++] = 0x5E;
        }
        else if (input[i] == 0x7D)
        {
            output[outputLen++] = 0x7D;
            output[outputLen++] = 0x5D;
        }
        else
        {
            output[outputLen++] = input[i];
        }
    }
    
    return outputLen;
}

// Byte destuffing: Reverse of stuffing
// Returns the number of bytes after destuffing, or -1 on error
int destuffBytes(const unsigned char *input, int len, unsigned char *output)
{
    int outputLen = 0;
    
    for (int i = 0; i < len; i++)
    {
        if (input[i] == 0x7D)
        {
            if (i + 1 >= len)
            {
                // Invalid: escape byte at end of data
                return -1;
            }
            
            i++; // Skip to next byte
            if (input[i] == 0x5E)
            {
                output[outputLen++] = 0x7E;
            }
            else if (input[i] == 0x5D)
            {
                output[outputLen++] = 0x7D;
            }
            else
            {
                // Invalid escape sequence
                return -1;
            }
        }
        else
        {
            output[outputLen++] = input[i];
        }
    }
    
    return outputLen;
}

// Helper function to receive RR or REJ frames
// Returns: 1 for RR, 2 for REJ, 0 for incomplete, -1 for error
int receiveSupervisionFrame(unsigned char *controlField)
{
    unsigned char byte;
    static int state = 0;
    static unsigned char receivedA = 0;
    static unsigned char receivedC = 0;

    const unsigned char FLAG = 0x7E;
    const unsigned char A = 0x01; // Receiver sends with A=0x01

    int res = readByteSerialPort(&byte);
    if (res <= 0)
        return 0;

    switch (state)
    {
    case 0:
        if (byte == FLAG) state = 1;
        break;
    case 1:
        if (byte == A)
        {
            receivedA = byte;
            state = 2;
        }
        else if (byte != FLAG) state = 0;
        break;
    case 2:
        receivedC = byte;
        // Check if it's RR or REJ
        if (byte == 0x05 || byte == 0x85 || byte == 0x01 || byte == 0x81)
        {
            state = 3;
        }
        else if (byte == FLAG) state = 1;
        else state = 0;
        break;
    case 3:
        // Check BCC1
        if (byte == (receivedA ^ receivedC))
        {
            state = 4;
        }
        else if (byte == FLAG) state = 1;
        else state = 0;
        break;
    case 4:
        if (byte == FLAG)
        {
            *controlField = receivedC;
            state = 0;
            
            // Determine if RR or REJ
            if (receivedC == 0x05 || receivedC == 0x85)
                return 1; // RR
            else if (receivedC == 0x01 || receivedC == 0x81)
                return 2; // REJ
        }
        else state = 0;
        break;
    default:
        state = 0;
        break;
    }

    return 0;
}

// Helper function to send supervision frames (RR or REJ)
int sendSupervisionFrame(unsigned char controlField)
{
    unsigned char frame[5];
    const unsigned char FLAG = 0x7E;
    const unsigned char A = 0x01; // Receiver sends with A=0x01
    const unsigned char BCC1 = A ^ controlField;

    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = controlField;
    frame[3] = BCC1;
    frame[4] = FLAG;

    int bytes = writeBytesSerialPort(frame, sizeof(frame));
    return bytes == 5 ? 0 : -1;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize)
{
    if (bufSize <= 0 || bufSize > MAX_PAYLOAD_SIZE)
    {
        printf("Error: Invalid buffer size %d\n", bufSize);
        return -1;
    }

    const unsigned char FLAG = 0x7E;
    const unsigned char A = 0x03; // Transmitter sends with A=0x03
    const unsigned char C = (Ns == 0) ? 0x00 : 0x80; // Control field with sequence number
    const unsigned char BCC1 = A ^ C;

    // Calculate BCC2 (XOR of all data bytes before stuffing)
    unsigned char BCC2 = 0x00;
    for (int i = 0; i < bufSize; i++)
    {
        BCC2 ^= buf[i];
    }

    // Prepare data + BCC2 for stuffing
    unsigned char dataWithBCC2[MAX_PAYLOAD_SIZE + 1];
    memcpy(dataWithBCC2, buf, bufSize);
    dataWithBCC2[bufSize] = BCC2;

    // Apply byte stuffing to data + BCC2
    unsigned char stuffedData[MAX_PAYLOAD_SIZE * 2 + 2]; // Worst case: every byte needs stuffing
    int stuffedLen = stuffBytes(dataWithBCC2, bufSize + 1, stuffedData);

    // Build complete frame: FLAG | A | C | BCC1 | STUFFED_DATA | FLAG
    unsigned char frame[MAX_PAYLOAD_SIZE * 2 + 10];
    int frameLen = 0;
    frame[frameLen++] = FLAG;
    frame[frameLen++] = A;
    frame[frameLen++] = C;
    frame[frameLen++] = BCC1;
    memcpy(frame + frameLen, stuffedData, stuffedLen);
    frameLen += stuffedLen;
    frame[frameLen++] = FLAG;

    // Setup alarm handler
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    int retries = 0;
    int frameAccepted = FALSE;

    printf("Transmitter: Sending I frame with Ns=%d\n", Ns);

    while (retries < connectionParams.nRetransmissions && !frameAccepted)
    {
        retries++;

        // Send I frame
        int bytes = writeBytesSerialPort(frame, frameLen);
        if (bytes < 0)
        {
            perror("Error sending I frame");
            return -1;
        }
        printf("I frame sent (%d bytes, attempt #%d)\n", bytes, retries);

        alarmEnabled = TRUE;
        alarm(connectionParams.timeout);

        // Wait for RR or REJ
        while (alarmEnabled && !frameAccepted)
        {
            unsigned char controlField;
            int result = receiveSupervisionFrame(&controlField);

            if (result == 1) // RR received
            {
                // Extract sequence number from RR
                unsigned char rrNr = (controlField == 0x85) ? 1 : 0;
                
                // Check if it's acknowledging our frame
                if (rrNr == ((Ns + 1) % 2))
                {
                    printf("RR%d received - Frame acknowledged\n", rrNr);
                    frameAccepted = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                    
                    // Toggle sequence number for next frame
                    Ns = (Ns + 1) % 2;
                }
                else
                {
                    printf("RR%d received but expected RR%d - ignoring\n", 
                           rrNr, (Ns + 1) % 2);
                }
            }
            else if (result == 2) // REJ received
            {
                unsigned char rejNr = (controlField == 0x81) ? 1 : 0;
                printf("REJ%d received - Retransmitting frame\n", rejNr);
                alarm(0);
                alarmEnabled = FALSE;
                break; // Exit inner loop to retransmit
            }
        }

        if (!frameAccepted && !alarmEnabled)
        {
            printf("Timeout #%d - retransmitting I frame...\n", retries);
        }
    }

    if (!frameAccepted)
    {
        printf("Failed to send I frame after %d attempts\n", connectionParams.nRetransmissions);
        return -1;
    }

    return bufSize;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet)
{
    const unsigned char FLAG = 0x7E;
    const unsigned char A = 0x03; // Transmitter sends with A=0x03
    
    int state = 0;
    unsigned char receivedA = 0;
    unsigned char receivedC = 0;
    unsigned char receivedBCC1 = 0;
    unsigned char receivedNs = 0;
    
    // Buffer for stuffed data (including BCC2)
    unsigned char stuffedData[MAX_PAYLOAD_SIZE * 2 + 2];
    int stuffedDataLen = 0;
    
    printf("Receiver: Waiting for I frame...\n");
    
    // State machine to receive I frame
    while (TRUE)
    {
        unsigned char byte;
        int res = readByteSerialPort(&byte);
        
        if (res < 0)
        {
            perror("Error reading from serial port");
            return -1;
        }
        if (res == 0)
        {
            continue; // No data available
        }
        
        switch (state)
        {
        case 0: // Wait for FLAG
            if (byte == FLAG)
            {
                state = 1;
                stuffedDataLen = 0;
            }
            break;
            
        case 1: // Wait for Address
            if (byte == A)
            {
                receivedA = byte;
                state = 2;
            }
            else if (byte == FLAG)
            {
                state = 1; // Stay in FLAG state
            }
            else
            {
                state = 0;
            }
            break;
            
        case 2: // Wait for Control
            if (byte == 0x00 || byte == 0x80) // Valid control fields
            {
                receivedC = byte;
                receivedNs = (byte == 0x80) ? 1 : 0;
                state = 3;
            }
            else if (byte == FLAG)
            {
                state = 1;
            }
            else
            {
                state = 0;
            }
            break;
            
        case 3: // Wait for BCC1
            receivedBCC1 = byte;
            if (byte == (receivedA ^ receivedC))
            {
                state = 4; // BCC1 valid, move to data
            }
            else if (byte == FLAG)
            {
                state = 1;
            }
            else
            {
                printf("BCC1 error: expected 0x%02X, got 0x%02X\n", 
                       receivedA ^ receivedC, byte);
                state = 0; // Invalid BCC1, restart
            }
            break;
            
        case 4: // Read data (stuffed)
            if (byte == FLAG)
            {
                // End of frame detected
                printf("I frame received (Ns=%d, stuffed data length=%d)\n", 
                       receivedNs, stuffedDataLen);
                
                // Destuff data
                unsigned char destuffedData[MAX_PAYLOAD_SIZE + 1];
                int destuffedLen = destuffBytes(stuffedData, stuffedDataLen, destuffedData);
                
                if (destuffedLen < 2) // Need at least 1 byte data + BCC2
                {
                    printf("Error: Destuffing failed or data too short\n");
                    sendSupervisionFrame((expectedNs == 0) ? 0x01 : 0x81); // Send REJ
                    state = 0;
                    break;
                }
                
                // Extract BCC2 (last byte)
                unsigned char receivedBCC2 = destuffedData[destuffedLen - 1];
                int dataLen = destuffedLen - 1;
                
                // Calculate expected BCC2
                unsigned char calculatedBCC2 = 0x00;
                for (int i = 0; i < dataLen; i++)
                {
                    calculatedBCC2 ^= destuffedData[i];
                }
                
                // Validate BCC2
                if (receivedBCC2 != calculatedBCC2)
                {
                    printf("BCC2 error: expected 0x%02X, got 0x%02X\n", 
                           calculatedBCC2, receivedBCC2);
                    sendSupervisionFrame((expectedNs == 0) ? 0x01 : 0x81); // Send REJ
                    state = 0;
                    break;
                }
                
                // Check sequence number
                if (receivedNs != expectedNs)
                {
                    printf("Duplicate frame (Ns=%d, expected=%d) - sending RR anyway\n", 
                           receivedNs, expectedNs);
                    // Send RR for next expected frame
                    sendSupervisionFrame((expectedNs == 0) ? 0x85 : 0x05);
                    state = 0;
                    break;
                }
                
                // Frame is valid and new
                printf("Frame valid - sending RR%d\n", (expectedNs + 1) % 2);
                
                // Copy data to output packet
                memcpy(packet, destuffedData, dataLen);
                
                // Send RR acknowledging this frame
                sendSupervisionFrame(((expectedNs + 1) % 2 == 1) ? 0x85 : 0x05);
                
                // Toggle expected sequence number
                expectedNs = (expectedNs + 1) % 2;
                
                return dataLen;
            }
            else
            {
                // Accumulate data bytes
                if (stuffedDataLen < MAX_PAYLOAD_SIZE * 2 + 2)
                {
                    stuffedData[stuffedDataLen++] = byte;
                }
                else
                {
                    printf("Error: Data buffer overflow\n");
                    sendSupervisionFrame((expectedNs == 0) ? 0x01 : 0x81); // Send REJ
                    state = 0;
                }
            }
            break;
            
        default:
            state = 0;
            break;
        }
    }
    
    return -1;
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