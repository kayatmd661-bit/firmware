#include <Wire.h>
#include <PCF8574.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

// JARVIS HOME CONTROLLER - FINAL
// Fans and Main Gate removed.
// 7 Lights, TV, Sound, Projector, 2 Windows, 4 Curtains, Door.
// Home Assistant MQTT Discovery: one parent Device, multiple entities.
// Dedicated dashboard: Wi-Fi, MQTT, Home Assistant settings, motor timings.
// Motor limit switches remain primary stops; configurable timings are backup timeouts.

// ================= ESP32 INPUT / LIMIT PINS ==================
#define I2C_SDA 21
#define I2C_SCL 22
#define LOCK_BUTTON 23
#define BOOT_BUTTON 0
#define ACCESS_CONTROL_PIN 36
#define LIM_W1_OP 32
#define LIM_W1_CL 33
#define LIM_W2_OP 25
#define LIM_W2_CL 26
#define LIM_C1_OP 27
#define LIM_C1_CL 14
#define LIM_C2_OP 12
#define LIM_C2_CL 13
#define L_DOOR_OP 2
#define L_DOOR_CL 15
#define LIM_C3_OP 4
#define LIM_C3_CL 5
#define LIM_C4_OP 18
#define LIM_C4_CL 19

// ================= PCF8574 OUTPUTS ===========================
#define L1_PIN 0
#define L2_PIN 1
#define L3_PIN 2
#define L4_PIN 3
#define L5_PIN 4
#define L6_PIN 5
#define L7_PIN 6

#define TV_PIN 3
#define PROJ_PIN 5
#define SOUND_PIN 6

#define PIN_M_WIN1_A 0
#define PIN_M_WIN1_B 1
#define PIN_M_WIN2_A 2
#define PIN_M_WIN2_B 3
#define PIN_M_CUR1_A 4
#define PIN_M_CUR1_B 5
#define PIN_M_CUR2_A 6
#define PIN_M_CUR2_B 7
#define PIN_M_CUR3_A 0
#define PIN_M_CUR3_B 1
#define PIN_M_DOOR_A 2
#define PIN_M_DOOR_B 3
#define PIN_M_CUR4_A 6
#define PIN_M_CUR4_B 7

#define WDT_TIMEOUT 8
#define DEFAULT_MOTOR_TIMEOUT_MS 25000UL
#define MAX_MOTOR_TIME_MS 120000UL
#define CONFIG_PORTAL_TIMEOUT_SEC 180

// ================= I2C DEVICES ==============================
PCF8574 pcf1(0x20);
PCF8574 pcf2(0x21);
PCF8574 pcf3(0x22);
PCF8574 pcf4(0x23);

// ================= STORAGE / NETWORK ========================
Preferences pref;
WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);
WiFiManager wifiManager;

WiFiManagerParameter *wmMqttHost = nullptr;
WiFiManagerParameter *wmMqttPort = nullptr;
WiFiManagerParameter *wmMqttUser = nullptr;
WiFiManagerParameter *wmMqttPass = nullptr;
WiFiManagerParameter *wmHaHost = nullptr;
WiFiManagerParameter *wmHaPort = nullptr;
WiFiManagerParameter *wmHaCode = nullptr;

String wifiSsid;
String wifiPassword;
String mqttServer = "homeassistant.local";
uint16_t mqttPort = 1883;
String mqttUser = "esp32";
String mqttPass = "12345678";
String haHost = "homeassistant.local";
uint16_t haPort = 8123;
String haCode;

const char *availTopic = "jarvis/status/availability";
const char *mqttBase = "jarvis";
const char *haDeviceId = "jarvis_home_controller";
const char *haDeviceName = "JARVIS Home Controller";

unsigned long lastReconnectAttempt = 0;
unsigned long resetStartTime = 0;
bool isResetButtonPressed = false;
bool discoverySent = false;
bool dashboardStarted = false;

// ================= DEVICE STATES =============================
enum MotorAction { STOPPED = 0, OPENING = 1, CLOSING = 2 };

MotorAction win1 = STOPPED, win2 = STOPPED;
MotorAction cur1 = STOPPED, cur2 = STOPPED, cur3 = STOPPED, cur4 = STOPPED;
MotorAction door = STOPPED;

unsigned long motorStartTime[7] = {0};
unsigned long motorOpenTime[7];
unsigned long motorCloseTime[7];

bool lightState[7] = {false, false, false, false, false, false, false};
bool tvState = false;
bool soundState = false;
bool projectorState = false;

// ================= HELPERS ==================================
String htmlEscape(const String &s) {
  String r;
  r.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&': r += F("&amp;"); break;
      case '<': r += F("&lt;"); break;
      case '>': r += F("&gt;"); break;
      case '"': r += F("&quot;"); break;
      case '\'': r += F("&#39;"); break;
      default: r += s[i]; break;
    }
  }
  return r;
}

bool parsePort(const String &value, uint16_t &out) {
  if (value.length() == 0) return false;
  long p = value.toInt();
  if (p < 1 || p > 65535) return false;
  out = (uint16_t)p;
  return true;
}

