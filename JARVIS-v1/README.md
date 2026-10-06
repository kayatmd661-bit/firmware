# JARVIS Home Controller --- Final Architecture & Wiring README

> **Firmware:** `JARVIS_HOME_CONTROLLER_MQTT_DISCOVERY_FIXED.ino`\
> **Architecture:** ESP32 + 4× PCF8574 + Wi‑Fi + MQTT + Home Assistant
> MQTT Discovery\
> **Current scope:** 7 Lights + TV + Sound + Projector + 2 Windows + 4
> Curtains + Door\
> **Important:** Fans and Main Gate are intentionally removed from this
> final architecture.

------------------------------------------------------------------------

## 1. System overview

এই firmware-এর মূল লক্ষ্য হলো একটি ESP32-কে একটি local-network Home
Assistant hardware controller হিসেবে ব্যবহার করা।

ESP32-এর ভিতরে:

-   hardware control logic
-   motor state machine
-   limit-switch safety stop
-   configurable motor timeout
-   persistent configuration/state
-   Wi‑Fi configuration portal
-   local web dashboard
-   MQTT client
-   Home Assistant MQTT Discovery
-   OTA update
-   8-second task watchdog
-   projector/curtain cascade logic

সব একসাথে কাজ করে।

### High-level architecture

``` mermaid
flowchart LR
    HA[Home Assistant]
    MQTT[MQTT Broker]
    ESP[ESP32<br/>JARVIS Home Controller]

    P1[PCF8574 0x20<br/>Window 1, Window 2<br/>Curtain 1, Curtain 2]
    P2[PCF8574 0x21<br/>Curtain 3, Door, Curtain 4]
    P3[PCF8574 0x22<br/>Light 1-7]
    P4[PCF8574 0x23<br/>TV, Sound, Projector]

    MS[Limit Switches<br/>Open / Close]
    NET[Local Wi-Fi]
    DASH[ESP32 Web Dashboard]
    OTA[Arduino OTA]

    HA <--> MQTT
    MQTT <--> ESP
    NET --- ESP
    DASH --- ESP
    OTA --- ESP

    ESP <--> P1
    ESP <--> P2
    ESP <--> P3
    ESP <--> P4
    MS --> ESP
```

------------------------------------------------------------------------

# 2. Controlled devices

## 2.1 Home Assistant entities

  Category   UID       Entity
  ---------- --------- ----------
  Light      `l1`      লাইট ১
  Light      `l2`      লাইট ২
  Light      `l3`      লাইট ৩
  Light      `l4`      লাইট ৪
  Light      `l5`      লাইট ৫
  Light      `l6`      লাইট ৬
  Light      `l7`      লাইট ৭
  Switch     `tv`      টিভি
  Switch     `sound`   সাউন্ড
  Switch     `proj`    প্রজেক্টর
  Cover      `door`    দরজা
  Cover      `win1`    জানালা ১
  Cover      `win2`    জানালা ২
  Cover      `cur1`    পর্দা ১
  Cover      `cur2`    পর্দা ২
  Cover      `cur3`    পর্দা ৩
  Cover      `cur4`    পর্দা ৪

**Total:** 17 Home Assistant entities.

সব entity একই Home Assistant device-এর অধীনে group হয়:

``` text
JARVIS Home Controller
└── 17 entities
    ├── 7 × Light
    ├── 3 × Switch
    └── 7 × Cover
```

------------------------------------------------------------------------

# 3. Physical architecture

``` mermaid
flowchart TB
    PS[DC Power Supply]
    ESP[ESP32]

    I2C[I²C Bus<br/>SDA GPIO21<br/>SCL GPIO22]

    PCF1[PCF8574<br/>0x20]
    PCF2[PCF8574<br/>0x21]
    PCF3[PCF8574<br/>0x22]
    PCF4[PCF8574<br/>0x23]

    DRV1[Motor Driver / Relay Interface<br/>Windows + Curtains 1-2]
    DRV2[Motor Driver / Relay Interface<br/>Curtain 3 + Door + Curtain 4]
    LGT[Light Relay / Interface]
    AV[AV Relay / Interface]

    W1[Window 1 Motor]
    W2[Window 2 Motor]
    C1[Curtain 1 Motor]
    C2[Curtain 2 Motor]
    C3[Curtain 3 Motor]
    D[Door Motor]
    C4[Curtain 4 Motor]

    L[7 Lights]
    TV[TV]
    SO[Sound]
    PR[Projector]

    ESP --> I2C
    I2C --> PCF1
    I2C --> PCF2
    I2C --> PCF3
    I2C --> PCF4

    PCF1 --> DRV1
    DRV1 --> W1
    DRV1 --> W2
    DRV1 --> C1
    DRV1 --> C2

    PCF2 --> DRV2
    DRV2 --> C3
    DRV2 --> D
    DRV2 --> C4

    PCF3 --> LGT
    LGT --> L

    PCF4 --> AV
    AV --> TV
    AV --> SO
    AV --> PR

    PS --> ESP
```

