# llwrite() and llread() Implementation Guide

## Overview
Implementation of Stop & Wait ARQ protocol with byte stuffing for reliable data transmission over a serial link.

---

## Frame Formats

### Information (I) Frame Structure
```
+------+-----+-----+------+-------------+------+------+
| FLAG |  A  |  C  | BCC1 | STUFFED_DATA| BCC2 | FLAG |
+------+-----+-----+------+-------------+------+------+
| 0x7E | 0x03| Ns  | A^C  |   DATA      | XOR  | 0x7E |
+------+-----+-----+------+-------------+------+------+
```

**Field Descriptions:**
- **FLAG**: Frame delimiter (0x7E)
- **A**: Address field (0x03 for Tx → Rx)
- **C**: Control field
  - `0x00` for Ns=0
  - `0x80` for Ns=1
- **BCC1**: Header checksum (A XOR C)
- **DATA**: Application payload (after byte stuffing)
- **BCC2**: Data checksum (XOR of all data bytes before stuffing)
- **FLAG**: Frame delimiter (0x7E)

### Supervision Frame Structure (RR/REJ)
```
+------+-----+-----+------+------+
| FLAG |  A  |  C  | BCC1 | FLAG |
+------+-----+-----+------+------+
| 0x7E | 0x01| RR/REJ| A^C| 0x7E |
+------+-----+-----+------+------+
```

**Control Field Values:**
- **RR0** = `0x05` (Ready to receive frame 0)
- **RR1** = `0x85` (Ready to receive frame 1)
- **REJ0** = `0x01` (Reject frame 0, request retransmission)
- **REJ1** = `0x81` (Reject frame 1, request retransmission)

---

## Byte Stuffing Mechanism

### Purpose
Prevent confusion between data bytes and FLAG bytes (0x7E) inside the frame.

### Stuffing Rules
| Original Byte | Stuffed Sequence | Reason |
|--------------|------------------|---------|
| `0x7E` | `0x7D 0x5E` | FLAG byte in data |
| `0x7D` | `0x7D 0x5D` | Escape byte in data |

### Destuffing Rules
| Stuffed Sequence | Original Byte |
|-----------------|---------------|
| `0x7D 0x5E` | `0x7E` |
| `0x7D 0x5D` | `0x7D` |

### Example
**Original data:** `[0x01, 0x7E, 0x03, 0x7D, 0x05]`  
**After stuffing:** `[0x01, 0x7D, 0x5E, 0x03, 0x7D, 0x5D, 0x05]`

---

## llwrite() — Transmitter Side

### Function Signature
```c
int llwrite(const unsigned char *buf, int bufSize);
```

### Parameters
- `buf`: Pointer to data buffer to send
- `bufSize`: Number of bytes to send (must be ≤ MAX_PAYLOAD_SIZE)

### Return Values
- **Success**: `bufSize` (number of bytes sent)
- **Error**: `-1`

### Algorithm Steps

1. **Validate Input**
   - Check `bufSize` is valid (0 < bufSize ≤ MAX_PAYLOAD_SIZE)

2. **Build Frame Header**
   ```c
   A = 0x03
   C = (Ns == 0) ? 0x00 : 0x80
   BCC1 = A ^ C
   ```

3. **Calculate BCC2**
   ```c
   BCC2 = buf[0] ^ buf[1] ^ ... ^ buf[bufSize-1]
   ```

4. **Apply Byte Stuffing**
   - Stuff DATA + BCC2 together
   - Use `stuffBytes()` helper function

5. **Assemble Complete Frame**
   ```
   [FLAG][A][C][BCC1][STUFFED_DATA][FLAG]
   ```

6. **Transmission Loop** (with retries)
   - Send frame via `writeBytesSerialPort()`
   - Start alarm timer
   - Wait for RR or REJ response
   
7. **Process Response**
   - **RR received**: Check sequence number matches, toggle Ns, return success
   - **REJ received**: Retransmit same frame
   - **Timeout**: Retransmit (up to `nRetransmissions`)

8. **Return Result**
   - Success: return `bufSize`
   - Failure after max retries: return `-1`

