# llclose() Implementation Guide

## Overview
The `llclose()` function implements the disconnection protocol for the data link layer, handling different sequences for Transmitter (Tx) and Receiver (Rx) roles.

## Protocol Sequences

### Transmitter (Tx) Sequence
1. **Send DISC frame** (A = 0x03, C = 0x0B)
2. **Wait for DISC response** from Receiver (A = 0x01, C = 0x0B)
   - Implements retransmission mechanism with timeout
   - Maximum retries: configured in `connectionParams.nRetransmissions`
3. **Send UA frame** (A = 0x01, C = 0x07)
4. **Close serial port**

### Receiver (Rx) Sequence
1. **Wait for DISC frame** from Transmitter (A = 0x03, C = 0x0B)
   - Timeout: configured in `connectionParams.timeout`
2. **Send DISC response** (A = 0x01, C = 0x0B)
3. **Wait for UA frame** from Transmitter (A = 0x03, C = 0x07)
   - Uses state machine for frame reception
4. **Close serial port**

## Frame Formats

### DISC Frame (Transmitter → Receiver)
```
FLAG | A=0x03 | C=0x0B | BCC=0x08 | FLAG
0x7E | 0x03   | 0x0B   | 0x08     | 0x7E
```

### DISC Frame (Receiver → Transmitter)
```
FLAG | A=0x01 | C=0x0B | BCC=0x0A | FLAG
0x7E | 0x01   | 0x0B   | 0x0A     | 0x7E
```

### UA Frame (Transmitter → Receiver after receiving DISC)
```
FLAG | A=0x01 | C=0x07 | BCC=0x06 | FLAG
0x7E | 0x01   | 0x07   | 0x06     | 0x7E
```

### UA Frame (different context - if needed)
```
FLAG | A=0x03 | C=0x07 | BCC=0x04 | FLAG
0x7E | 0x03   | 0x07   | 0x04     | 0x7E
```

## Implementation Details

### Main Functions

#### `int llclose()`
- Entry point for disconnect sequence
- Determines role (Tx or Rx) from stored connection parameters
- Calls appropriate helper function
- Closes serial port
- Returns 0 on success, -1 on error

#### `int txDisconnect(int timeoutSeconds, int maxRetries)`
- Implements Transmitter disconnect sequence
- Features:
  - Sends DISC frame with retransmission support
  - Uses alarm for timeout handling
  - Waits for DISC response using `receiveDISC(0x01)`
  - Sends UA frame upon successful receipt
- Returns 0 on success, -1 on error

#### `int rxDisconnect(int timeoutSeconds)`
- Implements Receiver disconnect sequence
- Features:
  - Waits for DISC frame using `receiveDISC(0x03)`
  - Sends DISC response
  - Waits for UA frame using state machine
  - Timeout protection for both receive operations
- Returns 0 on success, -1 on error

#### `int receiveDISC(unsigned char expectedA)`
- State machine to receive DISC frames
- Parameter `expectedA`: expected address field (0x01 or 0x03)
- Returns TRUE when complete frame received, FALSE otherwise
- Handles byte-by-byte reception with proper state transitions

### Key Implementation Features

1. **Stored Connection Parameters**
   - Global `connectionParams` variable stores configuration from `llopen()`
   - Used in `llclose()` to determine role, timeout, and retry count

2. **Alarm-Based Timeout**
   - Uses `sigaction()` to configure alarm handler
   - `alarmEnabled` flag tracks timeout state
   - Automatic retransmission on timeout (Tx only)

3. **State Machine Frame Reception**
   - States: 0 (FLAG) → 1 (Address) → 2 (Control) → 3 (BCC) → 4 (Final FLAG)
   - Proper FLAG handling: restart on unexpected FLAG
   - BCC validation: A XOR C

4. **Error Handling**
   - Check return values from serial port operations
   - Timeout handling with retry limit
   - Proper cleanup on error (closes serial port)

## Usage Example

```c
// In application layer
LinkLayer connectionParameters;
connectionParameters.role = LlTx; // or LlRx
connectionParameters.timeout = 3;
connectionParameters.nRetransmissions = 3;
// ... set other parameters

// Open connection
if (llopen(connectionParameters) < 0) {
    // Handle error
}

// ... perform data transfer (llwrite/llread) ...

// Close connection
if (llclose() < 0) {
    // Handle error
}
```

## Testing Recommendations

1. **Test with both roles**
   - Run Tx and Rx in separate processes/terminals
   - Verify correct frame exchange sequence

2. **Test timeout scenarios**
   - Disconnect one side during handshake
   - Verify retransmission works (Tx side)
   - Verify timeout handling (Rx side)

3. **Monitor frame exchange**
   - Use printf statements to trace execution
   - Verify frame contents (can use serial port sniffer)

4. **Error conditions**
   - Test with invalid frames
   - Test with corrupted BCC
   - Test maximum retry exhaustion

## Common Issues & Solutions

### Issue: DISC not received
- **Solution**: Check cable connection, verify serial port settings
- Increase timeout value
- Check that both programs use same serial port

### Issue: Retransmission not working
- **Solution**: Verify alarm handler is properly configured
- Check that `alarmEnabled` flag is being set correctly
- Ensure timeout value is reasonable (not too short)

### Issue: State machine stuck
- **Solution**: Add debug prints in state machine
- Verify FLAG byte (0x7E) is being sent/received correctly
- Check BCC calculation

## Return Values

- **0**: Success - connection closed properly
- **-1**: Error - timeout, invalid frame, or serial port error

## Notes

- The `llclose()` function uses the connection parameters stored during `llopen()`
- The implementation reuses patterns from `sendSetAndWaitForUa()` and `waitForSetsendUA()`
- Both Tx and Rx must complete their sequences for proper disconnection
- Serial port is closed regardless of disconnect sequence success (best effort cleanup)
