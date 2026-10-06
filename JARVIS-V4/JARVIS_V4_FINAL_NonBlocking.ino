#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

// ============================================================
// JARVIS V4 FINAL
// Existing hardware logic retained.
// Added/fixed:
// - Non-blocking WiFiManager
// - WiFi/MQTT configuration stored in Preferences
// - MQTT settings available in first-time WiFi portal
// - Dedicated dashboard on HTTP port 80
// - WiFiManager portal on HTTP port 81 so both can coexist
// - Automatic WiFi retry / setup AP
// - MQTT reconnect never blocks the hardware loop
// - Main gate 1500ms/2000ms delays made non-blocking
// - mDNS: http://jarvis-v4.local/
// ============================================================

#define WDT_TIMEOUT 3

// DEFAULT values only. They are used when no saved value exists.
const char* DEFAULT_MQTT_SERVER = "homeassistant.local";
const uint16_t DEFAULT_MQTT_PORT = 1883;
const char* DEFAULT_MQTT_USER = "esp32";
const char* DEFAULT_MQTT_PASS = "12345678";
const char* DEFAULT_AVAILABILITY_TOPIC = "jarvis_v4/status/availability";
const char* DEFAULT_HOSTNAME = "jarvis-v4";

// ============================================================
// GPIO CONFIGURATION - UNCHANGED
// ============================================================

#define MAIN_GATE_MOTOR_FWD 12
#define MAIN_GATE_MOTOR_REV 13
#define MAIN_GATE_LOCK      14
#define MAIN_GATE_BTN       33
#define LIMIT_OPEN          27
#define LIMIT_CLOSE         26
#define MAIN_OPTO_IN        34

#define BD1_LOCK            21
#define BD1_BTN             4
#define BD1_OPTO_IN        35

#define BLC_LOCK            19
#define BLC_BTN             23
#define BLC_OPTO_IN         36

#define BD2_LOCK            5
#define BD2_BTN             16
#define BD2_OPTO_IN         39

// ============================================================
// OBJECTS
// ============================================================

WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;
WebServer server(80);
WiFiManager wm;

// WiFiManager custom parameters must remain alive for non-blocking mode.
WiFiManagerParameter* p_mqtt_server = nullptr;
WiFiManagerParameter* p_mqtt_port = nullptr;
WiFiManagerParameter* p_mqtt_user = nullptr;
WiFiManagerParameter* p_mqtt_pass = nullptr;
WiFiManagerParameter* p_availability = nullptr;
WiFiManagerParameter* p_hostname = nullptr;

// ============================================================
// MOTOR STATE
// ============================================================

enum MotorAction {
    STOPPED = 0,
    OPENING = 1,
    CLOSING = 2
};

MotorAction mainGateAction = STOPPED;
unsigned long motorStartTime = 0;

// These replace blocking delay() calls only.
// Hardware behavior/timing remains the same.
bool gateOpeningDelayPending = false;
unsigned long gateOpeningDelayStart = 0;

bool gateLockDelayPending = false;
unsigned long gateLockDelayStart = 0;

// ============================================================
// DEVICE STATES
// ============================================================

bool bd1S;
bool blcS;
bool bd2S;
bool gateS;
bool lockS;

// ============================================================
// RUNTIME CONFIGURATION
// ============================================================

String mqttServer;
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUser;
String mqttPass;
String mqttAvailabilityTopic;
String deviceHostname = DEFAULT_HOSTNAME;

// ============================================================
// MQTT RECONNECT CONTROL
// ============================================================

unsigned long lastMqttAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

// ============================================================
// WIFI CONTROL
// ============================================================

bool wifiPortalStarted = false;
bool wifiConfigSaved = false;
bool wifiParametersInitialized = false;

unsigned long wifiConnectStarted = 0;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastWifiPortalAttempt = 0;

const unsigned long WIFI_CONNECT_GRACE = 15000;
const unsigned long WIFI_RECONNECT_INTERVAL = 10000;
const unsigned long WIFI_PORTAL_RETRY_INTERVAL = 30000;

// ============================================================
// MDNS
// ============================================================

bool mdnsStarted = false;

// ============================================================
// HTML HELPERS
// ============================================================

String htmlHeader(const String& title) {
    String html;
    html += "<!DOCTYPE html><html lang='en'><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>" + title + "</title>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:0}";
    html += ".container{max-width:700px;margin:auto;padding:20px}";
    html += ".card{background:#1d1d1d;border-radius:12px;padding:18px;margin-bottom:15px;box-shadow:0 2px 10px #000}";
    html += "h1,h2{margin-top:0}";
    html += "input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 12px;border-radius:7px;border:1px solid #555;background:#222;color:#fff}";
    html += "button{padding:12px 18px;border:0;border-radius:7px;cursor:pointer;margin:4px}";
    html += ".save{background:#1976d2;color:white}.danger{background:#b71c1c;color:white}";
    html += ".ok{color:#4caf50}.bad{color:#f44336}.warn{color:#ffb300}";
    html += "table{width:100%;border-collapse:collapse}td{padding:8px;border-bottom:1px solid #333}";
    html += "a{color:#64b5f6}";
    html += "</style></head><body>";
    return html;
}

String htmlFooter() {
    return "</div></body></html>";
}

