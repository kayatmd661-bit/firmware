# JARVIS AC FAN + WATER CONTROLLER --- COMPLETE ARCHITECTURE

> **Architecture source:** the supplied
> `JARVIS_AC_FAN_WATER_CONTROLLER_COMPLETE_FIXED.ino` audit/build/wiring
> guide.
>
> **Important:** This diagram represents the documented firmware
> architecture and explicitly documented connections. It does not invent
> mains component values or undocumented interfaces.

## 1. Complete System Architecture

The following Mermaid diagram is embedded directly in this README. It is
designed to render in GitHub, GitLab, VS Code Markdown preview (with
Mermaid support), and other Mermaid-compatible documentation viewers.

``` mermaid
flowchart TB

    %% ============================================================
    %% EXTERNAL NETWORK / HOME ASSISTANT
    %% ============================================================
    HA["HOME ASSISTANT<br/>MQTT Discovery + Entities"]
    MQTT["MQTT BROKER<br/>Port 1883"]
    HA <--> |MQTT| MQTT

    %% ============================================================
    %% ESP32 CONTROLLER
    %% ============================================================
    subgraph ESP["ESP32 — JARVIS AC FAN + WATER CONTROLLER"]
        direction TB

        CORE["CONTROL CORE<br/>Fan State • Speed • Water Logic<br/>ZC Timing • Phase-Angle Control"]

        NET["NETWORK SERVICES<br/>WiFi • MQTT • OTA<br/>Non-blocking reconnect"]
        WEB["WEB DASHBOARD<br/>HTTP :80<br/>Status • Control • Configuration"]
        WM["WiFiManager<br/>First-Time AP: Jarvis_AP"]
        NVS["Preferences / NVS<br/>namespace: jarvis_sys"]
        WDT["Task Watchdog<br/>5 s"]

        ZCE["Zero-Cross Engine<br/>4 channels<br/>Profile + Polarity + Offset<br/>-2000…+2000 µs"]
        PAE["Phase-Angle Engine<br/>50 Hz nominal<br/>20 µs timer tick<br/>120 µs gate pulse"]

        SENS["SENSOR PROCESSING"]
        WATER["WATER CONTROL LOGIC<br/>Distance → Water %<br/>Start ≤15% • Stop ≥98%"]

        CORE --> ZCE
        CORE --> PAE
        CORE --> WATER
        NET --> CORE
        WEB --> CORE
        WM --> NET
        NVS <--> CORE
        WDT -. supervises .-> CORE
        SENS --> CORE
    end

    MQTT <--> |Commands • States • Discovery| NET
    WEB --> |Browser LAN access| USER["LAN Browser / Phone"]
    USER --> WEB
    WM --> |Initial Wi-Fi + MQTT + HA fields| NET

    %% ============================================================
    %% SENSORS
    %% ============================================================
    subgraph SENSORS["LOW-VOLTAGE / SENSOR SIDE"]
        DHT["DHT22<br/>DATA → GPIO27"]
        PIR["PIR<br/>OUT → GPIO26"]
        US["Ultrasonic<br/>TRIG → GPIO32<br/>ECHO → GPIO33"]
        BMP["BMP280<br/>I²C SDA21 / SCL22<br/>Address 0x76"]
        PZEM["PZEM004T v3.x<br/>Serial2 RX16 / TX17<br/>9600 8N1"]
    end

    DHT --> SENS
    PIR --> SENS
    US --> SENS
    BMP --> SENS
    PZEM --> SENS

    %% ============================================================
    %% ISOLATION BOUNDARY
    %% ============================================================
    ISO["ISOLATION BOUNDARY<br/>ESP32 GPIOs must NOT connect directly to mains"]

    %% ============================================================
    %% FAN CONTROL — FOUR CHANNELS
    %% ============================================================
    subgraph FANCTRL["4× INDEPENDENT FAN CHANNELS"]
        direction TB

        subgraph F1["FAN 1"]
            F1TR["TRIAC GPIO13"]
            F1OP["Random-Phase Optotriac<br/>MOC302x / equivalent"]
            F1PT["Power TRIAC"]
            F1FAN["AC FAN 1"]
            F1Z["Isolated ZC Detector"]
            F1ZG["ZC GPIO23"]
            F1TR --> F1OP --> F1PT --> F1FAN
            F1Z --> F1ZG
        end

        subgraph F2["FAN 2"]
            F2TR["TRIAC GPIO14"]
            F2OP["Random-Phase Optotriac"]
            F2PT["Power TRIAC"]
            F2FAN["AC FAN 2"]
            F2Z["Isolated ZC Detector"]
            F2ZG["ZC GPIO34<br/>Input-only + external bias"]
            F2TR --> F2OP --> F2PT --> F2FAN
            F2Z --> F2ZG
        end

        subgraph F3["FAN 3"]
            F3TR["TRIAC GPIO18"]
            F3OP["Random-Phase Optotriac"]
            F3PT["Power TRIAC"]
            F3FAN["AC FAN 3"]
            F3Z["Isolated ZC Detector"]
            F3ZG["ZC GPIO35<br/>Input-only + external bias"]
            F3TR --> F3OP --> F3PT --> F3FAN
            F3Z --> F3ZG
        end

        subgraph F4["FAN 4"]
            F4TR["TRIAC GPIO19"]
            F4OP["Random-Phase Optotriac"]
            F4PT["Power TRIAC"]
            F4FAN["AC FAN 4"]
            F4Z["Isolated ZC Detector"]
            F4ZG["ZC GPIO36<br/>Input-only + external bias"]
            F4TR --> F4OP --> F4PT --> F4FAN
            F4Z --> F4ZG
        end
    end

    PAE --> F1TR
    PAE --> F2TR
    PAE --> F3TR
    PAE --> F4TR

    F1ZG --> ZCE
    F2ZG --> ZCE
    F3ZG --> ZCE
    F4ZG --> ZCE

    ISO --- F1Z
    ISO --- F2Z
    ISO --- F3Z
    ISO --- F4Z

    %% ============================================================
    %% MAINS POWER STAGE
    %% ============================================================
    subgraph MAINS["MAINS / HIGH-VOLTAGE POWER STAGE — SEPARATE FROM ESP32"]
        direction TB
        AC["AC MAINS<br/>L / N / PE as applicable"]
        PROT["Protection / Fuse / MOV / Snubber<br/>Values depend on actual mains + load"]
        AC --> PROT
        PROT --> F1PT
        PROT --> F2PT
        PROT --> F3PT
        PROT --> F4PT
        AC --> F1Z
        AC --> F2Z
        AC --> F3Z
        AC --> F4Z
    end

    %% ============================================================
    %% WATER MOTOR
    %% ============================================================
    subgraph WATERPOWER["WATER MOTOR POWER PATH"]
        GPIO25["ESP32 GPIO25"]
        DRIVER["Rated Motor Switching Driver<br/>Relay / Contactor / SSR as engineered"]
        MOTOR["WATER MOTOR / PUMP"]
        GPIO25 --> DRIVER --> MOTOR
    end

    WATER --> GPIO25

    %% ============================================================
    %% MQTT TOPICS
    %% ============================================================
    TOPICS["MQTT DATA GROUPS<br/>
    Fan: jarvis/sf1…sf4/cmd<br/>
    Speed: jarvis/sf1…sf4/speed/cmd<br/>
    State: jarvis/status/...<br/>
    ZC: jarvis/status/sf1…sf4/zc<br/>
    Sensors: jarvis/sensor/...<br/>
    Motor: jarvis/motor/cmd + status<br/>
    Discovery: homeassistant/..."]

    NET <--> TOPICS
    TOPICS <--> MQTT

    %% ============================================================
    %% SAFETY / AUDIT
    %% ============================================================
    AUDIT["PRE-PRODUCTION REVIEW<br/>
    [REVIEW REQUIRED] WiFiManager custom-field lifecycle<br/>
    [SECURITY WARNING] Dashboard has no authentication<br/>
    [SECURITY WARNING] OTA has no authentication<br/>
    [MUST CHANGE] MQTT default password 12345678<br/>
    [SAFETY REVIEW] Motor saved-ON restore behavior<br/>
    [BEHAVIOR REVIEW] Fan saved-speed restore<br/>
    [VERIFY] Exact ZC / optotriac / TRIAC / protection design"]

    AUDIT -.-> ESP
    AUDIT -.-> MAINS

    %% ============================================================
    %% VISUAL GROUP CONNECTIONS
    %% ============================================================
    F1FAN -. mains return .-> AC
    F2FAN -. mains return .-> AC
    F3FAN -. mains return .-> AC
    F4FAN -. mains return .-> AC

    %% ============================================================
    %% STYLING
    %% ============================================================
    classDef controller fill:#17324d,color:#fff,stroke:#7ec8ff,stroke-width:2px;
    classDef network fill:#254d3b,color:#fff,stroke:#8ee0b0,stroke-width:2px;
    classDef sensor fill:#4b3d62,color:#fff,stroke:#c5a8ff,stroke-width:2px;
    classDef isolation fill:#60451f,color:#fff,stroke:#ffd27a,stroke-width:2px;
    classDef mains fill:#542b2b,color:#fff,stroke:#ff9d9d,stroke-width:2px;
    classDef warning fill:#5a4b1e,color:#fff,stroke:#ffe083,stroke-width:2px;

    class CORE,ZCE,PAE,WATER controller;
    class HA,MQTT,NET,WEB,WM,NVS,WDT,TOPICS network;
    class DHT,PIR,US,BMP,PZEM,SENS sensor;
    class ISO isolation;
    class AC,PROT,MAINS,F1PT,F2PT,F3PT,F4PT,F1FAN,F2FAN,F3FAN,F4FAN,F1Z,F2Z,F3Z,F4Z mains;
    class AUDIT warning;
```

