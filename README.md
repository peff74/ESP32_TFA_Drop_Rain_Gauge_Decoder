# TFA Drop Rain Gauge Decoder (30.3233.01)
 - This is a TFA Drop Rain Gauge 30.3233.01 decoder script for Arduino
 - The 30.3233.01 is a 433MHz wireless rain gauge with 0.254mm resolution
 - Decodes 64-bit PWM messages with CRC-8 validation
 - Works with ESP32 and similar boards
 - **Two receiver options**: RX470-4 superheterodyne or CC1101 transceiver

![TFA Rain Gauge logo](https://github.com/peff74/ESP32_TFA_Drop_Rain_Gauge_Decoder/blob/main/TFA_Regenmesser.jpeg)
![TFA Rain Gauge logo](https://github.com/peff74/ESP32_TFA_Drop_Rain_Gauge_Decoder/blob/main/RF433.jpg)
![TFA Rain Gauge logo](https://github.com/peff74/ESP32_TFA_Drop_Rain_Gauge_Decoder/blob/main/cc1101.jpg)


## Arduino script features
 - Real-time 433MHz RF signal decoding
 - Hardware interrupt-based signal capture
 - Multi-stage message validation (Prefix → Constant → CRC-8)
 - Duplicate message validation (200ms window)
 - PWM pulse decoding with timing tolerance
 - Non-blocking operation
 - IRQ overload protection
 - Reception statistics tracking
 - So that even beginners (like me) can understand 433MHz decoding

## Receiver Comparison

### Option 1: RX470-4 Superheterodyne (~$2)
**Pros:**
 - Very cheap
 - Simple wiring (3 pins)
 - No library required

**Cons:**
 - **Requires filter circuit** (100nF + 1kΩ)
 - High noise IRQ rate (5000-8000/s without filter)
 - Lower sensitivity
 - Antenna critical (exactly 17.3cm)

### Option 2: CC1101 Transceiver (~$3-5)
**Pros:**
 - **Excellent selectivity** - virtually no noise IRQs
 - **No filter circuit needed**
 - Much better sensitivity
 - Digital RSSI readings
 - Adjustable bandwidth and data rate
 - More reliable reception

**Cons:**
 - Slightly more expensive
 - More complex wiring (SPI)
 - Requires CC1101 library

**Recommendation:** Use CC1101 for production use. The improved signal quality and no filter requirement make it worth the small extra cost.

## How does it work

**RF Signal Reception**

*With RX470-4:*
The receiver outputs digital pulses on the DATA pin.
Every signal change triggers a hardware interrupt.

*With CC1101:*
The CC1101's GDO0 pin outputs demodulated ASK/OOK data.
Hardware interrupt on CHANGE captures signal edges with minimal noise.

    void IRAM_ATTR handleRFInterrupt() {
        irqCounter = irqCounter + 1;
        unsigned long currentTime = micros();
        unsigned long duration = currentTime - lastTime;
        // Store timing for PWM decoding

**PWM Decoding**
The rain gauge uses PWM encoding where pulse width determines bit value:
 - **Bit 0**: Short pulse (250µs) + Long gap (500µs)  
 - **Bit 1**: Long pulse (500µs) + Short gap (250µs)
 - **Sync**: Long pause (>1500µs) marks message start

*searchDecodeAndValidateData()*

    // PWM detection with tolerance
    if (abs((int)pulse - PULSE_SHORT) < TOLERANCE && 
        abs((int)gap - PULSE_LONG) < TOLERANCE) {
        bit = false;  // Short pulse = 0
    } else if (abs((int)pulse - PULSE_LONG) < TOLERANCE && 
               abs((int)gap - PULSE_SHORT) < TOLERANCE) {
        bit = true;   // Long pulse = 1
    }

**Message Format (64 bits)**
The decoded message contains weather station data:

    +--------+--------+--------+--------+--------+--------+--------+--------+
    |  Byte 0|  Byte 1|  Byte 2|  Byte 3|  Byte 4|  Byte 5|  Byte 6|  Byte 7|
    +--------+--------+--------+--------+--------+--------+--------+--------+
    |PPPPDDDD|DDDDDDDD|DDDDDDDD|BCUUXXXX|RRRRRRRR|CCCCCCCC|SSSSSSSS|MMMMMMMM|
    +--------+--------+--------+--------+--------+--------+--------+--------+
    
    P = Prefix (0x3)
    D = Device ID (20 bits)
    B = Battery Low flag
    C = Device Reset flag  
    U = Unknown
    X = TX Counter (4 bits)
    R = Rain Counter LSB
    C = Constant (0xAA)
    S = Rain Counter MSB
    M = CRC-8 checksum

**Duplicate Message Validation**
The rain gauge transmits each message twice within ~50-150ms.
The decoder validates duplicates to ensure data integrity:

*validateDuplicate()*

    // Store first message, wait for duplicate
    if (!hasPendingMessage) {
        memcpy(pendingMessage.data, data, MESSAGE_BYTES);
        pendingMessage.timestamp = currentTime;
        hasPendingMessage = true;
        return false;  // Wait for duplicate
    }
    
    // Validate duplicate matches
    if (memcmp(pendingMessage.data, data, MESSAGE_BYTES) == 0) {
        successfulDuplicates++;
        return true;  // Message validated!
    }

**200ms Timeout:**
 - If no duplicate arrives within 200ms → message discarded
 - If different message arrives → both discarded, new becomes first
 - Only matching duplicates are accepted

## Message Decoding Example

**Raw Data**: `3E42A800FEAAFF5E`

**Bit Layout**:
```
00111110 01000010 10101000 00000000 11111110 10101010 11111111 01011110
PPPPDDDD DDDDDDDD DDDDDDDD BCUUXXXX RRRRRRRR CCCCCCCC SSSSSSSS MMMMMMMM
```

**Decoded Values**:

*Byte 0 (0x3E)*: Prefix=0x3 ✓, Device-ID High=0xE  
*Byte 1 (0x42)*: Device-ID Middle=0x42  
*Byte 2 (0xA8)*: Device-ID Low=0xA8  
**→ Device-ID**: 0xE42A8 = 935,592

*Byte 3 (0x00)*: Battery=OK, Reset=No, TX-Counter=0  
*Byte 4 (0xFE)*: Rain Counter LSB=254  
*Byte 5 (0xAA)*: Constant=0xAA ✓  
*Byte 6 (0xFF)*: Rain Counter MSB=255  
**→ Rain Counter**: (255 << 8) + 254 = 65,534

*Byte 7 (0x5E)*: CRC-8 checksum

**Rain Calculation**:
```
Raw Counter: 65,534
Offset: 65,526 (RAIN_COUNTER_OFFSET)
Real Tips: 65,534 - 65,526 = 8 tips
Rainfall: 8 × 0.254 mm = 2.032 mm
```

**Multi-Stage Validation**
Fast validation prevents CPU overload from invalid signals:

*quickValidation()*

    // Stage 1: Quick prefix check (< 1µs)
    uint8_t prefix = (data[0] >> 4) & 0x0F;
    if (prefix != EXPECTED_PREFIX) return false;
    
    // Stage 2: Constant check
    if (data[5] != EXPECTED_CONSTANT) return false;

*validateCRC()*

    // Stage 3: CRC-8 validation (only if stages 1+2 pass)
    uint8_t computed_crc = lfsr_digest8_reflect(data, 7, 0x31, 0xF4);
    return (computed_crc == data[7]);

**CRC-8 Algorithm Details**
The rain gauge uses a reflected LFSR CRC-8:
 - **Generator Polynomial**: 0x31 (x⁸ + x⁵ + x⁴ + 1)
 - **Initial Key**: 0xF4  
 - **Reflection**: Bytes processed backwards (6→0), bits LSB to MSB

*lfsr_digest8_reflect()*

    // Process bytes from end to start (reflected)
    for (int k = bytes - 1; k >= 0; --k) {
        uint8_t data = message[k];
        // Process bits from LSB to MSB (reflected)
        for (int i = 0; i < 8; ++i) {
            if ((data >> i) & 1) {
                sum ^= key;
            }
            // LFSR shift with generator feedback
            if (key & 0x80) {
                key = ((key << 1) & 0xFF) ^ gen;
            } else {
                key = (key << 1) & 0xFF;
            }
        }
    }

**CRC Verification for Example Data**:
Input: `3E 42 A8 00 FE AA FF` (bytes 0-6)  
Processing order: FF → AA → FE → 00 → A8 → 42 → 3E  
Calculated CRC: `0x5E`  
Received CRC: `0x5E` ✓

**Rain Calculation**
The 16-bit rain counter has an offset that must be subtracted:

*calculateRealTips()*

    int realTips = rainCounter - RAIN_COUNTER_OFFSET;  // 65526
    if (realTips < 0) {
        // Handle counter overflow
        realTips = rainCounter + (65536 - RAIN_COUNTER_OFFSET);
    }
    float rainMM = realTips * RAIN_TIP_TO_MM;  // 0.254mm per tip




## Protocol Summary

**Signal Processing Flow:**
1. RF receiver detects 433MHz signal changes
2. Hardware interrupt captures pulse/gap timings
3. Sync pulse (>1500µs) triggers decoding start
4. 64 pulse/gap pairs decoded to bits (MSB first)
5. Multi-stage validation: Duplicate → Prefix → Constant → CRC-8
6. Rain counter converted to real tips and mm rainfall
7. Status flags (battery, reset, TX counter) extracted


## Hardware Setup

### Option 1: RX470-4 Superheterodyne

**Required Components:**
 - ESP32 development board
 - RX470-4 433MHz receiver module  
 - 17.3cm wire antenna
 - **Filter circuit required!**

**Connections:**
    
    RX470-4    →    ESP32
    --------         -----
    VCC        →    3.3V
    GND        →    GND  
    DATA       →    GPIO 22
    ANT        →    17.3cm wire

**⚠️ Filter Circuit Required**
Without filtering, RF noise can cause IRQ overload (>8000 IRQ/s):
```
RX470-4 DATA ──┬─── 1  kΩ ───┬─── ESP32 GPIO 22
               │             │
           100nF C           │
               │             │
               └─────────────┴─── GND
```

**Normal vs. Overloaded IRQ rates:**

    IRQ/s: 150-1500    ← Normal operation (with filter)
    IRQ/s: 8500        ← Overloaded! Add filter circuit

### Option 2: CC1101 Transceiver (Recommended)

**Required Components:**
 - ESP32 development board
 - CC1101 433MHz transceiver module
 - Wire antenna (optional - module has built-in)

**Connections (SPI):**
    
    CC1101     →    ESP32
    --------         -----
    VCC        →    3.3V
    GND        →    GND
    SCK        →    GPIO 18
    MISO       →    GPIO 19
    MOSI       →    GPIO 23
    CSN        →    GPIO 5
    GDO0       →    GPIO 27

**CC1101 Configuration:**
    
    cc1101.setMHZ(433.92);              // Frequency
    cc1101.setDataRate(2000);           // 2 kBaud
    cc1101.setRxBW(RX_BW_203_KHZ);      // Bandwidth
    cc1101.setModulation(ASK_OOK);      // ASK/OOK modulation
    cc1101.setRx();                     // Set to RX mode

**Library Required:**
```cpp
#include <CC1101_ESP_Arduino.h>
```

**⚠️ Important: Modified Library Needed**

For RSSI functionality, you need the modified version with RSSI support:
- **GitHub PR**: https://github.com/wladimir-computin/CC1101-ESP-Arduino/pull/11
- Standard library does NOT include `getRSSI()` function
- Clone the PR branch or wait for merge into main library

**Installation:**
```bash
# Clone modified library to Arduino/libraries/
git clone https://github.com/peff74/CC1101-ESP-Arduino.git
# Or download as ZIP and install manually
```

Without this modification, remove all `cc1101.getRSSI()` calls from the code.

**IRQ Performance Comparison:**

    RX470-4 (no filter):  5000-8500 IRQ/s
    RX470-4 (with filter): 150-1500 IRQ/s
    CC1101:                       0 IRQ/s  ← Clean signal!

**Additional CC1101 Benefits:**
 - Real-time RSSI measurements (signal strength in dBm)
 - No filter components needed
 - Better range and sensitivity
 - Cleaner data output

## Output Examples

**With CC1101:**

    === Rain Gauge Data ===
    Signal RSSI: -67 dBm
    Received Bits: 64
    Raw: 3E42A808E7AA005E
    Bits: 00111110 01000010 10101000 00001000 11100111 10101010 00000000 01011110
          CCCCIIII IIIIIIII IIIIIIII BCUUXXXX RRRRRRRR CCCCCCCC SSSSSSSS MMMMMMMM
    Device ID: 0xE42A8 (934568)
    Reset: NO
    TX Counter: 0x8 (8)
    Battery: OK
    Rain Counter 16-Bit: 231
    Real Tips: 241
    Precipitation: 61.214 mm
    CRC: computed=0x5E, received=0x5E (OK)
    IRQ/s: 87
    
    --- Reception Statistics ---
    Total Received: 156
    Successful Duplicates: 152 (97.4%)
    Duplicate Timeouts: 4 (2.6%)
    Different Messages: 0

**Duplicate Validation Messages:**

    [MSGS]: First message received, waiting 200ms for duplicate...
    [MSGS]: Duplicate received after 78ms - message valid!

    [FAIL]: Duplicate timeout after 201ms - discarding message

## Troubleshooting

### RX470-4 Issues

**No data received:**
 - Check antenna length (exactly 17.3cm for 433MHz)
 - Verify wiring connections
 - Add/check filter circuit
 - Move closer to rain gauge (max ~100m range)

**High IRQ count (>5000/s):**
 - **Add filter circuit** between receiver and ESP32
 - Move away from RF interference sources
 - Check receiver module quality

### CC1101 Issues

**No data received:**
 - Verify SPI connections (SCK, MISO, MOSI, CS)
 - Check CC1101 library installed correctly (see modified library requirement above!)
 - Verify 3.3V power supply (not 5V!)
 - Check GDO0 connection for interrupt
 - Test with `cc1101.getVersion()` in setup

**RSSI not working:**
 - Make sure you're using the modified library from PR #11
 - Standard library does not include `getRSSI()` function
 - See library installation section above

**Low RSSI (<-90 dBm):**
 - Move closer to rain gauge
 - Check antenna connection
 - Verify frequency setting (433.92 MHz)

**Good RSSI but no decode:**
 - Adjust RX bandwidth if needed
 - Check data rate setting (2000 baud)
 - Verify ASK/OOK modulation selected

## Files in Repository

 - `TFA_Drop_RX470.ino` - Original version with RX470-4 receiver
 - `TFA_Drop_CC1101.ino` - New version with CC1101 transceiver
 - `README.md` - This documentation

## License & Credits

Original decoder logic and protocol analysis by peff74  
CC1101 integration and improvements added 2025

Feel free to use and modify for your own projects!

![Badge](https://hitscounter.dev/api/hit?url=https%3A%2F%2Fgithub.com%2Fpeff74%2FESP32_TFA_Drop_Rain_Gauge_Decoder&label=Hits&icon=github&color=%23198754&message=&style=flat&tz=UTC)
