
#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

// =====================================================
// ESP32 / MAX30102 DIAGNOSTIC PROGRAM
// =====================================================

#define SDA_PIN 21
#define SCL_PIN 22

MAX30105 particleSensor;

// Avoid BUFFER_SIZE naming conflict with the library
const int SAMPLE_BUFFER_SIZE = 100;
const int NEW_SAMPLES = 25;

// Diagnostic thresholds.
// These are starting heuristics, NOT calibrated limits.
const uint32_t LOW_SIGNAL_THRESHOLD = 10000;
const uint32_t NEAR_ADC_MAX = 250000;
const unsigned long SAMPLE_TIMEOUT_MS = 1500;

// Sensor buffers
uint32_t irBuffer[SAMPLE_BUFFER_SIZE];
uint32_t redBuffer[SAMPLE_BUFFER_SIZE];

// Algorithm outputs
int32_t heartRate = 0;
int32_t spo2 = 0;

int8_t validHeartRate = 0;
int8_t validSPO2 = 0;

// Tracks whether we have a complete 100-sample window
bool bufferReady = false;

// The PC sends 'S' to trigger a complete 10-second capture.
// The Arduino sends timestamped raw samples and existing diagnostics.
const unsigned long CAPTURE_DURATION_MS = 10000;
bool captureActive = false;
unsigned long captureStartMs = 0;

void stopCapture() {
  Serial.println("LOG_END");
  captureActive = false;
  Serial.println("Capture complete. Send S for another 10-second capture.");
}


// =====================================================
// SIGNAL STATISTICS
// =====================================================

struct SignalStats {
  uint32_t minimum;
  uint32_t maximum;
  float average;
  float rangePercent;

  int lowSamples;
  int nearMaxSamples;
  int largeJumps;
  int repeatedPairs;
};

// Analyze any section of a sample buffer
SignalStats analyzeSignal(
  const uint32_t data[],
  int startIndex,
  int count
) {

  SignalStats result = {};

  result.minimum = 0xFFFFFFFFUL;
  result.maximum = 0;

  uint64_t sum = 0;

  for (int i = startIndex; i < startIndex + count; i++) {

    uint32_t value = data[i];

    sum += value;

    if (value < result.minimum)
      result.minimum = value;

    if (value > result.maximum)
      result.maximum = value;

    // Detect unusually low signal values
    if (value < LOW_SIGNAL_THRESHOLD)
      result.lowSamples++;

    // Detect values approaching the 18-bit ADC limit
    if (value >= NEAR_ADC_MAX)
      result.nearMaxSamples++;

    if (i > startIndex) {

      uint32_t previous = data[i - 1];

      // Calculate absolute difference between samples
      uint32_t difference =
        value >= previous ?
        value - previous :
        previous - value;

      // Flag changes >= 25% of previous sample
      if (previous > 0 &&
          (uint64_t)difference * 4 >= previous) {

        result.largeJumps++;
      }

      // Detect consecutive identical readings
      if (value == previous)
        result.repeatedPairs++;
    }
  }

  result.average = (float)sum / count;

  if (result.average > 0) {
    result.rangePercent =
      100.0f *
      (result.maximum - result.minimum) /
      result.average;
  } else {
    result.rangePercent = 0;
  }

  return result;
}

// =====================================================
// READ ONE RED/IR SAMPLE PAIR
// =====================================================

bool readSensor(int index) {

  unsigned long startTime = millis();

  // Wait for a sample, but don't wait forever
  while (particleSensor.available() == 0) {

    particleSensor.check();

    if (millis() - startTime > SAMPLE_TIMEOUT_MS) {
      return false;
    }

    delay(1);
  }

  // Read matching samples from the FIFO
  redBuffer[index] = particleSensor.getFIFORed();
  irBuffer[index] = particleSensor.getFIFOIR();

  // Timestamped raw samples: SAMPLE,elapsed_ms,IR,RED
  // This is useful for plotting the pulse waveform and finding dropouts.
  if (captureActive) {
    Serial.print("SAMPLE,");
    Serial.print(millis() - captureStartMs);
    Serial.print(",");
    Serial.print(irBuffer[index]);
    Serial.print(",");
    Serial.println(redBuffer[index]);
  }

  particleSensor.nextSample();

  return true;
}

// =====================================================
// HEART RATE AND SPO2 CALCULATION
// =====================================================

void calculateVitals() {

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    SAMPLE_BUFFER_SIZE,
    redBuffer,
    &spo2,
    &validSPO2,
    &heartRate,
    &validHeartRate
  );
}

// =====================================================
// PRINT SIGNAL STATISTICS
// =====================================================

