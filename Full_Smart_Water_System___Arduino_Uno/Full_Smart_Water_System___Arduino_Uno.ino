/*
  Full Smart Water System - Arduino Uno

  Sensors:
    pH sensor analog        -> A0
    EC sensor analog        -> A1
    MQ135 analog            -> A2
    YF-S201 flow signal     -> D2
    Float switch            -> D5
    DHT11 data (box temp)   -> D6
    BH1750 DAT/SDA          -> A4
    BH1750 SCL              -> A5
    DS18B20 data (water temp) -> D11

  Relays (6 total):
    Fan relay               -> D3  automatic from DHT11 temperature
    Light relay 1           -> D4  manual or timer period
    Light relay 2           -> D7  manual or timer period
    Light relay 3           -> D8  manual or timer period
    Solenoid valve relay    -> D9  automatic from float switch or manual
    Pump relay              -> D10 manual

  Serial baud: 115200

  Relay logic assumes active LOW relay module:
    LOW  = relay ON
    HIGH = relay OFF
*/

#include <Wire.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Servo.h>

// ---------- Pins ----------
const byte PH_PIN = A0;
const byte EC_PIN = A1;
const byte MQ135_PIN = A2;
const byte FLOW_PIN = 2;
const byte FAN_RELAY_PIN = 3;
const byte LIGHT1_RELAY_PIN = 4;
const byte FLOAT_PIN = 5;
const byte DHT_PIN = 6;
const byte LIGHT2_RELAY_PIN = 7;
const byte LIGHT3_RELAY_PIN = 8;
const byte SOLENOID_RELAY_PIN = 9;
const byte PUMP_RELAY_PIN = 10;
const byte DS18B20_PIN = 11;
const byte FEEDER_SERVO_PIN = 12;

// ---------- Relay config ----------
const byte RELAY_ON = LOW;
const byte RELAY_OFF = HIGH;

// ---------- DHT11 ----------
const byte DHT_TYPE = DHT11;
DHT dht(DHT_PIN, DHT_TYPE);
const float DHT_TEMP_OFFSET_C = -2.15; // From previous DHT11 calibration
const float FAN_ON_TEMP_C = 32.0;
const float FAN_OFF_TEMP_C = 29.0;

// ---------- DS18B20 water temperature ----------
OneWire oneWire(DS18B20_PIN);
DallasTemperature waterTempSensor(&oneWire);

// ---------- 360 servo fish feeder ----------
Servo feederServo;
const int SERVO_STOP_US = 1500;
const int SERVO_RUN_US = 2000;
bool feederAuto = true;
bool feederRunning = false;
unsigned long feederRunStartedMs = 0;
unsigned long feederDurationMs = 2480; // Increased about 100 degrees from 2000 ms setting
unsigned long feederIntervalMs = 12UL * 60UL * 60UL * 1000UL; // Twice per day
unsigned long nextFeedMs = 0;

// ---------- pH calibration ----------
const float PH_V3_7 = 1.471;
const float PH_3_7 = 3.70;
const float PH_V7 = 2.052;
const float PH_7 = 7.00;
const float PH_V9_5 = 2.181;
const float PH_9_5 = 9.50;

// ---------- EC two-point calibration ----------
const float EC_LOW_US_CM = 1413.0;
const float EC_LOW_VOLTAGE = 0.313;
const float EC_HIGH_US_CM = 12880.0;
const float EC_HIGH_VOLTAGE = 1.939;

// ---------- MQ135 ----------
const int BAD_AIR_RAW_THRESHOLD = 650;

// ---------- YF-S201 ----------
const float FLOW_CALIBRATION_FACTOR = 7.5;
volatile unsigned long flowPulses = 0;

// ---------- Uno ADC ----------
const float ADC_REF_VOLTAGE = 5.0;

// ---------- BH1750 ----------
const byte BH1750_ADDR_LOW = 0x23;
const byte BH1750_ADDR_HIGH = 0x5C;
const byte BH1750_POWER_ON = 0x01;
const byte BH1750_CONT_HIGH_RES = 0x10;
byte bh1750Address = 0;

