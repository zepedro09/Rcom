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

// Forward declarations of auxiliary functions
void alarmHandler(int signal);
int setupAlarmHandler();
int receiveSupervisionFrame(unsigned char *controlField);
int sendSupervisionFrame(unsigned char controlField);
int receiveFrame(unsigned char expectedA, unsigned char expectedC);
int sendFrame(unsigned char A, unsigned char C);
int stuffBytes(const unsigned char *input, int len, unsigned char *output);
int destuffBytes(const unsigned char *input, int len, unsigned char *output);


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

    // Setup alarm handler once
    if (setupAlarmHandler() == -1)
        return -1;

    if (connectionParameters.role == LlTx) {
        // Transmitter: Send SET and wait for UA
        const unsigned char SET_C = 0x03;
        const unsigned char UA_A = 0x01;
        const unsigned char UA_C = 0x07;
        
        int retries = 0;
        int success = FALSE;
        
        printf("Transmitter: Establishing connection...\n");
        
        while (retries < connectionParameters.nRetransmissions && !success) {
            retries++;
            
            // Send SET frame
            if (sendFrame(0x03, SET_C) == -1) {
                return -1;
            }
            printf("SET frame sent (attempt #%d)\n", retries);
            
            alarmEnabled = TRUE;
            alarm(connectionParameters.timeout);
            
            // Wait for UA response
            while (alarmEnabled && !success) {
                if (receiveFrame(UA_A, UA_C) == TRUE) {
                    success = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                    printf("UA frame received - Connection established!\n");
                }
            }
            
            if (!success && !alarmEnabled) {
                printf("Timeout #%d - retransmitting SET frame...\n", retries);
            }
        }
        
        if (!success) {
            printf("Failed to establish connection after %d attempts\n", connectionParameters.nRetransmissions);
            return -1;
        }
        
    } else if (connectionParameters.role == LlRx) {
        // Receiver: Wait for SET and send UA
        const unsigned char SET_A = 0x03;
        const unsigned char SET_C = 0x03;
        const unsigned char UA_C = 0x07;
        
        printf("Receiver: Waiting for SET frame...\n");
        
        alarmEnabled = TRUE;
        alarm(connectionParameters.timeout);
        
        int setReceived = FALSE;
        while (alarmEnabled && !setReceived) {
            if (receiveFrame(SET_A, SET_C) == TRUE) {
                setReceived = TRUE;
                alarm(0);
                alarmEnabled = FALSE;
                
                // Send UA response
                if (sendFrame(0x01, UA_C) == -1) {
                    return -1;
                }
                printf("SET received, UA sent - Connection established!\n");
            }
        }
        
        if (!setReceived) {
            printf("Timeout: Failed to receive SET frame\n");
            return -1;
        }
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
    const unsigned char DISC_C = 0x0B;
    const unsigned char UA_C = 0x07;
    
    int result = -1;

    if (connectionParams.role == LlTx)
    {
        // Transmitter: Send DISC, wait for DISC, send UA
        printf("Transmitter: Closing connection...\n");
        
        int retries = 0;
        int discReceived = FALSE;
        
        while (retries < connectionParams.nRetransmissions && !discReceived)
        {
            retries++;
            
            // Send DISC frame (A=0x03, C=0x0B)
            if (sendFrame(0x03, DISC_C) == -1) {
                closeSerialPort();
                return -1;
            }
            printf("DISC frame sent (attempt #%d)\n", retries);
            
            alarmEnabled = TRUE;
            alarm(connectionParams.timeout);
            
            // Wait for DISC response (A=0x01, C=0x0B)
            while (alarmEnabled && !discReceived)
            {
                if (receiveFrame(0x01, DISC_C) == TRUE) {
                    discReceived = TRUE;
                    alarm(0);
                    alarmEnabled = FALSE;
                    printf("DISC response received!\n");
                }
            }
            
            if (!discReceived && !alarmEnabled) {
                printf("Timeout #%d - retransmitting DISC frame...\n", retries);
            }
        }
        
        if (!discReceived) {
            printf("Failed to receive DISC response after %d attempts\n", connectionParams.nRetransmissions);
            closeSerialPort();
            return -1;
        }
        
        // Send UA frame (A=0x01, C=0x07)
        if (sendFrame(0x01, UA_C) == -1) {
            closeSerialPort();
            return -1;
        }
        printf("UA frame sent\n");
        result = 0;
    }
    else if (connectionParams.role == LlRx)
    {
        // Receiver: Wait for DISC, send DISC, wait for UA
        printf("Receiver: Waiting for DISC frame...\n");
        
        alarmEnabled = TRUE;
        alarm(connectionParams.timeout);
        
        int discReceived = FALSE;
        // Wait for DISC from Transmitter (A=0x03, C=0x0B)
        while (alarmEnabled && !discReceived)
        {
            if (receiveFrame(0x03, DISC_C) == TRUE) {
                discReceived = TRUE;
                alarm(0);
                alarmEnabled = FALSE;
                printf("DISC frame received!\n");
            }
        }

        if (!discReceived)
        {
            printf("Timeout: Failed to receive DISC frame from Transmitter\n");
            closeSerialPort();
            return -1;
        }

        // Send DISC response (A=0x01, C=0x0B)
        if (sendFrame(0x01, DISC_C) == -1) {
            closeSerialPort();
            return -1;
        }
        printf("DISC response sent\n");

        // Wait for UA from Transmitter (A=0x03, C=0x07)
        printf("Receiver: Waiting for UA frame...\n");
        
        alarmEnabled = TRUE;
        alarm(connectionParams.timeout);
        
        int uaReceived = FALSE;
        while (alarmEnabled && !uaReceived)
        {
            if (receiveFrame(0x03, UA_C) == TRUE) {
                uaReceived = TRUE;
                alarm(0);
                alarmEnabled = FALSE;
                printf("UA frame received!\n");
            }
        }

        if (!uaReceived)
        {
            printf("Timeout: Failed to receive UA frame from Transmitter\n");
            closeSerialPort();
            return -1;
        }
        
        result = 0;
    }
    else
    {
        printf("Error: Invalid role\n");
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
    return result;
}

////////////////////////////////////////////////
// AUXILIARY FUNCTIONS
////////////////////////////////////////////////

// Alarm handler - called when alarm() triggers
void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarm #%d triggered\n", alarmCount);
}

