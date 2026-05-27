/*
  Smart Aquaponics Dashboard - Arduino Uno firmware

  Arduino Uno sends one JSON line per second to Raspberry Pi over USB serial.
  Raspberry Pi dashboard can send commands back:
    PUMP:ON
    PUMP:OFF
    LIGHT:ON
    LIGHT:OFF

  Pin map:
    pH analog       -> A0
    EC analog       -> A1
    MQ135 analog    -> A2
    Float switch    -> D5  (other wire to GND, uses INPUT_PULLUP)
    DHT11 data      -> D6
    YF-S201 signal  -> D2  (interrupt pin)
    BH1750 SDA/DAT  -> A4
    BH1750 SCL      -> A5
    Pump relay      -> D8
    Light relay     -> D9

  Relay logic assumes LOW = relay ON. Change RELAY_ON/RELAY_OFF if your relay is active HIGH.
*/

#include <Wire.h>
#include <DHT.h>

const int PH_PIN = A0;
const int EC_PIN = A1;
const int MQ135_PIN = A2;
const int FLOW_PIN = 2;
const int FLOAT_PIN = 5;
const int DHT_PIN = 6;
const int PUMP_RELAY_PIN = 8;
const int LIGHT_RELAY_PIN = 9;

const int DHT_TYPE = DHT11;
DHT dht(DHT_PIN, DHT_TYPE);

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

volatile unsigned long flowPulses = 0;
float totalLiters = 0.0;
unsigned long lastSampleMs = 0;

// pH calibration from your ESP test.
const float PH_V3_7 = 1.471;
const float PH_3_7 = 3.70;
const float PH_V7 = 2.052;
const float PH_7 = 7.00;
const float PH_V9_5 = 2.181;
const float PH_9_5 = 9.50;

// EC two-point calibration from your ESP test.
const float EC_LOW_US_CM = 1413.0;
const float EC_LOW_VOLTAGE = 0.313;
const float EC_HIGH_US_CM = 12880.0;
const float EC_HIGH_VOLTAGE = 1.939;

// MQ135 threshold from your ESP test. Uno ADC uses 0-1023 too.
const int BAD_AIR_RAW_THRESHOLD = 650;

// YF-S201 standard factor.
const float FLOW_CALIBRATION_FACTOR = 7.5;

// Arduino Uno ADC reference.
const float ADC_REF_VOLTAGE = 5.0;

byte bh1750Address = 0;

void flowISR() {
  flowPulses++;
}

float analogVoltage(int pin) {
  long total = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return (total / (float)samples) * (ADC_REF_VOLTAGE / 1023.0);
}

float readPH(float voltage) {
  float mLow = (PH_7 - PH_3_7) / (PH_V7 - PH_V3_7);
  float mHigh = (PH_9_5 - PH_7) / (PH_V9_5 - PH_V7);
  if (voltage <= PH_V7) {
    return PH_3_7 + mLow * (voltage - PH_V3_7);
  }
  return PH_7 + mHigh * (voltage - PH_V7);
}

float readEC(float voltage) {
  float slope = (EC_HIGH_US_CM - EC_LOW_US_CM) / (EC_HIGH_VOLTAGE - EC_LOW_VOLTAGE);
  float ec = EC_LOW_US_CM + slope * (voltage - EC_LOW_VOLTAGE);
  return ec < 0 ? 0 : ec;
}

bool i2cExists(byte address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool startBH1750(byte address) {
  Wire.beginTransmission(address);
  Wire.write(0x01); // power on
  if (Wire.endTransmission() != 0) return false;
  delay(10);

  Wire.beginTransmission(address);
  Wire.write(0x10); // continuous high-res mode
  return Wire.endTransmission() == 0;
}

void detectBH1750() {
  if (i2cExists(0x23)) {
    bh1750Address = 0x23;
    startBH1750(bh1750Address);
  } else if (i2cExists(0x5C)) {
    bh1750Address = 0x5C;
    startBH1750(bh1750Address);
  } else {
    bh1750Address = 0;
  }
}

float readLux() {
  if (bh1750Address == 0) return -1;
  Wire.requestFrom((int)bh1750Address, 2);
  if (Wire.available() < 2) return -1;
  uint16_t raw = Wire.read();
  raw <<= 8;
  raw |= Wire.read();
  return raw / 1.2;
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd == "PUMP:ON") digitalWrite(PUMP_RELAY_PIN, RELAY_ON);
  else if (cmd == "PUMP:OFF") digitalWrite(PUMP_RELAY_PIN, RELAY_OFF);
  else if (cmd == "LIGHT:ON") digitalWrite(LIGHT_RELAY_PIN, RELAY_ON);
  else if (cmd == "LIGHT:OFF") digitalWrite(LIGHT_RELAY_PIN, RELAY_OFF);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  dht.begin();

  pinMode(FLOAT_PIN, INPUT_PULLUP);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, RELAY_OFF);
  digitalWrite(LIGHT_RELAY_PIN, RELAY_OFF);

  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, FALLING);
  detectBH1750();
  lastSampleMs = millis();
}

void loop() {
  while (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  unsigned long now = millis();
  if (now - lastSampleMs < 1000) return;

  float elapsedSeconds = (now - lastSampleMs) / 1000.0;
  lastSampleMs = now;

  noInterrupts();
  unsigned long pulses = flowPulses;
  flowPulses = 0;
  interrupts();

  float phVoltage = analogVoltage(PH_PIN);
  float ecVoltage = analogVoltage(EC_PIN);
  int mqRaw = analogRead(MQ135_PIN);
  float flowLMin = (pulses / elapsedSeconds) / FLOW_CALIBRATION_FACTOR;
  totalLiters += (flowLMin / 60.0) * elapsedSeconds;
  float tempC = dht.readTemperature();
  float humidity = dht.readHumidity();
  float lux = readLux();

  Serial.print("{\"ph\":"); Serial.print(readPH(phVoltage), 2);
  Serial.print(",\"ph_voltage\":"); Serial.print(phVoltage, 3);
  Serial.print(",\"ec_us\":"); Serial.print(readEC(ecVoltage), 0);
  Serial.print(",\"ec_voltage\":"); Serial.print(ecVoltage, 3);
  Serial.print(",\"air\":\""); Serial.print(mqRaw >= BAD_AIR_RAW_THRESHOLD ? "BAD" : "GOOD"); Serial.print("\"");
  Serial.print(",\"mq_raw\":"); Serial.print(mqRaw);
  Serial.print(",\"float\":\""); Serial.print(digitalRead(FLOAT_PIN) == LOW ? "FILLED" : "LOW WATER"); Serial.print("\"");
  Serial.print(",\"flow_l_min\":"); Serial.print(flowLMin, 2);
  Serial.print(",\"total_l\":"); Serial.print(totalLiters, 3);
  Serial.print(",\"temp_c\":"); if (isnan(tempC)) Serial.print("null"); else Serial.print(tempC, 1);
  Serial.print(",\"humidity\":"); if (isnan(humidity)) Serial.print("null"); else Serial.print(humidity, 1);
  Serial.print(",\"lux\":"); if (lux < 0) Serial.print("null"); else Serial.print(lux, 1);
  Serial.print(",\"pump\":\""); Serial.print(digitalRead(PUMP_RELAY_PIN) == RELAY_ON ? "ON" : "OFF"); Serial.print("\"");
  Serial.print(",\"light\":\""); Serial.print(digitalRead(LIGHT_RELAY_PIN) == RELAY_ON ? "ON" : "OFF"); Serial.print("\"");
  Serial.println("}");
}