> **Hardware safety note:** ESP32/PCF8574 GPIO দিয়ে motor বা mains load
> সরাসরি চালানো যাবে না। মাঝখানে উপযুক্ত isolated relay, motor driver,
> H-bridge, contactor বা interface circuit ব্যবহার করতে হবে। এই README
> firmware-এর logical output mapping দেখায়; নির্দিষ্ট driver/relay
> board-এর electrical schematic firmware-এ নির্ধারিত নেই।

------------------------------------------------------------------------

# 4. ESP32 ↔ PCF8574 I²C connection

ESP32 থেকে চারটি PCF8574 একই I²C bus-এ connected।

### ESP32 I²C pins

      ESP32 Signal
  --------- --------
    GPIO 21 SDA
    GPIO 22 SCL

### PCF8574 addresses

  Device     I²C Address Main role
  -------- ------------- --------------------------------------------
  PCF1            `0x20` Window 1/2 + Curtain 1/2 motor outputs
  PCF2            `0x21` Curtain 3 + Door + Curtain 4 motor outputs
  PCF3            `0x22` 7 lights
  PCF4            `0x23` TV + Sound + Projector

### I²C topology

``` text
ESP32 GPIO21 SDA ─────────┬── PCF1 0x20 SDA
                         ├── PCF2 0x21 SDA
                         ├── PCF3 0x22 SDA
                         └── PCF4 0x23 SDA

ESP32 GPIO22 SCL ─────────┬── PCF1 0x20 SCL
                         ├── PCF2 0x21 SCL
                         ├── PCF3 0x22 SCL
                         └── PCF4 0x23 SCL

ESP32 GND ────────────────┬── PCF1 GND
                         ├── PCF2 GND
                         ├── PCF3 GND
                         └── PCF4 GND
```

> I²C pull-up resistor value এবং PCF8574 supply voltage board-level
> design-এর উপর নির্ভর করে; এখানে firmware-এর logical bus/address mapping
> দেওয়া হয়েছে।

------------------------------------------------------------------------

# 5. PCF8574 output mapping

## PCF1 --- `0x20`

Windows এবং Curtain 1/2 motor direction outputs:

    PCF1 pin Firmware symbol   Function
  ---------- ----------------- -------------------
          P0 `PIN_M_WIN1_A`    Window 1 motor A
          P1 `PIN_M_WIN1_B`    Window 1 motor B
          P2 `PIN_M_WIN2_A`    Window 2 motor A
          P3 `PIN_M_WIN2_B`    Window 2 motor B
          P4 `PIN_M_CUR1_A`    Curtain 1 motor A
          P5 `PIN_M_CUR1_B`    Curtain 1 motor B
          P6 `PIN_M_CUR2_A`    Curtain 2 motor A
          P7 `PIN_M_CUR2_B`    Curtain 2 motor B

``` text
PCF1 0x20
┌───────────────────────────────┐
│ P0 ── Window 1 A              │
│ P1 ── Window 1 B              │
│ P2 ── Window 2 A              │
│ P3 ── Window 2 B              │
│ P4 ── Curtain 1 A             │
│ P5 ── Curtain 1 B             │
│ P6 ── Curtain 2 A             │
│ P7 ── Curtain 2 B             │
└───────────────────────────────┘
```

## PCF2 --- `0x21`

    PCF2 pin Firmware symbol               Function
  ---------- ----------------------------- -------------------
          P0 `PIN_M_CUR3_A`                Curtain 3 motor A
          P1 `PIN_M_CUR3_B`                Curtain 3 motor B
          P2 `PIN_M_DOOR_A`                Door motor A
          P3 `PIN_M_DOOR_B`                Door motor B
          P4 unused by current motor map   ---
          P5 unused by current motor map   ---
          P6 `PIN_M_CUR4_A`                Curtain 4 motor A
          P7 `PIN_M_CUR4_B`                Curtain 4 motor B

## PCF3 --- `0x22`

    PCF3 pin Firmware symbol       Function
  ---------- --------------------- ----------
          P0 `L1_PIN`              Light 1
          P1 `L2_PIN`              Light 2
          P2 `L3_PIN`              Light 3
          P3 `L4_PIN`              Light 4
          P4 `L5_PIN`              Light 5
          P5 `L6_PIN`              Light 6
          P6 `L7_PIN`              Light 7
          P7 unused by light map   ---

## PCF4 --- `0x23`

    PCF4 pin Firmware symbol   Function
  ---------- ----------------- -----------
          P0 unused            ---
          P1 unused            ---
          P2 unused            ---
          P3 `TV_PIN`          TV
          P4 unused            ---
          P5 `PROJ_PIN`        Projector
          P6 `SOUND_PIN`       Sound
          P7 unused            ---

------------------------------------------------------------------------

# 6. ESP32 input / limit-switch mapping