// ============================================================
// CONFIGURATION STORAGE
// ============================================================

void saveConfiguration() {
    preferences.begin("jarvis_config", false);
    preferences.putString("mqtt_server", mqttServer);
    preferences.putUShort("mqtt_port", mqttPort);
    preferences.putString("mqtt_user", mqttUser);
    preferences.putString("mqtt_pass", mqttPass);
    preferences.putString("avail_topic", mqttAvailabilityTopic);
    preferences.putString("hostname", deviceHostname);
    preferences.end();
    Serial.println("[Config] Configuration saved.");
}

void loadConfiguration() {
    preferences.begin("jarvis_config", true);

    mqttServer = preferences.getString("mqtt_server", DEFAULT_MQTT_SERVER);
    mqttPort = preferences.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
    mqttUser = preferences.getString("mqtt_user", DEFAULT_MQTT_USER);
    mqttPass = preferences.getString("mqtt_pass", DEFAULT_MQTT_PASS);
    mqttAvailabilityTopic = preferences.getString("avail_topic", DEFAULT_AVAILABILITY_TOPIC);
    deviceHostname = preferences.getString("hostname", DEFAULT_HOSTNAME);

    preferences.end();

    if (mqttServer.length() == 0) mqttServer = DEFAULT_MQTT_SERVER;
    if (mqttPort == 0) mqttPort = DEFAULT_MQTT_PORT;
    if (mqttAvailabilityTopic.length() == 0) mqttAvailabilityTopic = DEFAULT_AVAILABILITY_TOPIC;
    if (deviceHostname.length() == 0) deviceHostname = DEFAULT_HOSTNAME;

    Serial.println("[Config] Configuration loaded.");
    Serial.print("[Config] MQTT Server: "); Serial.println(mqttServer);
    Serial.print("[Config] MQTT Port: "); Serial.println(mqttPort);
    Serial.print("[Config] MQTT User: "); Serial.println(mqttUser);
    Serial.print("[Config] Hostname: "); Serial.println(deviceHostname);
}

// ============================================================
// DEVICE STATE STORAGE
// ============================================================

void saveStates() {
    preferences.begin("jarvis_v4_final", false);
    preferences.putBool("bd1", bd1S);
    preferences.putBool("blc", blcS);
    preferences.putBool("bd2", bd2S);
    preferences.putBool("gate", gateS);
    preferences.putBool("lock", lockS);
    preferences.end();
}

void loadStates() {
    preferences.begin("jarvis_v4_final", true);
    bd1S = preferences.getBool("bd1", true);
    blcS = preferences.getBool("blc", true);
    bd2S = preferences.getBool("bd2", true);
    gateS = preferences.getBool("gate", false);
    lockS = preferences.getBool("lock", true);
    preferences.end();
    Serial.println("[Storage] States loaded.");
}

// ============================================================
// MQTT STATUS
// ============================================================

void reportStatus(String uid, bool state) {
    // MQTT unavailable must NEVER stop local hardware.
    if (!client.connected()) return;

    String payload = (uid == "maingate")
        ? (state ? "ON" : "OFF")
        : (state ? "LOCKED" : "UNLOCKED");

    String topic = "jarvis_v4/status/" + uid;
    client.publish(topic.c_str(), payload.c_str(), true);
}

// ============================================================
// MQTT DISCOVERY
// ============================================================

void sendDiscovery(String name, String uid, String unique_suffix, bool isLock) {
    if (!client.connected()) return;

    String domain = isLock ? "lock" : "switch";
    String topic = "homeassistant/" + domain + "/jarvis_" + uid + "/config";

    String devInfo =
        ",\"dev\":{\"ids\":[\"jarvis_master_v4\"],"
        "\"name\":\"Jarvis Home System\","
        "\"mf\":\"Liton Tech\",\"mdl\":\"ESP32-V4\"}";

    String avail =
        ",\"avty_t\":\"" + mqttAvailabilityTopic +
        "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"";

    String payload;

    if (isLock) {
        payload =
            "{\"name\":\"" + name +
            "\",\"stat_t\":\"jarvis_v4/status/" + uid +
            "\",\"cmd_t\":\"jarvis_v4/" + uid + "/cmd" +
            "\",\"uniq_id\":\"" + unique_suffix + "_v13" +
            "\",\"pl_lck\":\"LOCK\",\"pl_unlck\":\"UNLOCK\"" +
            ",\"state_locked\":\"LOCKED\",\"state_unlocked\":\"UNLOCKED\"" +
            avail + devInfo + "}";
    } else {
        payload =
            "{\"name\":\"" + name +
            "\",\"stat_t\":\"jarvis_v4/status/" + uid +
            "\",\"cmd_t\":\"jarvis_v4/" + uid + "/cmd" +
            "\",\"uniq_id\":\"" + unique_suffix + "_v13\"" +
            ",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"" +
            ",\"state_on\":\"ON\",\"state_off\":\"OFF\"" +
            ",\"icon\":\"mdi:gate\"" +
            avail + devInfo + "}";
    }

    client.publish(topic.c_str(), payload.c_str(), true);
}