void printStats(const char* label, SignalStats s) {

  Serial.print(label);

  Serial.print(" Min=");
  Serial.print(s.minimum);

  Serial.print(" Max=");
  Serial.print(s.maximum);

  Serial.print(" Avg=");
  Serial.print(s.average, 1);

  Serial.print(" Range%=");
  Serial.print(s.rangePercent, 1);

  Serial.print(" Low=");
  Serial.print(s.lowSamples);

  Serial.print(" NearMax=");
  Serial.print(s.nearMaxSamples);

  Serial.print(" Jumps=");
  Serial.print(s.largeJumps);

  Serial.print(" Repeats=");
  Serial.println(s.repeatedPairs);
}

// =====================================================
// DISPLAY RESULTS AND DIAGNOSTICS
// =====================================================

void printVitals(int recentStart, int recentCount,
                 unsigned long collectionTime) {

  // Analyze the complete 100-sample window
  SignalStats irStats =
    analyzeSignal(irBuffer, 0, SAMPLE_BUFFER_SIZE);

  SignalStats redStats =
    analyzeSignal(redBuffer, 0, SAMPLE_BUFFER_SIZE);

  // Analyze newly collected samples separately
  SignalStats recentIR =
    analyzeSignal(irBuffer, recentStart, recentCount);

  SignalStats recentRED =
    analyzeSignal(redBuffer, recentStart, recentCount);

  Serial.println();
  Serial.println("================================");
  Serial.println("       MAX30102 DIAGNOSTICS");
  Serial.println("================================");

  // ---------------------------------------------
  // Latest readings
  // ---------------------------------------------

  Serial.print("Latest IR: ");
  Serial.println(irBuffer[SAMPLE_BUFFER_SIZE - 1]);

  Serial.print("Latest RED: ");
  Serial.println(redBuffer[SAMPLE_BUFFER_SIZE - 1]);

  Serial.print("Samples collected: ");
  Serial.println(recentCount);

  Serial.print("Collection time (ms): ");
  Serial.println(collectionTime);

  // ---------------------------------------------
  // Calculated results
  // ---------------------------------------------

  Serial.println("--- VITALS ---");

  Serial.print("Heart Rate: ");

  if (validHeartRate == 1 && heartRate > 0) {
    Serial.print(heartRate);
    Serial.println(" BPM");
  } else {
    Serial.println("INVALID");
  }

  Serial.print("Blood Oxygen: ");

  if (validSPO2 == 1 && spo2 >= 0 && spo2 <= 100) {
    Serial.print(spo2);
    Serial.println(" %");
  } else {
    Serial.println("INVALID");
  }

  // ---------------------------------------------
  // Raw outputs from Maxim algorithm
  // ---------------------------------------------

  Serial.println("--- ALGORITHM OUTPUT ---");

  Serial.print("Raw Heart Rate: ");
  Serial.println(heartRate);

  Serial.print("Heart Rate Valid Flag: ");
  Serial.println((int)validHeartRate);

  Serial.print("Raw SpO2: ");
  Serial.println(spo2);

  Serial.print("SpO2 Valid Flag: ");
  Serial.println((int)validSPO2);

  // ---------------------------------------------
  // Raw signal information
  // ---------------------------------------------

  Serial.println("--- FULL 100-SAMPLE WINDOW ---");

  printStats("IR: ", irStats);
  printStats("RED:", redStats);

  Serial.println("--- NEW SAMPLES ONLY ---");

  printStats("IR: ", recentIR);
  printStats("RED:", recentRED);

  // ---------------------------------------------
  // Diagnostic warnings
  // ---------------------------------------------

  Serial.println("--- DIAGNOSTIC REPORT ---");

  bool signalWarning = false;

  // Low optical signal
  if (recentIR.lowSamples > 0 ||
      recentRED.lowSamples > 0) {

    Serial.println(
      "WARNING: Low optical signal detected."
    );

    Serial.println(
      "Possible: finger shifted, poor contact,"
    );

    Serial.println(
      "weak illumination, or sensor read issue."
    );

    signalWarning = true;
  }

  // Abrupt variations in received light
  if (recentIR.largeJumps > 0 ||
      recentRED.largeJumps > 0) {

    Serial.println(
      "WARNING: Sudden signal changes detected."
    );

    Serial.println(
      "Possible: movement, unstable pressure,"
    );

    Serial.println(
      "loose connection, or sample corruption."
    );

    signalWarning = true;
  }

  // Large variation across complete window
  if (irStats.rangePercent > 30.0f ||
      redStats.rangePercent > 30.0f) {

    Serial.println(
      "WARNING: Large variation in 4-second window."
    );

    Serial.println(
      "Optical contact may have changed."
    );

    signalWarning = true;
  }

  // Approaching ADC limit
  if (irStats.nearMaxSamples > 0 ||
      redStats.nearMaxSamples > 0) {

    Serial.println(
      "WARNING: Signal approaching ADC maximum."
    );

    Serial.println(
      "Check LED brightness and sensor placement."
    );

    signalWarning = true;
  }

  // Repeated samples
  if (recentIR.repeatedPairs > 15 ||
      recentRED.repeatedPairs > 15) {

    Serial.println(
      "WARNING: Many consecutive identical samples."
    );

    Serial.println(
      "Possible weak or stalled signal acquisition."
    );

    signalWarning = true;
  }

  // Heart-rate algorithm failure
  if (validHeartRate != 1 || heartRate <= 0) {

    Serial.println(
      "ERROR: Heart-rate algorithm rejected result."
    );

    Serial.println(
      "It may not have found enough usable IR peaks."
    );
  }

  // SpO2 algorithm failure
  if (validSPO2 != 1 || spo2 < 0 || spo2 > 100) {

    Serial.println(
      "ERROR: SpO2 algorithm rejected result."
    );

    Serial.println(
      "Possible unusable RED/IR pulse ratios."
    );

    Serial.println(
      "Check signal quality and library algorithm."
    );
  }

  // Valid flag with questionable input
  if (signalWarning &&
      validSPO2 == 1 &&
      spo2 >= 0 &&
      spo2 <= 100) {

    Serial.println(
      "CAUTION: SpO2 was accepted despite"
    );

    Serial.println(
      "detected signal-quality warnings."
    );
  }

  if (!signalWarning) {
    Serial.println(
      "No major raw-signal warnings detected."
    );
  }

  Serial.println(
    "NOTE: Diagnostics are heuristic, not proof."
  );

  Serial.println("================================");
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println("Initializing MAX30102...");

  // ESP32 I2C pins
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {

    Serial.println("MAX30102 not found!");
    Serial.println("Check wiring and power.");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("MAX30102 initialized.");

  // Preserve original SpO2 algorithm settings
  byte ledBrightness = 60;
  byte sampleAverage = 4;
  byte ledMode = 2;

  int sampleRate = 100;
  int pulseWidth = 411;
  int adcRange = 4096;

  particleSensor.setup(
    ledBrightness,
    sampleAverage,
    ledMode,
    sampleRate,
    pulseWidth,
    adcRange
  );

  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("Sensor configured.");
  Serial.println("Secure finger over optical sensor.");
  Serial.println("Ready for a 10-second capture.");
  Serial.println("Close Arduino Serial Monitor, then run the Python logger.");
  Serial.println("Send S over serial to start capturing.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // Wait until the Windows Python logger requests a capture.
  // This avoids losing the first seconds while the serial port opens.
  if (!captureActive) {
    if (Serial.available()) {
      char command = Serial.read();

      if (command == 'S' || command == 's') {
        // Clear the calculation state for a fresh measurement session.
        bufferReady = false;
        captureStartMs = millis();
        captureActive = true;

        Serial.println("LOG_BEGIN");
        Serial.println("SAMPLE_HEADER,elapsed_ms,IR,RED");
        Serial.println("Collecting first 100 samples...");
      }
    }
    delay(5);
    return;
  }

  // Terminate the capture after 10 seconds (checked frequently).
  if (millis() - captureStartMs >= CAPTURE_DURATION_MS) {
    stopCapture();
    return;
  }

  int recentStart;
  int recentCount;
  const unsigned long collectionStartMs = millis();

  if (!bufferReady) {
    recentStart = 0;
    recentCount = SAMPLE_BUFFER_SIZE;
  } else {
    // Keep the most recent 75 readings, then append 25 new ones.
    for (int i = NEW_SAMPLES; i < SAMPLE_BUFFER_SIZE; i++) {
      redBuffer[i - NEW_SAMPLES] = redBuffer[i];
      irBuffer[i - NEW_SAMPLES] = irBuffer[i];
    }
    recentStart = SAMPLE_BUFFER_SIZE - NEW_SAMPLES;
    recentCount = NEW_SAMPLES;
  }

  for (int i = recentStart; i < SAMPLE_BUFFER_SIZE; i++) {
    if (millis() - captureStartMs >= CAPTURE_DURATION_MS) {
      // The partial window is intentionally not passed to the algorithm.
      stopCapture();
      return;
    }

    if (!readSensor(i)) {
      Serial.println("ERROR: Timed out waiting for MAX30102 sample.");
      Serial.println("Check sensor power, I2C connections, and USB.");
      stopCapture();
      return;
    }
  }

  bufferReady = true;
  const unsigned long collectionTime = millis() - collectionStartMs;

  // SpO2/HR require a complete rolling 100-sample window.
  calculateVitals();
  Serial.print("WINDOW_END_MS,");
  Serial.println(millis() - captureStartMs);
  printVitals(recentStart, recentCount, collectionTime);
}