সব limit switch `INPUT_PULLUP` হিসেবে configured এবং firmware-এ **LOW =
active** হিসেবে দেখা হয়।

    GPIO Function
  ------ -------------------------
      32 Window 1 OPEN limit
      33 Window 1 CLOSE limit
      25 Window 2 OPEN limit
      26 Window 2 CLOSE limit
      27 Curtain 1 OPEN limit
      14 Curtain 1 CLOSE limit
      12 Curtain 2 OPEN limit
      13 Curtain 2 CLOSE limit
       2 Door OPEN limit
      15 Door CLOSE limit
       4 Curtain 3 OPEN limit
       5 Curtain 3 CLOSE limit
      18 Curtain 4 OPEN limit
      19 Curtain 4 CLOSE limit
      23 Lock/reset button input
       0 Boot/reset button input
      36 Access-control input

### Limit switch concept

``` mermaid
flowchart LR
    CMD[OPEN / CLOSE command]
    STATE[Motor state machine]
    DRIVER[Motor driver]
    MOTOR[Motor]
    LIMIT[Open/Close limit switch]
    TIMEOUT[Configured timeout]
    STOP[STOPPED]

    CMD --> STATE
    STATE --> DRIVER
    DRIVER --> MOTOR
    MOTOR --> LIMIT
    LIMIT --> STATE
    STATE --> STOP

    STATE --> TIMEOUT
    TIMEOUT --> STOP
```

**Safety behavior:**

1.  Command দিলে motor `OPENING` বা `CLOSING` state নেয়।
2.  Correct limit switch active হলে motor output LOW হয় এবং state
    `STOPPED` হয়।
3.  Limit switch না এলে configured timeout backup stop হিসেবে কাজ করে।
4.  Reboot-এর সময় moving motor restore করা হয় না; motors stop করা হয়।

------------------------------------------------------------------------

# 7. Motor timing architecture

প্রতিটি motor-এর জন্য আলাদা:

-   Open time
-   Close time

রাখা হয়।

বর্তমান default:

``` text
DEFAULT_MOTOR_TIMEOUT = 25 seconds
MAXIMUM = 120 seconds
```

Dashboard থেকে 1--120 seconds-এর মধ্যে value দেওয়া যায়।

### Motor timing flow

``` mermaid
flowchart TD
    MEASURE[বাস্তব motor travel time মাপুন]
    OPENSET[Open Time সেট করুন]
    CLOSESET[Close Time সেট করুন]
    SAVE[Dashboard Save]
    NVS[ESP32 Preferences]
    RUN[Motor command]
    LIMIT{Limit switch active?}
    TIMER{Timeout reached?}
    STOP[Motor STOPPED]

    MEASURE --> OPENSET
    MEASURE --> CLOSESET
    OPENSET --> SAVE
    CLOSESET --> SAVE
    SAVE --> NVS
    NVS --> RUN
    RUN --> LIMIT
    LIMIT -->|Yes| STOP
    LIMIT -->|No| TIMER
    TIMER -->|Yes| STOP
    TIMER -->|No| RUN
```

### গুরুত্বপূর্ণ

Timing হলো **backup timeout**। Primary stop mechanism হলো physical limit
switch।

------------------------------------------------------------------------

# 8. Home Assistant + MQTT architecture

Firmware সরাসরি Home Assistant REST/API control loop-এর উপর নির্ভর করে
না। Current architecture-এ control path হলো:

``` text
Home Assistant
      │
      ▼
 MQTT Broker
      │
      ▼
 ESP32 JARVIS Controller
      │
      ▼
 PCF8574 / GPIO
      │
      ▼
 Relay / Driver / Motor / Light
```

Home Assistant configuration fields firmware dashboard-এ সংরক্ষণ করা যায়,
কিন্তু entity control/discovery-এর active transport হলো MQTT।

### MQTT base topics

``` text
jarvis/
├── status/
│   ├── availability
│   ├── l1
│   ├── l2
│   ├── ...
│   ├── tv
│   ├── sound
│   ├── proj
│   ├── door
│   ├── win1
│   ├── win2
│   ├── cur1
│   ├── cur2
│   ├── cur3
│   └── cur4
│
├── l1/cmd
├── l2/cmd
├── ...
├── tv/cmd
├── sound/cmd
├── proj/cmd
├── door/cmd
├── win1/cmd
├── win2/cmd
├── cur1/cmd
├── cur2/cmd
├── cur3/cmd
└── cur4/cmd
```

------------------------------------------------------------------------

# 9. MQTT command protocol

## Lights / AV

``` text
ON
OFF
```

## Covers / motors

``` text
OPEN
CLOSE
STOP
```

### Example

``` text
Home Assistant
   │
   │ publish
   ▼
jarvis/l1/cmd
   │
   └── "ON"
        │
        ▼
ESP32
   │
   ▼
PCF3 P0
   │
   ▼
Light 1 interface
```

Motor example:

``` text
Home Assistant
   │
   │ publish "OPEN"
   ▼
jarvis/cur4/cmd
   │
   ▼
ESP32
   │
   ├── Curtain 4 state = OPENING
   ├── PCF2 P6/P7 motor direction
   └── monitor GPIO18/GPIO19 limits
```