// Setup alarm signal handler (called once in llopen)
int setupAlarmHandler()
{
    struct sigaction sa;
    sa.sa_handler = alarmHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        perror("sigaction");
        return -1;
    }
    
    return 0;
}

// Generic function to receive a supervision frame (SET, UA, DISC)
// Returns TRUE if correct frame received, FALSE otherwise
int receiveFrame(unsigned char expectedA, unsigned char expectedC)
{
    unsigned char byte;
    int state = 0;
    unsigned char receivedA = 0, receivedC = 0, receivedBCC1 = 0;

    while (1) {
        if (read(fd, &byte, 1) <= 0) {
            return FALSE;
        }

        switch (state) {
        case 0: // Waiting for FLAG
            if (byte == FLAG) state = 1;
            break;
        case 1: // Waiting for A
            if (byte == expectedA) state = 2;
            else if (byte != FLAG) state = 0;
            break;
        case 2: // Waiting for C
            if (byte == expectedC) state = 3;
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 3: // Waiting for BCC1
            if (byte == (expectedA ^ expectedC)) state = 4;
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 4: // Waiting for closing FLAG
            if (byte == FLAG) {
                return TRUE;
            } else state = 0;
            break;
        default:
            state = 0;
            break;
        }
    }
}

// Generic function to send a supervision frame (SET, UA, DISC)
// Returns 0 on success, -1 on error
int sendFrame(unsigned char A, unsigned char C)
{
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = A;
    frame[2] = C;
    frame[3] = A ^ C; // BCC1
    frame[4] = FLAG;

    int bytesWritten = write(fd, frame, 5);
    if (bytesWritten != 5) {
        perror("Error sending frame");
        return -1;
    }

    return 0;
}