void publishAllDiscovery() {
    sendDiscovery("মেইন গেট", "maingate", "gate_sw", false);
    sendDiscovery("গেট লক", "mainlock", "gate_lck", true);
    sendDiscovery("বেডরুম ১", "bd1", "bd1_lck", true);
    sendDiscovery("বারান্দা", "blc", "blc_lck", true);
    sendDiscovery("বেডরুম ২", "bd2", "bd2_lck", true);
}

void publishAllStates() {
    reportStatus("maingate", gateS);
    reportStatus("mainlock", lockS);
    reportStatus("bd1", bd1S);
    reportStatus("blc", blcS);
    reportStatus("bd2", bd2S);
}

// ============================================================
// HARDWARE EXECUTION
// ============================================================

void executeHardwareByID(String uid, String cmd) {
    cmd.toUpperCase();

    if (cmd.indexOf(',') > -1) {
        cmd = cmd.substring(cmd.lastIndexOf(',') + 1);
        cmd.trim();
    }

    bool state = (cmd == "LOCK" || cmd == "ON");

    if (uid == "mainlock") {
        lockS = state;
        digitalWrite(MAIN_GATE_LOCK, state);
    }
    else if (uid == "maingate") {
        if (state) {
            digitalWrite(MAIN_GATE_LOCK, HIGH);
            lockS = true;
            reportStatus("mainlock", true);

            // Original delay(1500) is preserved as timing,
            // but implemented without blocking the main loop.
            gateOpeningDelayPending = true;
            gateOpeningDelayStart = millis();

            mainGateAction = STOPPED;
        } else {
            gateOpeningDelayPending = false;
            mainGateAction = CLOSING;
            motorStartTime = millis();
        }

        gateS = state;
    }
    else if (uid == "bd1") {
        bd1S = state;
        digitalWrite(BD1_LOCK, state);
    }
    else if (uid == "blc") {
        blcS = state;
        digitalWrite(BLC_LOCK, state);
    }
    else if (uid == "bd2") {
        bd2S = state;
        digitalWrite(BD2_LOCK, state);
    }

    reportStatus(uid, state);
    saveStates();
}

// ============================================================
// MAIN GATE MOTOR
// ============================================================

void handleMainGateMotor() {
    // --------------------------------------------------------
    // Non-blocking 1500ms unlock -> opening delay
    // --------------------------------------------------------
    if (gateOpeningDelayPending) {
        if (millis() - gateOpeningDelayStart >= 1500) {
            gateOpeningDelayPending = false;
            mainGateAction = OPENING;
            motorStartTime = millis();
        } else {
            // Keep both motor outputs safely OFF during unlock delay.
            digitalWrite(MAIN_GATE_MOTOR_FWD, LOW);
            digitalWrite(MAIN_GATE_MOTOR_REV, LOW);
        }
    }

    // --------------------------------------------------------
    // Main motor
    // --------------------------------------------------------
    if (mainGateAction != STOPPED) {
        bool openLimitReached =
            (mainGateAction == OPENING && digitalRead(LIMIT_OPEN) == LOW);

        bool closeLimitReached =
            (mainGateAction == CLOSING && digitalRead(LIMIT_CLOSE) == LOW);

        bool timeoutReached =
            (millis() - motorStartTime > 25000);

        if (openLimitReached || closeLimitReached || timeoutReached) {
            digitalWrite(MAIN_GATE_MOTOR_FWD, LOW);
            digitalWrite(MAIN_GATE_MOTOR_REV, LOW);

            if (mainGateAction == CLOSING) {
                // Original delay(2000) is preserved as a non-blocking timer.
                gateLockDelayPending = true;
                gateLockDelayStart = millis();
            }
            else if (mainGateAction == OPENING) {
                gateS = true;
                reportStatus("maingate", true);
            }

            mainGateAction = STOPPED;
            saveStates();
            Serial.println("[Motor] Task Finished.");
        }
        else {
            digitalWrite(
                MAIN_GATE_MOTOR_FWD,
                (mainGateAction == OPENING)
            );
            digitalWrite(
                MAIN_GATE_MOTOR_REV,
                (mainGateAction == CLOSING)
            );
        }
    }

    // --------------------------------------------------------
    // Non-blocking 2000ms close -> lock delay
    // --------------------------------------------------------
    if (gateLockDelayPending) {
        digitalWrite(MAIN_GATE_MOTOR_FWD, LOW);
        digitalWrite(MAIN_GATE_MOTOR_REV, LOW);

        if (millis() - gateLockDelayStart >= 2000) {
            gateLockDelayPending = false;

            digitalWrite(MAIN_GATE_LOCK, LOW);
            lockS = false;
            reportStatus("mainlock", false);

            gateS = false;
            reportStatus("maingate", false);

            saveStates();
        }
    }
}

// ============================================================
// MQTT CALLBACK
// ============================================================

void callback(char* topic, byte* payload, unsigned int length) {
    String message = "";

    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    String topicStr = String(topic);

    int firstSlash = topicStr.indexOf('/');
    int secondSlash = topicStr.indexOf('/', firstSlash + 1);

    if (firstSlash < 0 || secondSlash < 0) return;

    String uid = topicStr.substring(firstSlash + 1, secondSlash);
    executeHardwareByID(uid, message);
}

// ============================================================
// MQTT CONNECTION
// ============================================================