------------------------------------------------------------------------

# 10. Home Assistant MQTT Discovery

Firmware automatically publishes retained discovery configurations
under:

``` text
homeassistant/<component>/jarvis_final/<uid>/config
```

Examples:

``` text
homeassistant/light/jarvis_final/l1/config
homeassistant/light/jarvis_final/l2/config
homeassistant/switch/jarvis_final/tv/config
homeassistant/cover/jarvis_final/cur4/config
```

All entities use the same parent device:

``` text
Device ID:
jarvis_home_controller

Device name:
JARVIS Home Controller
```

### Discovery architecture

``` mermaid
flowchart TB
    ESP[ESP32]
    DISC[MQTT Discovery Config]
    HA[Home Assistant MQTT Integration]
    DEV[JARVIS Home Controller Device]

    ESP -->|retained config| DISC
    DISC --> HA
    HA --> DEV

    DEV --> L[7 Lights]
    DEV --> AV[TV / Sound / Projector]
    DEV --> CV[Door / Windows / Curtains]
```

### Discovery payload improvements in final firmware

The final firmware uses:

-   `unique_id`
-   `platform`
-   `availability_topic`
-   `payload_available`
-   `payload_not_available`
-   `state_topic`
-   `command_topic`
-   proper light ON/OFF payloads
-   proper cover OPEN/CLOSE/STOP payloads
-   cover state strings: `OPENING`, `CLOSING`, `STOPPED`
-   shared `device` metadata

এবং discovery payload-এর জন্য PubSubClient buffer `1024` bytes করা হয়েছে।

------------------------------------------------------------------------

# 11. Availability / online state

Availability topic:

``` text
jarvis/status/availability
```

Connected অবস্থায়:

``` text
online
```

MQTT Last Will হিসেবে:

``` text
offline
```

ব্যবহার করা হয়।

এর ফলে Home Assistant বুঝতে পারে controller MQTT-তে available কিনা।

------------------------------------------------------------------------

# 12. State reporting

## Lights / switches

State topic:

``` text
jarvis/status/<uid>
```

Payload:

``` text
ON
OFF
```

## Covers

State topic:

``` text
jarvis/status/<uid>
```

Payload:

``` text
OPENING
CLOSING
STOPPED
```

Position reporting-এর জন্য firmware আরও publish করে:

``` text
jarvis/status/<uid>/position
```

Position `0–100` range-এ report করা হয়।

> Current discovery configuration position-control entity হিসেবে command
> mapping দেয় না; position topic firmware-এর status reporting অংশ হিসেবে
> থাকে।

------------------------------------------------------------------------

# 13. Wi‑Fi configuration

প্রথম configuration-এর জন্য WiFiManager ব্যবহার করা হয়।

Configuration AP:

``` text
Jarvis_Config_AP
```

Wi‑Fi connection সফল হলে:

``` text
Wi-Fi connected
```

এবং local IP-এর মাধ্যমে dashboard চালু হয়।

### Non-blocking design

WiFiManager portal blocking করা হয়নি।

Main loop-এ:

``` text
wifiManager.process()
```

চলে।

এটি গুরুত্বপূর্ণ কারণ দীর্ঘ সময় configuration portal-এ থাকলেও watchdog reset
না হওয়ার জন্য cooperative loop সচল থাকে।

------------------------------------------------------------------------

# 14. Local dashboard

ESP32-এর local web dashboard:

``` text
http://<ESP32-IP>/
```

Dashboard-এ রয়েছে:

### Wi‑Fi

-   SSID
-   Password

### MQTT

-   MQTT Host / URL
-   MQTT Port
-   MQTT Username
-   MQTT Password

### Home Assistant

-   Home Assistant Local Host / IP
-   Home Assistant Port
-   Home Assistant Code / Identifier

### Motor timing

প্রতিটি motor-এর:

-   Open Time
-   Close Time

### System

-   Restart ESP32
-   Reset Wi‑Fi Settings

### Current State

-   7 lights
-   TV
-   Sound
-   Projector
-   2 windows
-   4 curtains
-   Door

------------------------------------------------------------------------

# 15. Dashboard request flow

``` mermaid
sequenceDiagram
    participant Phone as Phone/PC
    participant ESP as ESP32 Dashboard
    participant NVS as Preferences
    participant MQTT as MQTT Broker
    participant HA as Home Assistant

    Phone->>ESP: GET /
    ESP-->>Phone: Dashboard HTML

    Phone->>ESP: POST /save
    ESP->>NVS: Save configuration
    ESP->>MQTT: Disconnect / reconnect using new settings
    MQTT-->>ESP: Connected
    ESP->>HA: MQTT Discovery + states
```

------------------------------------------------------------------------

# 16. Persistent storage

ESP32 `Preferences` namespace:

``` text
jarvis
```

Persistent data includes:

### Network

``` text
mqtt_host
mqtt_port
mqtt_user
mqtt_pass
ha_host
ha_port
ha_code
wifi_ssid
wifi_pass
```