// ---------- Modes and schedules ----------
bool fanAuto = true;
bool solenoidAuto = true;
bool lightAuto[3] = {false, false, false};
unsigned long lightOnMs[3] = {0, 0, 0};
unsigned long lightOffMs[3] = {0, 0, 0};
unsigned long lightCycleStartMs[3] = {0, 0, 0};

unsigned long lastSampleMs = 0;
String inputLine;

void flowISR() {
  flowPulses++;
}

float analogVoltage(byte pin) {
  long total = 0;
  const int samples = 10;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return (total / (float)samples) * (ADC_REF_VOLTAGE / 1023.0);
}

float calculatePH(float voltage) {
  float mLow = (PH_7 - PH_3_7) / (PH_V7 - PH_V3_7);
  float mHigh = (PH_9_5 - PH_7) / (PH_V9_5 - PH_V7);
  if (voltage <= PH_V7) {
    return PH_3_7 + mLow * (voltage - PH_V3_7);
  }
  return PH_7 + mHigh * (voltage - PH_V7);
}

float calculateEC(float voltage) {
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
  Wire.write(BH1750_POWER_ON);
  if (Wire.endTransmission() != 0) return false;
  delay(10);

  Wire.beginTransmission(address);
  Wire.write(BH1750_CONT_HIGH_RES);
  return Wire.endTransmission() == 0;
}

void detectBH1750() {
  if (i2cExists(BH1750_ADDR_LOW)) {
    bh1750Address = BH1750_ADDR_LOW;
    startBH1750(bh1750Address);
  } else if (i2cExists(BH1750_ADDR_HIGH)) {
    bh1750Address = BH1750_ADDR_HIGH;
    startBH1750(bh1750Address);
  } else {
    bh1750Address = 0;
  }
}

float readLux() {
  if (bh1750Address == 0) {
    detectBH1750();
    if (bh1750Address == 0) return -1;
  }

  startBH1750(bh1750Address);
  delay(180);
  Wire.requestFrom((int)bh1750Address, 2);
  if (Wire.available() < 2) {
    bh1750Address = 0;
    return -1;
  }

  uint16_t raw = Wire.read();
  raw <<= 8;
  raw |= Wire.read();
  return raw / 1.2;
}