bool connectMQTT() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (client.connected()) return true;

    Serial.print("[MQTT] Connecting to ");
    Serial.print(mqttServer);
    Serial.print(":");
    Serial.println(mqttPort);

    String mqttClientId = "Jarvis_Humayun_Final_V13_";
    mqttClientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    bool connected;

    if (mqttUser.length() > 0) {
        connected = client.connect(
            mqttClientId.c_str(),
            mqttUser.c_str(),
            mqttPass.c_str(),
            mqttAvailabilityTopic.c_str(),
            1,
            true,
            "offline"
        );
    } else {
        connected = client.connect(
            mqttClientId.c_str(),
            mqttAvailabilityTopic.c_str(),
            1,
            true,
            "offline"
        );
    }

    if (connected) {
        Serial.println("[MQTT] Connected.");

        client.publish(
            mqttAvailabilityTopic.c_str(),
            "online",
            true
        );

        client.subscribe("jarvis_v4/+/cmd");

        publishAllDiscovery();
        publishAllStates();

        return true;
    }

    Serial.print("[MQTT] Failed, state=");
    Serial.println(client.state());
    return false;
}

// ============================================================
// NON-BLOCKING MQTT SERVICE
// ============================================================

void mqttService() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (client.connected()) {
        client.loop();
        return;
    }

    unsigned long now = millis();

    if (now - lastMqttAttempt >= MQTT_RECONNECT_INTERVAL) {
        lastMqttAttempt = now;

        // Keep watchdog alive before/after the network attempt.
        esp_task_wdt_reset();
        connectMQTT();
        esp_task_wdt_reset();
    }
}

// ============================================================
// WIFI MANAGER CALLBACK
// ============================================================

bool shouldSaveConfig = false;

void saveConfigCallback() {
    shouldSaveConfig = true;
    wifiConfigSaved = true;

    Serial.println("[WiFiManager] Configuration changed.");
}

// ============================================================
// WIFI MANAGER PARAMETER SETUP
// ============================================================

void setupWiFiManagerParameters() {
    if (wifiParametersInitialized) return;

    static char mqttServerBuffer[128];
    static char mqttPortBuffer[8];
    static char mqttUserBuffer[64];
    static char mqttPassBuffer[128];
    static char availBuffer[160];
    static char hostnameBuffer[64];

    mqttServer.toCharArray(mqttServerBuffer, sizeof(mqttServerBuffer));
    String(mqttPort).toCharArray(mqttPortBuffer, sizeof(mqttPortBuffer));
    mqttUser.toCharArray(mqttUserBuffer, sizeof(mqttUserBuffer));
    mqttPass.toCharArray(mqttPassBuffer, sizeof(mqttPassBuffer));
    mqttAvailabilityTopic.toCharArray(availBuffer, sizeof(availBuffer));
    deviceHostname.toCharArray(hostnameBuffer, sizeof(hostnameBuffer));

    p_mqtt_server = new WiFiManagerParameter(
        "mqtt_server",
        "MQTT Server / Home Assistant",
        mqttServerBuffer,
        sizeof(mqttServerBuffer) - 1
    );

    p_mqtt_port = new WiFiManagerParameter(
        "mqtt_port",
        "MQTT Port",
        mqttPortBuffer,
        sizeof(mqttPortBuffer) - 1
    );

    p_mqtt_user = new WiFiManagerParameter(
        "mqtt_user",
        "MQTT Username",
        mqttUserBuffer,
        sizeof(mqttUserBuffer) - 1
    );

    p_mqtt_pass = new WiFiManagerParameter(
        "mqtt_pass",
        "MQTT Password",
        mqttPassBuffer,
        sizeof(mqttPassBuffer) - 1
    );

    p_availability = new WiFiManagerParameter(
        "availability",
        "MQTT Availability Topic",
        availBuffer,
        sizeof(availBuffer) - 1
    );

    p_hostname = new WiFiManagerParameter(
        "hostname",
        "Device Hostname",
        hostnameBuffer,
        sizeof(hostnameBuffer) - 1
    );

    wm.addParameter(p_mqtt_server);
    wm.addParameter(p_mqtt_port);
    wm.addParameter(p_mqtt_user);
    wm.addParameter(p_mqtt_pass);
    wm.addParameter(p_availability);
    wm.addParameter(p_hostname);

    wifiParametersInitialized = true;
}

void updateWiFiManagerParameterValues() {
    if (!wifiParametersInitialized) return;

    p_mqtt_server->setValue(mqttServer.c_str(), 127);
    p_mqtt_port->setValue(String(mqttPort).c_str(), 7);
    p_mqtt_user->setValue(mqttUser.c_str(), 63);
    p_mqtt_pass->setValue(mqttPass.c_str(), 127);
    p_availability->setValue(mqttAvailabilityTopic.c_str(), 159);
    p_hostname->setValue(deviceHostname.c_str(), 63);
}

// ============================================================
// READ WIFI MANAGER PARAMETERS AFTER SAVE
// ============================================================