unsigned long secondsToMs(uint16_t seconds) {
  unsigned long ms = (unsigned long)seconds * 1000UL;
  if (ms < 1000UL) ms = 1000UL;
  if (ms > MAX_MOTOR_TIME_MS) ms = MAX_MOTOR_TIME_MS;
  return ms;
}

String motorText(MotorAction state) {
  if (state == OPENING) return "OPENING";
  if (state == CLOSING) return "CLOSING";
  return "STOPPED";
}

const char *motorName(int index) {
  static const char *names[7] = {
    "জানালা ১", "জানালা ২", "পর্দা ১", "পর্দা ২",
    "দরজা", "পর্দা ৩", "পর্দা ৪"
  };
  return names[index];
}

void saveMotorTimes() {
  for (int i = 0; i < 7; i++) {
    pref.putULong(("op" + String(i)).c_str(), motorOpenTime[i]);
    pref.putULong(("cl" + String(i)).c_str(), motorCloseTime[i]);
  }
}

void loadMotorTimes() {
  for (int i = 0; i < 7; i++) {
    motorOpenTime[i] = pref.getULong(("op" + String(i)).c_str(), DEFAULT_MOTOR_TIMEOUT_MS);
    motorCloseTime[i] = pref.getULong(("cl" + String(i)).c_str(), DEFAULT_MOTOR_TIMEOUT_MS);
    if (motorOpenTime[i] < 1000UL || motorOpenTime[i] > MAX_MOTOR_TIME_MS)
      motorOpenTime[i] = DEFAULT_MOTOR_TIMEOUT_MS;
    if (motorCloseTime[i] < 1000UL || motorCloseTime[i] > MAX_MOTOR_TIME_MS)
      motorCloseTime[i] = DEFAULT_MOTOR_TIMEOUT_MS;
  }
}

void loadNetworkConfig() {
  mqttServer = pref.getString("mqtt_host", "homeassistant.local");
  mqttPort = pref.getUShort("mqtt_port", 1883);
  mqttUser = pref.getString("mqtt_user", "esp32");
  mqttPass = pref.getString("mqtt_pass", "12345678");
  haHost = pref.getString("ha_host", "homeassistant.local");
  haPort = pref.getUShort("ha_port", 8123);
  haCode = pref.getString("ha_code", "");
  wifiSsid = pref.getString("wifi_ssid", "");
  wifiPassword = pref.getString("wifi_pass", "");

  if (mqttPort == 0) mqttPort = 1883;
  if (haPort == 0) haPort = 8123;
}

void saveNetworkConfig() {
  pref.putString("mqtt_host", mqttServer);
  pref.putUShort("mqtt_port", mqttPort);
  pref.putString("mqtt_user", mqttUser);
  pref.putString("mqtt_pass", mqttPass);
  pref.putString("ha_host", haHost);
  pref.putUShort("ha_port", haPort);
  pref.putString("ha_code", haCode);
  pref.putString("wifi_ssid", wifiSsid);
  pref.putString("wifi_pass", wifiPassword);
}

// ================= MQTT =====================================
void reportStatus(const String &uid, bool state) {
  if (!client.connected()) return;
  String topic = String(mqttBase) + "/status/" + uid;
  String payload = state ? "ON" : "OFF";
  client.publish(topic.c_str(), payload.c_str(), true);
}

void reportCoverStatus(const String &uid, MotorAction state) {
  if (!client.connected()) return;
  String topic = String(mqttBase) + "/status/" + uid;
  String payload = motorText(state);
  client.publish(topic.c_str(), payload.c_str(), true);
}

void reportCoverPosition(const String &uid, int position) {
  if (!client.connected()) return;
  if (position < 0) position = 0;
  if (position > 100) position = 100;
  String topic = String(mqttBase) + "/status/" + uid + "/position";
  String payload = String(position);
  client.publish(topic.c_str(), payload.c_str(), true);
}

String discoveryPayload(const char *component, const char *name, const char *uid) {
  // Build a standards-compliant Home Assistant MQTT Discovery payload.
  // Keep the existing topics, UIDs, device grouping and hardware command
  // protocol unchanged; only the discovery metadata is corrected.
  String statusTopic = String(mqttBase) + "/status/" + uid;
  String cmdTopic = String(mqttBase) + "/" + uid + "/cmd";

  String p = "{";
  p += "\"name\":\"" + String(name) + "\",";
  p += "\"unique_id\":\"jarvis_" + String(uid) + "\",";
  p += "\"platform\":\"" + String(component) + "\",";
  p += "\"availability_topic\":\"" + String(availTopic) + "\",";
  p += "\"payload_available\":\"online\",";
  p += "\"payload_not_available\":\"offline\",";
  p += "\"state_topic\":\"" + statusTopic + "\",";
  p += "\"command_topic\":\"" + cmdTopic + "\",";

  if (strcmp(component, "light") == 0) {
    // On/off only. The previous schema=template declaration was invalid
    // without command_on_template/command_off_template and could cause HA
    // to reject the discovery config.
    p += "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",";
  } else if (strcmp(component, "cover") == 0) {
    p += "\"payload_open\":\"OPEN\",\"payload_close\":\"CLOSE\",\"payload_stop\":\"STOP\",";
    p += "\"state_opening\":\"OPENING\",\"state_closing\":\"CLOSING\",\"state_stopped\":\"STOPPED\",";
  } else {
    p += "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",";
  }

  p += "\"device\":{";
  p += "\"identifiers\":[\"" + String(haDeviceId) + "\"],";
  p += "\"name\":\"" + String(haDeviceName) + "\",";
  p += "\"manufacturer\":\"Liton Tech\",";
  p += "\"model\":\"ESP32-PCF8574 Home Controller\",";
  p += "\"sw_version\":\"Final-Configurable-1.0\"}";
  p += "}";
  return p;
}