### Light / AV states

``` text
l1 ... l7
tv
sound
proj
```

### Motor timings

``` text
op0 ... op6
cl0 ... cl6
```

### Recovery rule

Lights এবং AV states reboot-এর পর restore হয়।

Motor moving state restore হয় না।

Reboot হলে:

``` text
All motors -> STOPPED
```

------------------------------------------------------------------------

# 17. Watchdog architecture

Watchdog timeout:

``` text
8 seconds
```

Main loop-এর শুরু এবং শেষে watchdog feed করা হয়।

``` mermaid
flowchart TD
    START[Loop start]
    W1[WDT reset]
    WM[WiFiManager process]
    MQTT[MQTT process/reconnect]
    WEB[Dashboard handleClient]
    OTA[OTA handle]
    BTN[Reset button]
    MOTOR[Motor processing]
    CASCADE[Cascade processing]
    W2[WDT reset]
    NEXT[Next loop]

    START --> W1
    W1 --> WM
    WM --> MQTT
    MQTT --> WEB
    WEB --> OTA
    OTA --> BTN
    BTN --> MOTOR
    MOTOR --> CASCADE
    CASCADE --> W2
    W2 --> NEXT
    NEXT --> START
```

------------------------------------------------------------------------

# 18. OTA update

ArduinoOTA hostname:

``` text
jarvis-home-controller
```

OTA শুরু হওয়ার সময় firmware:

``` text
stopAllMotors()
```

চালায়।

অর্থাৎ firmware update-এর সময় motor outputs active অবস্থায় রেখে OTA করা হয়
না।

------------------------------------------------------------------------

# 19. Runtime architecture

``` mermaid
flowchart TD
    LOOP[ESP32 cooperative loop]

    WDT1[Watchdog feed]
    WIFI[WiFiManager.process]
    CONFIG[Config save check]
    NET{Wi-Fi connected?}

    DASH[Dashboard]
    MQTT{MQTT connected?}
    RECON[MQTT reconnect]
    MQTTLOOP[MQTT client.loop]
    OTA[ArduinoOTA.handle]
    RESET[Reset button check]
    MOTORS[Motor state machine]
    CASCADE[Cascade logic]
    WDT2[Watchdog feed]

    LOOP --> WDT1
    WDT1 --> WIFI
    WIFI --> CONFIG
    CONFIG --> NET

    NET -->|Yes| DASH
    NET -->|Yes| MQTT
    NET -->|No| OTA

    MQTT -->|No| RECON
    MQTT -->|Yes| MQTTLOOP

    DASH --> OTA
    RECON --> OTA
    MQTTLOOP --> OTA
    OTA --> RESET
    RESET --> MOTORS
    MOTORS --> CASCADE
    CASCADE --> WDT2
    WDT2 --> LOOP
```

------------------------------------------------------------------------

# 20. Cascade automation

Firmware-এর বর্তমান cascade logic:

### Curtain 3 → Curtain 4

Curtain 3 opening শেষ হলে:

``` text
Curtain 3 STOPPED
        ↓
Curtain 4 OPEN
```

### Curtain 4 → Projector

Curtain 4 opening শেষ হলে:

``` text
Curtain 4 STOPPED
        ↓
Projector ON
```

### Projector OFF → Curtain 4 CLOSE

Projector OFF করলে:

``` text
Projector OFF
      ↓
Curtain 4 CLOSE
```

### Curtain 4 → Curtain 3

Curtain 4 closing শেষ হলে:

``` text
Curtain 4 STOPPED
        ↓
Curtain 3 CLOSE
```

### Cascade diagram

``` mermaid
flowchart LR
    C3O[Curtain 3 OPEN complete]
    C4O[Curtain 4 OPEN]
    PROJON[Projector ON]

    PROJOFF[Projector OFF]
    C4C[Curtain 4 CLOSE]
    C3C[Curtain 3 CLOSE]

    C3O --> C4O --> PROJON
    PROJOFF --> C4C --> C3C
```

------------------------------------------------------------------------

# 21. Command routing inside ESP32

``` mermaid
flowchart TD
    MQTTIN[MQTT message]
    TOPIC[jarvis/+/cmd]
    PARSE[Extract UID]
    CMD[Normalize command]

    LIGHT{Light UID?}
    AV{TV/Sound/Projector?}
    MOTOR{Window/Curtain/Door?}

    LH[setLightHardware]
    AH[setAVHardware]
    MH[startMotorCommand]
    STOP[stopMotorById]

    MQTTIN --> TOPIC
    TOPIC --> PARSE
    PARSE --> CMD

    CMD -->|STOP| STOP
    CMD --> LIGHT
    LIGHT -->|Yes| LH
    LIGHT -->|No| AV
    AV -->|Yes| AH
    AV -->|No| MOTOR
    MOTOR -->|Yes| MH
```

------------------------------------------------------------------------

# 22. Motor state machine