void applyWiFiManagerParameters() {
    if (!wifiParametersInitialized) return;

    String newMqttServer = p_mqtt_server->getValue();
    String newMqttPort = p_mqtt_port->getValue();
    String newMqttUser = p_mqtt_user->getValue();
    String newMqttPass = p_mqtt_pass->getValue();
    String newAvailability = p_availability->getValue();
    String newHostname = p_hostname->getValue();

    newMqttServer.trim();
    newMqttUser.trim();
    newMqttPass.trim();
    newAvailability.trim();
    newHostname.trim();

    uint16_t newPort = newMqttPort.toInt();

    if (newMqttServer.length() > 0)
        mqttServer = newMqttServer;

    if (newPort > 0)
        mqttPort = newPort;

    mqttUser = newMqttUser;
    mqttPass = newMqttPass;

    if (newAvailability.length() > 0)
        mqttAvailabilityTopic = newAvailability;

    if (newHostname.length() > 0)
        deviceHostname = newHostname;

    saveConfiguration();

    client.disconnect();
    client.setServer(mqttServer.c_str(), mqttPort);

    Serial.println("[WiFi] WiFi portal custom configuration applied.");
}

// ============================================================
// START WIFI CONFIGURATION PORTAL
// ============================================================

void startWiFiConfigurationPortal() {
    Serial.println("[WiFi] Preparing non-blocking configuration portal...");

    setupWiFiManagerParameters();
    updateWiFiManagerParameterValues();

    wm.setSaveConfigCallback(saveConfigCallback);

    // IMPORTANT:
    // WiFiManager portal uses 81.
    // Our permanent dashboard remains on 80.
    wm.setHttpPort(81);

    // Non-blocking mode: hardware loop continues while portal is open.
    wm.setConfigPortalBlocking(false);

    // Keep AP available until configuration is completed.
    wm.setConfigPortalTimeout(0);

    // Keep WiFi auto-reconnect enabled.
    wm.setWiFiAutoReconnect(true);

    String apName = "Jarvis_V4_Setup";

    if (!wm.getConfigPortalActive()) {
        wm.startConfigPortal(apName.c_str());
        wifiPortalStarted = true;

        Serial.println("[WiFi] Setup AP started.");
        Serial.print("[WiFi] AP SSID: ");
        Serial.println(apName);
        Serial.print("[WiFi] WiFi setup: http://");
        Serial.print(WiFi.softAPIP());
        Serial.println(":81/");
    }
}

// ============================================================
// WIFI SERVICE
// ============================================================

void startInitialWiFiConnection() {
    setupWiFiManagerParameters();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    String savedSSID = wm.getWiFiSSID(true);
    String savedPass = wm.getWiFiPass(true);

    if (savedSSID.length() > 0) {
        Serial.print("[WiFi] Saved SSID found: ");
        Serial.println(savedSSID);

        WiFi.begin(savedSSID.c_str(), savedPass.c_str());
        wifiConnectStarted = millis();

        Serial.println("[WiFi] Connection started asynchronously.");
    } else {
        Serial.println("[WiFi] No saved WiFi credentials.");
        startWiFiConfigurationPortal();
    }
}

void serviceWiFi() {
    // --------------------------------------------------------
    // WiFiManager portal processing
    // --------------------------------------------------------
    if (wm.getConfigPortalActive()) {
        wm.process();

        if (WiFi.status() == WL_CONNECTED) {
            wifiPortalStarted = false;
        }
    }

    // --------------------------------------------------------
    // Apply custom parameters after WiFiManager save callback
    // --------------------------------------------------------
    if (wifiConfigSaved && !wm.getConfigPortalActive()) {
        wifiConfigSaved = false;
        applyWiFiManagerParameters();
        wifiConnectStarted = millis();
    }

    // --------------------------------------------------------
    // Connected
    // --------------------------------------------------------
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnectStarted = 0;
        return;
    }

    // --------------------------------------------------------
    // Not connected
    // --------------------------------------------------------
    unsigned long now = millis();

    if (now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
        lastWifiReconnectAttempt = now;

        if (!wm.getConfigPortalActive()) {
            Serial.println("[WiFi] Attempting reconnect...");
            WiFi.reconnect();
        }
    }

    // --------------------------------------------------------
    // If connection cannot be established, start AP.
    // Hardware continues normally.
    // --------------------------------------------------------
    if (!wm.getConfigPortalActive()) {
        bool connectionTimedOut =
            (wifiConnectStarted != 0 &&
             now - wifiConnectStarted >= WIFI_CONNECT_GRACE);

        bool portalRetryAllowed =
            (now - lastWifiPortalAttempt >= WIFI_PORTAL_RETRY_INTERVAL);

        if ((connectionTimedOut || wifiConnectStarted == 0) &&
            portalRetryAllowed) {

            lastWifiPortalAttempt = now;
            Serial.println("[WiFi] WiFi unavailable. Starting setup AP...");
            startWiFiConfigurationPortal();
        }
    }
}

// ============================================================
// MDNS
// ============================================================

void stopMDNS() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
    }
}

void startMDNS() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (mdnsStarted) return;

    String host = deviceHostname;
    host.toLowerCase();
    host.replace(" ", "-");

    // Keep hostname simple and valid.
    String cleanHost = "";
    for (size_t i = 0; i < host.length(); i++) {
        char c = host[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-') {
            cleanHost += c;
        }
    }

    if (cleanHost.length() == 0)
        cleanHost = DEFAULT_HOSTNAME;

    if (MDNS.begin(cleanHost.c_str())) {
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;

        Serial.print("[mDNS] http://");
        Serial.print(cleanHost);
        Serial.println(".local/");
    } else {
        Serial.println("[mDNS] Failed.");
    }
}