void sendDiscovery(const char *component, const char *name, const char *uid) {
  if (!client.connected()) return;
  String topic = "homeassistant/" + String(component) + "/jarvis_final/" + uid + "/config";
  String payload = discoveryPayload(component, name, uid);
  bool published = client.publish(topic.c_str(), payload.c_str(), true);

  // Diagnostic only: no hardware or control logic is changed.
  Serial.print("MQTT Discovery ");
  Serial.print(component);
  Serial.print("/");
  Serial.print(uid);
  Serial.print(" -> ");
  Serial.print(published ? "OK" : "FAILED");
  Serial.print(" (bytes=");
  Serial.print(payload.length());
  Serial.println(")");
}

void setupAutoDiscovery() {
  if (discoverySent || !client.connected()) return;

  for (int i = 0; i < 7; i++) {
    String uid = "l" + String(i + 1);
    String name = "লাইট " + String(i + 1);
    sendDiscovery("light", name.c_str(), uid.c_str());
  }

  sendDiscovery("switch", "টিভি", "tv");
  sendDiscovery("switch", "সাউন্ড", "sound");
  sendDiscovery("switch", "প্রজেক্টর", "proj");

  sendDiscovery("cover", "দরজা", "door");
  sendDiscovery("cover", "জানালা ১", "win1");
  sendDiscovery("cover", "জানালা ২", "win2");
  sendDiscovery("cover", "পর্দা ১", "cur1");
  sendDiscovery("cover", "পর্দা ২", "cur2");
  sendDiscovery("cover", "পর্দা ৩", "cur3");
  sendDiscovery("cover", "পর্দা ৪", "cur4");

  discoverySent = true;
}

void syncAllStates() {
  for (int i = 0; i < 7; i++)
    reportStatus("l" + String(i + 1), lightState[i]);

  reportStatus("tv", tvState);
  reportStatus("sound", soundState);
  reportStatus("proj", projectorState);

  reportCoverStatus("door", door);
  reportCoverStatus("win1", win1);
  reportCoverStatus("win2", win2);
  reportCoverStatus("cur1", cur1);
  reportCoverStatus("cur2", cur2);
  reportCoverStatus("cur3", cur3);
  reportCoverStatus("cur4", cur4);
}

bool tryReconnect() {
  if (client.connected()) return true;

  client.setServer(mqttServer.c_str(), mqttPort);
  client.setSocketTimeout(1);

  String clientId = "Jarvis_Home_Controller-" +
                    String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;
  if (mqttUser.length()) {
    ok = client.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                        availTopic, 1, true, "offline");
  } else {
    ok = client.connect(clientId.c_str(), availTopic, 1, true, "offline");
  }

  if (!ok) return false;

  client.publish(availTopic, "online", true);
  client.subscribe("jarvis/+/cmd");
  setupAutoDiscovery();
  syncAllStates();

  Serial.println("MQTT Connected!");
  return true;
}

// ================= HARDWARE =================================
void setLightHardware(int index, bool state) {
  const int pins[7] = {L1_PIN,L2_PIN,L3_PIN,L4_PIN,L5_PIN,L6_PIN,L7_PIN};
  pcf3.write(pins[index], state ? HIGH : LOW);
  lightState[index] = state;
  pref.putBool(("l" + String(index + 1)).c_str(), state);
  reportStatus("l" + String(index + 1), state);
}

void setAVHardware(const String &uid, bool state) {
  if (uid == "tv") {
    pcf4.write(TV_PIN, state ? HIGH : LOW);
    tvState = state;
    pref.putBool("tv", state);
    reportStatus("tv", state);
  } else if (uid == "sound") {
    pcf4.write(SOUND_PIN, state ? HIGH : LOW);
    soundState = state;
    pref.putBool("sound", state);
    reportStatus("sound", state);
  } else if (uid == "proj") {
    pcf4.write(PROJ_PIN, state ? HIGH : LOW);
    projectorState = state;
    pref.putBool("proj", state);
    reportStatus("proj", state);

    // Existing cascade preserved: Projector OFF -> Curtain 4 CLOSE.
    if (!state) {
      Serial.println("Cascade: Projector OFF -> Closing Curtain 4");
      startMotorCommand("cur4", false);
    }
  }
}