void setRelay(byte pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

bool relayIsOn(byte pin) {
  return digitalRead(pin) == RELAY_ON;
}

void updateLightSchedules() {
  const byte pins[3] = {LIGHT1_RELAY_PIN, LIGHT2_RELAY_PIN, LIGHT3_RELAY_PIN};
  unsigned long now = millis();

  for (byte i = 0; i < 3; i++) {
    if (!lightAuto[i]) continue;
    unsigned long cycle = lightOnMs[i] + lightOffMs[i];
    if (cycle == 0) continue;
    unsigned long position = (now - lightCycleStartMs[i]) % cycle;
    setRelay(pins[i], position < lightOnMs[i]);
  }
}

void updateFan(float tempC) {
  if (!fanAuto || isnan(tempC)) return;
  if (tempC >= FAN_ON_TEMP_C) setRelay(FAN_RELAY_PIN, true);
  if (tempC <= FAN_OFF_TEMP_C) setRelay(FAN_RELAY_PIN, false);
}

void updateSolenoid() {
  if (!solenoidAuto) return;
  bool lowWater = digitalRead(FLOAT_PIN) == HIGH;
  setRelay(SOLENOID_RELAY_PIN, lowWater);
}

void printHelp() {
  Serial.println(F("# Commands:"));
  Serial.println(F("# FAN:AUTO | FAN:ON | FAN:OFF"));
  Serial.println(F("# SOL:AUTO | SOL:ON | SOL:OFF"));
  Serial.println(F("# PUMP:ON | PUMP:OFF"));
  Serial.println(F("# L1:ON | L1:OFF | L1:AUTO:onSeconds:offSeconds"));
  Serial.println(F("# L2:ON | L2:OFF | L2:AUTO:onSeconds:offSeconds"));
  Serial.println(F("# L3:ON | L3:OFF | L3:AUTO:onSeconds:offSeconds"));
  Serial.println(F("# FEED:NOW | FEED:AUTO | FEED:OFF | FEED:SET:intervalSeconds:durationMs"));
}

void startFeeder() {
  feederServo.writeMicroseconds(SERVO_RUN_US);
  feederRunning = true;
  feederRunStartedMs = millis();
}

void stopFeeder() {
  feederServo.writeMicroseconds(SERVO_STOP_US);
  feederRunning = false;
}

void updateFeeder() {
  unsigned long now = millis();

  if (feederRunning && now - feederRunStartedMs >= feederDurationMs) {
    stopFeeder();
    nextFeedMs = now + feederIntervalMs;
  }

  if (feederAuto && !feederRunning && now >= nextFeedMs) {
    startFeeder();
  }
}

void handleLightCommand(byte index, String action) {
  const byte pins[3] = {LIGHT1_RELAY_PIN, LIGHT2_RELAY_PIN, LIGHT3_RELAY_PIN};
  action.trim();
  action.toUpperCase();

  if (action == "ON") {
    lightAuto[index] = false;
    setRelay(pins[index], true);
    return;
  }
  if (action == "OFF") {
    lightAuto[index] = false;
    setRelay(pins[index], false);
    return;
  }
  if (action.startsWith("AUTO:")) {
    int split = action.indexOf(':', 5);
    if (split <= 5) return;
    unsigned long onSec = action.substring(5, split).toInt();
    unsigned long offSec = action.substring(split + 1).toInt();
    lightOnMs[index] = onSec * 1000UL;
    lightOffMs[index] = offSec * 1000UL;
    lightCycleStartMs[index] = millis();
    lightAuto[index] = true;
  }
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "HELP") printHelp();
  else if (cmd == "FAN:AUTO") fanAuto = true;
  else if (cmd == "FAN:ON") { fanAuto = false; setRelay(FAN_RELAY_PIN, true); }
  else if (cmd == "FAN:OFF") { fanAuto = false; setRelay(FAN_RELAY_PIN, false); }
  else if (cmd == "SOL:AUTO") solenoidAuto = true;
  else if (cmd == "SOL:ON") { solenoidAuto = false; setRelay(SOLENOID_RELAY_PIN, true); }
  else if (cmd == "SOL:OFF") { solenoidAuto = false; setRelay(SOLENOID_RELAY_PIN, false); }
  else if (cmd == "PUMP:ON") setRelay(PUMP_RELAY_PIN, true);
  else if (cmd == "PUMP:OFF") setRelay(PUMP_RELAY_PIN, false);
  else if (cmd == "FEED:NOW") startFeeder();
  else if (cmd == "FEED:AUTO") { feederAuto = true; nextFeedMs = millis() + feederIntervalMs; }
  else if (cmd == "FEED:OFF") { feederAuto = false; stopFeeder(); }
  else if (cmd.startsWith("FEED:SET:")) {
    int split = cmd.indexOf(':', 9);
    if (split > 9) {
      unsigned long intervalSec = cmd.substring(9, split).toInt();
      unsigned long duration = cmd.substring(split + 1).toInt();
      if (intervalSec > 0) feederIntervalMs = intervalSec * 1000UL;
      if (duration > 0) feederDurationMs = duration;
      nextFeedMs = millis() + feederIntervalMs;
    }
  }
  else if (cmd.startsWith("L1:")) handleLightCommand(0, cmd.substring(3));
  else if (cmd.startsWith("L2:")) handleLightCommand(1, cmd.substring(3));
  else if (cmd.startsWith("L3:")) handleLightCommand(2, cmd.substring(3));
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(inputLine);
      inputLine = "";
    } else {
      inputLine += c;
      if (inputLine.length() > 80) inputLine = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  dht.begin();
  waterTempSensor.begin();
  feederServo.attach(FEEDER_SERVO_PIN);
  stopFeeder();

  pinMode(FLOW_PIN, INPUT_PULLUP);
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  pinMode(LIGHT1_RELAY_PIN, OUTPUT);
  pinMode(LIGHT2_RELAY_PIN, OUTPUT);
  pinMode(LIGHT3_RELAY_PIN, OUTPUT);
  pinMode(SOLENOID_RELAY_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);

  setRelay(FAN_RELAY_PIN, false);
  setRelay(LIGHT1_RELAY_PIN, false);
  setRelay(LIGHT2_RELAY_PIN, false);
  setRelay(LIGHT3_RELAY_PIN, false);
  setRelay(SOLENOID_RELAY_PIN, false);
  setRelay(PUMP_RELAY_PIN, false);

  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, FALLING);
  detectBH1750();
  lastSampleMs = millis();
  nextFeedMs = millis() + feederIntervalMs;
  printHelp();
}

