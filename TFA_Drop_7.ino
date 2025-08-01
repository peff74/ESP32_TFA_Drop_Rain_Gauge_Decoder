#include <Arduino.h>

// Pin definition
#define RF_PIN 22


// Timing constants
#define PULSE_SHORT 250
#define PULSE_LONG 500
#define TOLERANCE 150
#define SYNC_PULSE_MIN 1500
#define MAX_CHANGES 150

// Protocol constants
#define MESSAGE_BITS 64
#define MESSAGE_BYTES 8
#define MIN_BITS 64

// Rain gauge constants
#define RAIN_COUNTER_OFFSET 65526
#define RAIN_TIP_TO_MM 0.254f
#define EXPECTED_PREFIX 0x3
#define EXPECTED_CONSTANT 0xAA

// CRC constants
#define CRC_GENERATOR 0x31
#define CRC_INITIAL_KEY 0xF4

// Bit masks
#define BATTERY_LOW_MASK 0x80
#define DEVICE_RESET_MASK 0x40
#define TX_COUNTER_MASK 0x0F

// Global variables (for interrupt)
volatile uint16_t timings[MAX_CHANGES];
volatile int timingIndex = 0;
volatile unsigned long lastTime = 0;
volatile bool recording = false;

// IRQ counter
volatile unsigned long irqCounter = 0;
unsigned long lastIrqCount = 0;
unsigned long lastIrqTime = 0;
unsigned long irqPerSecond = 0;

// Data processing state
bool dataReady = false;
uint8_t validData[MESSAGE_BYTES];

// LFSR CRC-8 Checksum-Funktion mit Byte-Reflektion
uint8_t lfsr_digest8_reflect(const uint8_t message[], int bytes, uint8_t gen, uint8_t key) {
  uint8_t sum = 0;

  // Verarbeite Bytes von hinten nach vorne (reflected)
  for (int k = bytes - 1; k >= 0; --k) {
    uint8_t data = message[k];

    // Verarbeite Bits von LSB zu MSB (reflected)
    for (int i = 0; i < 8; ++i) {
      // XOR key in sum wenn data bit gesetzt ist
      if ((data >> i) & 1) {
        sum ^= key;
      }

      // Key shift (optimiert)
      if (key & 0x80) {
        key = ((key << 1) & 0xFF) ^ gen;
      } else {
        key = (key << 1) & 0xFF;
      }
    }
  }

  return sum;
}

// Schnelle Vorvalidierung
bool quickValidation(const uint8_t* data) {
  // Erst Prefix prüfen
  uint8_t prefix = (data[0] >> 4) & 0x0F;
  if (prefix != EXPECTED_PREFIX) {
    return false;
  }

  // Dann Konstante prüfen
  if (data[5] != EXPECTED_CONSTANT) {
    return false;
  }

  return true;
}

// Vollständige CRC-Validierung (nur bei bestandener Schnellprüfung)
bool validateCRC(const uint8_t* data) {
  uint8_t computed_crc = lfsr_digest8_reflect(data, MESSAGE_BYTES - 1, CRC_GENERATOR, CRC_INITIAL_KEY);
  uint8_t expected_crc = data[MESSAGE_BYTES - 1];

  if (computed_crc == expected_crc) {

    return true;
  } else {
    Serial.printf("CRC Fehler: berechnet=0x%02X, erwartet=0x%02X\n", computed_crc, expected_crc);
    return false;
  }
}

// Optimierte Validierung mit mehrstufiger Prüfung
bool validateMessage(const uint8_t* data) {

  // Stufe 1: Schnelle Vorvalidierung (< 1µs)
  if (!quickValidation(data)) {
    return false;
  }

  // Stufe 2: CRC-Validierung (nur bei bestandener Vorprüfung, ~10-15µs)
  return validateCRC(data);
}