bool getLimitsForIndex(int idx, bool &openHit, bool &closeHit) {
  openHit = closeHit = false;
  switch (idx) {
    case 0: openHit = digitalRead(LIM_W1_OP) == LOW; closeHit = digitalRead(LIM_W1_CL) == LOW; break;
    case 1: openHit = digitalRead(LIM_W2_OP) == LOW; closeHit = digitalRead(LIM_W2_CL) == LOW; break;
    case 2: openHit = digitalRead(LIM_C1_OP) == LOW; closeHit = digitalRead(LIM_C1_CL) == LOW; break;
    case 3: openHit = digitalRead(LIM_C2_OP) == LOW; closeHit = digitalRead(LIM_C2_CL) == LOW; break;
    case 4: openHit = digitalRead(L_DOOR_OP) == LOW; closeHit = digitalRead(L_DOOR_CL) == LOW; break;
    case 5: openHit = digitalRead(LIM_C3_OP) == LOW; closeHit = digitalRead(LIM_C3_CL) == LOW; break;
    case 6: openHit = digitalRead(LIM_C4_OP) == LOW; closeHit = digitalRead(LIM_C4_CL) == LOW; break;
    default: return false;
  }
  return true;
}

MotorAction *getMotorState(const String &uid, int &idx) {
  idx = -1;
  if (uid == "win1") { idx = 0; return &win1; }
  if (uid == "win2") { idx = 1; return &win2; }
  if (uid == "cur1") { idx = 2; return &cur1; }
  if (uid == "cur2") { idx = 3; return &cur2; }
  if (uid == "door") { idx = 4; return &door; }
  if (uid == "cur3") { idx = 5; return &cur3; }
  if (uid == "cur4") { idx = 6; return &cur4; }
  return nullptr;
}

void startMotorCommand(const String &uid, bool open) {
  int idx;
  MotorAction *state = getMotorState(uid, idx);
  if (!state) return;

  bool openHit, closeHit;
  getLimitsForIndex(idx, openHit, closeHit);

  if (open && openHit) {
    *state = STOPPED;
    reportCoverStatus(uid, STOPPED);
    reportCoverPosition(uid, 100);
    return;
  }

  if (!open && closeHit) {
    *state = STOPPED;
    reportCoverStatus(uid, STOPPED);
    reportCoverPosition(uid, 0);
    return;
  }

  *state = open ? OPENING : CLOSING;
  motorStartTime[idx] = millis();
  reportCoverStatus(uid, *state);
}

void handleMotor(MotorAction &state, PCF8574 &pcf, int in1, int in2,
                 bool opHit, bool clHit, int idx, const String &uid) {
  if (state == STOPPED) {
    pcf.write(in1, LOW);
    pcf.write(in2, LOW);
    return;
  }

  unsigned long elapsed = millis() - motorStartTime[idx];
  unsigned long timeout = (state == OPENING) ? motorOpenTime[idx] : motorCloseTime[idx];

  bool limitReached = (state == OPENING) ? opHit : clHit;

  if (limitReached || elapsed >= timeout) {
    pcf.write(in1, LOW);
    pcf.write(in2, LOW);
    state = STOPPED;
    reportCoverStatus(uid, STOPPED);
    reportCoverPosition(uid, limitReached && state == STOPPED ? (opHit ? 100 : 0) : -1);
    return;
  }

  pcf.write(in1, state == OPENING ? HIGH : LOW);
  pcf.write(in2, state == CLOSING ? HIGH : LOW);

  int position = (int)min(99UL, (elapsed * 100UL) / timeout);
  if (state == CLOSING) position = 100 - position;
  reportCoverPosition(uid, position);
}

void stopMotorById(const String &uid) {
  int idx;
  MotorAction *state = getMotorState(uid, idx);
  if (!state) return;
  *state = STOPPED;
  reportCoverStatus(uid, STOPPED);
}

void stopAllMotors() {
  pcf1.write(PIN_M_WIN1_A, LOW); pcf1.write(PIN_M_WIN1_B, LOW);
  pcf1.write(PIN_M_WIN2_A, LOW); pcf1.write(PIN_M_WIN2_B, LOW);
  pcf1.write(PIN_M_CUR1_A, LOW); pcf1.write(PIN_M_CUR1_B, LOW);
  pcf1.write(PIN_M_CUR2_A, LOW); pcf1.write(PIN_M_CUR2_B, LOW);

  pcf2.write(PIN_M_CUR3_A, LOW); pcf2.write(PIN_M_CUR3_B, LOW);
  pcf2.write(PIN_M_DOOR_A, LOW); pcf2.write(PIN_M_DOOR_B, LOW);
  pcf2.write(PIN_M_CUR4_A, LOW); pcf2.write(PIN_M_CUR4_B, LOW);

  win1 = win2 = cur1 = cur2 = cur3 = cur4 = door = STOPPED;
}