// ============================================================
// WEB DASHBOARD
// ============================================================

void handleDashboard() {
    String html = htmlHeader("Jarvis V4 Dashboard");
    html += "<div class='container'>";

    html += "<div class='card'><h1>JARVIS V4</h1>";
    html += "<p>Central Hardware Controller</p></div>";

    html += "<div class='card'><h2>Wi-Fi Status</h2><table>";
    html += "<tr><td>Status</td><td>";

    if (WiFi.status() == WL_CONNECTED)
        html += "<span class='ok'>CONNECTED</span>";
    else
        html += "<span class='bad'>DISCONNECTED</span>";

    html += "</td></tr>";

    html += "<tr><td>SSID</td><td>" + WiFi.SSID() + "</td></tr>";
    html += "<tr><td>IP Address</td><td>" + WiFi.localIP().toString() + "</td></tr>";

    if (WiFi.status() == WL_CONNECTED)
        html += "<tr><td>RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";

    if (wm.getConfigPortalActive()) {
        html += "<tr><td>Setup AP</td><td><span class='warn'>ACTIVE</span></td></tr>";
        html += "<tr><td>WiFi Setup</td><td><a href='http://" +
                WiFi.softAPIP().toString() + ":81/'>Open WiFi Setup</a></td></tr>";
    }

    html += "</table></div>";

    html += "<div class='card'><h2>MQTT Status</h2><table>";
    html += "<tr><td>Status</td><td>";

    if (client.connected())
        html += "<span class='ok'>CONNECTED</span>";
    else
        html += "<span class='bad'>DISCONNECTED</span>";

    html += "</td></tr>";
    html += "<tr><td>Server</td><td>" + mqttServer + "</td></tr>";
    html += "<tr><td>Port</td><td>" + String(mqttPort) + "</td></tr>";
    html += "<tr><td>Username</td><td>" + mqttUser + "</td></tr>";
    html += "<tr><td>Availability</td><td>" + mqttAvailabilityTopic + "</td></tr>";
    html += "</table></div>";

    html += "<div class='card'><h2>Device States</h2><table>";
    html += "<tr><td>Main Gate</td><td>" + String(gateS ? "ON" : "OFF") + "</td></tr>";
    html += "<tr><td>Main Lock</td><td>" + String(lockS ? "LOCKED" : "UNLOCKED") + "</td></tr>";
    html += "<tr><td>Bedroom 1</td><td>" + String(bd1S ? "LOCKED" : "UNLOCKED") + "</td></tr>";
    html += "<tr><td>Balcony</td><td>" + String(blcS ? "LOCKED" : "UNLOCKED") + "</td></tr>";
    html += "<tr><td>Bedroom 2</td><td>" + String(bd2S ? "LOCKED" : "UNLOCKED") + "</td></tr>";
    html += "</table></div>";

    html += "<div class='card'><h2>Configuration</h2>";
    html += "<p><a href='/settings'>Open MQTT / Device Configuration</a></p>";
    html += "</div>";

    html += "<div class='card'><h2>Wi-Fi Reconfiguration</h2>";
    html += "<p>Use this if the Wi-Fi SSID/password changes.</p>";
    html += "<form method='POST' action='/start-wifi-setup'>";
    html += "<button class='save' type='submit'>Open Wi-Fi Setup AP</button>";
    html += "</form></div>";

    html += "<div class='card'><h2>Wi-Fi Reset</h2>";
    html += "<p>This erases saved Wi-Fi credentials and restarts the controller.</p>";
    html += "<form method='POST' action='/reset-wifi'>";
    html += "<button class='danger' type='submit'>Reset Wi-Fi</button>";
    html += "</form></div>";

    html += "</div>";
    html += htmlFooter();

    server.send(200, "text/html", html);
}

// ============================================================
// SETTINGS PAGE
// ============================================================

void handleSettings() {
    String html = htmlHeader("Jarvis V4 Settings");
    html += "<div class='container'>";

    html += "<div class='card'><h1>Jarvis V4 Settings</h1>";
    html += "<form method='POST' action='/save-settings'>";

    html += "<label>MQTT Server</label>";
    html += "<input name='mqtt_server' value='" + mqttServer + "'>";

    html += "<label>MQTT Port</label>";
    html += "<input name='mqtt_port' type='number' value='" + String(mqttPort) + "'>";

    html += "<label>MQTT Username</label>";
    html += "<input name='mqtt_user' value='" + mqttUser + "'>";

    // Do not expose the saved password in HTML.
    html += "<label>MQTT Password</label>";
    html += "<input name='mqtt_pass' type='password' placeholder='Enter new password to change'>";

    html += "<label>Availability Topic</label>";
    html += "<input name='availability' value='" + mqttAvailabilityTopic + "'>";

    html += "<label>Device Hostname</label>";
    html += "<input name='hostname' value='" + deviceHostname + "'>";

    html += "<button class='save' type='submit'>Save Settings</button>";
    html += "</form></div>";

    html += "<div class='card'><a href='/'>← Back to Dashboard</a></div>";

    html += "</div>";
    html += htmlFooter();

    server.send(200, "text/html", html);
}