// Hardware Interrupt
void IRAM_ATTR handleRFInterrupt() {
  irqCounter++;

  unsigned long currentTime = micros();

  if (lastTime == 0) {
    lastTime = currentTime;
    return;
  }

  unsigned long duration = currentTime - lastTime;
  lastTime = currentTime;

  // Ignore very short or very long pulses
  if (duration < 100 || duration > 10000) {
    return;
  }

  // Sync detection (long pause starts recording)
  if (duration > SYNC_PULSE_MIN) {
    if (timingIndex >= MIN_BITS) {
      recording = false;  // Enough data for analysis available
    } else {
      timingIndex = 0;  // Start new recording
      recording = true;
    }
    return;
  }

  // Only record when we are recording
  if (recording && timingIndex < MAX_CHANGES) {
    timings[timingIndex++] = duration;
  }

  // Buffer full? Stop recording
  if (timingIndex >= MAX_CHANGES) {
    recording = false;
  }
}

// Hauptverarbeitungsfunktion
bool searchDecodeAndValidateData() {
  // Check if we have enough data for analysis
  if (recording || timingIndex < MIN_BITS) {
    return false;
  }

  uint8_t data[MESSAGE_BYTES] = { 0 };
  int bitIndex = 0;

  // Copy timings to avoid interference
  uint16_t localTimings[MAX_CHANGES];
  int localTimingIndex;

  noInterrupts();
  memcpy(localTimings, (const void*)timings, sizeof(localTimings));
  localTimingIndex = timingIndex;
  interrupts();

  // Skip sync signal
  int startIndex = 0;
  for (int i = 0; i < 6 && i < localTimingIndex; i++) {
    if (localTimings[i] > 600) {
      startIndex = i + 1;
    }
  }

  // PWM decoding: Pulses come in pairs (pulse + gap)
  for (int i = startIndex; i < localTimingIndex - 1 && bitIndex < MESSAGE_BITS; i += 2) {
    unsigned long pulse = localTimings[i];
    unsigned long gap = localTimings[i + 1];

    bool bit = false;

    // PWM detection: 0 = short pulse + long gap, 1 = long pulse + short gap
    if (abs((int)pulse - PULSE_SHORT) < TOLERANCE && abs((int)gap - PULSE_LONG) < TOLERANCE) {
      bit = false;
    } else if (abs((int)pulse - PULSE_LONG) < TOLERANCE && abs((int)gap - PULSE_SHORT) < TOLERANCE) {
      bit = true;
    }

    // Store bit (MSB first)
    int byteIndex = bitIndex / 8;
    int bitPos = 7 - (bitIndex % 8);

    if (bit) {
      data[byteIndex] |= (1 << bitPos);
    }

    bitIndex++;
  }

  // Only accept exactly 64 bits
  if (bitIndex != MESSAGE_BITS) {
    // Reset für nächsten Versuch
    noInterrupts();
    timingIndex = 0;
    recording = false;
    lastTime = 0;
    interrupts();
    return false;
  }

  // Mehrstufige Validierung
  bool isValid = validateMessage(data);

  if (isValid) {
    // Gültige Daten gefunden - für Ausgabe speichern
    memcpy(validData, data, MESSAGE_BYTES);
    return true;
  } else {
    // Ungültige Daten - Reset für nächsten Versuch
    noInterrupts();
    timingIndex = 0;
    recording = false;
    lastTime = 0;
    interrupts();
    return false;
  }
}

// Ausgabe der Daten und Reset für neue Suche
void outputDataAndReset() {

  printData(validData, MESSAGE_BITS);

  // Reset für neue Datensuche
  noInterrupts();
  timingIndex = 0;
  recording = false;
  lastTime = 0;
  interrupts();

  dataReady = false;
}

// Berechnet echte Tipps aus Regenzähler
int calculateRealTips(uint16_t rainCounter) {
  int realTips = rainCounter - RAIN_COUNTER_OFFSET;
  if (realTips < 0) {
    // Consider overflow (after 65535 comes 0)
    realTips = rainCounter + (65536 - RAIN_COUNTER_OFFSET);
  }
  return realTips;
}