// ================= COMMAND ROUTING ==========================
void executeHardwareByID(String uid, String cmd) {
  uid.trim();
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "STOP") {
    stopMotorById(uid);
    return;
  }

  bool state = (cmd == "ON" || cmd == "OPEN");

  Serial.println("--- Hardware Execute ---");
  Serial.print("Device UID: "); Serial.println(uid);
  Serial.print("Command: "); Serial.println(cmd);

  if (uid == "l1") setLightHardware(0, state);
  else if (uid == "l2") setLightHardware(1, state);
  else if (uid == "l3") setLightHardware(2, state);
  else if (uid == "l4") setLightHardware(3, state);
  else if (uid == "l5") setLightHardware(4, state);
  else if (uid == "l6") setLightHardware(5, state);
  else if (uid == "l7") setLightHardware(6, state);
  else if (uid == "tv" || uid == "sound" || uid == "proj") setAVHardware(uid, state);
  else if (uid == "win1" || uid == "win2" || uid == "cur1" ||
           uid == "cur2" || uid == "door" || uid == "cur3" || uid == "cur4")
    startMotorCommand(uid, state);
}

// ================= STATE RECOVERY ===========================
void recoverStates() {
  for (int i = 0; i < 7; i++) {
    lightState[i] = pref.getBool(("l" + String(i + 1)).c_str(), false);
    const int pins[7] = {L1_PIN,L2_PIN,L3_PIN,L4_PIN,L5_PIN,L6_PIN,L7_PIN};
    pcf3.write(pins[i], lightState[i] ? HIGH : LOW);
  }

  tvState = pref.getBool("tv", false);
  soundState = pref.getBool("sound", false);
  projectorState = pref.getBool("proj", false);

  pcf4.write(TV_PIN, tvState ? HIGH : LOW);
  pcf4.write(SOUND_PIN, soundState ? HIGH : LOW);
  pcf4.write(PROJ_PIN, projectorState ? HIGH : LOW);

  // Never restore a moving motor after reboot.
  stopAllMotors();
}

// ================= MQTT CALLBACK ============================
void callback(char *topic, byte *payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  String topicStr = String(topic);
  int first = topicStr.indexOf('/');
  int second = topicStr.indexOf('/', first + 1);
  if (first < 0 || second < 0) return;

  String uid = topicStr.substring(first + 1, second);
  uid.trim();

  executeHardwareByID(uid, message);
}

// ================= DASHBOARD ================================
String dashboardHtml() {
  String h;
  h.reserve(15000);

  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>JARVIS Home Controller</title><style>");
  h += F("body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:18px}");
  h += F(".wrap{max-width:1050px;margin:auto}.card{background:#1d1d1d;border-radius:12px;padding:18px;margin:12px 0}");
  h += F("h1,h2{margin-top:0}label{display:block;margin:10px 0 4px}");
  h += F("input{width:100%;box-sizing:border-box;padding:10px;border-radius:7px;border:1px solid #555;background:#292929;color:#fff}");
  h += F("button{padding:10px 14px;border:0;border-radius:7px;margin:6px 4px 6px 0;cursor:pointer}");
  h += F(".save{background:#2e8b57;color:#fff}.danger{background:#a33;color:#fff}.muted{color:#aaa}");
  h += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}");
  h += F("table{width:100%;border-collapse:collapse}td,th{border-bottom:1px solid #444;padding:8px;text-align:left}");
  h += F("</style></head><body><div class='wrap'>");

  h += F("<div class='card'><h1>JARVIS Home Controller</h1>");
  h += F("<div class='muted'>7 Lights | TV | Sound | Projector | Windows | Curtains | Door</div>");
  h += F("<p>Wi-Fi: ");
  h += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Disconnected";
  h += F(" | MQTT: ");
  h += client.connected() ? "Connected" : "Disconnected";
  h += F("</p></div>");

  h += F("<form method='POST' action='/save'>");

  h += F("<div class='card'><h2>Wi-Fi</h2>");
  h += F("<label>Wi-Fi Name / SSID</label><input name='wifi_ssid' value='");
  h += htmlEscape(wifiSsid);
  h += F("'><label>Wi-Fi Password</label><input type='password' name='wifi_pass' value='");
  h += htmlEscape(wifiPassword);
  h += F("'></div>");

  h += F("<div class='card'><h2>MQTT</h2>");
  h += F("<label>MQTT URL / Host</label><input name='mqtt_host' value='");
  h += htmlEscape(mqttServer);
  h += F("'><label>MQTT Port</label><input type='number' min='1' max='65535' name='mqtt_port' value='");
  h += String(mqttPort);
  h += F("'><label>MQTT Username</label><input name='mqtt_user' value='");
  h += htmlEscape(mqttUser);
  h += F("'><label>MQTT Password</label><input type='password' name='mqtt_pass' value='");
  h += htmlEscape(mqttPass);
  h += F("'></div>");

  h += F("<div class='card'><h2>Home Assistant</h2>");
  h += F("<label>Home Assistant Local Host / IP</label><input name='ha_host' value='");
  h += htmlEscape(haHost);
  h += F("'><label>Home Assistant Port</label><input type='number' min='1' max='65535' name='ha_port' value='");
  h += String(haPort);
  h += F("'><label>Home Assistant Code / Identifier</label><input name='ha_code' value='");
  h += htmlEscape(haCode);
  h += F("'><p class='muted'>এই firmware HA MQTT Discovery ব্যবহার করে; HA fields persistent configuration হিসেবে সংরক্ষিত থাকে।</p></div>");

  h += F("<div class='card'><h2>Motor Timing</h2><div class='grid'>");
  for (int i = 0; i < 7; i++) {
    h += F("<div><h3>"); h += motorName(i); h += F("</h3>");
    h += F("<label>Open Time (seconds)</label><input type='number' min='1' max='120' name='op");
    h += String(i); h += F("' value='"); h += String(motorOpenTime[i] / 1000UL); h += F("'>");
    h += F("<label>Close Time (seconds)</label><input type='number' min='1' max='120' name='cl");
    h += String(i); h += F("' value='"); h += String(motorCloseTime[i] / 1000UL); h += F("'></div>");
  }
  h += F("</div><button class='save' type='submit'>Save Configuration</button></div></form>");

  h += F("<div class='card'><h2>System</h2>");
  h += F("<form method='POST' action='/restart'><button type='submit'>Restart ESP32</button></form>");
  h += F("<form method='POST' action='/resetwifi'><button class='danger' type='submit'>Reset Wi-Fi Settings</button></form>");
  h += F("<p class='muted'>Firmware: Final-Configurable-1.0</p></div>");

  h += F("<div class='card'><h2>Current State</h2><table><tr><th>Device</th><th>Status</th></tr>");
  for (int i = 0; i < 7; i++) {
    h += F("<tr><td>লাইট "); h += String(i + 1); h += F("</td><td>");
    h += lightState[i] ? "ON" : "OFF"; h += F("</td></tr>");
  }
  h += F("<tr><td>TV</td><td>"); h += tvState ? "ON" : "OFF"; h += F("</td></tr>");
  h += F("<tr><td>Sound</td><td>"); h += soundState ? "ON" : "OFF"; h += F("</td></tr>");
  h += F("<tr><td>Projector</td><td>"); h += projectorState ? "ON" : "OFF"; h += F("</td></tr>");

  MotorAction states[7] = {win1,win2,cur1,cur2,door,cur3,cur4};
  for (int i = 0; i < 7; i++) {
    h += F("<tr><td>"); h += motorName(i); h += F("</td><td>");
    h += motorText(states[i]); h += F("</td></tr>");
  }
  h += F("</table></div></div></body></html>");
  return h;
}