## 2. Firmware-to-Hardware Pin Architecture

  Function            GPIO Direction / Interface   Architecture role
  ----------------- ------ ----------------------- ------------------------------------
  PZEM RX               16 Serial2 RX              Electrical measurement
  PZEM TX               17 Serial2 TX              Electrical measurement
  Fan 1 TRIAC           13 Output                  Random-phase optotriac trigger
  Fan 2 TRIAC           14 Output                  Random-phase optotriac trigger
  Fan 3 TRIAC           18 Output                  Random-phase optotriac trigger
  Fan 4 TRIAC           19 Output                  Random-phase optotriac trigger
  Fan 1 ZC              23 Input/interrupt         Isolated zero-cross feedback
  Fan 2 ZC              34 Input/interrupt         Input-only; external bias required
  Fan 3 ZC              35 Input/interrupt         Input-only; external bias required
  Fan 4 ZC              36 Input/interrupt         Input-only; external bias required
  DHT22                 27 Digital                 Temperature / humidity
  PIR                   26 Input                   Motion
  Ultrasonic TRIG       32 Output                  Water-level measurement
  Ultrasonic ECHO       33 Input                   Water-level measurement
  Water motor           25 Output                  Motor switching driver control
  BMP280 SDA            21 I²C                     Pressure
  BMP280 SCL            22 I²C                     Pressure