### State Variables
- `Ns`: Current sequence number (0 or 1, toggles after successful transmission)

### Key Features
- ✅ Automatic retransmission on timeout
- ✅ Handles REJ (explicit negative acknowledgment)
- ✅ Byte stuffing for transparency
- ✅ BCC2 for data integrity

---

## llread() — Receiver Side

### Function Signature
```c
int llread(unsigned char *packet);
```

### Parameters
- `packet`: Buffer to store received data (after destuffing)

### Return Values
- **Success**: Number of bytes read
- **Error**: `-1`

### Algorithm Steps

1. **Frame Reception (State Machine)**
   ```
   State 0: Wait for FLAG (0x7E)
   State 1: Read Address (expect 0x03)
   State 2: Read Control (0x00 or 0x80)
   State 3: Read BCC1 (validate A ^ C)
   State 4: Read stuffed data until final FLAG
   ```

2. **Header Validation**
   - Verify `A == 0x03`
   - Extract `Ns` from control field
   - Check `BCC1 == A ^ C`
   - If invalid: discard frame, wait for next

3. **Data Destuffing**
   - Apply `destuffBytes()` to received data
   - Extract BCC2 (last byte after destuffing)

4. **BCC2 Validation**
   ```c
   calculated_BCC2 = data[0] ^ data[1] ^ ... ^ data[n-1]
   if (received_BCC2 != calculated_BCC2):
       send REJ(expectedNs)
       return error
   ```

5. **Duplicate Detection**
   ```c
   if (receivedNs != expectedNs):
       // Duplicate frame
       send RR(expectedNs + 1)  // Acknowledge again
       discard data
       continue waiting
   ```

6. **Send Acknowledgment**
   - **Success**: Send `RR(expectedNs + 1 mod 2)`
   - **Error**: Send `REJ(expectedNs)`

7. **Update State**
   - Toggle `expectedNs`
   - Copy data to output buffer

8. **Return**
   - Return number of data bytes (excluding BCC2)

### State Variables
- `expectedNs`: Expected sequence number (0 or 1, toggles after successful reception)

### Key Features
- ✅ State machine for reliable frame reception
- ✅ Duplicate frame detection
- ✅ BCC1 and BCC2 validation
- ✅ Automatic REJ on errors
- ✅ Byte destuffing

---

## Stop & Wait ARQ Protocol

### Normal Operation Flow

```
Transmitter (Tx)              Receiver (Rx)
================              =============

Send I(Ns=0) ────────────────>
                               Validate frame
                               Extract data
                               Send RR1
             <──────────────── RR1
Receive RR1
Toggle Ns to 1

Send I(Ns=1) ────────────────>
                               Validate frame
                               Extract data
                               Send RR0
             <──────────────── RR0
Receive RR0
Toggle Ns to 0

(Repeat...)
```

### Error Scenarios

#### Scenario 1: Frame Lost (Timeout)
```
Tx: Send I(Ns=0) ─────X
                    (lost)
Tx: Timeout!
Tx: Retransmit I(Ns=0) ──────>
                               Rx: Receive I(Ns=0)
             <──────────────── Rx: Send RR1
Tx: Success!
```

#### Scenario 2: Corrupted Frame (BCC Error)
```
Tx: Send I(Ns=0) ────────────>
                               Rx: BCC2 error detected!
             <──────────────── Rx: Send REJ0
Tx: Receive REJ0
Tx: Retransmit I(Ns=0) ──────>
                               Rx: Frame OK
             <──────────────── Rx: Send RR1
```

#### Scenario 3: Duplicate Frame
```
Tx: Send I(Ns=0) ────────────>
                               Rx: Frame OK, send RR1
             <─────X────────── RR1 lost
Tx: Timeout!
Tx: Retransmit I(Ns=0) ──────>
                               Rx: Ns=0 but expected 1
                               Rx: Duplicate detected!
             <──────────────── Rx: Send RR1 again
                               Rx: Discard duplicate data
```

---

## Helper Functions

### stuffBytes()
```c
int stuffBytes(const unsigned char *input, int len, unsigned char *output);
```
**Purpose**: Apply byte stuffing to data  
**Returns**: Length of stuffed data  