// ============================================================
// SAVE SETTINGS
// ============================================================

void handleSaveSettings() {
    if (!server.hasArg("mqtt_server") ||
        !server.hasArg("mqtt_port") ||
        !server.hasArg("mqtt_user") ||
        !server.hasArg("mqtt_pass") ||
        !server.hasArg("availability") ||
        !server.hasArg("hostname")) {

        server.send(400, "text/plain", "Missing configuration fields.");
        return;
    }

    String newMqttServer = server.arg("mqtt_server");
    String newPort = server.arg("mqtt_port");
    String newUser = server.arg("mqtt_user");
    String newPass = server.arg("mqtt_pass");
    String newAvailability = server.arg("availability");
    String newHostname = server.arg("hostname");

    newMqttServer.trim();
    newUser.trim();
    newPass.trim();
    newAvailability.trim();
    newHostname.trim();

    uint16_t newPortNumber = newPort.toInt();

    if (newMqttServer.length() == 0) {
        server.send(400, "text/plain", "MQTT server cannot be empty.");
        return;
    }

    if (newPortNumber == 0 || newPortNumber > 65535) {
        server.send(400, "text/plain", "Invalid MQTT port.");
        return;
    }

    if (newAvailability.length() == 0) {
        server.send(400, "text/plain", "Availability topic cannot be empty.");
        return;
    }

    if (newHostname.length() == 0) {
        server.send(400, "text/plain", "Hostname cannot be empty.");
        return;
    }

    mqttServer = newMqttServer;
    mqttPort = newPortNumber;
    mqttUser = newUser;

    // Empty password means "keep existing password".
    if (newPass.length() > 0)
        mqttPass = newPass;

    mqttAvailabilityTopic = newAvailability;
    deviceHostname = newHostname;

    saveConfiguration();

    client.disconnect();
    client.setServer(mqttServer.c_str(), mqttPort);

    stopMDNS();

    String html = htmlHeader("Settings Saved");
    html += "<div class='container'><div class='card'>";
    html += "<h1>Settings Saved</h1>";
    html += "<p class='ok'>MQTT/device configuration saved successfully.</p>";
    html += "<p>MQTT will reconnect automatically.</p>";
    html += "<p><a href='/'>Return to Dashboard</a></p>";
    html += "</div></div>";
    html += htmlFooter();

    server.send(200, "text/html", html);
}

// ============================================================
// START WIFI SETUP FROM DASHBOARD
// ============================================================

void handleStartWiFiSetup() {
    String html = htmlHeader("Wi-Fi Setup");
    html += "<div class='container'><div class='card'>";
    html += "<h1>Wi-Fi Setup AP</h1>";
    html += "<p>The Wi-Fi setup portal is being started.</p>";
    html += "<p>Connect to <b>Jarvis_V4_Setup</b> and open:</p>";
    html += "<p><b>http://192.168.4.1:81/</b></p>";
    html += "</div></div>";
    html += htmlFooter();

    server.send(200, "text/html", html);

    delay(50);

    if (!wm.getConfigPortalActive()) {
        updateWiFiManagerParameterValues();
        wm.setConfigPortalBlocking(false);
        wm.setConfigPortalTimeout(0);
        wm.setHttpPort(81);
        wm.startConfigPortal("Jarvis_V4_Setup");
        wifiPortalStarted = true;
    }
}

// ============================================================
// RESET WIFI
// ============================================================

void handleResetWiFi() {
    String html = htmlHeader("Reset Wi-Fi");
    html += "<div class='container'><div class='card'>";
    html += "<h1>Resetting Wi-Fi...</h1>";
    html += "<p>The device will restart and open the Wi-Fi setup portal.</p>";
    html += "</div></div>";
    html += htmlFooter();

    server.send(200, "text/html", html);

    delay(300);

    wm.resetSettings();

    WiFi.disconnect(true, true);

    delay(300);

    ESP.restart();
}

// ============================================================
// WEB SERVER ROUTES
// ============================================================

void startWebServer() {
    server.on("/", HTTP_GET, handleDashboard);

    server.on("/settings", HTTP_GET, handleSettings);

    server.on("/save-settings", HTTP_POST, handleSaveSettings);

    server.on("/start-wifi-setup", HTTP_POST, handleStartWiFiSetup);

    server.on("/reset-wifi", HTTP_POST, handleResetWiFi);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Not Found");
    });

    server.begin();

    Serial.println("[Web] Dashboard started on port 80.");
}

// ============================================================
// BUTTON / OPTO PROCESSING
// ============================================================