## 3. Critical Architecture Rules

### Fan channels

Each channel has two independent paths:

**Zero-cross feedback**

`AC mains → isolated ZC detector → ESP32 ZC GPIO`

**Phase-angle trigger**

`ESP32 TRIAC GPIO → random-phase optotriac → power TRIAC → fan`

The zero-cross detector is **not** the TRIAC driver.

### Water motor

`ESP32 GPIO25 → properly rated switching driver → motor`

GPIO25 must never directly drive a mains motor.

### Home Assistant

The documented transport is:

`Home Assistant ↔ MQTT Broker ↔ ESP32`

The configured `haHost` / `haPort` values are metadata/configuration
fields. They do **not** establish a direct Home Assistant REST/WebSocket
control path in this firmware.

## 4. Network and MQTT Architecture

``` text
                    HOME ASSISTANT
                          │
                          │ MQTT
                          ▼
                   MQTT BROKER :1883
                          │
                          ▼
                 ESP32 MQTT CLIENT
                          │
        ┌─────────────────┼──────────────────┐
        │                 │                  │
     COMMANDS           STATES           DISCOVERY
        │                 │                  │
   Fan ON/OFF          Fan state       homeassistant/...
   Fan speed           Fan speed
   Motor command       ZC health
                       Sensors
                       Motor state
```

### Main documented MQTT groups

-   `jarvis/sf1/cmd` ... `jarvis/sf4/cmd`
-   `jarvis/sf1/speed/cmd` ... `jarvis/sf4/speed/cmd`
-   `jarvis/status/sf1` ... `jarvis/status/sf4`
-   `jarvis/status/sf1/speed` ... `jarvis/status/sf4/speed`
-   `jarvis/status/sf1/zc` ... `jarvis/status/sf4/zc`
-   `jarvis/sensor/water_pct`
-   `jarvis/sensor/temp`
-   `jarvis/sensor/hum`
-   `jarvis/sensor/pressure`
-   `jarvis/sensor/volt`
-   `jarvis/sensor/curr`
-   `jarvis/sensor/pwr`
-   `jarvis/sensor/pir`
-   `jarvis/motor/cmd`
-   `jarvis/status/motor`
-   `jarvis/settings/empty/set`
-   `jarvis/settings/full/set`
-   `homeassistant/...` for MQTT Discovery

