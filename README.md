# TFA Drop Rain Gauge Decoder (30.3233.01)
 - This is a TFA Drop Rain Gauge 30.3233.01 decoder script for Arduino
 - The TFA 30.3233.01 is a 433MHz wireless rain gauge with 0.254mm resolution
 - Decodes 64-bit PWM messages with CRC-8 validation
 - Works with ESP32 and similar boards
 - Uses RX470-4 receiver module (less than $2 on Ali)

![TFA Rain Gauge logo](https://github.com/peff74/ESP32_TFA_Drop_Rain_Gauge_Decoder/blob/main/TFA_Regenmesser.jpeg)
![TFA Rain Gauge logo](https://github.com/peff74/ESP32_TFA_Drop_Rain_Gauge_Decoder/blob/main/RF433.jpg)


## Arduino script features
 - Real-time 433MHz RF signal decoding
 - Hardware interrupt-based signal capture
 - Multi-stage message validation (Prefix → Constant → CRC-8)
 - PWM pulse decoding with timing tolerance
 - Non-blocking operation
 - IRQ overload protection
 - Production and debug output modes
 - So that even beginners (like me) can understand 433MHz decoding

## How does it work

**RF Signal Reception**
The RX470-4 receiver outputs digital pulses on the DATA pin.
Every signal change triggers a hardware interrupt:

    void IRAM_ATTR handleRFInterrupt() {
        irqCounter++;
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
5. Three-stage validation: Prefix → Constant → CRC-8
6. Rain counter converted to real tips and mm rainfall
7. Status flags (battery, reset, TX counter) extracted


## Hardware Setup

**Required Components:**
 - ESP32 development board
 - RX470-4 433MHz receiver module  
 - 17.3cm wire antenna
 - Optional: Filter circuit (recommended!)

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

    IRQ/s: 150-1500    ← Normal operation
    IRQ/s: 8500   ← Overloaded! Add filter circuit


## Output Examples

    === Regenmesser-Daten  ===
    Empfangene Bits: 64
    Raw: 3E42A808E7AA005E
    Bits: 00111110 01000010 10101000 00001000 11100111 10101010 00000000 01011110
    CCCCIIII IIIIIIII IIIIIIII BCUUXXXX RRRRRRRR CCCCCCCC SSSSSSSS MMMMMMMM
    Device-ID: 0xE42A8 (934568)
    Reset: NEIN
    TX-Zaehler: 0x8 (8)
    Batterie: OK
    Regenzaehler 16-Bit: 231
    Echte Tipps: 241
    Niederschlag: 61.214 mm
    CRC: berechnet=0x5E, empfangen=0x5E (O.K.)
    IRQ/s: 1523



## Troubleshooting

**No data received:**
 - Check antenna length (17.3cm for 433MHz)
 - Verify wiring connections
 - Move closer to rain gauge (max ~100m range)

**High IRQ count (>5000/s):**
 - Add filter circuit between receiver and ESP32
 - Move away from RF interference sources
 - Check receiver module quality

![Badge](https://hitscounter.dev/api/hit?url=https%3A%2F%2Fgithub.com%2Fpeff74%2FESP32_TFA_Drop_Rain_Gauge_Decoder&label=Hits&icon=github&color=%23198754&message=&style=flat&tz=UTC)