void handleLocalInputs() {
    static unsigned long lastBtnTrigger = 0;

    if (millis() - lastBtnTrigger <= 350)
        return;

    if (digitalRead(MAIN_GATE_BTN) == LOW ||
        digitalRead(MAIN_OPTO_IN) == HIGH) {

        delay(50);

        if (digitalRead(MAIN_GATE_BTN) == LOW ||
            digitalRead(MAIN_OPTO_IN) == HIGH) {

            executeHardwareByID("maingate", gateS ? "OFF" : "ON");
            lastBtnTrigger = millis();
        }
    }

    if (digitalRead(BD1_BTN) == LOW ||
        digitalRead(BD1_OPTO_IN) == HIGH) {

        delay(50);

        if (digitalRead(BD1_BTN) == LOW ||
            digitalRead(BD1_OPTO_IN) == HIGH) {

            executeHardwareByID("bd1", bd1S ? "UNLOCK" : "LOCK");
            lastBtnTrigger = millis();
        }
    }

    if (digitalRead(BLC_BTN) == LOW ||
        digitalRead(BLC_OPTO_IN) == HIGH) {

        delay(50);

        if (digitalRead(BLC_BTN) == LOW ||
            digitalRead(BLC_OPTO_IN) == HIGH) {

            executeHardwareByID("blc", blcS ? "UNLOCK" : "LOCK");
            lastBtnTrigger = millis();
        }
    }

    if (digitalRead(BD2_BTN) == LOW ||
        digitalRead(BD2_OPTO_IN) == HIGH) {

        delay(50);

        if (digitalRead(BD2_BTN) == LOW ||
            digitalRead(BD2_OPTO_IN) == HIGH) {

            executeHardwareByID("bd2", bd2S ? "UNLOCK" : "LOCK");
            lastBtnTrigger = millis();
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(200);

    // --------------------------------------------------------
    // Watchdog
    // --------------------------------------------------------
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };

    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

    // --------------------------------------------------------
    // Load configuration/state
    // --------------------------------------------------------
    loadConfiguration();
    loadStates();

    // --------------------------------------------------------
    // OUTPUT PINS - UNCHANGED
    // --------------------------------------------------------
    int outPins[] = {12, 13, 14, 21, 19, 5};

    for (int p : outPins)
        pinMode(p, OUTPUT);

    // Restore outputs
    digitalWrite(MAIN_GATE_LOCK, lockS);
    digitalWrite(BD1_LOCK, bd1S);
    digitalWrite(BLC_LOCK, blcS);
    digitalWrite(BD2_LOCK, bd2S);

    // Motor outputs MUST start OFF.
    digitalWrite(MAIN_GATE_MOTOR_FWD, LOW);
    digitalWrite(MAIN_GATE_MOTOR_REV, LOW);

    // --------------------------------------------------------
    // INPUT PINS - UNCHANGED
    // --------------------------------------------------------
    int inPins[] = {33, 27, 26, 4, 23, 16};

    for (int p : inPins)
        pinMode(p, INPUT_PULLUP);

    pinMode(MAIN_OPTO_IN, INPUT);
    pinMode(BD1_OPTO_IN, INPUT);
    pinMode(BLC_OPTO_IN, INPUT);
    pinMode(BD2_OPTO_IN, INPUT);

    // --------------------------------------------------------
    // WiFiManager parameters
    // --------------------------------------------------------
    setupWiFiManagerParameters();

    // --------------------------------------------------------
    // WiFi
    // --------------------------------------------------------
    startInitialWiFiConnection();

    // --------------------------------------------------------
    // MQTT
    // --------------------------------------------------------
    client.setServer(mqttServer.c_str(), mqttPort);
    client.setCallback(callback);
    client.setBufferSize(2048);

    // IMPORTANT:
    // PubSubClient default socket timeout is 15s.
    // That is too long for a 3s watchdog.
    // 1s prevents a dead MQTT server from holding the loop.
    client.setSocketTimeout(1);

    // --------------------------------------------------------
    // Web Dashboard
    // --------------------------------------------------------
    startWebServer();

    // --------------------------------------------------------
    // mDNS will start automatically once WiFi connects.
    // --------------------------------------------------------

    Serial.println("==================================");
    Serial.println(" JARVIS V4 FINAL READY");
    Serial.println(" Hardware loop is WiFi/MQTT independent");
    Serial.println(" Dashboard: http://jarvis-v4.local/");
    Serial.println(" WiFi setup AP: Jarvis_V4_Setup");
    Serial.println(" WiFi setup port: 81");
    Serial.println("==================================");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    // --------------------------------------------------------
    // Watchdog
    // --------------------------------------------------------
    esp_task_wdt_reset();

    // --------------------------------------------------------
    // WiFi service
    // Hardware keeps running even if WiFi is absent.
    // --------------------------------------------------------
    serviceWiFi();

    // --------------------------------------------------------
    // mDNS
    // --------------------------------------------------------
    if (WiFi.status() == WL_CONNECTED) {
        startMDNS();
    } else if (mdnsStarted) {
        stopMDNS();
    }

    // --------------------------------------------------------
    // MQTT
    // --------------------------------------------------------
    mqttService();

    // --------------------------------------------------------
    // Main Gate Motor
    // --------------------------------------------------------
    handleMainGateMotor();

    // --------------------------------------------------------
    // Local buttons + optocouplers
    // --------------------------------------------------------
    handleLocalInputs();

    // --------------------------------------------------------
    // Dashboard
    // --------------------------------------------------------
    server.handleClient();

    // --------------------------------------------------------
    // Watchdog
    // --------------------------------------------------------
    esp_task_wdt_reset();

    // --------------------------------------------------------
    // Yield
    // --------------------------------------------------------
    delay(1);
}