void handleDashboard() {
  server.send(200, "text/html; charset=utf-8", dashboardHtml());
}

void handleSave() {
  if (server.hasArg("wifi_ssid")) wifiSsid = server.arg("wifi_ssid");
  if (server.hasArg("wifi_pass")) wifiPassword = server.arg("wifi_pass");
  if (server.hasArg("mqtt_host")) mqttServer = server.arg("mqtt_host");
  if (server.hasArg("mqtt_port")) parsePort(server.arg("mqtt_port"), mqttPort);
  if (server.hasArg("mqtt_user")) mqttUser = server.arg("mqtt_user");
  if (server.hasArg("mqtt_pass")) mqttPass = server.arg("mqtt_pass");
  if (server.hasArg("ha_host")) haHost = server.arg("ha_host");
  if (server.hasArg("ha_port")) parsePort(server.arg("ha_port"), haPort);
  if (server.hasArg("ha_code")) haCode = server.arg("ha_code");

  for (int i = 0; i < 7; i++) {
    String opName = "op" + String(i);
    String clName = "cl" + String(i);
    if (server.hasArg(opName)) {
      uint16_t sec;
      if (parsePort(server.arg(opName), sec)) motorOpenTime[i] = secondsToMs(sec);
    }
    if (server.hasArg(clName)) {
      uint16_t sec;
      if (parsePort(server.arg(clName), sec)) motorCloseTime[i] = secondsToMs(sec);
    }
  }

  saveNetworkConfig();
  saveMotorTimes();
  client.disconnect();
  discoverySent = false;

  server.send(200, "text/html; charset=utf-8",
              "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2;url=/'></head>"
              "<body style='font-family:Arial;padding:30px'><h2>Configuration saved.</h2>"
              "<p>MQTT will reconnect using the new settings.</p></body></html>");
}

void handleRestart() {
  server.send(200, "text/html; charset=utf-8",
              "<html><body><h2>Restarting...</h2></body></html>");
  delay(100);
  ESP.restart();
}

void handleResetWifi() {
  pref.remove("wifi_ssid");
  pref.remove("wifi_pass");
  server.send(200, "text/html; charset=utf-8",
              "<html><body><h2>Wi-Fi settings cleared.</h2><p>Restarting...</p></body></html>");
  delay(100);
  ESP.restart();
}