``` text
                 OPEN command
                      │
                      ▼
                  OPENING
                 /       \
                /         \
       open limit        timeout
            │               │
            └──────┬────────┘
                   ▼
                STOPPED
                   ▲
            ┌──────┴────────┐
            │               │
       close limit       timeout
            │               │
            └──────┬────────┘
                   │
                 CLOSING
                   ▲
                   │
              CLOSE command
```

Explicit STOP:

``` text
OPENING ──STOP──> STOPPED
CLOSING ──STOP──> STOPPED
```

------------------------------------------------------------------------

# 23. Startup sequence

``` mermaid
sequenceDiagram
    participant ESP as ESP32
    participant NVS as Preferences
    participant PCF as PCF8574
    participant WIFI as Wi-Fi
    participant MQTT as MQTT Broker
    participant HA as Home Assistant

    ESP->>NVS: Load network config
    ESP->>NVS: Load motor timing
    ESP->>PCF: Initialize I²C devices
    ESP->>PCF: Stop all motors
    ESP->>NVS: Recover light/AV states
    ESP->>WIFI: Start WiFiManager
    WIFI-->>ESP: Wi-Fi connected
    ESP->>ESP: Start dashboard + OTA
    ESP->>MQTT: Connect
    MQTT-->>ESP: Connected
    ESP->>MQTT: Publish availability online
    ESP->>MQTT: Subscribe jarvis/+/cmd
    ESP->>MQTT: Publish Discovery configs
    MQTT-->>HA: Discovery retained messages
    ESP->>MQTT: Synchronize current states
```

------------------------------------------------------------------------

# 24. First installation / wiring procedure

## Step 1 --- Power architecture

প্রথমে নিশ্চিত করুন:

``` text
ESP32 supply
PCF8574 supply
Relay/driver supply
Motor supply
```

একই voltage ধরে নেওয়া যাবে না। Board/module-এর actual electrical
requirements অনুসরণ করতে হবে।

## Step 2 --- I²C

ESP32:

``` text
GPIO21 -> SDA
GPIO22 -> SCL
```

চার PCF8574:

``` text
0x20
0x21
0x22
0x23
```

এবং common ground নিশ্চিত করুন।

## Step 3 --- PCF1

Connect:

``` text
Window 1 A/B
Window 2 A/B
Curtain 1 A/B
Curtain 2 A/B
```

## Step 4 --- PCF2

Connect:

``` text
Curtain 3 A/B
Door A/B
Curtain 4 A/B
```

## Step 5 --- PCF3

Connect:

``` text
Light 1
Light 2
Light 3
Light 4
Light 5
Light 6
Light 7
```

## Step 6 --- PCF4

Connect:

``` text
TV
Sound
Projector
```

## Step 7 --- Limit switches

প্রতিটি motor-এর:

``` text
OPEN limit
CLOSE limit
```

সংশ্লিষ্ট ESP32 GPIO-তে connect করুন।

Firmware অনুযায়ী active state:

``` text
LOW = limit active
```

## Step 8 --- Motor driver

PCF8574 output → motor driver/relay input → motor.

**PCF8574 output সরাসরি motor-এ connect করবেন না।**

------------------------------------------------------------------------

# 25. Motor commissioning procedure

প্রথম installation-এর সময় সবচেয়ে গুরুত্বপূর্ণ হলো actual travel time মাপা।

প্রতিটি device-এর জন্য:

1.  Fully closed অবস্থায় রাখুন।
2.  OPEN command দিন।
3.  Full open হতে কত seconds লাগে মাপুন।
4.  Dashboard-এ Open Time দিন।
5.  Fully open থেকে CLOSE command দিন।
6.  Full close হতে কত seconds লাগে মাপুন।
7.  Dashboard-এ Close Time দিন।
8.  Save Configuration চাপুন।
9.  Limit switch কাজ করছে কিনা test করুন।
10. তারপর repeated open/close test করুন।

### Example

``` text
Window 1
Open = 17 sec
Close = 16 sec
```

Dashboard:

``` text
Window 1
Open Time: 17
Close Time: 16
```

------------------------------------------------------------------------

# 26. Recommended commissioning order

``` mermaid
flowchart TD
    P[Power-on test]
    I2C[I²C device detection]
    OUT[Output test]
    LIMIT[Limit switch test]
    MOTOR[Motor low-risk test]
    TIME[Timing calibration]
    WIFI[Wi-Fi]
    MQTT[MQTT]
    DISC[HA Discovery]
    HA[HA entity control]
    CASCADE[Cascade test]
    FINAL[Final full-system test]

    P --> I2C
    I2C --> OUT
    OUT --> LIMIT
    LIMIT --> MOTOR
    MOTOR --> TIME
    TIME --> WIFI
    WIFI --> MQTT
    MQTT --> DISC
    DISC --> HA
    HA --> CASCADE
    CASCADE --> FINAL
```

------------------------------------------------------------------------

# 27. Safety checklist

### Before connecting motors