void loop() {
  readSerialCommands();
  updateLightSchedules();
  updateSolenoid();
  updateFeeder();

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
  float tempRaw = dht.readTemperature();
  float humidity = dht.readHumidity();
  float boxTempC = isnan(tempRaw) ? NAN : tempRaw + DHT_TEMP_OFFSET_C;
  waterTempSensor.requestTemperatures();
  float waterTempC = waterTempSensor.getTempCByIndex(0);
  if (waterTempC == DEVICE_DISCONNECTED_C) waterTempC = NAN;
  float lux = readLux();

  updateFan(boxTempC);

  Serial.print(F("{"));
  Serial.print(F("\"ph\":")); Serial.print(calculatePH(phVoltage), 2);
  Serial.print(F(",\"ec_us\":")); Serial.print(calculateEC(ecVoltage), 0);
  Serial.print(F(",\"air\":\"")); Serial.print(mqRaw >= BAD_AIR_RAW_THRESHOLD ? F("BAD") : F("GOOD")); Serial.print(F("\""));
  Serial.print(F(",\"water_level\":\"")); Serial.print(digitalRead(FLOAT_PIN) == LOW ? F("FULL") : F("LOW")); Serial.print(F("\""));
  Serial.print(F(",\"flow\":\"")); Serial.print(pulses > 0 ? F("YES") : F("NO")); Serial.print(F("\""));
  Serial.print(F(",\"box_temp_c\":")); if (isnan(boxTempC)) Serial.print(F("null")); else Serial.print(boxTempC, 1);
  Serial.print(F(",\"humidity\":")); if (isnan(humidity)) Serial.print(F("null")); else Serial.print(humidity, 1);
  Serial.print(F(",\"water_temp_c\":")); if (isnan(waterTempC)) Serial.print(F("null")); else Serial.print(waterTempC, 1);
  Serial.print(F(",\"light\":\""));
  if (lux < 0) Serial.print(F("UNKNOWN"));
  else if (lux < 10) Serial.print(F("NO LIGHT"));
  else if (lux < 1000) Serial.print(F("NORMAL"));
  else Serial.print(F("BRIGHT"));
  Serial.print(F("\""));
  Serial.print(F(",\"lux\":")); if (lux < 0) Serial.print(F("null")); else Serial.print(lux, 1);
  Serial.print(F(",\"fan\":\"")); Serial.print(relayIsOn(FAN_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"fan_auto\":")); Serial.print(fanAuto ? F("true") : F("false"));
  Serial.print(F(",\"light1\":\"")); Serial.print(relayIsOn(LIGHT1_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"light2\":\"")); Serial.print(relayIsOn(LIGHT2_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"light3\":\"")); Serial.print(relayIsOn(LIGHT3_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"solenoid\":\"")); Serial.print(relayIsOn(SOLENOID_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"solenoid_auto\":")); Serial.print(solenoidAuto ? F("true") : F("false"));
  Serial.print(F(",\"pump\":\"")); Serial.print(relayIsOn(PUMP_RELAY_PIN) ? F("ON") : F("OFF")); Serial.print(F("\""));
  Serial.print(F(",\"feeder\":\"")); Serial.print(feederRunning ? F("FEEDING") : F("IDLE")); Serial.print(F("\""));
  Serial.print(F(",\"feeder_auto\":")); Serial.print(feederAuto ? F("true") : F("false"));
  Serial.println(F("}"));
}