void startDashboard() {
  if (dashboardStarted || WiFi.status() != WL_CONNECTED) return;
  server.on("/", HTTP_GET, handleDashboard);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/resetwifi", HTTP_POST, handleResetWifi);
  server.begin();
  dashboardStarted = true;
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.localIP());
}

// ================= WIFI MANAGER ==============================
// WiFiManager is intentionally non-blocking. The configuration portal must
// never hold the main task for longer than the watchdog timeout.
volatile bool wifiManagerConfigSaved = false;

void onWiFiManagerSaveConfig() {
  wifiManagerConfigSaved = true;
}

void buildWiFiManagerParameters() {
  if (wmMqttHost) return;

  String mp = String(mqttPort);
  String hp = String(haPort);

  wmMqttHost = new WiFiManagerParameter("mqtt_host", "MQTT Host / URL",
                                        mqttServer.c_str(), 96);
  wmMqttPort = new WiFiManagerParameter("mqtt_port", "MQTT Port",
                                        mp.c_str(), 6);
  wmMqttUser = new WiFiManagerParameter("mqtt_user", "MQTT Username",
                                        mqttUser.c_str(), 64);
  wmMqttPass = new WiFiManagerParameter("mqtt_pass", "MQTT Password",
                                        mqttPass.c_str(), 64);
  wmHaHost = new WiFiManagerParameter("ha_host", "Home Assistant Host / IP",
                                      haHost.c_str(), 96);
  wmHaPort = new WiFiManagerParameter("ha_port", "Home Assistant Port",
                                      hp.c_str(), 6);
  wmHaCode = new WiFiManagerParameter("ha_code", "Home Assistant Code / Identifier",
                                      haCode.c_str(), 96);

  wifiManager.addParameter(wmMqttHost);
  wifiManager.addParameter(wmMqttPort);
  wifiManager.addParameter(wmMqttUser);
  wifiManager.addParameter(wmMqttPass);
  wifiManager.addParameter(wmHaHost);
  wifiManager.addParameter(wmHaPort);
  wifiManager.addParameter(wmHaCode);

  // IMPORTANT: keep the configuration portal non-blocking. The old blocking
  // autoConnect() could leave the ESP32 task without a watchdog feed and
  // cause rst:0xc / SW_CPU_RESET before the user could enter Wi-Fi details.
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);
  wifiManager.setBreakAfterConfig(true);
  wifiManager.setSaveConfigCallback(onWiFiManagerSaveConfig);
}

void saveWiFiManagerValues() {
  if (wmMqttHost) mqttServer = wmMqttHost->getValue();
  if (wmMqttPort) parsePort(wmMqttPort->getValue(), mqttPort);
  if (wmMqttUser) mqttUser = wmMqttUser->getValue();
  if (wmMqttPass) mqttPass = wmMqttPass->getValue();
  if (wmHaHost) haHost = wmHaHost->getValue();
  if (wmHaPort) parsePort(wmHaPort->getValue(), haPort);
  if (wmHaCode) haCode = wmHaCode->getValue();

  wifiSsid = WiFi.SSID();
  saveNetworkConfig();
}

