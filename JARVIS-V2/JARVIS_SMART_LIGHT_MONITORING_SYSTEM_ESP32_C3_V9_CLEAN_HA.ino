/*
  JARVIS SMART LIGHT MONITORING SYSTEM
  ESP32-C3 / Arduino-ESP32 3.x
  V9 UNIVERSAL BATTERY / EXAMPLE-STYLE LIGHTWEIGHT CONNECTIVITY - PRESERVED V3 CONTROL LOGIC

  PURPOSE
  -------
  - 12V smart light control
  - MOSFET OR relay light output support
  - OFF / SOLID / BLINK / PERIODIC modes
  - camera-live full-brightness override
  - Solar/Grid isolated source detection
  - Battery voltage/SOC monitoring
  - Charging status + charging source monitoring
  - Solar-priority / Grid-fallback charging decision
  - Grid-light / Battery-backup source decision
  - Optional relay/MOSFET source transfer outputs
  - MQTT + Home Assistant MQTT Discovery
  - ONLY controllable functions are exposed as HA entities
  - Read-only status is published to MQTT but not discovered as HA entities
  - Full local authenticated web dashboard for all configuration/status
  - WiFiManager captive setup
  - OTA
  - NVS/Preferences persistence
  - BOOT-button factory reset
  - built-in LED status
  - non-blocking main control loops
  - concise Serial status at 115200 baud

  IMPORTANT HARDWARE SAFETY
  -------------------------
  1) NEVER connect mains directly to an ESP32 GPIO.
  2) Grid/solar sensing must use a properly rated isolated sensing circuit.
  3) The ESP32 does NOT charge a battery directly. Use an appropriate charger/BMS.
  4) Source-transfer hardware must be electrically interlocked so GRID and BATTERY
     are never shorted together. Break-before-make is implemented in software, but
     hardware interlock is still required.
  5) Exact GPIOs must be verified against the actual ESP32-C3 board.
  6) Battery SOC is chemistry dependent. Select the correct chemistry in the
     dashboard before relying on percentage values.
  7) This firmware does not invent charger current, mains sensing values, or
     battery chemistry. Hardware calibration is required.

  REQUIRED LIBRARIES
  ------------------
  - Arduino-ESP32 3.x
  - WiFiManager
  - PubSubClient

  Built-in Arduino-ESP32 libraries:
  WiFi, WebServer, Preferences, ArduinoOTA, time
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>

// ========================= PIN CONFIGURATION =========================
#ifndef LED_BUILTIN
#define LED_BUILTIN 8
#endif

#define STATUS_LED_PIN              LED_BUILTIN
#define BOOT_BUTTON_PIN             9
#define LIGHT_OUTPUT_PIN            4
#define SOLAR_SENSE_PIN             5
#define GRID_SENSE_PIN              6

#define BATTERY_VOLTAGE_PIN         0
#define CHARGE_STATUS_PIN           1

#define GRID_LIGHT_SELECT_PIN       2
#define BATTERY_LIGHT_SELECT_PIN    3
#define SOLAR_CHARGE_ENABLE_PIN     7
#define GRID_CHARGE_ENABLE_PIN      -1

#define LIGHT_OUTPUT_ACTIVE_HIGH    true
#define SOURCE_OUTPUT_ACTIVE_HIGH   true
#define CHARGE_OUTPUT_ACTIVE_HIGH   true
#define SENSOR_ACTIVE_LOW           true

#define DEFAULT_BATTERY_DIVIDER_RATIO 4.0303f
#define DEFAULT_ADC_CALIBRATION      1.0000f

// ========================= LIMITS / TIMERS ===========================
#define WIFI_PORTAL_TIMEOUT_SEC      180
#define MQTT_RETRY_MS                5000UL
#define MQTT_SOCKET_TIMEOUT_SEC      2U
#define STATUS_PUBLISH_MS            10000UL
#define SENSOR_READ_MS               500UL
#define LIGHT_TRANSFER_SETTLE_MS     100UL
#define FACTORY_RESET_HOLD_MS        5000UL
#define MIN_BLINK_MS                 50UL
#define MAX_BLINK_MS                 60000UL
#define MIN_PERIOD_MS                100UL
#define MAX_PERIOD_MS                3600000UL
#define MAX_BATTERY_SERIES           16
#define DISCOVERY_RETRY_MS            30000UL

// ========================= TYPES / CONFIG ============================
enum LightMode : uint8_t {
  MODE_OFF = 0,
  MODE_SOLID,
  MODE_BLINK,
  MODE_PERIODIC
};

enum BatteryChemistry : uint8_t {
  BATTERY_LEAD_ACID = 0,
  BATTERY_LIFEPO4,
  BATTERY_LIION
};

enum LightSource : uint8_t {
  LIGHT_SOURCE_NONE = 0,
  LIGHT_SOURCE_GRID,
  LIGHT_SOURCE_BATTERY
};

enum ChargeSource : uint8_t {
  CHARGE_NONE = 0,
  CHARGE_SOLAR,
  CHARGE_GRID
};

enum SwitchHardware : uint8_t {
  SWITCH_MOSFET = 0,
  SWITCH_RELAY
};

enum BmsInterface : uint8_t {
  BMS_PASSIVE = 0,
  BMS_SMART_OPTIONAL
};

struct Config {
  String deviceName;

  String wifiSsid;
  String wifiPass;

  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;

  String haHost;
  uint16_t haPort;
  String haName;
  String haDeviceId;

  String adminUser;
  String adminPass;

  LightMode lightMode;
  SwitchHardware lightHardware;

  bool scheduleEnabled;
  uint8_t scheduleOnHour;
  uint8_t scheduleOnMinute;
  uint8_t scheduleOffHour;
  uint8_t scheduleOffMinute;

  uint32_t blinkOnMs;
  uint32_t blinkOffMs;
  uint32_t periodicOnMs;
  uint32_t periodicOffMs;

  bool cameraLiveOverride;

  BatteryChemistry batteryChemistry;
  uint8_t batterySeries;
  float batteryCellNominalVoltage;
  float batteryCellFullVoltage;
  float batteryCellEmptyVoltage;
  float batteryCapacityAh;
  float batteryNominalVoltage;
  float batteryMaxPackVoltage;
  float batteryDividerRatio;
  float batteryAdcCalibration;
  float batteryLowPct;
  float batteryCriticalPct;
  float batteryTargetPct;
  float batteryFullPct;
  float batteryMinVoltage;
  BmsInterface bmsInterface;

  bool sourceTransferEnabled;
  bool chargerControlEnabled;
  bool solarPriority;

  uint16_t sourceTransferDelayMs;
};

Config cfg;

// ========================= STATE ===================================
WebServer server(80);
WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;
WiFiManagerParameter p_mqtt_host("mqtt_host", "MQTT Host", "", 64);
WiFiManagerParameter p_mqtt_port("mqtt_port", "MQTT Port", "1883", 6);
WiFiManagerParameter p_mqtt_user("mqtt_user", "MQTT Username", "", 64);
WiFiManagerParameter p_mqtt_pass("mqtt_pass", "MQTT Password", "", 64, "type=\"password\"");
WiFiManagerParameter p_ha_host("ha_host", "Home Assistant Host/IP", "", 64);
WiFiManagerParameter p_ha_port("ha_port", "Home Assistant Port", "8123", 6);
WiFiManagerParameter p_ha_name("ha_name", "HA Device Name", "JARVIS Smart Light", 64);
WiFiManagerParameter p_ha_id("ha_id", "HA Device ID", "", 64);
bool wifiManagerParametersAdded = false;
bool shouldSaveConfig = false;
bool wifiPortalStarted = false;
bool dashboardStarted = false;

String deviceId;
String baseTopic;
String discoveryPrefix = "homeassistant";
String tHAStatus;

bool lightOutput = false;
bool lightCommand = false;
bool scheduleDemand = false;
bool cameraOverrideActive = false;

bool solarAvailable = false;
bool gridAvailable = false;
bool charging = false;
bool chargingRequested = false;
bool chargingVerified = false;
ChargeSource chargingSource = CHARGE_NONE;
LightSource lightSource = LIGHT_SOURCE_NONE;

float batteryVoltage = NAN;
float batteryPercent = NAN;

bool batteryVoltageValid = false;
bool batteryPercentValid = false;
bool chargeStatusInputValid = false;
bool chargerStatusDetected = false;

bool sourceTransferInProgress = false;
LightSource requestedLightSource = LIGHT_SOURCE_NONE;
uint32_t sourceTransferStartedAt = 0;
LightSource transferStageSource = LIGHT_SOURCE_NONE;

bool mqttReady = false;
bool haDiscoveryOk = false;

uint32_t lastMqttTry = 0;
uint32_t lastDiscoveryRetry = 0;
uint32_t lastStatusPublish = 0;
uint32_t lastSensorRead = 0;
uint32_t lastLightTick = 0;
uint32_t bootPressStart = 0;
bool animationPhase = false;

// ========================= MQTT TOPICS =============================
String tAvailability;
String tLightSet;
String tLightState;
String tModeSet;
String tModeState;
String tBlinkOnSet;
String tBlinkOnState;
String tBlinkOffSet;
String tBlinkOffState;
String tPeriodicOnSet;
String tPeriodicOnState;
String tPeriodicOffSet;
String tPeriodicOffState;
String tScheduleSet;
String tScheduleState;
String tScheduleStartSet;
String tScheduleStartState;
String tScheduleEndSet;
String tScheduleEndState;
String tBatterySeriesSet;
String tBatterySeriesState;
String tBatteryChemistrySet;
String tBatteryChemistryState;
String tBatteryLowSet;
String tBatteryLowState;
String tBatteryCriticalSet;
String tBatteryCriticalState;
String tBatteryTargetSet;
String tBatteryTargetState;
String tBatteryMinVoltageSet;
String tBatteryMinVoltageState;
String tSolarPrioritySet;
String tSolarPriorityState;
String tSourceTransferSet;
String tSourceTransferState;
String tChargerControlSet;
String tChargerControlState;
String tCameraSet;
String tCameraState;

String tBatteryVoltage;
String tBatteryPercent;
String tCharging;
String tChargingRequested;
String tChargingVerified;
String tChargingSource;
String tSolar;
String tGrid;
String tLightSource;
String tWiFiRSSI;
String tMQTT;
String tFirmware;
String tUptime;
String tBatteryChemistry;
String tPowerState;

// ========================= HELPERS ================================
String chipId() {
  uint64_t id = ESP.getEfuseMac();
  char b[17];
  snprintf(b, sizeof(b), "%08X%08X", (uint32_t)(id >> 32), (uint32_t)id);
  return String(b);
}

String modeText(LightMode m) {
  switch (m) {
    case MODE_SOLID: return "SOLID";
    case MODE_BLINK: return "BLINK";
    case MODE_PERIODIC: return "PERIODIC";
    default: return "OFF";
  }
}

String chemistryText(BatteryChemistry c) {
  switch (c) {
    case BATTERY_LIFEPO4: return "LiFePO4";
    case BATTERY_LIION: return "Li-Ion " + String(cfg.batterySeries) + "S";
    default: return "Lead-Acid";
  }
}

String chemistryOptionText(BatteryChemistry c) {
  switch (c) {
    case BATTERY_LIFEPO4: return "LiFePO4";
    case BATTERY_LIION: return "Li-Ion";
    default: return "Lead-Acid";
  }
}

String chargeSourceText(ChargeSource s) {
  switch (s) {
    case CHARGE_SOLAR: return "SOLAR";
    case CHARGE_GRID: return "GRID";
    default: return "NONE";
  }
}

String lightSourceText(LightSource s) {
  switch (s) {
    case LIGHT_SOURCE_GRID: return "GRID";
    case LIGHT_SOURCE_BATTERY: return "BATTERY";
    default: return "NONE";
  }
}

String jsonEscape(const String &in) {
  String s = in;
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

bool parseTimeHHMM(const String &s, uint8_t &h, uint8_t &m) {
  int colon = s.indexOf(':');
  if (colon < 1) return false;
  int hh = s.substring(0, colon).toInt();
  int mm = s.substring(colon + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = (uint8_t)hh;
  m = (uint8_t)mm;
  return true;
}

uint32_t clampMs(String s, uint32_t current, uint32_t minV, uint32_t maxV) {
  long long v = s.toInt();
  if (v < (long long)minV || v > (long long)maxV) return current;
  return (uint32_t)v;
}

String uptimeText() {
  uint32_t sec = millis() / 1000UL;
  uint32_t d = sec / 86400UL;
  sec %= 86400UL;
  uint32_t h = sec / 3600UL;
  sec %= 3600UL;
  uint32_t m = sec / 60UL;
  sec %= 60UL;
  char b[64];
  snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu", (unsigned long)d,
           (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  return String(b);
}

String nowTimeText() {
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return "--:--";
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &ti);
  return String(b);
}

bool timeInSchedule() {
  if (!cfg.scheduleEnabled) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return false;

  int now = ti.tm_hour * 60 + ti.tm_min;
  int on = cfg.scheduleOnHour * 60 + cfg.scheduleOnMinute;
  int off = cfg.scheduleOffHour * 60 + cfg.scheduleOffMinute;

  if (on == off) return true;
  if (on < off) return now >= on && now < off;
  return now >= on || now < off; // overnight schedule
}

// ========================= PREFERENCES =============================
void setDefaults() {
  cfg.deviceName = "JARVIS Smart Light";
  cfg.wifiSsid = "";
  cfg.wifiPass = "";
  cfg.mqttHost = "homeassistant.local";
  cfg.mqttPort = 1883;
  cfg.mqttUser = "";
  cfg.mqttPass = "";
  cfg.haHost = "";
  cfg.haPort = 8123;
  cfg.haName = "JARVIS Smart Light";
  cfg.haDeviceId = "";
  cfg.adminUser = "admin";
  cfg.adminPass = "admin12345";

  cfg.lightMode = MODE_OFF;
  cfg.lightHardware = SWITCH_MOSFET;

  cfg.scheduleEnabled = false;
  cfg.scheduleOnHour = 18;
  cfg.scheduleOnMinute = 0;
  cfg.scheduleOffHour = 23;
  cfg.scheduleOffMinute = 0;

  cfg.blinkOnMs = 500;
  cfg.blinkOffMs = 500;
  cfg.periodicOnMs = 5000;
  cfg.periodicOffMs = 55000;

  cfg.cameraLiveOverride = false;

  cfg.batteryChemistry = BATTERY_LIION;
  cfg.batterySeries = 3;
  cfg.batteryCellNominalVoltage = 3.70f;
  cfg.batteryCellFullVoltage = 4.20f;
  cfg.batteryCellEmptyVoltage = 3.20f;
  cfg.batteryCapacityAh = 0.0f;
  cfg.batteryNominalVoltage = 11.10f;
  cfg.batteryMaxPackVoltage = 12.60f;
  cfg.batteryDividerRatio = DEFAULT_BATTERY_DIVIDER_RATIO;
  cfg.batteryAdcCalibration = DEFAULT_ADC_CALIBRATION;
  cfg.batteryLowPct = 25.0f;
  cfg.batteryCriticalPct = 15.0f;
  cfg.batteryTargetPct = 95.0f;
  cfg.batteryFullPct = 100.0f;
  cfg.batteryMinVoltage = 9.60f;
  cfg.bmsInterface = BMS_PASSIVE;

  cfg.sourceTransferEnabled = true;
  cfg.chargerControlEnabled = false;
  cfg.solarPriority = true;
  cfg.sourceTransferDelayMs = LIGHT_TRANSFER_SETTLE_MS;
}

void saveConfig() {
  prefs.begin("smartlight", false);

  prefs.putString("name", cfg.deviceName);
  prefs.putString("wssid", cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiPass);
  prefs.putString("mhost", cfg.mqttHost);
  prefs.putUShort("mport", cfg.mqttPort);
  prefs.putString("muser", cfg.mqttUser);
  prefs.putString("mpass", cfg.mqttPass);
  prefs.putString("hahost", cfg.haHost);
  prefs.putUShort("haport", cfg.haPort);
  prefs.putString("haname", cfg.haName);
  prefs.putString("haid", cfg.haDeviceId);
  prefs.putString("auser", cfg.adminUser);
  prefs.putString("apass", cfg.adminPass);

  prefs.putUChar("lmode", (uint8_t)cfg.lightMode);
  prefs.putUChar("lhw", (uint8_t)cfg.lightHardware);

  prefs.putBool("sen", cfg.scheduleEnabled);
  prefs.putUChar("soh", cfg.scheduleOnHour);
  prefs.putUChar("som", cfg.scheduleOnMinute);
  prefs.putUChar("sfh", cfg.scheduleOffHour);
  prefs.putUChar("sfm", cfg.scheduleOffMinute);

  prefs.putUInt("bon", cfg.blinkOnMs);
  prefs.putUInt("boff", cfg.blinkOffMs);
  prefs.putUInt("pon", cfg.periodicOnMs);
  prefs.putUInt("poff", cfg.periodicOffMs);
  prefs.putBool("cam", cfg.cameraLiveOverride);

  prefs.putUChar("bchem", (uint8_t)cfg.batteryChemistry);
  prefs.putUChar("bser", cfg.batterySeries);
  prefs.putFloat("bcnom", cfg.batteryCellNominalVoltage);
  prefs.putFloat("bcfull", cfg.batteryCellFullVoltage);
  prefs.putFloat("bcempty", cfg.batteryCellEmptyVoltage);
  prefs.putFloat("bcap", cfg.batteryCapacityAh);
  prefs.putFloat("bnom", cfg.batteryNominalVoltage);
  prefs.putFloat("bmaxv", cfg.batteryMaxPackVoltage);
  prefs.putFloat("bdiv", cfg.batteryDividerRatio);
  prefs.putFloat("badc", cfg.batteryAdcCalibration);
  prefs.putFloat("blow", cfg.batteryLowPct);
  prefs.putFloat("bcrit", cfg.batteryCriticalPct);
  prefs.putFloat("btgt", cfg.batteryTargetPct);
  prefs.putFloat("bfull", cfg.batteryFullPct);
  prefs.putFloat("bminv", cfg.batteryMinVoltage);
  prefs.putUChar("bms", (uint8_t)cfg.bmsInterface);

  prefs.putBool("transfer", cfg.sourceTransferEnabled);
  prefs.putBool("charger", cfg.chargerControlEnabled);
  prefs.putBool("solarpri", cfg.solarPriority);
  prefs.putUShort("xferms", cfg.sourceTransferDelayMs);

  prefs.end();
}

void loadConfig() {
  setDefaults();
  prefs.begin("smartlight", true);

  cfg.deviceName = prefs.getString("name", cfg.deviceName);
  cfg.wifiSsid = prefs.getString("wssid", cfg.wifiSsid);
  cfg.wifiPass = prefs.getString("wpass", cfg.wifiPass);
  cfg.mqttHost = prefs.getString("mhost", cfg.mqttHost);
  cfg.mqttPort = prefs.getUShort("mport", cfg.mqttPort);
  cfg.mqttUser = prefs.getString("muser", cfg.mqttUser);
  cfg.mqttPass = prefs.getString("mpass", cfg.mqttPass);
  cfg.haHost = prefs.getString("hahost", cfg.haHost);
  cfg.haPort = prefs.getUShort("haport", cfg.haPort);
  cfg.haName = prefs.getString("haname", cfg.haName);
  cfg.haDeviceId = prefs.getString("haid", cfg.haDeviceId);
  cfg.adminUser = prefs.getString("auser", cfg.adminUser);
  cfg.adminPass = prefs.getString("apass", cfg.adminPass);

  cfg.lightMode = (LightMode)prefs.getUChar("lmode", (uint8_t)cfg.lightMode);
  cfg.lightHardware = (SwitchHardware)prefs.getUChar("lhw", (uint8_t)cfg.lightHardware);

  cfg.scheduleEnabled = prefs.getBool("sen", cfg.scheduleEnabled);
  cfg.scheduleOnHour = prefs.getUChar("soh", cfg.scheduleOnHour);
  cfg.scheduleOnMinute = prefs.getUChar("som", cfg.scheduleOnMinute);
  cfg.scheduleOffHour = prefs.getUChar("sfh", cfg.scheduleOffHour);
  cfg.scheduleOffMinute = prefs.getUChar("sfm", cfg.scheduleOffMinute);

  cfg.blinkOnMs = prefs.getUInt("bon", cfg.blinkOnMs);
  cfg.blinkOffMs = prefs.getUInt("boff", cfg.blinkOffMs);
  cfg.periodicOnMs = prefs.getUInt("pon", cfg.periodicOnMs);
  cfg.periodicOffMs = prefs.getUInt("poff", cfg.periodicOffMs);
  cfg.cameraLiveOverride = prefs.getBool("cam", cfg.cameraLiveOverride);

  cfg.batteryChemistry = (BatteryChemistry)prefs.getUChar("bchem", (uint8_t)cfg.batteryChemistry);
  cfg.batterySeries = prefs.getUChar("bser", cfg.batterySeries);
  cfg.batteryCellNominalVoltage = prefs.getFloat("bcnom", cfg.batteryCellNominalVoltage);
  cfg.batteryCellFullVoltage = prefs.getFloat("bcfull", cfg.batteryCellFullVoltage);
  cfg.batteryCellEmptyVoltage = prefs.getFloat("bcempty", cfg.batteryCellEmptyVoltage);
  cfg.batteryCapacityAh = prefs.getFloat("bcap", cfg.batteryCapacityAh);
  cfg.batteryNominalVoltage = prefs.getFloat("bnom", cfg.batteryNominalVoltage);
  cfg.batteryMaxPackVoltage = prefs.getFloat("bmaxv", cfg.batteryMaxPackVoltage);
  cfg.batteryDividerRatio = prefs.getFloat("bdiv", cfg.batteryDividerRatio);
  cfg.batteryAdcCalibration = prefs.getFloat("badc", cfg.batteryAdcCalibration);
  cfg.batteryLowPct = prefs.getFloat("blow", cfg.batteryLowPct);
  cfg.batteryCriticalPct = prefs.getFloat("bcrit", cfg.batteryCriticalPct);
  cfg.batteryTargetPct = prefs.getFloat("btgt", cfg.batteryTargetPct);
  cfg.batteryFullPct = prefs.getFloat("bfull", cfg.batteryFullPct);
  cfg.batteryMinVoltage = prefs.getFloat("bminv", cfg.batteryMinVoltage);
  cfg.bmsInterface = (BmsInterface)prefs.getUChar("bms", (uint8_t)cfg.bmsInterface);

  cfg.sourceTransferEnabled = prefs.getBool("transfer", cfg.sourceTransferEnabled);
  cfg.chargerControlEnabled = prefs.getBool("charger", cfg.chargerControlEnabled);
  cfg.solarPriority = prefs.getBool("solarpri", cfg.solarPriority);
  cfg.sourceTransferDelayMs = prefs.getUShort("xferms", cfg.sourceTransferDelayMs);

  prefs.end();

  if (cfg.batterySeries < 1 || cfg.batterySeries > MAX_BATTERY_SERIES) cfg.batterySeries = 3;
  if (cfg.batteryCellNominalVoltage <= 0.0f) cfg.batteryCellNominalVoltage = 3.70f;
  if (cfg.batteryCellFullVoltage <= cfg.batteryCellEmptyVoltage) cfg.batteryCellFullVoltage = 4.20f;
  if (cfg.batteryCellEmptyVoltage <= 0.0f) cfg.batteryCellEmptyVoltage = 3.20f;
  if (cfg.batteryCapacityAh < 0.0f) cfg.batteryCapacityAh = 0.0f;
  if (cfg.batteryMaxPackVoltage <= 0.0f) cfg.batteryMaxPackVoltage = cfg.batteryCellFullVoltage * cfg.batterySeries;
  cfg.batteryNominalVoltage = cfg.batteryCellNominalVoltage * cfg.batterySeries;
  if (cfg.batteryDividerRatio < 0.1f) cfg.batteryDividerRatio = DEFAULT_BATTERY_DIVIDER_RATIO;
  if (cfg.batteryAdcCalibration < 0.5f || cfg.batteryAdcCalibration > 1.5f) cfg.batteryAdcCalibration = 1.0f;
  if (cfg.batteryFullPct <= 0.0f || cfg.batteryFullPct > 100.0f) cfg.batteryFullPct = 100.0f;
  if (cfg.batteryCriticalPct > cfg.batteryLowPct) cfg.batteryCriticalPct = cfg.batteryLowPct;
  if (cfg.batteryTargetPct < cfg.batteryLowPct) cfg.batteryTargetPct = cfg.batteryLowPct;
}

void factoryReset() {
  digitalWrite(LIGHT_OUTPUT_PIN, LIGHT_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);
  if (GRID_LIGHT_SELECT_PIN >= 0) digitalWrite(GRID_LIGHT_SELECT_PIN, SOURCE_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);
  if (BATTERY_LIGHT_SELECT_PIN >= 0) digitalWrite(BATTERY_LIGHT_SELECT_PIN, SOURCE_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);
  if (SOLAR_CHARGE_ENABLE_PIN >= 0) digitalWrite(SOLAR_CHARGE_ENABLE_PIN, CHARGE_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);
  if (GRID_CHARGE_ENABLE_PIN >= 0) digitalWrite(GRID_CHARGE_ENABLE_PIN, CHARGE_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);

  prefs.begin("smartlight", false);
  prefs.clear();
  prefs.end();

  wifiManager.resetSettings();
  delay(500);
  ESP.restart();
}

// ========================= GPIO ==================================
void writeActive(int pin, bool active, bool activeHigh) {
  if (pin < 0) return;
  digitalWrite(pin, active ? (activeHigh ? HIGH : LOW) : (activeHigh ? LOW : HIGH));
}

bool readPresence(int pin) {
  if (pin < 0) return false;
  int v = digitalRead(pin);
  return SENSOR_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

void initGPIO() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LIGHT_OUTPUT_PIN, OUTPUT);
  writeActive(LIGHT_OUTPUT_PIN, false, LIGHT_OUTPUT_ACTIVE_HIGH);

  pinMode(SOLAR_SENSE_PIN, INPUT_PULLUP);
  pinMode(GRID_SENSE_PIN, INPUT_PULLUP);

  if (BATTERY_VOLTAGE_PIN >= 0) {
    pinMode(BATTERY_VOLTAGE_PIN, INPUT);
    analogReadResolution(12);
  }

  if (CHARGE_STATUS_PIN >= 0) pinMode(CHARGE_STATUS_PIN, INPUT_PULLUP);

  if (GRID_LIGHT_SELECT_PIN >= 0) {
    pinMode(GRID_LIGHT_SELECT_PIN, OUTPUT);
    writeActive(GRID_LIGHT_SELECT_PIN, false, SOURCE_OUTPUT_ACTIVE_HIGH);
  }
  if (BATTERY_LIGHT_SELECT_PIN >= 0) {
    pinMode(BATTERY_LIGHT_SELECT_PIN, OUTPUT);
    writeActive(BATTERY_LIGHT_SELECT_PIN, false, SOURCE_OUTPUT_ACTIVE_HIGH);
  }
  if (SOLAR_CHARGE_ENABLE_PIN >= 0) {
    pinMode(SOLAR_CHARGE_ENABLE_PIN, OUTPUT);
    writeActive(SOLAR_CHARGE_ENABLE_PIN, false, CHARGE_OUTPUT_ACTIVE_HIGH);
  }
  if (GRID_CHARGE_ENABLE_PIN >= 0) {
    pinMode(GRID_CHARGE_ENABLE_PIN, OUTPUT);
    writeActive(GRID_CHARGE_ENABLE_PIN, false, CHARGE_OUTPUT_ACTIVE_HIGH);
  }
}

// ========================= BATTERY ================================
float readBatteryVoltage() {
  if (BATTERY_VOLTAGE_PIN < 0) return NAN;

  // ESP32-C3 ADC is board/reference dependent. Calibration is mandatory.
  uint32_t sumMv = 0;
  const int samples = 12;
  for (int i = 0; i < samples; ++i) {
    sumMv += analogReadMilliVolts(BATTERY_VOLTAGE_PIN);
    delayMicroseconds(300);
  }
  float adcV = (sumMv / (float)samples) / 1000.0f;
  return adcV * cfg.batteryDividerRatio * cfg.batteryAdcCalibration;
}

float interpolateCurve(float v, const float *x, const float *p, size_t n) {
  if (n < 2) return NAN;
  if (v <= x[0]) return p[0];
  if (v >= x[n - 1]) return p[n - 1];
  for (size_t i = 0; i + 1 < n; ++i) {
    if (v <= x[i + 1]) {
      return p[i] + (v - x[i]) * (p[i + 1] - p[i]) / (x[i + 1] - x[i]);
    }
  }
  return p[n - 1];
}

float socLeadAcid(float packV) {
  float cells = cfg.batterySeries > 0 ? cfg.batterySeries : 1;
  float v = packV / cells;
  const float x[] = {1.9667f, 2.0000f, 2.0333f, 2.0667f, 2.1000f, 2.1167f};
  const float p[] = {0, 20, 40, 60, 80, 100};
  return interpolateCurve(v, x, p, 6);
}

float socLiFePO4(float packV) {
  float cells = cfg.batterySeries > 0 ? cfg.batterySeries : 1;
  float v = packV / cells;
  const float x[] = {3.200f, 3.250f, 3.2875f, 3.3125f, 3.3375f, 3.3625f};
  const float p[] = {5, 10, 30, 60, 85, 100};
  return interpolateCurve(v, x, p, 6);
}

float socLiIon(float packV) {
  float cells = cfg.batterySeries > 0 ? cfg.batterySeries : 1;
  float v = packV / cells;
  const float x[] = {3.200f, 3.400f, 3.500f, 3.600f, 3.700f, 3.800f, 3.900f, 4.000f, 4.100f, 4.200f};
  const float p[] = {0, 5, 10, 20, 30, 45, 60, 75, 90, 100};
  return interpolateCurve(v, x, p, 10);
}

float calculateSOC(float v) {
  if (!isfinite(v) || v <= 0 || cfg.batterySeries < 1) return NAN;
  float p;
  switch (cfg.batteryChemistry) {
    case BATTERY_LIFEPO4: p = socLiFePO4(v); break;
    case BATTERY_LIION: p = socLiIon(v); break;
    default: p = socLeadAcid(v); break;
  }
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

void updateBatteryState() {
  if (millis() - lastSensorRead < SENSOR_READ_MS) return;
  lastSensorRead = millis();

  batteryVoltage = readBatteryVoltage();
  batteryVoltageValid = isfinite(batteryVoltage);
  batteryPercent = batteryVoltageValid ? calculateSOC(batteryVoltage) : NAN;
  batteryPercentValid = isfinite(batteryPercent);

  if (CHARGE_STATUS_PIN >= 0) {
    chargeStatusInputValid = true;
    chargerStatusDetected = readPresence(CHARGE_STATUS_PIN);
  } else {
    chargeStatusInputValid = false;
    chargerStatusDetected = false;
  }
}

// ========================= SOURCE / CHARGING ======================
void updateSourceDetection() {
  solarAvailable = readPresence(SOLAR_SENSE_PIN);
  gridAvailable = readPresence(GRID_SENSE_PIN);
}

// ========================= POWER ENGINE ===========================
bool batteryNeedsCharge() {
  if (!batteryPercentValid) return false;
  return batteryPercent < cfg.batteryTargetPct;
}

bool batteryCritical() {
  if (!batteryPercentValid) return false;
  return batteryPercent <= cfg.batteryCriticalPct;
}

void setChargingOutputs(ChargeSource source) {
  if (!cfg.chargerControlEnabled) return;

  writeActive(SOLAR_CHARGE_ENABLE_PIN, source == CHARGE_SOLAR, CHARGE_OUTPUT_ACTIVE_HIGH);
  writeActive(GRID_CHARGE_ENABLE_PIN, source == CHARGE_GRID, CHARGE_OUTPUT_ACTIVE_HIGH);
}

void decideCharging() {
  ChargeSource desired = CHARGE_NONE;

  if (batteryNeedsCharge()) {
    if (cfg.solarPriority && solarAvailable) {
      desired = CHARGE_SOLAR;
    } else if (!solarAvailable && gridAvailable) {
      desired = CHARGE_GRID;
    } else if (!cfg.solarPriority && gridAvailable) {
      desired = CHARGE_GRID;
    } else if (solarAvailable) {
      desired = CHARGE_SOLAR;
    }
  }

  chargingSource = desired;
  chargingRequested = (desired != CHARGE_NONE);
  chargingVerified = chargeStatusInputValid ? chargerStatusDetected : false;
  charging = chargeStatusInputValid ? chargingVerified : chargingRequested;

  setChargingOutputs(desired);
}

LightSource desiredLightSource() {
  if (gridAvailable) return LIGHT_SOURCE_GRID;
  return LIGHT_SOURCE_BATTERY;
}

void applySourceOutputs(LightSource source) {
  if (!cfg.sourceTransferEnabled) return;
  if (GRID_LIGHT_SELECT_PIN < 0 || BATTERY_LIGHT_SELECT_PIN < 0) return;

  writeActive(GRID_LIGHT_SELECT_PIN, false, SOURCE_OUTPUT_ACTIVE_HIGH);
  writeActive(BATTERY_LIGHT_SELECT_PIN, false, SOURCE_OUTPUT_ACTIVE_HIGH);

  sourceTransferInProgress = true;
  sourceTransferStartedAt = millis();
  transferStageSource = source;
  requestedLightSource = source;
}

void updateSourceTransfer() {
  LightSource desired = desiredLightSource();
  lightSource = desired;

  if (!cfg.sourceTransferEnabled) {
    sourceTransferInProgress = false;
    return;
  }

  if (GRID_LIGHT_SELECT_PIN < 0 || BATTERY_LIGHT_SELECT_PIN < 0) {
    sourceTransferInProgress = false;
    return;
  }

  if (!sourceTransferInProgress && requestedLightSource != desired) {
    applySourceOutputs(desired);
    return;
  }

  if (sourceTransferInProgress && requestedLightSource == desired &&
      millis() - sourceTransferStartedAt >= cfg.sourceTransferDelayMs) {
    writeActive(GRID_LIGHT_SELECT_PIN, desired == LIGHT_SOURCE_GRID, SOURCE_OUTPUT_ACTIVE_HIGH);
    writeActive(BATTERY_LIGHT_SELECT_PIN, desired == LIGHT_SOURCE_BATTERY, SOURCE_OUTPUT_ACTIVE_HIGH);
    sourceTransferInProgress = false;
  }
}

// ========================= LIGHT CONTROL ==========================
void setLightPhysical(bool on) {
  lightOutput = on;
  writeActive(LIGHT_OUTPUT_PIN, on, LIGHT_OUTPUT_ACTIVE_HIGH);
}

void resetAnimation() {
  animationPhase = false;
  lastLightTick = millis();
}

bool baseLightDemand() {
  if (cameraOverrideActive) return true;
  if (cfg.lightMode == MODE_OFF) return false;
  if (cfg.lightMode == MODE_SOLID) return true;
  if (cfg.scheduleEnabled) return timeInSchedule();
  return true;
}

bool animationOutputDemand() {
  if (cameraOverrideActive) return true;

  bool scheduled = cfg.scheduleEnabled ? timeInSchedule() : true;
  if (!scheduled) return false;

  if (cfg.lightMode == MODE_OFF) return false;
  if (cfg.lightMode == MODE_SOLID) return true;

  uint32_t now = millis();
  uint32_t interval = animationPhase
    ? (cfg.lightMode == MODE_BLINK ? cfg.blinkOnMs : cfg.periodicOnMs)
    : (cfg.lightMode == MODE_BLINK ? cfg.blinkOffMs : cfg.periodicOffMs);

  if (now - lastLightTick >= interval) {
    lastLightTick = now;
    animationPhase = !animationPhase;
  }

  return animationPhase;
}

void updateLightController() {
  cameraOverrideActive = cfg.cameraLiveOverride;

  bool demand = animationOutputDemand();

  if (!gridAvailable && batteryVoltageValid && batteryVoltage <= cfg.batteryMinVoltage) {
    demand = false;
  }

  if (!gridAvailable && !batteryVoltageValid && BATTERY_VOLTAGE_PIN >= 0) {
    demand = false;
  }

  setLightPhysical(demand);
}

// ========================= MQTT TOPICS ============================
void buildTopics() {
  deviceId = "smartlight_" + chipId();
  baseTopic = "jarvis/" + deviceId;

  tAvailability = baseTopic + "/availability";
  tHAStatus = discoveryPrefix + "/status";

  tLightSet = baseTopic + "/light/set";
  tLightState = baseTopic + "/light/state";

  tModeSet = baseTopic + "/animation/set";
  tModeState = baseTopic + "/animation/state";

  tBlinkOnSet = baseTopic + "/animation/blink_on/set";
  tBlinkOnState = baseTopic + "/animation/blink_on/state";
  tBlinkOffSet = baseTopic + "/animation/blink_off/set";
  tBlinkOffState = baseTopic + "/animation/blink_off/state";
  tPeriodicOnSet = baseTopic + "/animation/periodic_on/set";
  tPeriodicOnState = baseTopic + "/animation/periodic_on/state";
  tPeriodicOffSet = baseTopic + "/animation/periodic_off/set";
  tPeriodicOffState = baseTopic + "/animation/periodic_off/state";

  tScheduleSet = baseTopic + "/schedule/enable/set";
  tScheduleState = baseTopic + "/schedule/enable/state";
  tScheduleStartSet = baseTopic + "/schedule/start/set";
  tScheduleStartState = baseTopic + "/schedule/start/state";
  tScheduleEndSet = baseTopic + "/schedule/end/set";
  tScheduleEndState = baseTopic + "/schedule/end/state";

  tBatterySeriesSet = baseTopic + "/battery/series/set";
  tBatterySeriesState = baseTopic + "/battery/series/state";
  tBatteryChemistrySet = baseTopic + "/battery/chemistry/set";
  tBatteryChemistryState = baseTopic + "/battery/chemistry/state";
  tBatteryLowSet = baseTopic + "/battery/low/set";
  tBatteryLowState = baseTopic + "/battery/low/state";
  tBatteryCriticalSet = baseTopic + "/battery/critical/set";
  tBatteryCriticalState = baseTopic + "/battery/critical/state";
  tBatteryTargetSet = baseTopic + "/battery/target/set";
  tBatteryTargetState = baseTopic + "/battery/target/state";
  tBatteryMinVoltageSet = baseTopic + "/battery/min_voltage/set";
  tBatteryMinVoltageState = baseTopic + "/battery/min_voltage/state";
  tSolarPrioritySet = baseTopic + "/power/solar_priority/set";
  tSolarPriorityState = baseTopic + "/power/solar_priority/state";
  tSourceTransferSet = baseTopic + "/power/source_transfer/set";
  tSourceTransferState = baseTopic + "/power/source_transfer/state";
  tChargerControlSet = baseTopic + "/power/charger_control/set";
  tChargerControlState = baseTopic + "/power/charger_control/state";

  tCameraSet = baseTopic + "/camera_live/set";
  tCameraState = baseTopic + "/camera_live/state";

  tBatteryVoltage = baseTopic + "/status/battery_voltage";
  tBatteryPercent = baseTopic + "/status/battery_percent";
  tCharging = baseTopic + "/status/charging";
  tChargingRequested = baseTopic + "/status/charging_requested";
  tChargingVerified = baseTopic + "/status/charging_verified";
  tChargingSource = baseTopic + "/status/charging_source";
  tSolar = baseTopic + "/status/solar";
  tGrid = baseTopic + "/status/grid";
  tLightSource = baseTopic + "/status/light_source";
  tWiFiRSSI = baseTopic + "/status/wifi_rssi";
  tMQTT = baseTopic + "/status/mqtt";
  tFirmware = baseTopic + "/status/firmware";
  tUptime = baseTopic + "/status/uptime";
  tBatteryChemistry = baseTopic + "/status/battery_chemistry";
  tPowerState = baseTopic + "/status/power_state";
}

// ========================= MQTT DISCOVERY =========================
String mqttStateText(int state) {
  switch (state) {
    case 0: return "CONNECTED";
    case -1: return "TIMEOUT";
    case -2: return "CONNECTION_LOST";
    case -3: return "CONNECT_FAILED";
    case -4: return "BAD_PROTOCOL";
    case -5: return "BAD_CLIENT_ID";
    case -6: return "UNAVAILABLE";
    case -7: return "BAD_CREDENTIALS";
    case -8: return "UNAUTHORIZED";
    default: return "UNKNOWN";
  }
}

bool mqttPublishChecked(const String &topic, const String &payload, bool retain,
                       const String &label, bool logSuccess) {
  (void)logSuccess;
  if (!mqtt.connected()) return false;
  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), retain);
  if (!ok) {
    Serial.print("[MQTT PUBLISH FAIL] ");
    Serial.print(label);
    Serial.print(" topic=");
    Serial.println(topic);
  }
  return ok;
}

bool mqttSubscribeChecked(const String &topic) {
  bool ok = mqtt.subscribe(topic.c_str());
  if (!ok) {
    Serial.print("[MQTT SUBSCRIBE FAIL] ");
    Serial.println(topic);
  }
  return ok;
}

String haDeviceJson() {
  return "\"origin\":{\"name\":\"JARVIS Smart Light Firmware\",\"sw_version\":\"9.0.0\"},"
         "\"device\":{\"identifiers\":[\"" + deviceId + "\"],"
         "\"name\":\"" + jsonEscape(cfg.deviceName) + "\","
         "\"manufacturer\":\"JARVIS\","
         "\"model\":\"ESP32-C3 Smart Light\","
         "\"sw_version\":\"9.0.0\"}";
}

bool publishDiscovery(const String &component, const String &objectId, const String &payload) {
  if (!mqtt.connected()) {
    haDiscoveryOk = false;
    return false;
  }

  String topic = discoveryPrefix + "/" + component + "/" + deviceId + "/" + objectId + "/config";
  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);

  if (!ok) {
    haDiscoveryOk = false;
    Serial.print("[HA DISCOVERY FAIL] ");
    Serial.println(topic);
  }

  return ok;
}

// Remove entities created by older firmware versions.
// This is intentionally kept so an upgrade does not leave stale
// controls/sensors inside the Home Assistant device.
void clearObsoleteDiscovery() {
  struct DiscoveryEntry {
    const char *component;
    const char *objectId;
  };

  const DiscoveryEntry obsolete[] = {
    {"select", "animation"},
    {"number", "blink_on"},
    {"number", "blink_off"},
    {"number", "periodic_on"},
    {"number", "periodic_off"},

    {"number", "battery_series"},
    {"number", "battery_low"},
    {"number", "battery_critical"},
    {"number", "battery_target"},
    {"number", "battery_min_voltage"},
    {"select", "battery_chemistry"},

    {"switch", "solar_priority"},
    {"switch", "source_transfer"},
    {"switch", "charger_control"},
    {"switch", "camera_live"},

    {"sensor", "battery_voltage"},
    {"sensor", "battery_percent"},
    {"sensor", "charging"},
    {"sensor", "charging_requested"},
    {"sensor", "charging_verified"},
    {"sensor", "charging_source"},
    {"sensor", "solar"},
    {"sensor", "grid"},
    {"sensor", "light_source"},
    {"sensor", "wifi_rssi"},
    {"sensor", "mqtt"},
    {"sensor", "firmware"},
    {"sensor", "uptime"},
    {"sensor", "battery_chemistry"},
    {"sensor", "power_state"},

    {"binary_sensor", "charging"},
    {"binary_sensor", "solar"},
    {"binary_sensor", "grid"}
  };

  for (const auto &e : obsolete) {
    String topic = discoveryPrefix + "/" + e.component + "/" +
                   deviceId + "/" + e.objectId + "/config";
    mqtt.publish(topic.c_str(), "", true);
  }
}

void publishHADiscovery() {
  if (!mqtt.connected()) {
    haDiscoveryOk = false;
    Serial.println("[HA DISCOVERY] skipped: MQTT disconnected");
    return;
  }

  haDiscoveryOk = true;
  clearObsoleteDiscovery();

  String dev = haDeviceJson();
  String avail =
      "\"availability_topic\":\"" + tAvailability +
      "\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",";

  // ---------------------------------------------------------------
  // 1. ONLY LIGHT CONTROL
  // ---------------------------------------------------------------
  String p = "{" + avail +
             "\"name\":\"Light\","
             "\"unique_id\":\"" + deviceId + "_light\","
             "\"command_topic\":\"" + tLightSet + "\","
             "\"state_topic\":\"" + tLightState + "\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"state_on\":\"ON\",\"state_off\":\"OFF\","
             + dev + "}";
  publishDiscovery("light", "light", p);

  // ---------------------------------------------------------------
  // 2. ONLY SCHEDULE CONTROLS
  // ---------------------------------------------------------------
  p = "{" + avail +
      "\"name\":\"Schedule Enable\","
      "\"unique_id\":\"" + deviceId + "_schedule\","
      "\"command_topic\":\"" + tScheduleSet + "\","
      "\"state_topic\":\"" + tScheduleState + "\","
      "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
      "\"state_on\":\"ON\",\"state_off\":\"OFF\","
      + dev + "}";
  publishDiscovery("switch", "schedule", p);

  p = "{" + avail +
      "\"name\":\"Schedule Start\","
      "\"unique_id\":\"" + deviceId + "_schedule_start\","
      "\"command_topic\":\"" + tScheduleStartSet + "\","
      "\"state_topic\":\"" + tScheduleStartState + "\","
      "\"min\":0,\"max\":1439,\"step\":1,"
      "\"unit_of_measurement\":\"min\",\"mode\":\"box\","
      + dev + "}";
  publishDiscovery("number", "schedule_start", p);

  p = "{" + avail +
      "\"name\":\"Schedule End\","
      "\"unique_id\":\"" + deviceId + "_schedule_end\","
      "\"command_topic\":\"" + tScheduleEndSet + "\","
      "\"state_topic\":\"" + tScheduleEndState + "\","
      "\"min\":0,\"max\":1439,\"step\":1,"
      "\"unit_of_measurement\":\"min\",\"mode\":\"box\","
      + dev + "}";
  publishDiscovery("number", "schedule_end", p);

  // ---------------------------------------------------------------
  // 3. READ-ONLY BATTERY / CHARGING STATUS
  //    No buttons, switches, selects or configuration controls.
  // ---------------------------------------------------------------
  p = "{" + avail +
      "\"name\":\"Battery Percentage\","
      "\"unique_id\":\"" + deviceId + "_battery_percent\","
      "\"state_topic\":\"" + tBatteryPercent + "\","
      "\"unit_of_measurement\":\"%\","
      "\"device_class\":\"battery\","
      "\"state_class\":\"measurement\","
      "\"entity_category\":\"diagnostic\","
      + dev + "}";
  publishDiscovery("sensor", "battery_percent", p);

  p = "{" + avail +
      "\"name\":\"Battery Voltage\","
      "\"unique_id\":\"" + deviceId + "_battery_voltage\","
      "\"state_topic\":\"" + tBatteryVoltage + "\","
      "\"unit_of_measurement\":\"V\","
      "\"device_class\":\"voltage\","
      "\"state_class\":\"measurement\","
      "\"entity_category\":\"diagnostic\","
      + dev + "}";
  publishDiscovery("sensor", "battery_voltage", p);

  p = "{" + avail +
      "\"name\":\"Charging\","
      "\"unique_id\":\"" + deviceId + "_charging\","
      "\"state_topic\":\"" + tCharging + "\","
      "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
      "\"device_class\":\"battery_charging\","
      "\"entity_category\":\"diagnostic\","
      + dev + "}";
  publishDiscovery("binary_sensor", "charging", p);

  if (haDiscoveryOk) {
    Serial.println("[HA DISCOVERY] CLEAN SET OK");
  } else {
    Serial.println("[HA DISCOVERY] FAILED - will retry in background");
  }
}

// ========================= MQTT ==================================
void publishStatus() {
  if (!mqtt.connected()) return;

  // Keep the MQTT state topics used by the firmware, but expose only the
  // intentionally selected Home Assistant entities through discovery.
  mqttPublishChecked(tAvailability.c_str(), "online", true, "STATUS", false);
  mqttPublishChecked(tLightState.c_str(), lightOutput ? "ON" : "OFF", true, "STATUS", false);
  mqttPublishChecked(tScheduleState.c_str(), cfg.scheduleEnabled ? "ON" : "OFF", true, "STATUS", false);
  mqttPublishChecked(tScheduleStartState.c_str(),
                     String(cfg.scheduleOnHour * 60 + cfg.scheduleOnMinute).c_str(),
                     true, "STATUS", false);
  mqttPublishChecked(tScheduleEndState.c_str(),
                     String(cfg.scheduleOffHour * 60 + cfg.scheduleOffMinute).c_str(),
                     true, "STATUS", false);

  if (batteryVoltageValid) {
    mqttPublishChecked(tBatteryVoltage.c_str(),
                       String(batteryVoltage, 2).c_str(),
                       true, "STATUS", false);
  } else {
    mqttPublishChecked(tBatteryVoltage.c_str(), "UNAVAILABLE", true, "STATUS", false);
  }

  if (batteryPercentValid) {
    mqttPublishChecked(tBatteryPercent.c_str(),
                       String(batteryPercent, 1).c_str(),
                       true, "STATUS", false);
  } else {
    mqttPublishChecked(tBatteryPercent.c_str(), "UNAVAILABLE", true, "STATUS", false);
  }

  mqttPublishChecked(tCharging.c_str(), charging ? "ON" : "OFF", true, "STATUS", false);
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String msg;
  msg.reserve(len + 1);
  for (unsigned int i = 0; i < len; ++i) msg += (char)payload[i];
  msg.trim();
  String t(topic);

  if (t == tHAStatus) {
    if (msg.equalsIgnoreCase("online")) {
      // Do not publish the full discovery set from inside the MQTT callback.
      // The callback runs while mqtt.loop() is processing an incoming packet;
      // re-entering the broker with dozens of retained publishes here can
      // fragment heap / create network re-entrancy pressure on ESP32-C3.
      // Let the normal main loop perform the retry instead.
      haDiscoveryOk = false;
      lastDiscoveryRetry = 0;
    }
    return;
  }

  if (t == tLightSet) {
    if (msg.equalsIgnoreCase("ON")) {
      lightCommand = true;
      cfg.lightMode = MODE_SOLID;
      saveConfig();
      resetAnimation();
    } else if (msg.equalsIgnoreCase("OFF")) {
      lightCommand = false;
      cfg.lightMode = MODE_OFF;
      saveConfig();
      resetAnimation();
    }
  } else if (t == tModeSet) {
    if (msg.equalsIgnoreCase("OFF")) cfg.lightMode = MODE_OFF;
    else if (msg.equalsIgnoreCase("SOLID") || msg.equalsIgnoreCase("ON")) cfg.lightMode = MODE_SOLID;
    else if (msg.equalsIgnoreCase("BLINK")) cfg.lightMode = MODE_BLINK;
    else if (msg.equalsIgnoreCase("PERIODIC")) cfg.lightMode = MODE_PERIODIC;
    saveConfig();
    resetAnimation();
  } else if (t == tBlinkOnSet) {
    cfg.blinkOnMs = clampMs(msg, cfg.blinkOnMs, MIN_BLINK_MS, MAX_BLINK_MS);
    saveConfig();
    resetAnimation();
  } else if (t == tBlinkOffSet) {
    cfg.blinkOffMs = clampMs(msg, cfg.blinkOffMs, MIN_BLINK_MS, MAX_BLINK_MS);
    saveConfig();
    resetAnimation();
  } else if (t == tPeriodicOnSet) {
    cfg.periodicOnMs = clampMs(msg, cfg.periodicOnMs, MIN_PERIOD_MS, MAX_PERIOD_MS);
    saveConfig();
    resetAnimation();
  } else if (t == tPeriodicOffSet) {
    cfg.periodicOffMs = clampMs(msg, cfg.periodicOffMs, MIN_PERIOD_MS, MAX_PERIOD_MS);
    saveConfig();
    resetAnimation();
  } else if (t == tScheduleSet) {
    cfg.scheduleEnabled = msg.equalsIgnoreCase("ON");
    saveConfig();
  } else if (t == tScheduleStartSet) {
    int total = msg.toInt();
    if (total >= 0 && total <= 1439) {
      cfg.scheduleOnHour = total / 60;
      cfg.scheduleOnMinute = total % 60;
      saveConfig();
    }
  } else if (t == tScheduleEndSet) {
    int total = msg.toInt();
    if (total >= 0 && total <= 1439) {
      cfg.scheduleOffHour = total / 60;
      cfg.scheduleOffMinute = total % 60;
      saveConfig();
    }
  } else if (t == tBatterySeriesSet) {
    int series = msg.toInt();
    if (series >= 1 && series <= MAX_BATTERY_SERIES) {
      float oldDerivedMax = cfg.batteryCellFullVoltage * cfg.batterySeries;
      float oldDerivedMin = cfg.batteryCellEmptyVoltage * cfg.batterySeries;
      bool maxWasDerived = fabs(cfg.batteryMaxPackVoltage - oldDerivedMax) < 0.05f;
      bool minWasDerived = fabs(cfg.batteryMinVoltage - oldDerivedMin) < 0.05f;
      cfg.batterySeries = (uint8_t)series;
      cfg.batteryNominalVoltage = cfg.batteryCellNominalVoltage * cfg.batterySeries;
      if (maxWasDerived) cfg.batteryMaxPackVoltage = cfg.batteryCellFullVoltage * cfg.batterySeries;
      if (minWasDerived) cfg.batteryMinVoltage = cfg.batteryCellEmptyVoltage * cfg.batterySeries;
      saveConfig();
    }
  } else if (t == tBatteryChemistrySet) {
    if (msg.equalsIgnoreCase("Li-Ion")) {
      cfg.batteryChemistry = BATTERY_LIION;
      cfg.batteryCellNominalVoltage = 3.70f;
      cfg.batteryCellFullVoltage = 4.20f;
      cfg.batteryCellEmptyVoltage = 3.20f;
    } else if (msg.equalsIgnoreCase("LiFePO4")) {
      cfg.batteryChemistry = BATTERY_LIFEPO4;
      cfg.batteryCellNominalVoltage = 3.20f;
      cfg.batteryCellFullVoltage = 3.65f;
      cfg.batteryCellEmptyVoltage = 2.80f;
    } else if (msg.equalsIgnoreCase("Lead-Acid")) {
      cfg.batteryChemistry = BATTERY_LEAD_ACID;
      cfg.batteryCellNominalVoltage = 2.00f;
      cfg.batteryCellFullVoltage = 2.12f;
      cfg.batteryCellEmptyVoltage = 1.97f;
    }
    cfg.batteryNominalVoltage = cfg.batteryCellNominalVoltage * cfg.batterySeries;
    cfg.batteryMaxPackVoltage = cfg.batteryCellFullVoltage * cfg.batterySeries;
    cfg.batteryMinVoltage = cfg.batteryCellEmptyVoltage * cfg.batterySeries;
    saveConfig();
  } else if (t == tBatteryLowSet) {
    cfg.batteryLowPct = constrain(msg.toFloat(), 0.0f, 100.0f);
    if (cfg.batteryCriticalPct > cfg.batteryLowPct) cfg.batteryCriticalPct = cfg.batteryLowPct;
    if (cfg.batteryTargetPct < cfg.batteryLowPct) cfg.batteryTargetPct = cfg.batteryLowPct;
    saveConfig();
  } else if (t == tBatteryCriticalSet) {
    cfg.batteryCriticalPct = constrain(msg.toFloat(), 0.0f, cfg.batteryLowPct);
    saveConfig();
  } else if (t == tBatteryTargetSet) {
    cfg.batteryTargetPct = constrain(msg.toFloat(), cfg.batteryLowPct, 100.0f);
    saveConfig();
  } else if (t == tBatteryMinVoltageSet) {
    float v = msg.toFloat();
    if (v > 0.0f && v <= cfg.batteryMaxPackVoltage) {
      cfg.batteryMinVoltage = v;
      saveConfig();
    }
  } else if (t == tSolarPrioritySet) {
    cfg.solarPriority = msg.equalsIgnoreCase("ON");
    saveConfig();
  } else if (t == tSourceTransferSet) {
    cfg.sourceTransferEnabled = msg.equalsIgnoreCase("ON");
    saveConfig();
  } else if (t == tChargerControlSet) {
    cfg.chargerControlEnabled = msg.equalsIgnoreCase("ON");
    saveConfig();
  } else if (t == tCameraSet) {
    cfg.cameraLiveOverride = msg.equalsIgnoreCase("ON");
    saveConfig();
    resetAnimation();
  }

  updateLightController();
  publishStatus();
}

void subscribeMQTT() {
  mqttSubscribeChecked(tLightSet);
  mqttSubscribeChecked(tScheduleSet);
  mqttSubscribeChecked(tScheduleStartSet);
  mqttSubscribeChecked(tScheduleEndSet);
  mqttSubscribeChecked(tHAStatus);
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();
  if (!cfg.mqttHost.length()) return;

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  String clientId = deviceId;
  bool ok;
  if (cfg.mqttUser.length()) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(),
                      tAvailability.c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), tAvailability.c_str(), 0, true, "offline");
  }

  if (!ok) {
    Serial.print("[MQTT] connect failed state=");
    Serial.println(mqtt.state());
    return;
  }

  Serial.println("[MQTT] connected");
  mqtt.publish(tAvailability.c_str(), "online", true);
  subscribeMQTT();
  publishHADiscovery();
  publishStatus();
}

// ========================= WIFIMANAGER / OTA ======================
void saveConfigCallback() {
  // WiFiManager owns the Wi-Fi credentials. Copy the newly submitted
  // credentials into our Config immediately so the same credentials are
  // available after reboot and the firmware does not depend on two
  // independent credential stores.
  String savedSsid = wifiManager.getWiFiSSID(true);
  String savedPass = wifiManager.getWiFiPass(true);
  if (savedSsid.length()) cfg.wifiSsid = savedSsid;
  cfg.wifiPass = savedPass;

  cfg.mqttHost = p_mqtt_host.getValue();
  cfg.mqttPort = (uint16_t)constrain(atoi(p_mqtt_port.getValue()), 1, 65535);
  cfg.mqttUser = p_mqtt_user.getValue();
  cfg.mqttPass = p_mqtt_pass.getValue();
  cfg.haHost = p_ha_host.getValue();
  cfg.haPort = (uint16_t)constrain(atoi(p_ha_port.getValue()), 1, 65535);
  cfg.haName = p_ha_name.getValue();
  cfg.haDeviceId = p_ha_id.getValue();
  shouldSaveConfig = true;
}

void startWiFiManagerPortal() {
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);
  wifiManager.setConnectTimeout(5);

  p_mqtt_host.setValue(cfg.mqttHost.c_str(), 64);
  char mqttPortBuf[8];
  snprintf(mqttPortBuf, sizeof(mqttPortBuf), "%u", cfg.mqttPort);
  p_mqtt_port.setValue(mqttPortBuf, 6);
  p_mqtt_user.setValue(cfg.mqttUser.c_str(), 64);
  p_mqtt_pass.setValue(cfg.mqttPass.c_str(), 64);
  p_ha_host.setValue(cfg.haHost.c_str(), 64);
  char haPortBuf[8];
  snprintf(haPortBuf, sizeof(haPortBuf), "%u", cfg.haPort);
  p_ha_port.setValue(haPortBuf, 6);
  p_ha_name.setValue(cfg.haName.c_str(), 64);
  p_ha_id.setValue(cfg.haDeviceId.c_str(), 64);

  if (!wifiManagerParametersAdded) {
    wifiManager.addParameter(&p_mqtt_host);
    wifiManager.addParameter(&p_mqtt_port);
    wifiManager.addParameter(&p_mqtt_user);
    wifiManager.addParameter(&p_mqtt_pass);
    wifiManager.addParameter(&p_ha_host);
    wifiManager.addParameter(&p_ha_port);
    wifiManager.addParameter(&p_ha_name);
    wifiManager.addParameter(&p_ha_id);
    wifiManagerParametersAdded = true;
  }

  wifiPortalStarted = !wifiManager.autoConnect("Jarvis_AP");
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  if (cfg.wifiSsid.length()) {
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  } else {
    startWiFiManagerPortal();
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void serviceWiFi() {
  static uint32_t wifiFailAt = 0;
  static bool reportedConnected = false;

  // WiFiManager must be serviced only while its non-blocking portal is active.
  if (wifiPortalStarted) {
    wifiManager.process();

    if (shouldSaveConfig) {
      // saveConfigCallback() has already copied both Wi-Fi and custom
      // parameters. Persist everything in the same configuration store.
      saveConfig();
      shouldSaveConfig = false;
      mqtt.disconnect();
      mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
      Serial.println("[WIFI] configuration saved");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiPortalStarted) {
      wifiManager.stopConfigPortal();
      wifiPortalStarted = false;
    }
    if (!reportedConnected) {
      reportedConnected = true;
      Serial.print("[WIFI] connected | SSID=");
      Serial.print(WiFi.SSID());
      Serial.print(" | IP=");
      Serial.print(WiFi.localIP());
      Serial.print(" | RSSI=");
      Serial.println(WiFi.RSSI());
    }
    wifiFailAt = 0;
    return;
  }

  reportedConnected = false;

  if (!wifiPortalStarted && cfg.wifiSsid.length()) {
    if (wifiFailAt == 0) wifiFailAt = millis();
    if (millis() - wifiFailAt >= 20000UL) {
      Serial.print("[WIFI] connection failed | status=");
      Serial.println((int)WiFi.status());
      startWiFiManagerPortal();
      wifiFailAt = millis();
    }
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(deviceId.c_str());
  ArduinoOTA.setPassword(cfg.adminPass.c_str());
  ArduinoOTA.begin();
  Serial.println("[OTA] ready | hostname=" + deviceId);
}

// ========================= STATUS / FACTORY RESET ================
void updateStatusLED() {
  static uint32_t mark = 0;
  static bool state = false;

  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    return;
  }

  if (millis() - mark >= 500) {
    mark = millis();
    state = !state;
    digitalWrite(STATUS_LED_PIN, state ? HIGH : LOW);
  }
}

void handleFactoryResetButton() {
  bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;

  if (pressed && bootPressStart == 0) bootPressStart = millis();

  if (!pressed) {
    bootPressStart = 0;
    return;
  }

  if (millis() - bootPressStart >= FACTORY_RESET_HOLD_MS) {
    for (int i = 0; i < 8; ++i) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      delay(80);
      digitalWrite(STATUS_LED_PIN, LOW);
      delay(80);
    }
    factoryReset();
  }
}

// ========================= DASHBOARD ==============================
bool requireAuth() {
  if (!server.authenticate(cfg.adminUser.c_str(), cfg.adminPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

String css() {
  return "<style>"
         "body{font-family:Arial,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:18px;max-width:1200px;margin:auto}"
         ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}"
         ".card{background:#161b22;border:1px solid #30363d;border-radius:14px;padding:16px;margin-bottom:14px}"
         "h1,h2{margin-top:0}label{display:block;margin-top:9px;font-size:.9rem}"
         "input,select,button{box-sizing:border-box;width:100%;padding:10px;margin-top:4px;background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:8px}"
         "button{background:#238636;border:0;cursor:pointer;margin-top:10px}"
         ".danger{background:#da3633}.warn{background:#9e6a03}.ok{color:#3fb950}.bad{color:#f85149}.muted{color:#8b949e}"
         ".kv{display:flex;justify-content:space-between;border-bottom:1px solid #21262d;padding:7px 0}"
         ".mono{font-family:monospace;word-break:break-all}"
         "</style>";
}

String statusValue(bool v) {
  return v ? "<span class='ok'>AVAILABLE</span>" : "<span class='bad'>NOT AVAILABLE</span>";
}

String dashboardPage() {
  String h = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + htmlEscape(cfg.deviceName) + "</title>" + css() + "</head><body>";
  h += "<div class='card'><h1>JARVIS Smart Light</h1><div class='muted'>ESP32-C3 • " + htmlEscape(deviceId) + "</div></div>";

  h += "<div class='grid'>";
  h += "<div class='card'><h2>Power Status</h2>";
  h += "<div class='kv'><span>Battery</span><b>" + (batteryPercentValid ? String(batteryPercent,1)+" %" : "N/A") + "</b></div>";
  h += "<div class='kv'><span>Battery Voltage</span><b>" + (batteryVoltageValid ? String(batteryVoltage,2)+" V" : "N/A") + "</b></div>";
  h += "<div class='kv'><span>Charging</span><b>" + String(charging ? "YES" : "NO") + "</b></div>";
  h += "<div class='kv'><span>Charge Requested</span><b>" + String(chargingRequested ? "YES" : "NO") + "</b></div>";
  h += "<div class='kv'><span>Charge Verified</span><b>" + String(chargingVerified ? "YES" : "NO") + "</b></div>";
  h += "<div class='kv'><span>Charge Source</span><b>" + chargeSourceText(chargingSource) + "</b></div>";
  h += "<div class='kv'><span>Solar</span><b>" + statusValue(solarAvailable) + "</b></div>";
  h += "<div class='kv'><span>Grid</span><b>" + statusValue(gridAvailable) + "</b></div>";
  h += "<div class='kv'><span>Light Source</span><b>" + lightSourceText(lightSource) + "</b></div></div>";

  h += "<div class='card'><h2>Light Control</h2>";
  h += "<div class='kv'><span>Output</span><b>" + String(lightOutput ? "ON" : "OFF") + "</b></div>";
  h += "<form method='post' action='/light'><button name='action' value='ON'>SOLID ON</button><button class='danger' name='action' value='OFF'>OFF</button></form>";
  h += "<form method='post' action='/mode'><select name='mode'>";
  const char *modes[] = {"OFF","SOLID","BLINK","PERIODIC"};
  for (const char *m : modes) {
    h += "<option" + String(modeText(cfg.lightMode).equals(m) ? " selected" : "") + ">" + m + "</option>";
  }
  h += "</select><button name='save' value='1'>Apply Animation Mode</button></form>";
  h += "<form method='post' action='/camera'><button class='warn' name='state' value='ON'>Camera Live: FULL ON</button><button name='state' value='OFF'>Camera Live Override OFF</button></form></div>";
  h += "</div>";

  h += "<div class='card'><h2>Animation / Schedule</h2>";
  h += "<form method='post' action='/timing'>";
  h += "<label>Blink ON (ms)<input type='number' name='bon' min='50' max='60000' value='" + String(cfg.blinkOnMs) + "'></label>";
  h += "<label>Blink OFF (ms)<input type='number' name='boff' min='50' max='60000' value='" + String(cfg.blinkOffMs) + "'></label>";
  h += "<label>Periodic ON (ms)<input type='number' name='pon' min='100' max='3600000' value='" + String(cfg.periodicOnMs) + "'></label>";
  h += "<label>Periodic OFF (ms)<input type='number' name='poff' min='100' max='3600000' value='" + String(cfg.periodicOffMs) + "'></label>";
  h += "<button>Save Timing</button></form>";
  h += "<form method='post' action='/schedule'>";
  h += "<label>Schedule <select name='enable'><option value='OFF'" + String(!cfg.scheduleEnabled ? " selected" : "") + ">OFF</option><option value='ON'" + String(cfg.scheduleEnabled ? " selected" : "") + ">ON</option></select></label>";
  h += "<label>Start <input type='time' name='start' value='" + String(cfg.scheduleOnHour<10?"0":"") + String(cfg.scheduleOnHour) + ":" + String(cfg.scheduleOnMinute<10?"0":"") + String(cfg.scheduleOnMinute) + "'></label>";
  h += "<label>End <input type='time' name='end' value='" + String(cfg.scheduleOffHour<10?"0":"") + String(cfg.scheduleOffHour) + ":" + String(cfg.scheduleOffMinute<10?"0":"") + String(cfg.scheduleOffMinute) + "'></label>";
  h += "<button>Save Schedule</button></form></div>";

  h += "<div class='card'><h2>Battery / Power Configuration</h2>";
  h += "<p><b>Universal battery profile:</b> " + String(cfg.batterySeries) + "S " + chemistryText(cfg.batteryChemistry) + " • " + String(cfg.batteryNominalVoltage,2) + "V nominal • " + String(cfg.batteryMaxPackVoltage,2) + "V full</p>";
  h += "<p class='muted'>Basic/passive BMS requires no ESP32 BMS GPIO. The ESP32 measures pack voltage through the external ADC divider. Smart BMS communication is optional and not required by this firmware. Series count is configurable up to the firmware limit; the real safe limit is determined by your battery, BMS, charger, divider resistors, ADC range, wiring and power hardware.</p>";
  h += "<form method='post' action='/power'>";
  h += "<label>Battery Chemistry<select name='chem'>"
      "<option value='Li-Ion'" + String(cfg.batteryChemistry==BATTERY_LIION?" selected":"") + ">Li-Ion / NMC</option>"
      "<option value='LiFePO4'" + String(cfg.batteryChemistry==BATTERY_LIFEPO4?" selected":"") + ">LiFePO4</option>"
      "<option value='Lead-Acid'" + String(cfg.batteryChemistry==BATTERY_LEAD_ACID?" selected":"") + ">Lead-Acid</option></select></label>";
  h += "<label>Battery Series Count<input type='number' name='series' min='1' max='16' value='" + String(cfg.batterySeries) + "'></label>";
  h += "<label>Cell Nominal Voltage<input type='number' step='0.01' name='cellnom' value='" + String(cfg.batteryCellNominalVoltage,2) + "'></label>";
  h += "<label>Cell Full Voltage<input type='number' step='0.01' name='cellfull' value='" + String(cfg.batteryCellFullVoltage,2) + "'></label>";
  h += "<label>Cell Empty Voltage<input type='number' step='0.01' name='cellempty' value='" + String(cfg.batteryCellEmptyVoltage,2) + "'></label>";
  h += "<label>Battery Capacity (Ah, optional)<input type='number' step='0.1' name='cap' min='0' value='" + String(cfg.batteryCapacityAh,1) + "'></label>";
  h += "<label>Maximum Pack Voltage<input type='number' step='0.01' name='maxv' value='" + String(cfg.batteryMaxPackVoltage,2) + "'></label>";
  h += "<label>ADC Divider Ratio<input type='number' step='0.0001' name='div' value='" + String(cfg.batteryDividerRatio,4) + "'></label>";
  h += "<label>ADC Calibration<input type='number' step='0.0001' name='adc' value='" + String(cfg.batteryAdcCalibration,4) + "'></label>";
  h += "<label>Low Battery %<input type='number' step='0.1' name='low' min='0' max='100' value='" + String(cfg.batteryLowPct,1) + "'></label>";
  h += "<label>Critical Battery %<input type='number' step='0.1' name='crit' min='0' max='100' value='" + String(cfg.batteryCriticalPct,1) + "'></label>";
  h += "<label>Target Charge %<input type='number' step='0.1' name='tgt' min='0' max='100' value='" + String(cfg.batteryTargetPct,1) + "'></label>";
  h += "<label>Minimum Battery Voltage<input type='number' step='0.01' name='minv' value='" + String(cfg.batteryMinVoltage,2) + "'></label>";
  h += "<label>Solar Priority<select name='solarpri'><option value='ON'" + String(cfg.solarPriority?" selected":"") + ">ON</option><option value='OFF'" + String(!cfg.solarPriority?" selected":"") + ">OFF</option></select></label>";
  h += "<label>Source Transfer<select name='transfer'><option value='ON'" + String(cfg.sourceTransferEnabled?" selected":"") + ">ON</option><option value='OFF'" + String(!cfg.sourceTransferEnabled?" selected":"") + ">OFF</option></select></label>";
  h += "<label>Charger Control<select name='charger'><option value='ON'" + String(cfg.chargerControlEnabled?" selected":"") + ">ON</option><option value='OFF'" + String(!cfg.chargerControlEnabled?" selected":"") + ">OFF</option></select></label>";
  h += "<button>Save Power Configuration</button></form>";
  h += "<p class='muted'>Battery percentage is an approximate voltage-based SOC unless an external battery monitor/BMS value is used. Calibrate to your real battery/charger system.</p></div>";

  h += "<div class='card'><h2>Network / MQTT</h2><form method='post' action='/network'>";
  h += "<label>Wi-Fi SSID<input name='ssid' value='" + htmlEscape(cfg.wifiSsid) + "'></label>";
  h += "<label>Wi-Fi Password<input type='password' name='wpass' placeholder='Leave empty to keep current'></label>";
  h += "<label>Device Name<input name='name' value='" + htmlEscape(cfg.deviceName) + "'></label>";
  h += "<label>MQTT Host<input name='host' value='" + htmlEscape(cfg.mqttHost) + "'></label>";
  h += "<label>MQTT Port<input type='number' name='port' min='1' max='65535' value='" + String(cfg.mqttPort) + "'></label>";
  h += "<label>MQTT Username<input name='user' value='" + htmlEscape(cfg.mqttUser) + "'></label>";
  h += "<label>MQTT Password<input type='password' name='pass' value='" + htmlEscape(cfg.mqttPass) + "'></label>";
  h += "<label>Home Assistant Host/IP<input name='hahost' value='" + htmlEscape(cfg.haHost) + "'></label>";
  h += "<label>Home Assistant Port<input type='number' name='haport' min='1' max='65535' value='" + String(cfg.haPort) + "'></label>";
  h += "<label>HA Device Name<input name='haname' value='" + htmlEscape(cfg.haName) + "'></label>";
  h += "<label>HA Device ID<input name='haid' value='" + htmlEscape(cfg.haDeviceId) + "'></label>";
  h += "<button>Save Network / MQTT / HA</button></form></div>";

  h += "<div class='card'><h2>Security</h2><form method='post' action='/security'>";
  h += "<label>Admin Username<input name='user' value='" + htmlEscape(cfg.adminUser) + "'></label>";
  h += "<label>New Admin Password<input type='password' name='pass' placeholder='Leave empty to keep current'></label>";
  h += "<button>Save Security</button></form></div>";

  h += "<div class='card'><h2>System</h2>";
  h += "<div class='kv'><span>Firmware</span><b>9.0.0</b></div>";
  h += "<div class='kv'><span>IP</span><b class='mono'>" + WiFi.localIP().toString() + "</b></div>";
  h += "<div class='kv'><span>MQTT</span><b>" + String(mqtt.connected()?"CONNECTED":"DISCONNECTED") + "</b></div>";
  h += "<div class='kv'><span>RSSI</span><b>" + String(WiFi.RSSI()) + " dBm</b></div>";
  h += "<div class='kv'><span>Uptime</span><b>" + uptimeText() + "</b></div>";
  h += "<div class='kv'><span>Local Time</span><b>" + nowTimeText() + "</b></div>";
  h += "<form method='post' action='/restart'><button class='warn'>Restart ESP32</button></form>";
  h += "<form method='post' action='/factory'><button class='danger'>Factory Reset</button></form>";
  h += "<p class='muted'>OTA uses ArduinoOTA and the configured admin password.</p></div>";

  h += "</body></html>";
  return h;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Saved");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (!requireAuth()) return;
    server.send(200, "text/html", dashboardPage());
  });

  server.on("/light", HTTP_POST, []() {
    if (!requireAuth()) return;
    String a = server.arg("action");
    if (a == "ON") cfg.lightMode = MODE_SOLID;
    else if (a == "OFF") cfg.lightMode = MODE_OFF;
    saveConfig();
    resetAnimation();
    updateLightController();
    redirectHome();
  });

  server.on("/mode", HTTP_POST, []() {
    if (!requireAuth()) return;
    String m = server.arg("mode");
    if (m == "OFF") cfg.lightMode = MODE_OFF;
    else if (m == "SOLID") cfg.lightMode = MODE_SOLID;
    else if (m == "BLINK") cfg.lightMode = MODE_BLINK;
    else if (m == "PERIODIC") cfg.lightMode = MODE_PERIODIC;
    saveConfig();
    resetAnimation();
    updateLightController();
    redirectHome();
  });

  server.on("/camera", HTTP_POST, []() {
    if (!requireAuth()) return;
    cfg.cameraLiveOverride = server.arg("state") == "ON";
    saveConfig();
    resetAnimation();
    updateLightController();
    redirectHome();
  });

  server.on("/timing", HTTP_POST, []() {
    if (!requireAuth()) return;
    cfg.blinkOnMs = clampMs(server.arg("bon"), cfg.blinkOnMs, MIN_BLINK_MS, MAX_BLINK_MS);
    cfg.blinkOffMs = clampMs(server.arg("boff"), cfg.blinkOffMs, MIN_BLINK_MS, MAX_BLINK_MS);
    cfg.periodicOnMs = clampMs(server.arg("pon"), cfg.periodicOnMs, MIN_PERIOD_MS, MAX_PERIOD_MS);
    cfg.periodicOffMs = clampMs(server.arg("poff"), cfg.periodicOffMs, MIN_PERIOD_MS, MAX_PERIOD_MS);
    saveConfig();
    resetAnimation();
    redirectHome();
  });

  server.on("/schedule", HTTP_POST, []() {
    if (!requireAuth()) return;
    cfg.scheduleEnabled = server.arg("enable") == "ON";
    uint8_t h, m;
    if (parseTimeHHMM(server.arg("start"), h, m)) { cfg.scheduleOnHour = h; cfg.scheduleOnMinute = m; }
    if (parseTimeHHMM(server.arg("end"), h, m)) { cfg.scheduleOffHour = h; cfg.scheduleOffMinute = m; }
    saveConfig();
    redirectHome();
  });

  server.on("/power", HTTP_POST, []() {
    if (!requireAuth()) return;
    String chem = server.arg("chem");
    if (chem == "Li-Ion") { cfg.batteryChemistry = BATTERY_LIION; cfg.batteryCellNominalVoltage = 3.70f; cfg.batteryCellFullVoltage = 4.20f; cfg.batteryCellEmptyVoltage = 3.20f; }
    else if (chem == "LiFePO4") { cfg.batteryChemistry = BATTERY_LIFEPO4; cfg.batteryCellNominalVoltage = 3.20f; cfg.batteryCellFullVoltage = 3.65f; cfg.batteryCellEmptyVoltage = 2.80f; }
    else if (chem == "Lead-Acid") { cfg.batteryChemistry = BATTERY_LEAD_ACID; cfg.batteryCellNominalVoltage = 2.00f; cfg.batteryCellFullVoltage = 2.12f; cfg.batteryCellEmptyVoltage = 1.97f; }
    int series = server.arg("series").toInt();
    if (series >= 1 && series <= MAX_BATTERY_SERIES) cfg.batterySeries = (uint8_t)series;
    float cn = server.arg("cellnom").toFloat(); if (cn > 0.1f && cn < 10.0f) cfg.batteryCellNominalVoltage = cn;
    float cf = server.arg("cellfull").toFloat(); if (cf > 0.1f && cf < 10.0f) cfg.batteryCellFullVoltage = cf;
    float ce = server.arg("cellempty").toFloat(); if (ce > 0.1f && ce < cfg.batteryCellFullVoltage) cfg.batteryCellEmptyVoltage = ce;
    cfg.batteryCapacityAh = max(0.0f, server.arg("cap").toFloat());
    cfg.batteryNominalVoltage = cfg.batteryCellNominalVoltage * cfg.batterySeries;
    float maxv = server.arg("maxv").toFloat();
    cfg.batteryMaxPackVoltage = (maxv > 0.0f) ? maxv : (cfg.batteryCellFullVoltage * cfg.batterySeries);
    if (cfg.batteryMaxPackVoltage < cfg.batteryCellFullVoltage * cfg.batterySeries) cfg.batteryMaxPackVoltage = cfg.batteryCellFullVoltage * cfg.batterySeries;
    cfg.batteryDividerRatio = server.arg("div").toFloat();
    cfg.batteryAdcCalibration = server.arg("adc").toFloat();
    cfg.batteryLowPct = constrain(server.arg("low").toFloat(), 0.0f, 100.0f);
    cfg.batteryCriticalPct = constrain(server.arg("crit").toFloat(), 0.0f, 100.0f);
    cfg.batteryTargetPct = constrain(server.arg("tgt").toFloat(), 0.0f, 100.0f);
    cfg.batteryMinVoltage = server.arg("minv").toFloat();
    cfg.solarPriority = server.arg("solarpri") == "ON";
    cfg.sourceTransferEnabled = server.arg("transfer") == "ON";
    cfg.chargerControlEnabled = server.arg("charger") == "ON";

    if (cfg.batteryDividerRatio < 0.1f) cfg.batteryDividerRatio = DEFAULT_BATTERY_DIVIDER_RATIO;
    if (cfg.batteryAdcCalibration < 0.5f || cfg.batteryAdcCalibration > 1.5f) cfg.batteryAdcCalibration = 1.0f;
    if (cfg.batteryCriticalPct > cfg.batteryLowPct) cfg.batteryCriticalPct = cfg.batteryLowPct;
    if (cfg.batteryTargetPct < cfg.batteryLowPct) cfg.batteryTargetPct = cfg.batteryLowPct;

    saveConfig();
    redirectHome();
  });

  server.on("/network", HTTP_POST, []() {
    if (!requireAuth()) return;
    String ssid = server.arg("ssid");
    String wpass = server.arg("wpass");
    String name = server.arg("name");
    String host = server.arg("host");
    if (ssid.length()) cfg.wifiSsid = ssid;
    if (wpass.length()) cfg.wifiPass = wpass;
    if (name.length()) cfg.deviceName = name;
    if (host.length()) cfg.mqttHost = host;
    int port = server.arg("port").toInt();
    if (port > 0 && port <= 65535) cfg.mqttPort = (uint16_t)port;
    cfg.mqttUser = server.arg("user");
    cfg.mqttPass = server.arg("pass");
    cfg.haHost = server.arg("hahost");
    int haPort = server.arg("haport").toInt();
    if (haPort > 0 && haPort <= 65535) cfg.haPort = (uint16_t)haPort;
    cfg.haName = server.arg("haname");
    cfg.haDeviceId = server.arg("haid");
    saveConfig();
    mqtt.disconnect();
    server.send(200, "text/plain", "Network settings saved. Restarting...");
    delay(200);
    ESP.restart();
  });

  server.on("/security", HTTP_POST, []() {
    if (!requireAuth()) return;
    String u = server.arg("user");
    String p = server.arg("pass");
    if (u.length()) cfg.adminUser = u;
    if (p.length() >= 6) cfg.adminPass = p;
    saveConfig();
    server.send(200, "text/plain", "Security settings saved. Restarting...");
    delay(200);
    ESP.restart();
  });

  server.on("/restart", HTTP_POST, []() {
    if (!requireAuth()) return;
    server.send(200, "text/plain", "Restarting...");
    delay(200);
    ESP.restart();
  });

  server.on("/factory", HTTP_POST, []() {
    if (!requireAuth()) return;
    server.send(200, "text/plain", "Factory reset...");
    delay(200);
    factoryReset();
  });

}

// Start the existing WebServer only after STA Wi-Fi is actually connected.
// This fixes the dashboard lifecycle without changing any dashboard routes,
// handlers, authentication, MQTT, or device-control logic.
void startDashboard() {
  if (dashboardStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;
  server.begin();
  dashboardStarted = true;
  Serial.print("[DASHBOARD] http://");
  Serial.println(WiFi.localIP());
}

// ========================= CONTROL ENGINE ========================
void updateAllControl() {
  updateSourceDetection();
  updateBatteryState();
  decideCharging();
  updateSourceTransfer();
  updateLightController();
}

// ========================= SETUP / LOOP ===========================
void setup() {
  Serial.begin(115200);
  delay(100);

  loadConfig();
  buildTopics();
  initGPIO();

  setLightPhysical(false);
  requestedLightSource = LIGHT_SOURCE_NONE;
  lightSource = LIGHT_SOURCE_NONE;

  setupWiFi();
  setupOTA();
  setupWebServer();
  startDashboard();

  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);

  updateAllControl();
}

void loop() {
  ArduinoOTA.handle();
  handleFactoryResetButton();

  serviceWiFi();
  if (dashboardStarted) server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    startDashboard();
    connectMQTT();
    if (mqtt.connected()) mqtt.loop();
  }

  updateAllControl();
  updateStatusLED();

  if (mqtt.connected() && !haDiscoveryOk &&
      millis() - lastDiscoveryRetry >= DISCOVERY_RETRY_MS) {
    lastDiscoveryRetry = millis();
    publishHADiscovery();
    publishStatus();
  }

  if (mqtt.connected() && millis() - lastStatusPublish >= STATUS_PUBLISH_MS) {
    lastStatusPublish = millis();
    publishStatus();
  }

  delay(1);
}

