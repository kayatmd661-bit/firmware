/*
 * JARVIS AC FAN + WATER CONTROLLER
 * ESP32 / Arduino-ESP32 3.x
 *
 * IMPORTANT:
 * - Fan speed is AC phase-angle TRIAC control, NOT PWM.
 * - ESP32 is connected only to the LOW-VOLTAGE isolated side of the
 *   zero-cross detector and optotriac drivers.
 * - Use a RANDOM-PHASE optotriac such as MOC3021/MOC3023.
 * - Do NOT use MOC3062/MOC3063 for phase-angle control.
 * - Mains-side TRIAC/fuse/MOV/snubber/heatsink/creepage must be designed
 *   for the actual fan current and mains voltage by a qualified person.
 *
 * Preserved application functions from the supplied firmware:
 * - 4 fans / ON-OFF / 0-100% speed persistence
 * - MQTT topics
 * - Home Assistant MQTT Discovery
 * - DHT22 / BMP280 / PIR / ultrasonic water level / PZEM
 * - automatic water motor
 * - Preferences/NVS
 * - OTA
 * - WDT
 * - WiFiManager
 *
 * Changed only where required:
 * - LEDC PWM fan control -> zero-cross synchronized phase-angle TRIAC control
 * - blocking WiFiManager/MQTT reconnect -> non-blocking operation
 * - hard-coded MQTT settings -> Preferences + dashboard configuration
 * - modernized HA discovery payloads
 * - status LED: BLINKING until WiFi + MQTT are both connected, SOLID when
 *   both connections are established; on every reset/boot it starts blinking.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <PZEM004Tv30.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include "esp_timer.h"

// ========================= PIN CONFIGURATION =========================

// Existing application pins
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

#define FAN1_TRIAC_PIN 13
#define FAN2_TRIAC_PIN 14
#define FAN3_TRIAC_PIN 18
#define FAN4_TRIAC_PIN 19

// Four isolated zero-cross detector inputs.
// GPIO34-36 are input-only; use an external bias as required by the
// particular zero-cross module. GPIO23 is a normal digital input.
#define FAN1_ZC_PIN 23
#define FAN2_ZC_PIN 34
#define FAN3_ZC_PIN 35
#define FAN4_ZC_PIN 36

#define DHTPIN 27
#define PIRPIN 26
#define TRIG_PIN 32
#define ECHO_PIN 33
#define MOTOR_PIN 25
#define DHTTYPE DHT22

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// Built-in / board status LED.
// Default ESP32 Dev Module LED is commonly GPIO2.
// If your board uses another LED pin, change only this definition.
#define STATUS_LED_PIN 2
#define STATUS_LED_ACTIVE_HIGH true

// LED behavior:
//   - BLINKING: boot/reset, WiFi disconnected, or MQTT disconnected
//   - SOLID:    WiFi connected AND MQTT connected
#define STATUS_LED_BLINK_MS 500UL

#define WDT_TIMEOUT 5

// ========================= AC CONTROL ================================

#define MAINS_FREQUENCY_HZ 50
#define HALF_CYCLE_US (1000000UL / (2UL * MAINS_FREQUENCY_HZ))

// Timer resolution. 20 us gives adequate phase resolution while keeping
// ISR load modest.
#define TRIAC_TIMER_TICK_US 20

// Gate pulse width. Tune for the actual optotriac + power TRIAC.
#define TRIAC_GATE_PULSE_US 120

// The following define the usable phase-angle window.
// 100% is close to the beginning of the half cycle.
// 1% is close to the end, but a safety margin is retained.
#define TRIAC_MIN_FIRE_DELAY_US TRIAC_EARLY_MARGIN_US
#define TRIAC_MAX_FIRE_DELAY_US (HALF_CYCLE_US - TRIAC_LATE_MARGIN_US)

// Minimum user speed at which a fan is allowed to fire.
// 0 = off. Speeds below this are clamped to this value.
#define FAN_MIN_SPEED_DEFAULT 20

// ========================= UNIVERSAL ZC / OPTO ADAPTER =============
// The firmware does NOT depend on one specific detector IC.  Each fan can
// select a detector profile below and the selection is stored in NVS.
//
// APPROVED ZERO-CROSS DETECTOR CLASSES / PARTS:
//   ZC_PROFILE_AC_OPTO_RISING:
//     H11AA1, IL250, IL252, LTV-814/LTV-824/LTV-844 (AC-input versions),
//     when wired as the standard isolated phototransistor + pull-up detector.
//   ZC_PROFILE_AC_OPTO_FALLING:
//     Same AC-input optocoupler family when the low-voltage interface is
//     intentionally inverted.
//   ZC_PROFILE_MODULE_RISING / FALLING:
//     Commercial isolated zero-cross modules whose OUTPUT is ESP32-safe
//     digital HIGH/LOW.
//   ZC_PROFILE_PC817_RISING / FALLING:
//     PC817-based isolated AC detector circuits using the proper mains-side
//     rectifier/resistor network. PC817 itself is NOT connected directly to AC.
//
// IMPORTANT: a bare optocoupler is not automatically a zero-cross detector.
// The mains-side current limiting/rectifier and the isolated low-voltage
// pull-up/output stage must match the selected part.
//
// TRIAC DRIVER OPTOCOUPLER LIST (RANDOM-PHASE ONLY):
//   MOC3020/MOC3021/MOC3022/MOC3023
//   MOC3051/MOC3052
//   Vishay VOT8121 family / equivalent RANDOM-PHASE phototriac driver
// Do NOT use zero-cross phototriac drivers such as MOC306x/VOT8024 for
// phase-angle firing. They are suitable for zero-cross switching, not this
// speed-control method.

// For 120/127 VAC, MOC302x-class parts are suitable within their ratings.
// For 220/230/240 VAC, use a driver/power TRIAC combination with adequate
// repetitive blocking voltage margin for the actual mains. MOC3051/3052 or
// an equivalent 600/800 V random-phase driver is a safer listed choice.

enum ZcEdgeMode : uint8_t {
  ZC_EDGE_RISING = 0,
  ZC_EDGE_FALLING = 1
};

enum ZcProfile : uint8_t {
  ZC_PROFILE_AC_OPTO_RISING = 0,
  ZC_PROFILE_AC_OPTO_FALLING = 1,
  ZC_PROFILE_MODULE_RISING = 2,
  ZC_PROFILE_MODULE_FALLING = 3,
  ZC_PROFILE_PC817_RISING = 4,
  ZC_PROFILE_PC817_FALLING = 5
};

// Default hardware profile for each fan. Change from the dashboard after
// first boot; the values are persisted in Preferences/NVS.
#define ZC_DEFAULT_PROFILE_FAN1 ZC_PROFILE_AC_OPTO_RISING
#define ZC_DEFAULT_PROFILE_FAN2 ZC_PROFILE_AC_OPTO_RISING
#define ZC_DEFAULT_PROFILE_FAN3 ZC_PROFILE_AC_OPTO_RISING
#define ZC_DEFAULT_PROFILE_FAN4 ZC_PROFILE_AC_OPTO_RISING

#define ZC_MIN_EDGE_SPACING_US 2500UL
#define ZC_TIMING_OFFSET_US_DEFAULT 0
#define AUTO_MEASURE_MAINS_FREQUENCY true
#define ZC_MIN_HALF_CYCLE_US 7000UL
#define ZC_MAX_HALF_CYCLE_US 12000UL
#define TRIAC_EARLY_MARGIN_US 250
#define TRIAC_LATE_MARGIN_US 500

// ========================= OBJECTS ==================================

PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
WiFiClient espClient;
PubSubClient client(espClient);
Preferences pref;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;
WebServer server(80);
WiFiManager wm;

// WiFiManager custom parameters must remain alive while the non-blocking
// portal is running. Keeping them at file scope fixes the save lifecycle
// without changing the portal architecture.
WiFiManagerParameter p_mqtt_host("mqtt_host", "MQTT Host", "", 64);
WiFiManagerParameter p_mqtt_port("mqtt_port", "MQTT Port", "1883", 6);
WiFiManagerParameter p_mqtt_user("mqtt_user", "MQTT Username", "esp32", 64);
WiFiManagerParameter p_mqtt_pass("mqtt_pass", "MQTT Password", "", 64, "type='password'");
WiFiManagerParameter p_ha_host("ha_host", "Home Assistant Host/IP", "", 64);
WiFiManagerParameter p_ha_port("ha_port", "Home Assistant Port", "8123", 6);
WiFiManagerParameter p_ha_name("ha_name", "HA Device Name", "Jarvis Fan System", 64);
WiFiManagerParameter p_ha_id("ha_id", "HA Device ID", "jarvis_fan_system_final", 64);
bool wifiManagerParametersAdded = false;

// ========================= CONFIGURATION =============================

String mqttHost = "homeassistant.local";
uint16_t mqttPort = 1883;
String mqttUser = "esp32";
String mqttPass = "12345678";

// Dashboard administrator credentials are deliberately separate from MQTT
// credentials. They are stored in Preferences/NVS and can be changed from
// the authenticated dashboard.
String adminUser = "admin";
String adminPass = "admin12345";

String haHost = "";
uint16_t haPort = 8123;
String haName = "Jarvis Fan System";
String haDeviceId = "jarvis_fan_system_final";

String fanNames[4] = {
  "স্মার্ট ফ্যান ১",
  "স্মার্ট ফ্যান ২",
  "স্মার্ট ফ্যান ৩",
  "স্মার্ট ফ্যান ৪"
};

String fanUIDs[4] = {"sf1", "sf2", "sf3", "sf4"};

int fanMinSpeed = FAN_MIN_SPEED_DEFAULT;
int fanMaxSpeed = 100;
int fanStartupSpeed = 100;

int tank_empty_dist = 110;
int tank_full_dist = 10;
int motor_start_pct = 15;
int motor_stop_pct = 98;

uint8_t zcProfile[4] = {
  ZC_DEFAULT_PROFILE_FAN1,
  ZC_DEFAULT_PROFILE_FAN2,
  ZC_DEFAULT_PROFILE_FAN3,
  ZC_DEFAULT_PROFILE_FAN4
};
int32_t zcTimingOffsetUs[4] = {
  ZC_TIMING_OFFSET_US_DEFAULT,
  ZC_TIMING_OFFSET_US_DEFAULT,
  ZC_TIMING_OFFSET_US_DEFAULT,
  ZC_TIMING_OFFSET_US_DEFAULT
};

bool configPortalRunning = false;
bool shouldSaveConfig = false;
bool restartRequested = false;

// ========================= STATE =====================================

volatile int fanSpeed[4] = {0, 0, 0, 0};
volatile bool fanEnabled[4] = {false, false, false, false};
volatile bool zcSeen[4] = {false, false, false, false};
volatile uint32_t lastZcUs[4] = {0, 0, 0, 0};
volatile uint32_t fireAtUs[4] = {0, 0, 0, 0};
volatile uint32_t gateOffAtUs[4] = {0, 0, 0, 0};
volatile bool gateHigh[4] = {false, false, false, false};
volatile bool firePending[4] = {false, false, false, false};
volatile uint32_t previousZcUs[4] = {0, 0, 0, 0};
volatile bool zcHavePeriod[4] = {false, false, false, false};
volatile uint32_t measuredHalfCycleUs = HALF_CYCLE_US;

hw_timer_t *triacTimer = nullptr;

float lastTemp = -1000;
float lastHum = -1000;
float lastVolt = -1000;
float lastWater = -1000;
float lastPressure = -1000;
float lastCurrent = -1000;
float lastPower = -1000;

bool lastPir = false;
bool lastMotorState = false;

unsigned long lastSensorRead = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastDiscovery = 0;

// Status LED state
bool statusLedState = false;
unsigned long lastStatusLedToggle = 0;

const char *availability_topic = "jarvis/status/availability";

// ========================= STATUS LED =================================

void writeStatusLed(bool on) {
  bool level = STATUS_LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
  statusLedState = on;
}

void statusLedTask() {
  // Solid ON only when BOTH WiFi and MQTT are connected.
  const bool systemConnected =
    (WiFi.status() == WL_CONNECTED) && client.connected();

  if (systemConnected) {
    if (!statusLedState) {
      writeStatusLed(true);
    }
    lastStatusLedToggle = millis();
    return;
  }

  // Any missing connection -> blink.
  if (millis() - lastStatusLedToggle >= STATUS_LED_BLINK_MS) {
    lastStatusLedToggle = millis();
    writeStatusLed(!statusLedState);
  }
}

// ========================= ISR ======================================

static inline uint32_t elapsedUs(uint32_t now, uint32_t then) {
  return (uint32_t)(now - then);
}

ZcEdgeMode zcProfileEdge(uint8_t profile) {
  switch (profile) {
    case ZC_PROFILE_AC_OPTO_FALLING:
    case ZC_PROFILE_MODULE_FALLING:
    case ZC_PROFILE_PC817_FALLING:
      return ZC_EDGE_FALLING;
    default:
      return ZC_EDGE_RISING;
  }
}

const char* zcProfileName(uint8_t profile) {
  switch (profile) {
    case ZC_PROFILE_AC_OPTO_RISING:  return "AC-OPTO/RISING";
    case ZC_PROFILE_AC_OPTO_FALLING: return "AC-OPTO/FALLING";
    case ZC_PROFILE_MODULE_RISING:   return "MODULE/RISING";
    case ZC_PROFILE_MODULE_FALLING:  return "MODULE/FALLING";
    case ZC_PROFILE_PC817_RISING:    return "PC817/RISING";
    case ZC_PROFILE_PC817_FALLING:   return "PC817/FALLING";
    default: return "UNKNOWN";
  }
}

uint8_t defaultZcProfile(uint8_t idx) {
  switch (idx) {
    case 0: return ZC_DEFAULT_PROFILE_FAN1;
    case 1: return ZC_DEFAULT_PROFILE_FAN2;
    case 2: return ZC_DEFAULT_PROFILE_FAN3;
    default: return ZC_DEFAULT_PROFILE_FAN4;
  }
}

void zcISR0();
void zcISR1();
void zcISR2();
void zcISR3();

void applyZcInterrupts() {
  const int pins[4] = {FAN1_ZC_PIN, FAN2_ZC_PIN, FAN3_ZC_PIN, FAN4_ZC_PIN};
  void (*isrs[4])() = {zcISR0, zcISR1, zcISR2, zcISR3};

  for (int i = 0; i < 4; i++) {
    detachInterrupt(digitalPinToInterrupt(pins[i]));
    int mode = (zcProfileEdge(zcProfile[i]) == ZC_EDGE_FALLING) ? FALLING : RISING;
    attachInterrupt(digitalPinToInterrupt(pins[i]), isrs[i], mode);
    previousZcUs[i] = 0;
    zcSeen[i] = false;
    zcHavePeriod[i] = false;
  }
}

void configureZcInput(uint8_t idx) {
  if (idx > 3) return;
  const int pins[4] = {FAN1_ZC_PIN, FAN2_ZC_PIN, FAN3_ZC_PIN, FAN4_ZC_PIN};
  pinMode(pins[idx], INPUT);
}

void IRAM_ATTR zcEvent(uint8_t idx) {
  uint32_t now = micros();
  if (idx > 3) return;

  if (previousZcUs[idx] != 0) {
    uint32_t period = (uint32_t)(now - previousZcUs[idx]);
    if (period < ZC_MIN_EDGE_SPACING_US) return;
    if (AUTO_MEASURE_MAINS_FREQUENCY && period >= ZC_MIN_HALF_CYCLE_US && period <= ZC_MAX_HALF_CYCLE_US) {
      measuredHalfCycleUs = (measuredHalfCycleUs * 3UL + period) / 4UL;
      zcHavePeriod[idx] = true;
    }
  }

  previousZcUs[idx] = now;
  lastZcUs[idx] = now;
  zcSeen[idx] = true;

  if (fanEnabled[idx]) {
    uint32_t halfCycle = AUTO_MEASURE_MAINS_FREQUENCY ? measuredHalfCycleUs : HALF_CYCLE_US;
    uint32_t minDelay = TRIAC_EARLY_MARGIN_US;
    uint32_t maxDelay = (halfCycle > TRIAC_LATE_MARGIN_US) ? halfCycle - TRIAC_LATE_MARGIN_US : halfCycle / 2;
    if (maxDelay <= minDelay) maxDelay = minDelay + 100;
    int speed = fanSpeed[idx];
    speed = speed < 1 ? 1 : (speed > 100 ? 100 : speed);
    uint32_t delayUs = minDelay + ((uint32_t)(100 - speed) * (maxDelay - minDelay)) / 99UL;
    int32_t corrected = (int32_t)delayUs + zcTimingOffsetUs[idx];
    if (corrected < 0) corrected = 0;
    fireAtUs[idx] = now + (uint32_t)corrected;
    firePending[idx] = true;
  } else {
    firePending[idx] = false;
  }
}

void IRAM_ATTR zcISR0() { zcEvent(0); }
void IRAM_ATTR zcISR1() { zcEvent(1); }
void IRAM_ATTR zcISR2() { zcEvent(2); }
void IRAM_ATTR zcISR3() { zcEvent(3); }

void IRAM_ATTR triacTimerISR() {
  uint32_t now = micros();

  for (int i = 0; i < 4; i++) {
    if (firePending[i] && (int32_t)(now - fireAtUs[i]) >= 0) {
      digitalWrite(i == 0 ? FAN1_TRIAC_PIN :
                   i == 1 ? FAN2_TRIAC_PIN :
                   i == 2 ? FAN3_TRIAC_PIN : FAN4_TRIAC_PIN, HIGH);
      gateHigh[i] = true;
      gateOffAtUs[i] = now + TRIAC_GATE_PULSE_US;
      firePending[i] = false;
    }

    if (gateHigh[i] && (int32_t)(now - gateOffAtUs[i]) >= 0) {
      digitalWrite(i == 0 ? FAN1_TRIAC_PIN :
                   i == 1 ? FAN2_TRIAC_PIN :
                   i == 2 ? FAN3_TRIAC_PIN : FAN4_TRIAC_PIN, LOW);
      gateHigh[i] = false;
    }
  }
}

// ========================= FAN CONTROL ===============================

uint32_t speedToDelayUs(int speedPercent) {
  speedPercent = constrain(speedPercent, 1, 100);
  uint32_t halfCycle = AUTO_MEASURE_MAINS_FREQUENCY ? measuredHalfCycleUs : HALF_CYCLE_US;
  uint32_t minDelay = TRIAC_EARLY_MARGIN_US;
  uint32_t maxDelay = (halfCycle > TRIAC_LATE_MARGIN_US) ? halfCycle - TRIAC_LATE_MARGIN_US : halfCycle / 2;
  if (maxDelay <= minDelay) maxDelay = minDelay + 100;
  uint32_t delayUs = minDelay + ((uint32_t)(100 - speedPercent) * (maxDelay - minDelay)) / 99UL;
  int32_t corrected = (int32_t)delayUs + (int32_t)ZC_TIMING_OFFSET_US_DEFAULT;
  return corrected < 0 ? 0UL : (uint32_t)corrected;
}

void applyFanHardware(int fanIdx, int speedPercent) {
  if (fanIdx < 0 || fanIdx > 3) return;

  speedPercent = constrain(speedPercent, 0, 100);

  noInterrupts();

  fanSpeed[fanIdx] = speedPercent;
  fanEnabled[fanIdx] = (speedPercent > 0);

  if (!fanEnabled[fanIdx]) {
    firePending[fanIdx] = false;
    gateHigh[fanIdx] = false;
  }

  interrupts();

  int triacPins[4] = {
    FAN1_TRIAC_PIN, FAN2_TRIAC_PIN, FAN3_TRIAC_PIN, FAN4_TRIAC_PIN
  };

  if (speedPercent == 0) {
    digitalWrite(triacPins[fanIdx], LOW);
  }
}

void setFanSpeed(int fanIdx, int speedPercent, bool save = true) {
  if (fanIdx < 0 || fanIdx > 3) return;

  speedPercent = constrain(speedPercent, 0, fanMaxSpeed);

  if (speedPercent > 0 && speedPercent < fanMinSpeed) {
    speedPercent = fanMinSpeed;
  }

  applyFanHardware(fanIdx, speedPercent);

  if (save) {
    pref.putInt((fanUIDs[fanIdx] + "_sp").c_str(), speedPercent);
  }

  if (client.connected()) {
    client.publish(
      ("jarvis/status/" + fanUIDs[fanIdx]).c_str(),
      (speedPercent > 0 ? "ON" : "OFF"),
      true
    );

    client.publish(
      ("jarvis/status/" + fanUIDs[fanIdx] + "/speed").c_str(),
      String(speedPercent).c_str(),
      true
    );
  }
}

void setFanState(int fanIdx, bool state, bool save = true) {
  if (fanIdx < 0 || fanIdx > 3) return;

  if (state) {
    int lastSpeed = pref.getInt(
      (fanUIDs[fanIdx] + "_sp").c_str(),
      fanStartupSpeed
    );

    if (lastSpeed <= 0) lastSpeed = fanStartupSpeed;
    setFanSpeed(fanIdx, lastSpeed, save);
  } else {
    setFanSpeed(fanIdx, 0, save);
  }
}

bool fanZeroCrossHealthy(int idx) {
  if (idx < 0 || idx > 3) return false;

  uint32_t last = lastZcUs[idx];
  if (!zcSeen[idx]) return false;

  return elapsedUs(micros(), last) < 100000UL;
}

// ========================= SENSOR ====================================

long getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return duration * 0.034 / 2;
}

// ========================= MQTT DISCOVERY ============================

String deviceJson() {
  return ",\"device\":{"
         "\"identifiers\":[\"" + haDeviceId + "\"],"
         "\"name\":\"" + haName + "\","
         "\"manufacturer\":\"Jarvis\","
         "\"model\":\"AC Fan & Water Controller\","
         "\"sw_version\":\"2026.10\""
         "}";
}

void publishDiscovery(const String &topic, const String &payload) {
  if (!client.connected()) return;

  bool ok = client.publish(topic.c_str(), payload.c_str(), true);

  if (!ok) {
    Serial.print("Discovery publish failed: ");
    Serial.println(topic);
  }
}

void sendDiscovery() {
  String dev = deviceJson();

  for (int i = 0; i < 4; i++) {
    String configTopic = "homeassistant/fan/" + fanUIDs[i] + "/config";

    String payload =
      "{"
      "\"name\":\"" + fanNames[i] + "\","
      "\"unique_id\":\"" + fanUIDs[i] + "_2026\","
      "\"state_topic\":\"jarvis/status/" + fanUIDs[i] + "\","
      "\"command_topic\":\"jarvis/" + fanUIDs[i] + "/cmd\","
      "\"percentage_state_topic\":\"jarvis/status/" + fanUIDs[i] + "/speed\","
      "\"percentage_command_topic\":\"jarvis/" + fanUIDs[i] + "/speed/cmd\","
      "\"availability_topic\":\"" + String(availability_topic) + "\","
      "\"payload_available\":\"online\","
      "\"payload_not_available\":\"offline\","
      "\"payload_on\":\"ON\","
      "\"payload_off\":\"OFF\","
      "\"percentage_command_template\":\"{{ value | int }}\","
      "\"percentage_value_template\":\"{{ value | int }}\"" +
      dev +
      "}";

    publishDiscovery(configTopic, payload);
  }

  publishDiscovery(
    "homeassistant/sensor/jarvis_temp/config",
    "{\"name\":\"তাপমাত্রা\","
    "\"unique_id\":\"j_temp_2026\","
    "\"state_topic\":\"jarvis/sensor/temp\","
    "\"unit_of_measurement\":\"°C\","
    "\"device_class\":\"temperature\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_hum/config",
    "{\"name\":\"আর্দ্রতা\","
    "\"unique_id\":\"j_hum_2026\","
    "\"state_topic\":\"jarvis/sensor/hum\","
    "\"unit_of_measurement\":\"%\","
    "\"device_class\":\"humidity\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_water_pct/config",
    "{\"name\":\"পানির লেভেল\","
    "\"unique_id\":\"j_water_pct_2026\","
    "\"state_topic\":\"jarvis/sensor/water_pct\","
    "\"unit_of_measurement\":\"%\","
    "\"icon\":\"mdi:water-percent\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_volt/config",
    "{\"name\":\"ভোল্টেজ\","
    "\"unique_id\":\"j_volt_2026\","
    "\"state_topic\":\"jarvis/sensor/volt\","
    "\"unit_of_measurement\":\"V\","
    "\"device_class\":\"voltage\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_curr/config",
    "{\"name\":\"কারেন্ট\","
    "\"unique_id\":\"j_curr_2026\","
    "\"state_topic\":\"jarvis/sensor/curr\","
    "\"unit_of_measurement\":\"A\","
    "\"device_class\":\"current\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_pwr/config",
    "{\"name\":\"পাওয়ার\","
    "\"unique_id\":\"j_pwr_2026\","
    "\"state_topic\":\"jarvis/sensor/pwr\","
    "\"unit_of_measurement\":\"W\","
    "\"device_class\":\"power\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/sensor/jarvis_pressure/config",
    "{\"name\":\"বায়ুচাপ\","
    "\"unique_id\":\"j_pressure_2026\","
    "\"state_topic\":\"jarvis/sensor/pressure\","
    "\"unit_of_measurement\":\"hPa\","
    "\"device_class\":\"atmospheric_pressure\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/binary_sensor/jarvis_pir/config",
    "{\"name\":\"PIR Motion\","
    "\"unique_id\":\"j_pir_2026\","
    "\"state_topic\":\"jarvis/sensor/pir\","
    "\"payload_on\":\"ON\","
    "\"payload_off\":\"OFF\","
    "\"device_class\":\"motion\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  publishDiscovery(
    "homeassistant/switch/jarvis_motor/config",
    "{\"name\":\"পানির মোটর\","
    "\"unique_id\":\"j_motor_2026\","
    "\"state_topic\":\"jarvis/status/motor\","
    "\"command_topic\":\"jarvis/motor/cmd\","
    "\"payload_on\":\"ON\","
    "\"payload_off\":\"OFF\","
    "\"availability_topic\":\"" + String(availability_topic) + "\""
    + dev + "}"
  );

  for (int i = 0; i < 4; i++) {
    publishDiscovery(
      "homeassistant/binary_sensor/" + fanUIDs[i] + "_zc/config",
      "{\"name\":\"" + fanNames[i] + " Zero Cross\","
      "\"unique_id\":\"" + fanUIDs[i] + "_zc_2026\","
      "\"state_topic\":\"jarvis/status/" + fanUIDs[i] + "/zc\","
      "\"payload_on\":\"OK\","
      "\"payload_off\":\"FAULT\","
      "\"device_class\":\"connectivity\","
      "\"availability_topic\":\"" + String(availability_topic) + "\""
      + dev + "}"
    );
  }
}

// ========================= MQTT CALLBACK =============================

void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();

  String topicStr = String(topic);

  for (int i = 0; i < 4; i++) {
    if (topicStr == "jarvis/" + fanUIDs[i] + "/speed/cmd") {
      setFanSpeed(i, message.toInt());
    }
    else if (topicStr == "jarvis/" + fanUIDs[i] + "/cmd") {
      if (message == "ON") {
        setFanState(i, true);
      } else if (message == "OFF") {
        setFanState(i, false, true);
      }
    }
  }

  if (topicStr == "jarvis/motor/cmd") {
    bool state = (message == "ON");

    digitalWrite(MOTOR_PIN, state ? HIGH : LOW);
    pref.putBool("m_st", state);

    if (client.connected()) {
      client.publish(
        "jarvis/status/motor",
        state ? "ON" : "OFF",
        true
      );
    }
  }
  else if (topicStr == "jarvis/settings/empty/set") {
    tank_empty_dist = message.toInt();
    pref.putInt("t_e", tank_empty_dist);
  }
  else if (topicStr == "jarvis/settings/full/set") {
    tank_full_dist = message.toInt();
    pref.putInt("t_f", tank_full_dist);
  }
}

// ========================= MQTT =====================================

void subscribeTopics() {
  client.subscribe("jarvis/+/cmd");
  client.subscribe("jarvis/+/speed/cmd");
  client.subscribe("jarvis/motor/cmd");
  client.subscribe("jarvis/settings/+/set");
}

void restoreOutputs() {
  (void)pref.getBool("m_st", false);
  digitalWrite(MOTOR_PIN, LOW);
  lastMotorState = false;

  for (int i = 0; i < 4; i++) {
    int saved = pref.getInt(
      (fanUIDs[i] + "_sp").c_str(),
      0
    );

    setFanSpeed(i, saved, false);
  }
}

bool connectMqttOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (client.connected()) return true;

  String clientId = "Jarvis_Final_2026_" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok = false;

  if (mqttUser.length() > 0) {
    ok = client.connect(
      clientId.c_str(),
      mqttUser.c_str(),
      mqttPass.c_str(),
      availability_topic,
      0,
      true,
      "offline"
    );
  } else {
    ok = client.connect(
      clientId.c_str(),
      availability_topic,
      0,
      true,
      "offline"
    );
  }

  if (ok) {
    client.publish(availability_topic, "online", true);
    subscribeTopics();
    sendDiscovery();
    lastDiscovery = millis();

    Serial.println("MQTT connected");
  } else {
    Serial.print("MQTT failed, state=");
    Serial.println(client.state());
  }

  return ok;
}

void mqttTask() {
  if (client.connected()) {
    client.loop();
    return;
  }

  if (millis() - lastMqttAttempt >= 5000) {
    lastMqttAttempt = millis();
    connectMqttOnce();
  }
}

// ========================= PREFERENCES ===============================

void loadPreferences() {
  mqttHost = pref.getString("mqtt_host", "homeassistant.local");
  mqttPort = pref.getUShort("mqtt_port", 1883);
  mqttUser = pref.getString("mqtt_user", "esp32");
  mqttPass = pref.getString("mqtt_pass", "12345678");

  adminUser = pref.getString("adm_user", "admin");
  adminPass = pref.getString("adm_pass", "admin12345");
  if (adminUser.length() == 0) adminUser = "admin";
  if (adminPass.length() < 8) adminPass = "admin12345";

  haHost = pref.getString("ha_host", "");
  haPort = pref.getUShort("ha_port", 8123);
  haName = pref.getString("ha_name", "Jarvis Fan System");
  haDeviceId = pref.getString("ha_id", "jarvis_fan_system_final");

  fanMinSpeed = pref.getInt("f_min", FAN_MIN_SPEED_DEFAULT);
  fanMaxSpeed = pref.getInt("f_max", 100);
  fanStartupSpeed = pref.getInt("f_start", 100);

  tank_empty_dist = pref.getInt("t_e", 110);
  tank_full_dist = pref.getInt("t_f", 10);
  motor_start_pct = pref.getInt("m_s", 15);
  motor_stop_pct = pref.getInt("m_p", 98);

  for (int i = 0; i < 4; i++) {
    String key = "zc" + String(i);
    String offKey = "zco" + String(i);
    zcProfile[i] = pref.getUChar(key.c_str(), defaultZcProfile(i));
    zcTimingOffsetUs[i] = pref.getInt(offKey.c_str(), ZC_TIMING_OFFSET_US_DEFAULT);
    zcTimingOffsetUs[i] = constrain(zcTimingOffsetUs[i], -2000L, 2000L);
    if (zcProfile[i] > ZC_PROFILE_PC817_FALLING) zcProfile[i] = defaultZcProfile(i);
  }

  mqttPort = (uint16_t)constrain((int)mqttPort, 1, 65535);
  haPort = (uint16_t)constrain((int)haPort, 1, 65535);

  fanMinSpeed = constrain(fanMinSpeed, 1, 100);
  fanMaxSpeed = constrain(fanMaxSpeed, fanMinSpeed, 100);
  fanStartupSpeed = constrain(fanStartupSpeed, fanMinSpeed, fanMaxSpeed);

  tank_empty_dist = max(tank_empty_dist, 1);
  tank_full_dist = max(tank_full_dist, 0);
  if (tank_full_dist >= tank_empty_dist) tank_full_dist = tank_empty_dist - 1;

  motor_start_pct = constrain(motor_start_pct, 0, 100);
  motor_stop_pct = constrain(motor_stop_pct, 0, 100);
  if (motor_stop_pct < motor_start_pct) {
    int t = motor_stop_pct;
    motor_stop_pct = motor_start_pct;
    motor_start_pct = t;
  }
}

void savePreferences() {
  pref.putString("mqtt_host", mqttHost);
  pref.putUShort("mqtt_port", mqttPort);
  pref.putString("mqtt_user", mqttUser);
  pref.putString("mqtt_pass", mqttPass);

  pref.putString("adm_user", adminUser);
  pref.putString("adm_pass", adminPass);

  pref.putString("ha_host", haHost);
  pref.putUShort("ha_port", haPort);
  pref.putString("ha_name", haName);
  pref.putString("ha_id", haDeviceId);

  pref.putInt("f_min", fanMinSpeed);
  pref.putInt("f_max", fanMaxSpeed);
  pref.putInt("f_start", fanStartupSpeed);

  pref.putInt("t_e", tank_empty_dist);
  pref.putInt("t_f", tank_full_dist);
  pref.putInt("m_s", motor_start_pct);
  pref.putInt("m_p", motor_stop_pct);

  for (int i = 0; i < 4; i++) {
    String key = "zc" + String(i);
    String offKey = "zco" + String(i);
    pref.putUChar(key.c_str(), zcProfile[i]);
    pref.putInt(offKey.c_str(), zcTimingOffsetUs[i]);
  }
}

// ========================= DASHBOARD ================================

String htmlEscape(const String &s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  return r;
}

String jsonString(const String &s) {
  String r = s;
  r.replace("\\", "\\\\");
  r.replace("\"", "\\\"");
  r.replace("\n", "");
  r.replace("\r", "");
  return r;
}

String buildDashboard() {
  String h;
  h.reserve(22000);

  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>Jarvis Fan Controller</title>");
  h += F("<style>");
  h += F("body{font-family:Arial,sans-serif;margin:0;background:#10141b;color:#eee}");
  h += F(".wrap{max-width:1100px;margin:auto;padding:16px}");
  h += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}");
  h += F(".card{background:#1a202b;border-radius:14px;padding:16px;box-shadow:0 2px 10px #0005}");
  h += F("h1{font-size:24px}.fan h2{margin-top:0}");
  h += F("button{border:0;border-radius:10px;padding:10px 14px;margin:4px;cursor:pointer}");
  h += F("input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #555;background:#0d1117;color:#fff;margin:5px 0 10px}");
  h += F("input[type=range]{padding:0}.value{font-size:26px;font-weight:bold}");
  h += F(".on{color:#66e39b}.off{color:#ff7d7d}.ok{color:#66e39b}.bad{color:#ff7d7d}");
  h += F(".muted{color:#9aa4b2;font-size:13px}.wide{grid-column:1/-1}");
  h += F("</style></head><body><div class='wrap'>");
  h += F("<h1>JARVIS AC FAN & WATER CONTROLLER</h1>");
  h += F("<p class='muted'>Zero-Cross + Phase-Angle TRIAC control</p>");

  h += F("<div class='grid'>");

  for (int i = 0; i < 4; i++) {
    h += "<div class='card fan'><h2>" + htmlEscape(fanNames[i]) + "</h2>";
    h += "<div id='f" + String(i) + "state' class='value off'>OFF</div>";
    h += "<div>Speed: <span id='f" + String(i) + "val'>0</span>%</div>";
    h += "<input id='f" + String(i) + "' type='range' min='0' max='100' value='0' "
         "oninput='sp(" + String(i) + ",this.value)'>";
    h += "<button onclick='onoff(" + String(i) + ",1)'>ON</button>";
    h += "<button onclick='onoff(" + String(i) + ",0)'>OFF</button>";
    h += "<div>ZC: <span id='z" + String(i) + "'>--</span></div></div>";
  }

  h += F("<div class='card'><h2>Temperature</h2><div id='temp' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Humidity</h2><div id='hum' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Water</h2><div id='water' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Motor</h2><div id='motor' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Voltage</h2><div id='volt' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Current</h2><div id='curr' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Power</h2><div id='pwr' class='value'>--</div></div>");
  h += F("<div class='card'><h2>Pressure</h2><div id='pressure' class='value'>--</div></div>");
  h += F("<div class='card'><h2>WiFi</h2><div id='wifi' class='value'>--</div></div>");
  h += F("<div class='card'><h2>MQTT</h2><div id='mqtt' class='value'>--</div></div>");

  h += F("<div class='card wide'><h2>Configuration</h2>");
  h += "<form method='POST' action='/save'>";

  h += F("<label>MQTT Host</label><input name='mqtt_host' value='");
  h += htmlEscape(mqttHost);
  h += F("'>");

  h += F("<label>MQTT Port</label><input type='number' name='mqtt_port' value='");
  h += String(mqttPort);
  h += F("'>");

  h += F("<label>MQTT Username</label><input name='mqtt_user' value='");
  h += htmlEscape(mqttUser);
  h += F("'>");

  h += F("<label>MQTT Password</label><input type='password' name='mqtt_pass' value='");
  h += htmlEscape(mqttPass);
  h += F("'>");

  h += F("<label>Home Assistant Host/IP</label><input name='ha_host' value='");
  h += htmlEscape(haHost);
  h += F("'>");

  h += F("<label>Home Assistant Port</label><input type='number' name='ha_port' value='");
  h += String(haPort);
  h += F("'>");

  h += F("<label>HA Device Name</label><input name='ha_name' value='");
  h += htmlEscape(haName);
  h += F("'>");

  h += F("<label>HA Device ID</label><input name='ha_id' value='");
  h += htmlEscape(haDeviceId);
  h += F("'>");

  h += F("<label>Fan Minimum Speed (%)</label><input type='number' min='1' max='100' name='f_min' value='");
  h += String(fanMinSpeed);
  h += F("'>");

  h += F("<label>Fan Maximum Speed (%)</label><input type='number' min='1' max='100' name='f_max' value='");
  h += String(fanMaxSpeed);
  h += F("'>");

  h += F("<label>Fan Startup Speed (%)</label><input type='number' min='1' max='100' name='f_start' value='");
  h += String(fanStartupSpeed);
  h += F("'>");

  h += F("<label>Tank Empty Distance (cm)</label><input type='number' name='t_e' value='");
  h += String(tank_empty_dist);
  h += F("'>");

  h += F("<label>Tank Full Distance (cm)</label><input type='number' name='t_f' value='");
  h += String(tank_full_dist);
  h += F("'>");

  h += F("<label>Motor Start Water (%)</label><input type='number' min='0' max='100' name='m_s' value='");
  h += String(motor_start_pct);
  h += F("'>");

  h += F("<label>Motor Stop Water (%)</label><input type='number' min='0' max='100' name='m_p' value='");
  h += String(motor_stop_pct);
  h += F("'>");

  h += F("<h3>Universal Zero-Cross Detector Selection</h3>");
  h += F("<p class='muted'>Select the profile matching your isolated ZC output circuit. The selected profile is stored in NVS; no firmware rebuild is required.</p>");
  const char* zcOptions[6] = {
    "AC-OPTO / RISING (H11AA1, IL250/252, LTV-814 family)",
    "AC-OPTO / FALLING (inverted output)",
    "MODULE / RISING (ESP32-safe digital output)",
    "MODULE / FALLING (ESP32-safe active-low output)",
    "PC817 AC DETECTOR / RISING (with proper rectifier/front-end)",
    "PC817 AC DETECTOR / FALLING (inverted output)"
  };
  for (int i = 0; i < 4; i++) {
    h += "<label>Fan " + String(i + 1) + " ZC profile</label><select name='zc" + String(i) + "'>";
    for (int p = 0; p < 6; p++) {
      h += "<option value='" + String(p) + "'" + (zcProfile[i] == p ? " selected" : "") + ">" + String(zcOptions[p]) + "</option>";
    }
    h += "</select>";
    h += "<label>Fan " + String(i + 1) + " ZC timing offset (µs)</label><input type='number' min='-2000' max='2000' name='zco" + String(i) + "' value='" + String(zcTimingOffsetUs[i]) + "'>";
  }

  h += F("<button type='submit'>SAVE CONFIGURATION</button></form></div>");

  h += F("<div class='card wide'><h2>Dashboard Admin Security</h2>");
  h += F("<p class='muted'>Dashboard login is separate from MQTT credentials. "
         "Change the administrator password here. Minimum 8 characters.</p>");
  h += F("<form method='POST' action='/change-password'>");
  h += F("<label>Current Admin Password</label><input type='password' name='current_password' autocomplete='current-password' required>");
  h += F("<label>New Admin Password</label><input type='password' name='new_password' minlength='8' autocomplete='new-password' required>");
  h += F("<label>Confirm New Admin Password</label><input type='password' name='confirm_password' minlength='8' autocomplete='new-password' required>");
  h += F("<button type='submit'>CHANGE ADMIN PASSWORD</button></form></div>");

  h += F("<div class='card wide'><p class='muted'>WiFi configuration is handled by WiFiManager. Open the WiFiManager portal when WiFi credentials need to be changed.</p></div>");

  h += F("</div>");

  h += F("<script>");
  h += F("let timer;");
  h += F("function sp(i,v){document.getElementById('f'+i+'val').innerText=v;fetch('/api/fan?i='+i+'&speed='+v).catch(()=>{});}");
  h += F("function onoff(i,s){fetch('/api/fan?i='+i+'&state='+s).then(refresh);}");
  h += F("function txt(id,v){document.getElementById(id).innerText=v;}");
  h += F("function refresh(){fetch('/api/status').then(r=>r.json()).then(x=>{");
  h += F("for(let i=0;i<4;i++){");
  h += F("txt('f'+i+'val',x.fans[i].speed);");
  h += F("txt('f'+i+'state',x.fans[i].on?'ON':'OFF');");
  h += F("document.getElementById('f'+i+'state').className='value '+(x.fans[i].on?'on':'off');");
  h += F("document.getElementById('f'+i).value=x.fans[i].speed;");
  h += F("txt('z'+i,x.fans[i].zc?'OK':'FAULT');");
  h += F("document.getElementById('z'+i).className=x.fans[i].zc?'ok':'bad';");
  h += F("}");
  h += F("txt('temp',x.temp+' °C');txt('hum',x.hum+' %');txt('water',x.water+' %');");
  h += F("txt('motor',x.motor?'ON':'OFF');txt('volt',x.volt+' V');txt('curr',x.curr+' A');");
  h += F("txt('pwr',x.pwr+' W');txt('pressure',x.pressure+' hPa');");
  h += F("txt('wifi',x.wifi+' dBm');txt('mqtt',x.mqtt?'CONNECTED':'DISCONNECTED');");
  h += F("}).catch(()=>{});}");
  h += F("refresh();setInterval(refresh,2000);");
  h += F("</script></body></html>");

  return h;
}

bool authorizeWebRequest() {
  if (adminUser.length() == 0 || adminPass.length() < 8) {
    server.send(503, "text/plain", "Dashboard administrator credentials are not configured");
    return false;
  }

  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) {
    server.requestAuthentication();
    return false;
  }

  return true;
}

void handleDashboard() {
  if (!authorizeWebRequest()) return;
  server.send(200, "text/html; charset=utf-8", buildDashboard());
}

void handleFanApi() {
  if (!authorizeWebRequest()) return;
  int idx = server.arg("i").toInt();

  if (idx < 0 || idx > 3) {
    server.send(400, "text/plain", "invalid fan");
    return;
  }

  if (server.hasArg("speed")) {
    setFanSpeed(idx, server.arg("speed").toInt());
  }

  if (server.hasArg("state")) {
    setFanState(idx, server.arg("state").toInt() != 0);
  }

  server.send(200, "text/plain", "OK");
}

void handleStatusApi() {
  if (!authorizeWebRequest()) return;

  String j;
  j.reserve(3000);

  j += "{\"fans\":[";
  for (int i = 0; i < 4; i++) {
    if (i) j += ",";
    j += "{\"speed\":";
    j += String((int)fanSpeed[i]);
    j += ",\"on\":";
    j += fanEnabled[i] ? "true" : "false";
    j += ",\"zc\":";
    j += fanZeroCrossHealthy(i) ? "true" : "false";
    j += ",\"halfCycleUs\":";
    j += String((uint32_t)measuredHalfCycleUs);
    j += ",\"zcProfile\":\"";
    j += jsonString(String(zcProfileName(zcProfile[i])));
    j += "\",\"zcOffsetUs\":";
    j += String(zcTimingOffsetUs[i]);
    j += "}";
  }
  j += "],";

  j += "\"temp\":";
  j += String(lastTemp, 1);
  j += ",\"hum\":";
  j += String(lastHum, 1);
  j += ",\"water\":";
  j += String(lastWater, 0);
  j += ",\"motor\":";
  j += lastMotorState ? "true" : "false";
  j += ",\"volt\":";
  j += String(lastVolt, 1);
  j += ",\"curr\":";
  j += String(lastCurrent, 2);
  j += ",\"pwr\":";
  j += String(lastPower, 1);
  j += ",\"pressure\":";
  j += String(lastPressure, 1);
  j += ",\"wifi\":";
  j += String(WiFi.RSSI());
  j += ",\"mqtt\":";
  j += client.connected() ? "true" : "false";
  j += "}";

  server.send(200, "application/json", j);
}

void handleSaveConfig() {
  if (!authorizeWebRequest()) return;

  if (server.hasArg("mqtt_host")) mqttHost = server.arg("mqtt_host");
  if (server.hasArg("mqtt_port")) mqttPort = (uint16_t)constrain(server.arg("mqtt_port").toInt(), 1, 65535);
  if (server.hasArg("mqtt_user")) mqttUser = server.arg("mqtt_user");
  if (server.hasArg("mqtt_pass")) mqttPass = server.arg("mqtt_pass");

  if (server.hasArg("ha_host")) haHost = server.arg("ha_host");
  if (server.hasArg("ha_port")) haPort = (uint16_t)constrain(server.arg("ha_port").toInt(), 1, 65535);
  if (server.hasArg("ha_name")) haName = server.arg("ha_name");
  if (server.hasArg("ha_id")) haDeviceId = server.arg("ha_id");

  if (server.hasArg("f_min")) fanMinSpeed = constrain(server.arg("f_min").toInt(), 1, 100);
  if (server.hasArg("f_max")) fanMaxSpeed = constrain(server.arg("f_max").toInt(), fanMinSpeed, 100);
  if (server.hasArg("f_start")) fanStartupSpeed = constrain(server.arg("f_start").toInt(), fanMinSpeed, fanMaxSpeed);

  if (server.hasArg("t_e")) tank_empty_dist = server.arg("t_e").toInt();
  if (server.hasArg("t_f")) tank_full_dist = server.arg("t_f").toInt();

  if (server.hasArg("m_s")) motor_start_pct = constrain(server.arg("m_s").toInt(), 0, 100);
  if (server.hasArg("m_p")) motor_stop_pct = constrain(server.arg("m_p").toInt(), 0, 100);

  for (int i = 0; i < 4; i++) {
    String key = "zc" + String(i);
    if (server.hasArg(key)) {
      int profile = server.arg(key).toInt();
      if (profile >= 0 && profile <= ZC_PROFILE_PC817_FALLING) zcProfile[i] = (uint8_t)profile;
    }
    String offKey = "zco" + String(i);
    if (server.hasArg(offKey)) {
      zcTimingOffsetUs[i] = constrain(server.arg(offKey).toInt(), -2000, 2000);
    }
  }

  savePreferences();
  applyZcInterrupts();

  client.disconnect();

  server.send(200, "text/html",
              "<html><body><h2>Saved.</h2><p>MQTT settings will reconnect automatically.</p>"
              "<a href='/'>Back</a></body></html>");
}

void handleChangePassword() {
  if (!authorizeWebRequest()) return;

  if (!server.hasArg("current_password") ||
      !server.hasArg("new_password") ||
      !server.hasArg("confirm_password")) {
    server.send(400, "text/plain", "Missing password fields");
    return;
  }

  String currentPassword = server.arg("current_password");
  String newPassword = server.arg("new_password");
  String confirmPassword = server.arg("confirm_password");

  if (currentPassword != adminPass) {
    server.send(403, "text/plain",
                "<html><body><h2>Password change failed</h2>"
                "<p>Current admin password is incorrect.</p>"
                "<a href='/'>Back to dashboard</a></body></html>");
    return;
  }

  if (newPassword.length() < 8) {
    server.send(400, "text/plain",
                "<html><body><h2>Password change failed</h2>"
                "<p>New admin password must contain at least 8 characters.</p>"
                "<a href='/'>Back to dashboard</a></body></html>");
    return;
  }

  if (newPassword != confirmPassword) {
    server.send(400, "text/plain",
                "<html><body><h2>Password change failed</h2>"
                "<p>New password and confirmation do not match.</p>"
                "<a href='/'>Back to dashboard</a></body></html>");
    return;
  }

  if (newPassword == adminPass) {
    server.send(400, "text/plain",
                "<html><body><h2>Password change failed</h2>"
                "<p>New password must be different from the current password.</p>"
                "<a href='/'>Back to dashboard</a></body></html>");
    return;
  }

  adminPass = newPassword;
  pref.putString("adm_pass", adminPass);

  server.send(401, "text/html",
              "<html><body><h2>Admin password changed</h2>"
              "<p>The new password has been saved. The dashboard will require "
              "the new password on the next request.</p>"
              "<p>Please reload the page and sign in again.</p></body></html>");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleDashboard);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/fan", HTTP_GET, handleFanApi);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/change-password", HTTP_POST, handleChangePassword);

  server.begin();
}

// ========================= WIFIMANAGER ===============================

void saveConfigCallback() {
  mqttHost = p_mqtt_host.getValue();
  mqttPort = (uint16_t)constrain(atoi(p_mqtt_port.getValue()), 1, 65535);
  mqttUser = p_mqtt_user.getValue();
  mqttPass = p_mqtt_pass.getValue();

  haHost = p_ha_host.getValue();
  haPort = (uint16_t)constrain(atoi(p_ha_port.getValue()), 1, 65535);
  haName = p_ha_name.getValue();
  haDeviceId = p_ha_id.getValue();

  shouldSaveConfig = true;
}

void startWiFiManagerPortal() {
  wm.setConfigPortalBlocking(false);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);

  p_mqtt_host.setValue(mqttHost.c_str(), 64);
  char mqttPortBuf[8];
  snprintf(mqttPortBuf, sizeof(mqttPortBuf), "%u", mqttPort);
  p_mqtt_port.setValue(mqttPortBuf, 6);
  p_mqtt_user.setValue(mqttUser.c_str(), 64);
  p_mqtt_pass.setValue(mqttPass.c_str(), 64);
  p_ha_host.setValue(haHost.c_str(), 64);
  char haPortBuf[8];
  snprintf(haPortBuf, sizeof(haPortBuf), "%u", haPort);
  p_ha_port.setValue(haPortBuf, 6);
  p_ha_name.setValue(haName.c_str(), 64);
  p_ha_id.setValue(haDeviceId.c_str(), 64);

  if (!wifiManagerParametersAdded) {
    wm.addParameter(&p_mqtt_host);
    wm.addParameter(&p_mqtt_port);
    wm.addParameter(&p_mqtt_user);
    wm.addParameter(&p_mqtt_pass);
    wm.addParameter(&p_ha_host);
    wm.addParameter(&p_ha_port);
    wm.addParameter(&p_ha_name);
    wm.addParameter(&p_ha_id);
    wifiManagerParametersAdded = true;
  }

  if (!wm.autoConnect("Jarvis_AP")) {
    configPortalRunning = true;
  } else {
    configPortalRunning = false;
  }

  if (shouldSaveConfig) {
    savePreferences();
    shouldSaveConfig = false;
  }
}

void wifiTask() {
  if (configPortalRunning) {
    wm.process();

    if (shouldSaveConfig) {
      savePreferences();
      shouldSaveConfig = false;

      client.disconnect();
      client.setServer(mqttHost.c_str(), mqttPort);
    }
  }

  if (millis() - lastWiFiCheck >= 2000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      return;
    }

    if (configPortalRunning) {
      configPortalRunning = false;
    }
  }
}

// ========================= OTA ======================================

void setupOTA() {
  ArduinoOTA.setHostname("jarvis-fan-system");
  if (mqttPass.length() > 0) {
    ArduinoOTA.setPassword(mqttPass.c_str());
  }

  ArduinoOTA.onStart([]() {
    for (int i = 0; i < 4; i++) {
      applyFanHardware(i, 0);
    }
  });

  ArduinoOTA.begin();
}

// ========================= SETUP ====================================

void setup() {
  Serial.begin(115200);
  delay(100);

  // Status LED starts in BLINKING mode immediately after reset.
  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  lastStatusLedToggle = millis();

  Serial2.begin(
    9600,
    SERIAL_8N1,
    PZEM_RX_PIN,
    PZEM_TX_PIN
  );

  pinMode(FAN1_TRIAC_PIN, OUTPUT);
  pinMode(FAN2_TRIAC_PIN, OUTPUT);
  pinMode(FAN3_TRIAC_PIN, OUTPUT);
  pinMode(FAN4_TRIAC_PIN, OUTPUT);

  digitalWrite(FAN1_TRIAC_PIN, LOW);
  digitalWrite(FAN2_TRIAC_PIN, LOW);
  digitalWrite(FAN3_TRIAC_PIN, LOW);
  digitalWrite(FAN4_TRIAC_PIN, LOW);

  configureZcInput(0);
  configureZcInput(1);
  configureZcInput(2);
  configureZcInput(3);

  pinMode(PIRPIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(MOTOR_PIN, OUTPUT);

  digitalWrite(MOTOR_PIN, LOW);

  pref.begin("jarvis_sys", false);
  loadPreferences();

  if (mqttPass == "12345678") {
    Serial.println("WARNING: Default MQTT password is still in use. Change it before production deployment.");
  }

  Serial.println("Dashboard authentication: HTTP Basic Auth");
  Serial.print("Dashboard admin username: ");
  Serial.println(adminUser);
  if (adminPass == "admin12345") {
    Serial.println("WARNING: Default dashboard admin password is still in use. Change it from the dashboard.");
  }

  dht.begin();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  if (!bmp.begin(0x76)) {
    Serial.println("BMP Error");
  }

  triacTimer = timerBegin(1000000);
  if (triacTimer) {
    timerAttachInterrupt(triacTimer, &triacTimerISR);
    timerAlarm(triacTimer, TRIAC_TIMER_TICK_US, true, 0);
  } else {
    Serial.println("TRIAC timer init failed");
  }

  applyZcInterrupts();

  WiFi.mode(WIFI_STA);
  startWiFiManagerPortal();

  client.setServer(mqttHost.c_str(), mqttPort);
  client.setCallback(callback);
  client.setBufferSize(2048);

  setupOTA();
  setupWebServer();

  restoreOutputs();

  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_err_t wdtResult = esp_task_wdt_init(&twdt_config);

  if (wdtResult == ESP_OK || wdtResult == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_add(NULL);
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("JARVIS AC FAN SYSTEM READY");
  Serial.println("Phase-angle TRIAC control enabled");
  Serial.println("Status LED: BLINK = WiFi/MQTT not fully connected");
  Serial.println("Status LED: SOLID = WiFi + MQTT connected");
  Serial.println("ZC profiles:");
  for (int i = 0; i < 4; i++) {
    Serial.print("  FAN");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(zcProfileName(zcProfile[i]));
    Serial.print("  offset=");
    Serial.print(zcTimingOffsetUs[i]);
    Serial.println("us");
  }
  Serial.print("Auto mains timing: ");
  Serial.println(AUTO_MEASURE_MAINS_FREQUENCY ? "YES" : "NO");
  Serial.println("======================================");
}

// ========================= LOOP =====================================

void publishZeroCrossStates() {
  if (!client.connected()) return;

  static bool lastZcState[4] = {false, false, false, false};

  for (int i = 0; i < 4; i++) {
    bool state = fanZeroCrossHealthy(i);

    if (state != lastZcState[i]) {
      client.publish(
        ("jarvis/status/" + fanUIDs[i] + "/zc").c_str(),
        state ? "OK" : "FAULT",
        true
      );

      lastZcState[i] = state;
    }
  }
}

void sensorTask() {
  if (millis() - lastSensorRead < 5000) return;

  lastSensorRead = millis();

  long dist = getDistance();

  if (tank_empty_dist == tank_full_dist) {
    tank_full_dist = tank_empty_dist - 1;
  }

  int pct = constrain(
    map(
      dist,
      tank_empty_dist,
      tank_full_dist,
      0,
      100
    ),
    0,
    100
  );

  if (abs(pct - lastWater) >= 1) {
    if (client.connected()) {
      client.publish(
        "jarvis/sensor/water_pct",
        String(pct).c_str(),
        true
      );
    }

    lastWater = pct;
  }

  if (pct >= motor_stop_pct && digitalRead(MOTOR_PIN) == HIGH) {
    digitalWrite(MOTOR_PIN, LOW);
    pref.putBool("m_st", false);
    lastMotorState = false;

    if (client.connected()) {
      client.publish("jarvis/status/motor", "OFF", true);
    }
  }
  else if (pct <= motor_start_pct && digitalRead(MOTOR_PIN) == LOW) {
    digitalWrite(MOTOR_PIN, HIGH);
    pref.putBool("m_st", true);
    lastMotorState = true;

    if (client.connected()) {
      client.publish("jarvis/status/motor", "ON", true);
    }
  }

  float v = pzem.voltage();

  if (!isnan(v)) {
    float c = pzem.current();
    float p = pzem.power();

    if (abs(v - lastVolt) > 0.5) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/volt",
          String(v, 1).c_str(),
          true
        );
      }
      lastVolt = v;
    }

    if (!isnan(c)) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/curr",
          String(c, 2).c_str(),
          true
        );
      }
      lastCurrent = c;
    }

    if (!isnan(p)) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/pwr",
          String(p, 1).c_str(),
          true
        );
      }
      lastPower = p;
    }
  }

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) {
    if (abs(t - lastTemp) > 0.2) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/temp",
          String(t, 1).c_str(),
          true
        );
      }
      lastTemp = t;
    }
  }

  if (!isnan(h)) {
    if (abs(h - lastHum) > 0.5) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/hum",
          String(h, 1).c_str(),
          true
        );
      }
      lastHum = h;
    }
  }

  bool pir = digitalRead(PIRPIN);

  if (pir != lastPir) {
    if (client.connected()) {
      client.publish(
        "jarvis/sensor/pir",
        pir ? "ON" : "OFF",
        true
      );
    }

    lastPir = pir;
  }

  if (bmp.sensorID() != 0) {
    float pressure = bmp.readPressure() / 100.0F;

    if (!isnan(pressure) && abs(pressure - lastPressure) > 0.5) {
      if (client.connected()) {
        client.publish(
          "jarvis/sensor/pressure",
          String(pressure, 1).c_str(),
          true
        );
      }

      lastPressure = pressure;
    }
  }
}

void loop() {
  esp_task_wdt_reset();

  wifiTask();

  ArduinoOTA.handle();

  server.handleClient();

  mqttTask();

  statusLedTask();

  publishZeroCrossStates();

  sensorTask();

  esp_task_wdt_reset();

  delay(1);
}