-   [ ] Motor driver/relay output verified
-   [ ] Direction verified
-   [ ] Emergency/manual stop available
-   [ ] Open limit switch verified
-   [ ] Close limit switch verified
-   [ ] Motor supply isolated appropriately
-   [ ] ESP32 ground/reference arrangement verified
-   [ ] No ESP32/PCF8574 GPIO connected directly to motor power

### Before automatic operation

-   [ ] Open timing calibrated
-   [ ] Close timing calibrated
-   [ ] Limit switches stop movement
-   [ ] STOP command stops state/output
-   [ ] Reboot leaves motors stopped
-   [ ] Projector/curtain cascade tested

------------------------------------------------------------------------

# 28. Troubleshooting

## MQTT connected কিন্তু Home Assistant entity নেই

Check:

``` text
Serial Monitor @ 115200
```

Expected:

``` text
MQTT Connected!
MQTT Discovery light/l1 -> OK
MQTT Discovery light/l2 -> OK
...
MQTT Discovery cover/cur4 -> OK
```

If discovery says:

``` text
FAILED
```

then discovery publish was not accepted by MQTT client.

Final firmware uses:

``` cpp
client.setBufferSize(1024);
```

because discovery payload contains device metadata and can exceed
PubSubClient's default packet buffer.

------------------------------------------------------------------------

## Wi‑Fi portal reset হচ্ছে

Check that the current firmware retains:

``` text
wifiManager.setConfigPortalBlocking(false);
wifiManager.process();
```

Watchdog timeout:

``` text
8 seconds
```

The configuration portal is intentionally cooperative/non-blocking.

------------------------------------------------------------------------

## Motor runs too long

Check:

1.  Correct limit switch GPIO.
2.  Correct OPEN/CLOSE polarity.
3.  Limit switch becomes LOW when active.
4.  Dashboard timing.
5.  Motor driver direction wiring.

Timing is only a backup timeout; physical limit switches should stop the
motor at the end of travel.

------------------------------------------------------------------------

## Home Assistant state wrong

Check MQTT topics:

``` text
jarvis/status/<uid>
```

and command topics:

``` text
jarvis/<uid>/cmd
```

------------------------------------------------------------------------

# 29. Firmware libraries / software components

Current source includes:

``` text
Wire
PCF8574
WiFi
WebServer
WiFiManager
PubSubClient
Preferences
ArduinoOTA
esp_task_wdt
```

### Responsibilities

  Component        Responsibility
  ---------------- ----------------------------
  `Wire`           ESP32 ↔ PCF8574 I²C
  `PCF8574`        Expanded digital outputs
  `WiFi`           Network connection
  `WebServer`      Local dashboard
  `WiFiManager`    Wi-Fi configuration portal
  `PubSubClient`   MQTT
  `Preferences`    Persistent NVS storage
  `ArduinoOTA`     OTA firmware update
  `esp_task_wdt`   Watchdog

------------------------------------------------------------------------

# 30. Configuration defaults

  Setting                      Default
  ---------------------------- ------------------------------
  MQTT host                    `homeassistant.local`
  MQTT port                    `1883`
  MQTT user                    `esp32`
  MQTT password                `12345678`
  Home Assistant host          `homeassistant.local`
  Home Assistant port          `8123`
  MQTT base                    `jarvis`
  Availability topic           `jarvis/status/availability`
  HA device ID                 `jarvis_home_controller`
  HA device name               `JARVIS Home Controller`
  Motor default timeout        `25 s`
  Motor maximum timeout        `120 s`
  WiFiManager portal timeout   `180 s`
  Watchdog timeout             `8 s`

> **Security:** default credentials shown above are firmware defaults.
> Production installation-এ অবশ্যই নিজের MQTT credentials ব্যবহার করুন।

------------------------------------------------------------------------

# 31. Final feature list

## Hardware

-   [x] ESP32 controller
-   [x] 4× PCF8574
-   [x] I²C expansion
-   [x] 7 lights
-   [x] TV
-   [x] Sound
-   [x] Projector
-   [x] 2 windows
-   [x] 4 curtains
-   [x] Door
-   [x] Open/close limit switches
-   [x] Configurable motor timing
-   [x] Persistent motor timing
-   [x] Persistent light/AV states

## Networking

-   [x] Wi-Fi
-   [x] WiFiManager
-   [x] Non-blocking configuration portal
-   [x] Local web dashboard
-   [x] MQTT
-   [x] MQTT availability
-   [x] MQTT reconnect
-   [x] MQTT state synchronization
-   [x] Home Assistant MQTT Discovery
-   [x] OTA

## Reliability

-   [x] 8-second watchdog
-   [x] Watchdog feed around cooperative tasks
-   [x] Motors stopped on boot
-   [x] Motors stopped during OTA
-   [x] Physical limit switch stop
-   [x] Configurable timeout backup
-   [x] Persistent configuration
-   [x] Discovery payload buffer increased to 1024 bytes

## Automation

-   [x] Curtain 3 → Curtain 4 opening cascade
-   [x] Curtain 4 open → Projector ON
-   [x] Projector OFF → Curtain 4 CLOSE
-   [x] Curtain 4 close → Curtain 3 CLOSE