**Algorithm**:
```
for each byte in input:
    if byte == 0x7E:
        output[0x7D, 0x5E]
    else if byte == 0x7D:
        output[0x7D, 0x5D]
    else:
        output[byte]
```

### destuffBytes()
```c
int destuffBytes(const unsigned char *input, int len, unsigned char *output);
```
**Purpose**: Reverse byte stuffing  
**Returns**: Length of destuffed data, or -1 on error  

**Algorithm**:
```
for each byte in input:
    if byte == 0x7D:
        next_byte = input[++i]
        if next_byte == 0x5E:
            output[0x7E]
        else if next_byte == 0x5D:
            output[0x7D]
        else:
            error (invalid escape sequence)
    else:
        output[byte]
```

### receiveSupervisionFrame()
```c
int receiveSupervisionFrame(unsigned char *controlField);
```
**Purpose**: Receive and validate RR or REJ frames  
**Returns**: 
- `1` for RR
- `2` for REJ
- `0` for incomplete
- `-1` for error

### sendSupervisionFrame()
```c
int sendSupervisionFrame(unsigned char controlField);
```
**Purpose**: Send RR or REJ frame  
**Parameters**: Control field (0x05, 0x85, 0x01, or 0x81)  
**Returns**: `0` on success, `-1` on error

---

## Usage Example

### Application Layer Code

```c
LinkLayer params;
params.role = LlTx; // or LlRx
params.timeout = 3;
params.nRetransmissions = 3;
// ... other parameters

// Open connection
if (llopen(params) < 0) {
    fprintf(stderr, "Failed to open connection\n");
    exit(1);
}

// Transmitter side
if (params.role == LlTx) {
    unsigned char data[] = "Hello, World!";
    int bytes_written = llwrite(data, strlen(data));
    if (bytes_written < 0) {
        fprintf(stderr, "Failed to send data\n");
    } else {
        printf("Sent %d bytes\n", bytes_written);
    }
}

// Receiver side
if (params.role == LlRx) {
    unsigned char buffer[MAX_PAYLOAD_SIZE];
    int bytes_read = llread(buffer);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("Received: %s\n", buffer);
    }
}

// Close connection
llclose();
```

---

## Testing Recommendations

### Unit Tests

1. **Byte Stuffing Tests**
   ```c
   // Test 1: No special bytes
   input: [0x01, 0x02, 0x03]
   expected: [0x01, 0x02, 0x03]
   
   // Test 2: FLAG byte
   input: [0x01, 0x7E, 0x03]
   expected: [0x01, 0x7D, 0x5E, 0x03]
   
   // Test 3: Escape byte
   input: [0x01, 0x7D, 0x03]
   expected: [0x01, 0x7D, 0x5D, 0x03]
   
   // Test 4: Both special bytes
   input: [0x7E, 0x7D]
   expected: [0x7D, 0x5E, 0x7D, 0x5D]
   ```

2. **BCC Calculation Tests**
   ```c
   // Test XOR operation
   data: [0x01, 0x02, 0x03, 0x04]
   BCC2 = 0x01 ^ 0x02 ^ 0x03 ^ 0x04 = 0x04
   ```

### Integration Tests

1. **Normal Transmission**
   - Send small packet (< 10 bytes)
   - Verify received correctly
   - Check sequence numbers toggle

2. **Large Packet**
   - Send MAX_PAYLOAD_SIZE bytes
   - Verify no data loss
   - Check byte stuffing works

3. **Timeout Scenario**
   - Disconnect receiver during transmission
   - Verify retransmission occurs
   - Check max retry limit

4. **Error Injection**
   - Corrupt BCC2 in frame
   - Verify REJ is sent
   - Check retransmission works

5. **Duplicate Frame**
   - Simulate lost RR response
   - Verify duplicate detection
   - Check data not duplicated

---

## Performance Considerations

### Throughput Calculation
```
Effective_Throughput = Data_Size / (Transmission_Time + RTT + Processing_Time)

For Stop & Wait:
Utilization = (Data_Frame_Time) / (Data_Frame_Time + RTT)
```

