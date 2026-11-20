
#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ESP32 I2C connections (classic ESP32 DevKit)
#define SDA_PIN 21
#define SCL_PIN 22

MAX30105 particleSensor;

// Store 100 red and infrared samples
const int BUFFER_SIZE = 100;

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

// Calculated health measurements
int32_t heartRate = 0;
int32_t spo2 = 0;

// Flags indicating whether calculations are valid
int8_t validHeartRate = 0;
int8_t validSPO2 = 0;

// Read one sample from sensor
void readSensor(int index) {

  // Wait for a new sample
  while (!particleSensor.available()) {
    particleSensor.check();
  }

  // Save the red and infrared readings
  redBuffer[index] = particleSensor.getRed();
  irBuffer[index] = particleSensor.getIR();

  // Advance to next sample
  particleSensor.nextSample();
}

// Calculate heart rate and blood oxygen
void calculateVitals() {

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    BUFFER_SIZE,
    redBuffer,
    &spo2,
    &validSPO2,
    &heartRate,
    &validHeartRate
  );
}

// Display results in Serial Monitor
void printVitals() {

  Serial.println("----------------------------");

  Serial.print("IR Value: ");
  Serial.println(irBuffer[BUFFER_SIZE - 1]);

  Serial.print("Heart Rate: ");

  if (validHeartRate && heartRate > 0) {
    Serial.print(heartRate);
    Serial.println(" BPM");
  } else {
    Serial.println("Invalid reading");
  }

  Serial.print("Blood Oxygen: ");

  if (validSPO2 && spo2 >= 0 && spo2 <= 100) {
    Serial.print(spo2);
    Serial.println(" %");
  } else {
    Serial.println("Invalid reading");
  }

  Serial.println("----------------------------");
}

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println("Initializing MAX30102...");

  // Initialize ESP32 I2C communication
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize MAX30102 sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {

    Serial.println("MAX30102 not found!");
    Serial.println("Check wiring and power.");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("MAX30102 initialized.");

  // Sensor configuration
  byte ledBrightness = 60;
  byte sampleAverage = 4;
  byte ledMode = 2;        // Red and IR LEDs
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

  // MAX30102 does not need the green LED
  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("Place finger on sensor.");
  Serial.println("Collecting initial samples...");

  // Collect initial 100 samples
  for (int i = 0; i < BUFFER_SIZE; i++) {
    readSensor(i);
  }

  // Calculate initial values
  calculateVitals();

  Serial.println("Measurement started.");
}

void loop() {

  // Keep the most recent 75 samples
  for (int i = 25; i < BUFFER_SIZE; i++) {

    redBuffer[i - 25] = redBuffer[i];
    irBuffer[i - 25] = irBuffer[i];
  }

  // Collect 25 new samples
  for (int i = 75; i < BUFFER_SIZE; i++) {
    readSensor(i);
  }

  // Recalculate measurements
  calculateVitals();

  // Display values
  printVitals();
}