------------------------------------------------------------------------

# 32. Things intentionally NOT included

The following are **not part of this final firmware scope**:

``` text
❌ Fans
❌ Main Gate
❌ Light brightness/PWM
❌ Direct motor position command from HA
❌ Direct HA REST control path
```

Lights are ON/OFF only.

------------------------------------------------------------------------

# 33. Complete architecture at a glance

``` mermaid
flowchart TB
    subgraph USER["User / Home Assistant"]
        PHONE[Phone / PC]
        HA[Home Assistant]
    end

    subgraph NET["Local Network"]
        WIFI[Wi-Fi]
        MQTT[MQTT Broker]
    end

    subgraph ESP["ESP32 — JARVIS Home Controller"]
        WM[WiFiManager]
        DASH[Web Dashboard]
        MQ[MQTT Client]
        DISC[HA Discovery]
        STATE[State Manager]
        MOTOR[Motor State Machine]
        WDT[8s Watchdog]
        OTA[OTA]
        NVS[Preferences]
    end

    subgraph IO["I/O Expansion"]
        P1[PCF1 0x20]
        P2[PCF2 0x21]
        P3[PCF3 0x22]
        P4[PCF4 0x23]
        LS[Limit Switch Inputs]
    end

    subgraph LOAD["Loads / Interfaces"]
        MOT[Windows / Curtains / Door]
        LIGHTS[7 Lights]
        AV[TV / Sound / Projector]
    end

    PHONE --> DASH
    HA <--> MQTT
    WIFI <--> ESP

    WM --> ESP
    DASH --> ESP
    MQTT <--> MQ
    MQ --> DISC
    MQ --> STATE
    STATE --> MOTOR
    STATE --> NVS
    MOTOR --> P1
    MOTOR --> P2
    P3 --> LIGHTS
    P4 --> AV
    P1 --> MOT
    P2 --> MOT
    LS --> MOTOR
    WDT --> ESP
    OTA --> ESP
```

------------------------------------------------------------------------

# 34. Final connection summary

``` text
                         ┌─────────────────────────┐
                         │    HOME ASSISTANT       │
                         └───────────┬─────────────┘
                                     │ MQTT
                                     ▼
                         ┌─────────────────────────┐
                         │      MQTT BROKER        │
                         └───────────┬─────────────┘
                                     │
                                     │ Wi-Fi / MQTT
                                     ▼
                 ┌──────────────────────────────────────┐
                 │          ESP32 JARVIS CONTROLLER     │
                 │                                      │
                 │ WiFiManager │ Dashboard │ OTA │ WDT │
                 │        MQTT │ Discovery │ NVS       │
                 └───────────────┬──────────────────────┘
                                 │ I²C
               ┌─────────────────┼──────────────────┐
               │                 │                  │
               ▼                 ▼                  ▼
        ┌────────────┐    ┌────────────┐     ┌────────────┐
        │ PCF1 0x20  │    │ PCF2 0x21  │     │ PCF3 0x22  │
        │ Win 1/2    │    │ Cur 3      │     │ Light 1-7  │
        │ Cur 1/2    │    │ Door       │     └─────┬──────┘
        └─────┬──────┘    │ Cur 4      │           │
              │            └─────┬──────┘           ▼
              │                  │             Light Interface
              ▼                  ▼
        Motor Interfaces     Motor Interfaces
              │                  │
              ▼                  ▼
        Windows/Curtains       Door/Curtains

                         ┌────────────┐
                         │ PCF4 0x23  │
                         │ TV/Sound/  │
                         │ Projector  │
                         └─────┬──────┘
                               ▼
                         AV Interface
```

------------------------------------------------------------------------

# 35. Source of truth

এই README-এর architecture, entity list, GPIO map, PCF8574 address map,
MQTT topic structure, dashboard fields, timing behavior, watchdog
behavior, OTA behavior এবং cascade logic **বর্তমান
`JARVIS_HOME_CONTROLLER_MQTT_DISCOVERY_FIXED.ino` firmware-এর source
code থেকে নেওয়া হয়েছে**।

যেখানে firmware নির্দিষ্ট electrical component/driver model বা exact mains
wiring নির্ধারণ করে না, সেখানে README ইচ্ছাকৃতভাবে generic
`Motor Driver / Relay Interface` terminology ব্যবহার করেছে।

**এই README hardware-level mains schematic-এর বিকল্প নয়।**

------------------------------------------------------------------------

## Final status

**Architecture:** Final\
**Firmware scope:** Final\
**MQTT Discovery:** Fixed\
**Home Assistant grouping:** One parent device\
**Motor timing:** Persistent + dashboard configurable\
**Limit switches:** Primary motor stop\
**Timeout:** Backup motor stop\
**Wi-Fi portal:** Non-blocking\
**Watchdog:** 8 seconds\
**OTA:** Enabled\
**Fans:** Removed\
**Main Gate:** Removed