### Optimization Tips
1. **Reduce timeout value** (but not too small to cause false timeouts)
2. **Use larger packets** (up to MAX_PAYLOAD_SIZE)
3. **Minimize processing time** in state machines
4. **Consider pipelining** (future enhancement beyond Stop & Wait)

---

## Common Issues & Solutions

### Issue 1: "BCC2 error" messages
**Cause**: Data corruption during transmission  
**Solutions**:
- Check cable connections
- Verify baud rate settings match
- Ensure byte stuffing is applied correctly
- Check BCC2 calculation includes all data bytes

### Issue 2: Continuous retransmissions
**Cause**: RR/REJ frames not received or processed correctly  
**Solutions**:
- Add debug prints in `receiveSupervisionFrame()`
- Verify control field values (0x05, 0x85, 0x01, 0x81)
- Check BCC1 calculation in supervision frames
- Ensure state machine resets properly

### Issue 3: Duplicate data received
**Cause**: Duplicate detection not working  
**Solutions**:
- Verify `expectedNs` is incremented after successful reception
- Check sequence number extraction from control field
- Ensure duplicate frames don't update `expectedNs`

### Issue 4: Byte stuffing errors
**Cause**: Incorrect stuffing/destuffing implementation  
**Solutions**:
- Test stuffing functions independently
- Verify escape sequences (0x7D 0x5E and 0x7D 0x5D)
- Check for off-by-one errors in buffer indexing
- Ensure BCC2 is stuffed along with data

### Issue 5: Timeout too short/long
**Cause**: Improper timeout configuration  
**Solutions**:
- Measure actual RTT in your setup
- Set timeout = 2-3 × RTT
- Typical values: 3-5 seconds for serial links
- Adjust based on observed performance

---

## Debug Tips

### Enable Verbose Logging
Add these debug prints:

```c
// In llwrite()
printf("TX: Sending I(Ns=%d), data_len=%d, frame_len=%d\n", Ns, bufSize, frameLen);
printf("TX: Waiting for RR%d...\n", (Ns + 1) % 2);

// In llread()
printf("RX: Received I(Ns=%d), expected=%d\n", receivedNs, expectedNs);
printf("RX: Data length after destuff=%d\n", dataLen);
printf("RX: BCC2 calc=0x%02X, recv=0x%02X\n", calculatedBCC2, receivedBCC2);
```

### Hexdump Frames
```c
void print_frame(unsigned char *frame, int len) {
    printf("Frame (%d bytes): ", len);
    for (int i = 0; i < len; i++) {
        printf("%02X ", frame[i]);
    }
    printf("\n");
}
```

### Monitor State Machine
```c
printf("State transition: %d -> %d (byte=0x%02X)\n", old_state, new_state, byte);
```

---

## Future Enhancements

1. **Sliding Window Protocol**: Replace Stop & Wait with Go-Back-N or Selective Repeat
2. **FEC (Forward Error Correction)**: Add redundancy to correct errors without retransmission
3. **Adaptive Timeout**: Dynamically adjust timeout based on measured RTT
4. **Statistics**: Track frame success rate, retransmissions, throughput
5. **Fragmentation**: Split large application packets across multiple frames

---

## Summary

| Feature | llwrite() | llread() |
|---------|-----------|----------|
| **Role** | Transmitter | Receiver |
| **Main Task** | Send I frames | Receive I frames |
| **Acknowledgment** | Wait for RR/REJ | Send RR/REJ |
| **Error Handling** | Retransmit on timeout/REJ | Send REJ on error |
| **Sequence Control** | Toggle Ns after success | Toggle expectedNs after success |
| **Byte Stuffing** | Apply before sending | Remove after receiving |
| **Return Value** | bufSize or -1 | dataLen or -1 |

---

**Implementation Status**: ✅ Complete  
**Protocol**: Stop & Wait ARQ with byte stuffing  
**Reliability**: Error detection (BCC1, BCC2) + Retransmission  
**Transparency**: Byte stuffing for FLAG and ESCAPE bytes