## 5. Configuration and Persistence

``` text
                    ┌──────────────────────┐
                    │   WiFiManager AP     │
                    │     Jarvis_AP        │
                    └──────────┬───────────┘
                               │
                               ▼
                    Wi-Fi / MQTT configuration
                               │
                               ▼
                    ┌──────────────────────┐
                    │ ESP32 Configuration  │
                    │      Dashboard       │
                    └──────────┬───────────┘
                               │
                               ▼
                    Preferences / NVS
                    namespace: jarvis_sys
```

Stored configuration includes MQTT settings, HA metadata, fan
limits/startup speed, tank calibration, motor thresholds, ZC
profiles/offsets, fan speed state and motor state.

## 6. Runtime Control Flow

``` text
BOOT
  │
  ├── WiFi setup / WiFiManager
  │
  ├── Load Preferences/NVS
  │
  ├── Network services
  │      ├── MQTT
  │      ├── Web Dashboard
  │      └── OTA
  │
  ├── Sensors
  │      ├── DHT22
  │      ├── BMP280
  │      ├── PIR
  │      ├── Ultrasonic
  │      └── PZEM004T
  │
  ├── Zero-cross detection ×4
  │
  ├── Phase-angle fan control ×4
  │
  ├── Water-level calculation
  │
  ├── Motor decision
  │
  └── Watchdog / main loop
```

## 7. Safety Boundary

The architecture must maintain this physical separation:

``` text
┌──────────────────────────────────────────────────────────┐
│ LOW-VOLTAGE / CONTROL                                   │
│                                                          │
│ ESP32 • Wi-Fi • MQTT • Sensors • ZC logic output        │
│ Optotriac LED input • Dashboard • NVS • OTA             │
│                                                          │
└──────────────────────────┬───────────────────────────────┘
                           │
                    ISOLATION BARRIER
                           │
┌──────────────────────────┴───────────────────────────────┐
│ MAINS / HIGH-VOLTAGE                                     │
│                                                          │
│ AC line • Fuse • MOV • Snubber • ZC mains input         │
│ Random-phase optotriac output • Power TRIAC • Fan       │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

**Never connect an ESP32 GPIO directly to AC mains, a mains-referenced
detector output, a fan, a pump, or a power TRIAC gate.**

## 8. Production Audit Status

  -----------------------------------------------------------------------
  Item                                Status
  ----------------------------------- -----------------------------------
  4-channel ZC-synchronized           **GOOD / controlled bench
  phase-angle architecture            commissioning**

  WiFiManager custom parameter        **\[REVIEW REQUIRED\]**
  lifecycle                           

  Dashboard authentication            **\[SECURITY WARNING\]**

  OTA authentication                  **\[SECURITY WARNING\]**

  Default MQTT password               **\[MUST CHANGE\]**

  Saved motor ON restoration          **\[SAFETY REVIEW REQUIRED\]**

  Saved fan speed restoration         **\[BEHAVIOR REVIEW REQUIRED\]**

  HA direct REST/WebSocket path       **\[NOT USED / NOT DOCUMENTED\]**

  Universal mains component values    **\[INTENTIONALLY NOT SPECIFIED\]**

  Exact mains protection design       **\[MUST BE ENGINEER-VERIFIED\]**
  -----------------------------------------------------------------------

## 9. Important Component Architecture

### Zero-cross detector

Documented baseline:

-   Vishay H11AA1 class AC-input optocoupler
-   Isolated transistor output
-   Appropriate external pull-up/bias
-   Correct RISING/FALLING firmware profile

### Phase-angle optotriac

Must be **random-phase**, such as the documented MOC302x-class family or
an equivalent device selected for the actual design.

**Do not use a zero-cross-only MOC306x-class device for phase-angle
control.**

### Power TRIAC

The firmware intentionally does not define one universal TRIAC, fuse,
MOV, snubber or mains resistor network. These must be selected from the
actual:

-   mains voltage/frequency
-   fan current
-   inrush behavior
-   dv/dt and di/dt requirements
-   thermal conditions
-   enclosure and PCB requirements

## 10. Final Architecture Verdict

**Firmware architecture:** GOOD / READY FOR CONTROLLED BENCH
COMMISSIONING.

**Real-mains deployment:** NOT APPROVED solely from firmware
documentation. The production safety, isolation, mains protection,
TRIAC, optotriac, thermal, creepage/clearance and enclosure design must
be verified for the actual hardware and load.