// ================= OTA / WDT ================================
void setupOTA() {
  ArduinoOTA.setHostname("jarvis-home-controller");
  ArduinoOTA.onStart([]() {
    stopAllMotors();
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA End"); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

void setupWatchdog() {
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&twdt_config);
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    Serial.printf("WDT add error: %d\n", err);
}

// ================= SETUP ====================================
void setup() {
  Serial.begin(115200);
  delay(50);

  setupWatchdog();

  pref.begin("jarvis", false);
  loadNetworkConfig();
  loadMotorTimes();

  Wire.begin(I2C_SDA, I2C_SCL);
  pcf1.begin();
  pcf2.begin();
  pcf3.begin();
  pcf4.begin();

  stopAllMotors();

  int inputPins[] = {
    LIM_W1_OP, LIM_W1_CL, LIM_W2_OP, LIM_W2_CL,
    LIM_C1_OP, LIM_C1_CL, LIM_C2_OP, LIM_C2_CL,
    L_DOOR_OP, L_DOOR_CL, LIM_C3_OP, LIM_C3_CL,
    LIM_C4_OP, LIM_C4_CL, LOCK_BUTTON, BOOT_BUTTON
  };
  for (size_t i = 0; i < sizeof(inputPins) / sizeof(inputPins[0]); i++)
    pinMode(inputPins[i], INPUT_PULLUP);

  pinMode(ACCESS_CONTROL_PIN, INPUT);

  recoverStates();

  client.setCallback(callback);
  client.setSocketTimeout(1);

  // Home Assistant MQTT Discovery payloads contain device metadata and are
  // larger than PubSubClient's default 256-byte MQTT packet buffer. Without
  // enlarging this buffer, client.publish() silently fails for discovery
  // messages even though the MQTT connection itself succeeds.
  client.setBufferSize(1024);

  buildWiFiManagerParameters();

  Serial.println("Starting Wi-Fi / first-time configuration...");

  // Non-blocking: autoConnect() starts the AP/portal (when required) and
  // immediately returns control to loop(). This prevents the 8-second WDT
  // from resetting the ESP32 while the user is entering Wi-Fi credentials.
  bool wifiOk = wifiManager.autoConnect("Jarvis_Config_AP");

  if (wifiOk && WiFi.status() == WL_CONNECTED) {
    saveWiFiManagerValues();
    startDashboard();
    setupOTA();
    Serial.println("Wi-Fi connected.");
  } else {
    Serial.println("Wi-Fi connection pending. Jarvis_Config_AP remains available.");
  }

  client.setServer(mqttServer.c_str(), mqttPort);
  Serial.println("Jarvis System Online!");
}

// ================= RUNTIME ==================================
void processResetButton() {
  bool pressed = (digitalRead(LOCK_BUTTON) == LOW ||
                  digitalRead(BOOT_BUTTON) == LOW);

  if (pressed) {
    if (!isResetButtonPressed) {
      resetStartTime = millis();
      isResetButtonPressed = true;
    }

    if (millis() - resetStartTime >= 10000UL) {
      Serial.println("Long press: clearing Wi-Fi settings.");
      pref.remove("wifi_ssid");
      pref.remove("wifi_pass");
      ESP.restart();
    }
  } else {
    isResetButtonPressed = false;
  }
}

void processMotors() {
  handleMotor(win1, pcf1, PIN_M_WIN1_A, PIN_M_WIN1_B,
              digitalRead(LIM_W1_OP) == LOW, digitalRead(LIM_W1_CL) == LOW,
              0, "win1");

  handleMotor(win2, pcf1, PIN_M_WIN2_A, PIN_M_WIN2_B,
              digitalRead(LIM_W2_OP) == LOW, digitalRead(LIM_W2_CL) == LOW,
              1, "win2");

  handleMotor(cur1, pcf1, PIN_M_CUR1_A, PIN_M_CUR1_B,
              digitalRead(LIM_C1_OP) == LOW, digitalRead(LIM_C1_CL) == LOW,
              2, "cur1");

  handleMotor(cur2, pcf1, PIN_M_CUR2_A, PIN_M_CUR2_B,
              digitalRead(LIM_C2_OP) == LOW, digitalRead(LIM_C2_CL) == LOW,
              3, "cur2");

  handleMotor(door, pcf2, PIN_M_DOOR_A, PIN_M_DOOR_B,
              digitalRead(L_DOOR_OP) == LOW, digitalRead(L_DOOR_CL) == LOW,
              4, "door");

  handleMotor(cur3, pcf2, PIN_M_CUR3_A, PIN_M_CUR3_B,
              digitalRead(LIM_C3_OP) == LOW, digitalRead(LIM_C3_CL) == LOW,
              5, "cur3");

  handleMotor(cur4, pcf2, PIN_M_CUR4_A, PIN_M_CUR4_B,
              digitalRead(LIM_C4_OP) == LOW, digitalRead(LIM_C4_CL) == LOW,
              6, "cur4");
}

void processCascade() {
  static MotorAction prevCur3 = STOPPED;
  static MotorAction prevCur4 = STOPPED;

  if (prevCur3 == OPENING && cur3 == STOPPED) {
    Serial.println("Cascade: Curtain 3 Opened -> Opening Curtain 4");
    startMotorCommand("cur4", true);
  }

  if (prevCur4 == OPENING && cur4 == STOPPED) {
    Serial.println("Cascade: Curtain 4 Opened -> Turning ON Projector");
    setAVHardware("proj", true);
  }

  if (prevCur4 == CLOSING && cur4 == STOPPED) {
    Serial.println("Cascade: Curtain 4 Closed -> Closing Curtain 3");
    startMotorCommand("cur3", false);
  }

  prevCur3 = cur3;
  prevCur4 = cur4;
}

void loop() {
  // Feed immediately because WiFiManager, dashboard, MQTT and motor control
  // are all serviced from this same cooperative loop.
  esp_task_wdt_reset();

  // CRITICAL: WiFiManager portal processing must happen continuously.
  // This is what allows Jarvis_Config_AP to remain usable without blocking
  // the ESP32 task and triggering the watchdog.
  wifiManager.process();

  // When the WiFiManager portal accepts new configuration, copy the custom
  // fields into Preferences without changing any existing hardware logic.
  if (wifiManagerConfigSaved) {
    wifiManagerConfigSaved = false;
    saveWiFiManagerValues();
    discoverySent = false;
    Serial.println("WiFiManager configuration saved.");
  }

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  // Start network-dependent services only after Wi-Fi actually comes up.
  if (wifiConnected) {
    if (!dashboardStarted) {
      startDashboard();
      setupOTA();
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
    }

    if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= 5000UL) {
        lastReconnectAttempt = now;
        if (tryReconnect()) lastReconnectAttempt = 0;
      }
    } else {
      client.loop();
    }
  }

  if (dashboardStarted) server.handleClient();
  ArduinoOTA.handle();

  processResetButton();
  processMotors();
  processCascade();

  // Feed again after all cooperative tasks.
  esp_task_wdt_reset();
  delay(1);
}