// Ausgabefunktion
void printData(uint8_t* data, int bitCount) {
  Serial.printf("\n=== Regenmesser-Daten ===\n");
  Serial.printf("Empfangene Bits: %d\n", bitCount);

  // Raw data as HEX
  Serial.print("Raw: ");
  for (int i = 0; i < MESSAGE_BYTES; i++) {
    Serial.printf("%02X", data[i]);
  }
  Serial.println();

  // Bitwise output with formatting
  Serial.print("Bits: ");
  for (int i = 0; i < MESSAGE_BITS; i++) {
    int byteIndex = i / 8;
    int bitPos = 7 - (i % 8);
    bool bit = (data[byteIndex] & (1 << bitPos)) != 0;
    Serial.print(bit ? "1" : "0");

    if ((i + 1) % 8 == 0 && i < 63) {
      Serial.print(" ");
    }
  }
  Serial.println();
  Serial.println("      CCCCIIII IIIIIIII IIIIIIII BCUUXXXX RRRRRRRR CCCCCCCC SSSSSSSS MMMMMMMM");

  uint32_t deviceId = ((data[0] & 0x0F) << 16) | (data[1] << 8) | data[2];
  Serial.printf("Device-ID: 0x%05X (%u)\n", deviceId, deviceId);

  bool deviceReset = (data[3] & DEVICE_RESET_MASK) != 0;
  Serial.printf("Reset: %s\n", deviceReset ? "JA" : "NEIN");

  uint8_t txCounter = data[3] & TX_COUNTER_MASK;
  Serial.printf("TX-Zaehler: 0x%X (%d)\n", txCounter, txCounter);

  bool batteryLow = (data[3] & BATTERY_LOW_MASK) != 0;
  Serial.printf("Batterie: %s\n", batteryLow ? "SCHWACH" : "OK");

  // Rain counter calculation
  uint16_t rainCounter = (data[6] << 8) + data[4];  // MSB + LSB
  int realTips = calculateRealTips(rainCounter);
  float rainMM = realTips * RAIN_TIP_TO_MM;

  Serial.printf("Regenzaehler 16-Bit: %d\n", rainCounter);
  Serial.printf("Echte Tipps: %d\n", realTips);
  Serial.printf("Niederschlag: %.3f mm\n", rainMM);

  // CRC verification
  uint8_t computed_crc = lfsr_digest8_reflect(data, MESSAGE_BYTES - 1, CRC_GENERATOR, CRC_INITIAL_KEY);
  Serial.printf("CRC: berechnet=0x%02X, empfangen=0x%02X (%s)\n",
                computed_crc, data[7], (computed_crc == data[7]) ? "✓" : "✗");

  Serial.printf("IRQ/s: %lu\n", irqPerSecond);
  Serial.println();
}





// IRQ COUNTER FUNCTION
void updateIrqCounter() {
  unsigned long currentTime = millis();
  if (currentTime - lastIrqTime >= 1000) {
    noInterrupts();
    unsigned long currentIrqCount = irqCounter;
    interrupts();

    irqPerSecond = currentIrqCount - lastIrqCount;
    lastIrqCount = currentIrqCount;
    lastIrqTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);


  Serial.println("=== Regenmesser-Decoder mit CRC-Validierung ===");
  Serial.println("Timing-Parameter:");
  Serial.printf("- Kurzer Puls: %d us (±%d)\n", PULSE_SHORT, TOLERANCE);
  Serial.printf("- Langer Puls: %d us (±%d)\n", PULSE_LONG, TOLERANCE);
  Serial.printf("- Sync-Minimum: %d us\n", SYNC_PULSE_MIN);
  Serial.println("PWM-Dekodierung: Standard-Logik (kurz/lang -> 0/1)");
  Serial.println("Validierung: Mehrstufig (Prefix -> Konstante -> CRC)");
  Serial.printf("CRC-Parameter: Generator=0x%02X, Initial=0x%02X\n", CRC_GENERATOR, CRC_INITIAL_KEY);
  Serial.println("Akzeptiert nur exakt 64-Bit-Nachrichten mit gültiger CRC!");


  // Setup RF interrupt
  pinMode(RF_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(RF_PIN), handleRFInterrupt, CHANGE);

  Serial.println("Setup abgeschlossen. Warte auf Regenmesser-Signale...");
}

void loop() {
  // Update IRQ counter
  updateIrqCounter();

  // Kontinuierliche Suche nach verwertbaren Daten
  if (!dataReady) {
    dataReady = searchDecodeAndValidateData();
  }

  // Wenn gültige Daten gefunden wurden, ausgeben und reset
  if (dataReady) {
    outputDataAndReset();
  }

  delay(10);
}