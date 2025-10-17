/*
 * Test Program for llwrite() and llread()
 * ========================================
 * 
 * This file provides example usage and test scenarios
 * for the data link layer functions.
 */

#include "link_layer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Test data samples
const char* test_messages[] = {
    "Hello",
    "Test message with special bytes: \x7E\x7D",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
    "Short",
    "A",
    "",  // Empty (edge case)
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <role> <serial_port>\n", argv[0]);
        printf("  role: tx or rx\n");
        printf("  serial_port: e.g., /dev/ttyS0 or /dev/pts/3\n");
        return 1;
    }

    // Parse role
    LinkLayerRole role;
    if (strcmp(argv[1], "tx") == 0) {
        role = LlTx;
    } else if (strcmp(argv[1], "rx") == 0) {
        role = LlRx;
    } else {
        printf("Error: Invalid role '%s'. Use 'tx' or 'rx'\n", argv[1]);
        return 1;
    }

    // Configure connection parameters
    LinkLayer params;
    strcpy(params.serialPort, argv[2]);
    params.role = role;
    params.baudRate = 38400;
    params.nRetransmissions = 3;
    params.timeout = 3;

    // Open connection
    printf("Opening connection on %s as %s...\n", 
           params.serialPort, role == LlTx ? "Transmitter" : "Receiver");
    
    if (llopen(params) < 0) {
        fprintf(stderr, "Failed to open connection\n");
        return 1;
    }

    printf("Connection opened successfully!\n\n");

    // Transmitter mode
    if (role == LlTx) {
        printf("=== TRANSMITTER MODE ===\n\n");
        
        int num_tests = sizeof(test_messages) / sizeof(test_messages[0]) - 1; // Skip empty
        
        for (int i = 0; i < num_tests; i++) {
            const char *msg = test_messages[i];
            int msg_len = strlen(msg);
            
            printf("Test %d: Sending \"%s\" (%d bytes)\n", i + 1, msg, msg_len);
            printf("────────────────────────────────────────────────\n");
            
            int result = llwrite((unsigned char*)msg, msg_len);
            
            if (result < 0) {
                fprintf(stderr, "✗ Failed to send message %d\n\n", i + 1);
                continue;
            }
            
            printf("✓ Successfully sent %d bytes\n\n", result);
            
            // Small delay between messages
            sleep(1);
        }
        
        printf("All tests completed!\n");
    }
    // Receiver mode
    else {
        printf("=== RECEIVER MODE ===\n\n");
        
        int num_tests = sizeof(test_messages) / sizeof(test_messages[0]) - 1;
        
        for (int i = 0; i < num_tests; i++) {
            printf("Test %d: Waiting for message...\n", i + 1);
            printf("────────────────────────────────────────────────\n");
            
            unsigned char buffer[MAX_PAYLOAD_SIZE];
            int bytes_read = llread(buffer);
            
            if (bytes_read < 0) {
                fprintf(stderr, "✗ Failed to receive message %d\n\n", i + 1);
                continue;
            }
            
            // Null-terminate for printing
            buffer[bytes_read] = '\0';
            
            printf("✓ Received %d bytes: \"%s\"\n", bytes_read, buffer);
            
            // Print hex dump
            printf("  Hex: ");
            for (int j = 0; j < bytes_read; j++) {
                printf("%02X ", buffer[j]);
            }
            printf("\n\n");
        }
        
        printf("All tests completed!\n");
    }

    // Close connection
    printf("\nClosing connection...\n");
    if (llclose() < 0) {
        fprintf(stderr, "Failed to close connection properly\n");
        return 1;
    }

    printf("Connection closed successfully!\n");
    return 0;
}

/*
 * COMPILATION
 * ===========
 * 
 * gcc -Wall -o test_link test_link.c link_layer.c serial_port.c -I.
 * 
 * 
 * TESTING PROCEDURE
 * =================
 * 
 * 1. Create virtual serial ports (Linux/WSL):
 *    socat -d -d pty,raw,echo=0 pty,raw,echo=0
 *    
 *    This creates two connected ports, e.g.:
 *    /dev/pts/3 <-> /dev/pts/4
 * 
 * 2. Terminal 1 (Receiver):
 *    ./test_link rx /dev/pts/3
 * 
 * 3. Terminal 2 (Transmitter):
 *    ./test_link tx /dev/pts/4
 * 
 * 
 * EXPECTED OUTPUT
 * ===============
 * 
 * Transmitter:
 *   Test 1: Sending "Hello" (5 bytes)
 *   ────────────────────────────────────────────────
 *   Transmitter: Sending I frame with Ns=0
 *   I frame sent (13 bytes, attempt #1)
 *   RR1 received - Frame acknowledged
 *   ✓ Successfully sent 5 bytes
 *   
 *   Test 2: Sending "Test message..." (36 bytes)
 *   ...
 * 
 * Receiver:
 *   Test 1: Waiting for message...
 *   ────────────────────────────────────────────────
 *   Receiver: Waiting for I frame...
 *   I frame received (Ns=0, stuffed data length=6)
 *   Frame valid - sending RR1
 *   ✓ Received 5 bytes: "Hello"
 *     Hex: 48 65 6C 6C 6F 
 *   
 *   Test 2: Waiting for message...
 *   ...
 * 
 * 
 * TROUBLESHOOTING
 * ===============
 * 
 * Problem: "Failed to open connection"
 * Solution: 
 *   - Check serial port exists: ls -l /dev/pts/*
 *   - Check permissions: sudo chmod 666 /dev/pts/X
 *   - Verify socat is running
 * 
 * Problem: "Timeout" messages
 * Solution:
 *   - Make sure both programs are running
 *   - Check correct port assignment (swapped?)
 *   - Increase timeout in params.timeout
 * 
 * Problem: "BCC2 error"
 * Solution:
 *   - Check byte stuffing implementation
 *   - Verify BCC2 calculation includes all data
 *   - Test with simple data first (no special bytes)
 * 
 * Problem: Receiver gets duplicate messages
 * Solution:
 *   - Check expectedNs is toggling properly
 *   - Verify duplicate detection logic in llread()
 *   - Ensure RR frames have correct sequence number
 * 
 * 
 * ADVANCED TESTS
 * ==============
 * 
 * 1. Large packet test:
 *    Modify to send MAX_PAYLOAD_SIZE bytes
 * 
 * 2. Special bytes test:
 *    Send data with many 0x7E and 0x7D bytes
 * 
 * 3. Error injection:
 *    Modify llwrite to corrupt some frames
 *    Verify REJ and retransmission work
 * 
 * 4. Stress test:
 *    Send 1000 frames in a loop
 *    Measure success rate and performance
 */
