# JARVIS AC FAN + WATER CONTROLLER --- MASTER BUILD BOOK

## সম্পূর্ণ বাংলা Master Documentation --- Hardware + Firmware + Dashboard + MQTT + Home Assistant + Wiring + Security + Commissioning

> **Source basis:**
> `JARVIS_AC_FAN_WATER_CONTROLLER_COMPLETE_FIXED.ino`-এর বাস্তব source
> audit এবং আগের README audit।\
> **নিয়ম:** source-এ যা আছে সেটি **আছে**, যা নেই সেটি **নেই**, আর
> hardware-dependent বিষয় **VERIFY/LOAD-SPECIFIC** হিসেবে চিহ্নিত করা
> হয়েছে।

------------------------------------------------------------------------

# 1. প্রকল্পের পরিচয়

এটি ESP32-ভিত্তিক ৪-চ্যানেল AC ceiling-fan + water-motor controller।

### Built-in software features

-   ৪টি AC fan
-   AC phase-angle TRIAC speed control
-   ৪টি isolated zero-cross input
-   0--100% logical fan speed
-   প্রতি fan-এর ZC profile নির্বাচন
-   প্রতি fan-এর ZC timing offset: `-2000 … +2000 µs`
-   automatic mains half-cycle measurement
-   DHT22 temperature/humidity
-   BMP280 pressure
-   PIR motion
-   ultrasonic water level
-   automatic water motor control
-   PZEM004T electrical measurement
-   MQTT
-   Home Assistant MQTT Discovery
-   built-in web dashboard
-   WiFiManager first-time setup
-   Preferences/NVS
-   Arduino OTA
-   watchdog
-   non-blocking MQTT reconnect

**Fan speed control PWM নয়; এটি AC phase-angle control।**

------------------------------------------------------------------------

# 2. Dashboard কি আছে?

## হ্যাঁ, Dashboard আছে

Firmware-এ HTTP `WebServer` port `80`-এ চালু হয়।

``` text
http://<ESP32-IP>/
```

Dashboard-এ আছে:

-   Fan 1--4 ON/OFF
-   Fan 1--4 speed slider
-   Zero-cross health
-   Temperature
-   Humidity
-   Water %
-   Motor status
-   Voltage
-   Current
-   Power
-   Pressure
-   WiFi RSSI
-   MQTT status
-   MQTT settings
-   Home Assistant metadata
-   Fan minimum/maximum/startup speed
-   Tank empty/full distance
-   Motor start/stop %
-   প্রতি fan ZC profile
-   প্রতি fan ZC timing offset

------------------------------------------------------------------------

# 3. ⚠️ Admin username/password --- বাস্তব source status

এটি খুব গুরুত্বপূর্ণ।

## বর্তমান source-এ Dashboard authentication নেই।

অর্থাৎ:

``` text
Dashboard username = নেই
Dashboard password = নেই
Admin username     = নেই
Admin password     = নেই
```

বর্তমান endpoint:

``` text
GET  /
GET  /api/status
GET  /api/fan
POST /save
```

তাই `admin / 12345678`-কে Dashboard login credential হিসেবে ব্যবহার করা
যাবে না।

**আগের কথোপকথনে admin credential বলা হলেও বর্তমান source audit-এ তা পাওয়া
যায়নি। এই master document-এ তাই সেটিকে বাস্তব credential হিসেবে ঘোষণা করা
হয়নি।**

### Production security target

Dashboard-এ authentication যোগ করতে হবে:

``` text
admin username
+
strong unique password
```

এবং `/`, `/api/status`, `/api/fan`, `/save`---সব protected করতে হবে।

------------------------------------------------------------------------

# 4. বর্তমান MQTT credential

Source default:

``` text
MQTT Host     = homeassistant.local
MQTT Port     = 1883
MQTT Username = esp32
MQTT Password = 12345678
```

এগুলো Preferences/NVS-এ পরিবর্তনযোগ্য।

**Production-এ `12345678` ব্যবহার করা যাবে না।**

------------------------------------------------------------------------

# 5. WiFiManager

First boot setup AP:

``` text
SSID = Jarvis_AP
```

WiFiManager-এ:

``` text
WiFi SSID
WiFi Password

MQTT Host
MQTT Port
MQTT Username
MQTT Password

Home Assistant Host/IP
Home Assistant Port
HA Device Name
HA Device ID
```

**বর্তমান source-এ setup AP password explicitly configured নয়।
Production-এ setup security policy দরকার।**

------------------------------------------------------------------------

# 6. OTA

Hostname:

``` text
jarvis-fan-system
```

OTA আছে।

কিন্তু:

``` text
OTA password = নেই
```

**Production-এ authenticated OTA অথবা trusted maintenance network
দরকার।**

------------------------------------------------------------------------

# 7. সম্পূর্ণ architecture

``` text
                         HOME ASSISTANT
                    Dashboard / Automation
                              │
                              │ MQTT
                              ▼
                         MQTT BROKER
                              │
                              ▼
                    ┌───────────────────┐
                    │      ESP32        │
                    │ JARVIS CONTROLLER │
                    ├───────────────────┤
                    │ WiFiManager       │
                    │ Web Dashboard     │
                    │ MQTT              │
                    │ OTA               │
                    │ Preferences/NVS   │
                    │ Watchdog          │
                    │ ZC Engine         │
                    │ Phase-Angle       │
                    └───────┬───────────┘
                            │
          ┌─────────────────┼─────────────────┐
          │                 │                 │
       ZC ×4             TRIAC ×4          Sensors
          │                 │                 │
   isolated detector   random-phase       DHT/PIR/
   H11AA1/module       optotriac           BMP/US/PZEM
                            │
                       power TRIAC
                            │
                         FAN ×4

                         WATER CONTROL
                              │
                           GPIO25
                              │
                        motor driver
                              │
                         WATER MOTOR
```

------------------------------------------------------------------------

# 8. Embedded 3D-style diagram

``` text
                    ╔══════════════════════════════════╗
                   ╱       HOME ASSISTANT / MQTT      ╱│
                  ╱ Dashboard • Automation • HA     ╱ │
                 ╚══════════════════════════════════╝  │
                 │                                     │
                 │             MQTT / LAN              │
                 ▼                                     │
        ╔═══════════════════════════════════════════════╧══╗
       ╱               ESP32 JARVIS CONTROLLER             ╱│
      ╱ WiFi • Dashboard • MQTT • OTA • NVS • WDT • ZC   ╱ │
     ╚═══════════════════════════════════════════════════╝  │
     │       │       │       │       │       │             │
     ▼       ▼       ▼       ▼       ▼       ▼             │
    DHT     BMP     PIR     US     PZEM    MOTOR           │
     │       │       │       │       │       │             │
     └───────┴───────┴───────┴───────┘       ▼             │
                                             DRIVER          │
                                               │             │
                                               ▼             │
                                           WATER MOTOR       │
                                                           │
       ZC INPUT ×4                     TRIAC OUTPUT ×4     │
            │                                  │           │
     ╔══════╧══════╗                    ╔═════╧══════╗    │
    ╱ isolated ZC  ╱│                   ╱ random     ╱│    │
   ╚══════════════╝ │                  ╚════════════╝ │    │
   │ H11AA1/module │                  │ optotriac   │ │    │
   └───────────────┘                  └──────┬──────┘ │    │
                                            ▼          │
                                       POWER TRIAC     │
                                            ▼          │
                                           FAN         │
```

এই diagram-এ ESP32 এবং mains section-এর isolation boundary আলাদা ধরে
নিতে হবে।

------------------------------------------------------------------------

# 9. Exact GPIO map

  কাজ                    GPIO Direction   মন্তব্য
  -------------------- ------ ----------- -------------------
  PZEM RX                  16 RX          Serial2
  PZEM TX                  17 TX          Serial2
  Fan 1 TRIAC              13 OUTPUT      optotriac input
  Fan 2 TRIAC              14 OUTPUT      optotriac input
  Fan 3 TRIAC              18 OUTPUT      optotriac input
  Fan 4 TRIAC              19 OUTPUT      optotriac input
  Fan 1 ZC                 23 INPUT/INT   isolated detector
  Fan 2 ZC                 34 INPUT/INT   input-only
  Fan 3 ZC                 35 INPUT/INT   input-only
  Fan 4 ZC                 36 INPUT/INT   input-only
  DHT22                    27 Digital     DATA
  PIR                      26 INPUT       motion
  Ultrasonic TRIG          32 OUTPUT      trigger
  Ultrasonic ECHO          33 INPUT       echo
  Water motor driver       25 OUTPUT      driver only
  BMP280 SDA               21 I2C         address 0x76
  BMP280 SCL               22 I2C         address 0x76

ESP32 GPIO34--39 classic ESP32-তে input-only এবং internal
pull-up/pull-down নেই। তাই GPIO34/35/36-এর detector output-এ external
bias/pull-up প্রয়োজন।

------------------------------------------------------------------------

# 10. Fan control architecture

প্রতি channel:

``` text
ESP32 GPIO
   │
   ▼
LED resistor
   │
   ▼
RANDOM-PHASE OPTO-TRIAC
   │
   ▼
TRIAC gate network
   │
   ▼
POWER TRIAC
   │
   ▼
AC FAN
```

চারটি output:

``` text
Fan 1 → GPIO13
Fan 2 → GPIO14
Fan 3 → GPIO18
Fan 4 → GPIO19
```

**ESP32 GPIO কখনো mains-এ সরাসরি যাবে না।**

------------------------------------------------------------------------

# 11. Zero-cross architecture

প্রতি channel:

``` text
AC mains
   │
   ▼
rated isolated AC zero-cross detector
   │
   │ isolation barrier
   ▼
phototransistor / module output
   │
   ▼
external pull-up / logic stage
   │
   ▼
ESP32 ZC GPIO
```

Profiles:

``` text
AC-OPTO / RISING
AC-OPTO / FALLING

MODULE / RISING
MODULE / FALLING

PC817 AC DETECTOR / RISING
PC817 AC DETECTOR / FALLING
```

Recommended common class:

``` text
H11AA1-class AC optocoupler
```

PC817 হলে proper mains-side rectifier/current-limiting circuit ছাড়া
ব্যবহার করা যাবে না।

------------------------------------------------------------------------

# 12. Random-phase optotriac

Phase-angle control-এর জন্য:

``` text
MOC3020
MOC3021
MOC3022
MOC3023 / MOC3023M
MOC3051
MOC3052
সমমান random-phase phototriac
```

### ব্যবহার করা যাবে না

``` text
MOC306x
অথবা অন্য zero-cross-only optotriac
```

কারণ arbitrary phase firing দরকার।

বর্তমান distributor listings-এ MOC3023 original/variants ও alternatives
পাওয়া যায়; exact manufacturer/suffix production BOM-এ freeze করতে হবে।

------------------------------------------------------------------------

# 13. Power TRIAC

একটি universal TRIAC firmware থেকে নির্ধারিত হয়নি।

নির্ভর করে:

-   120/127/220/230/240 V
-   fan current
-   startup/inrush
-   Igt
-   holding current
-   dv/dt
-   di/dt
-   thermal design
-   heatsink
-   enclosure

BT136-600-class 600 V/4 A variants বাজারে পাওয়া যায়, কিন্তু **এটি আপনার
fan-এর final TRIAC ধরে নেওয়া যাবে না**।

Final TRIAC load এবং datasheet দেখে নির্বাচন করতে হবে।

------------------------------------------------------------------------

# 14. Mains protection

প্রয়োজন অনুযায়ী:

``` text
Fuse
MOV
RC snubber
Gate resistor network
Thermal management
Rated terminals
Creepage
Clearance
Touch-safe enclosure
```

### Universal component value দেওয়া হয়নি

কারণ:

``` text
Fuse = load/current/surge dependent
MOV = mains/system dependent
Snubber = TRIAC + motor dependent
Mains resistor = voltage + detector current + pulse/surge dependent
```

এগুলো আন্দাজ করে বসানো যাবে না।

------------------------------------------------------------------------

# 15. Low-voltage BOM

### Controller

-   ESP32 Dev Module / compatible ESP32
-   regulated power supply
-   terminal blocks
-   enclosure

### Fan ×4

প্রতি channel:

-   random-phase optotriac
-   power TRIAC
-   gate resistor network
-   required snubber/protection
-   fuse/protection
-   heatsink if required

### ZC ×4

প্রতি channel:

-   H11AA1-class detector অথবা verified isolated ZC module
-   mains-side rated resistor/network
-   external pull-up/bias
-   isolation barrier

### Sensors

-   DHT22
-   BMP280
-   PIR
-   ultrasonic sensor
-   PZEM004T v3.x class

### Motor

-   appropriately rated relay/contactor/SSR
-   driver stage
-   suppression
-   fuse/protection

------------------------------------------------------------------------

# 16. Sensor wiring

## DHT22

``` text
GPIO27 ← DATA
3.3V   → VCC
GND    → GND
```

## PIR

``` text
PIR OUT → GPIO26
PIR GND → GND
PIR VCC → module specification
```

## Ultrasonic

``` text
TRIG → GPIO32
ECHO → GPIO33
```

**5 V ECHO হলে level shifting ছাড়া ESP32-তে দেওয়া যাবে না।**

## BMP280

``` text
SDA → GPIO21
SCL → GPIO22
Address = 0x76
```

## PZEM

``` text
RX → GPIO16
TX → GPIO17
Serial2 = 9600 8N1
```

## Motor

``` text
GPIO25
   ↓
motor driver
   ↓
pump/motor
```

GPIO25 সরাসরি motor drive করবে না।

------------------------------------------------------------------------

# 17. Water logic

Default:

``` text
Empty distance = 110 cm
Full distance  = 10 cm

Motor START ≤ 15%
Motor STOP  ≥ 98%
```

Mapping:

``` text
110 cm → 0%
10 cm  → 100%
```

Logic:

``` text
water <= 15% → ON
water >= 98% → OFF
```

------------------------------------------------------------------------

# 18. Phase-angle timing

Current firmware:

``` text
Nominal mains = 50 Hz
Half-cycle    = 10 ms
Timer tick    = 20 µs
Gate pulse    = 120 µs
Early margin  = 250 µs
Late margin   = 500 µs
Default min   = 20%
```

Concept:

``` text
100% → half-cycle-এর শুরুতে firing
 50% → মাঝামাঝি usable window
 20% → late firing
  0% → gate pulse নেই
```

এটি AC phase-angle control, DC PWM নয়।

------------------------------------------------------------------------

# 19. ZC timing

Valid detector edge থেকে firmware half-cycle measure করে।

Accepted:

``` text
7 ms … 12 ms
```

Minimum edge spacing:

``` text
2500 µs
```

Per-fan correction:

``` text
-2000 … +2000 µs
```

Commissioning default:

``` text
Profile = actual detector polarity
Offset  = 0 µs
```

------------------------------------------------------------------------

# 20. MQTT master topics

### Availability

``` text
jarvis/status/availability
```

### Fan ON/OFF

``` text
jarvis/sf1/cmd
jarvis/sf2/cmd
jarvis/sf3/cmd
jarvis/sf4/cmd
```

Payload:

``` text
ON
OFF
```

### Fan speed

``` text
jarvis/sf1/speed/cmd
...
jarvis/sf4/speed/cmd
```

Payload:

``` text
0–100
```

### State

``` text
jarvis/status/sf1
...
jarvis/status/sf4
```

### Speed state

``` text
jarvis/status/sf1/speed
...
jarvis/status/sf4/speed
```

### ZC health

``` text
jarvis/status/sf1/zc
...
jarvis/status/sf4/zc
```

### Motor

``` text
jarvis/motor/cmd
jarvis/status/motor
```

### Sensors

``` text
jarvis/sensor/water_pct
jarvis/sensor/temp
jarvis/sensor/hum
jarvis/sensor/pressure
jarvis/sensor/volt
jarvis/sensor/curr
jarvis/sensor/pwr
jarvis/sensor/pir
```

### Tank settings

``` text
jarvis/settings/empty/set
jarvis/settings/full/set
```

------------------------------------------------------------------------

# 21. Home Assistant MQTT Discovery

Discovery prefix:

``` text
homeassistant
```

বর্তমান firmware discover করে:

-   4 fan
-   temperature
-   humidity
-   water %
-   voltage
-   current
-   power
-   pressure
-   PIR
-   water motor switch
-   4 zero-cross health binary sensors

MQTT Discovery-তে unique ID/device configuration ব্যবহার করা হয়েছে।

**Actual HA control/discovery transport = MQTT।**

`haHost`/`haPort` direct HA REST/WebSocket connection-এর প্রমাণ নয়।

------------------------------------------------------------------------

# 22. Dashboard/API master list

### Dashboard

``` text
GET /
```

### Status

``` text
GET /api/status
```

### Fan

``` text
GET /api/fan?i=0&speed=50
GET /api/fan?i=0&state=1
GET /api/fan?i=0&state=0
```

Fan index:

``` text
0 = Fan 1
1 = Fan 2
2 = Fan 3
3 = Fan 4
```

### Configuration

``` text
POST /save
```

------------------------------------------------------------------------

# 23. NVS / persistence

Namespace:

``` text
jarvis_sys
```

Stored:

``` text
MQTT host/port/user/password
HA host/port/name/id
Fan min/max/startup
Tank empty/full
Motor start/stop
ZC profile ×4
ZC offset ×4
Fan speed states
Motor state
```

Power-cycle-এর পর configuration থাকে।

------------------------------------------------------------------------

# 24. Power restore safety

বর্তমান source saved output state restore করতে পারে।

### Fan

Saved non-zero speed থাকলে fan restore হতে পারে।

### Motor

Saved motor state `ON` হলে GPIO25 HIGH হতে পারে।

**Production safety decision হিসেবে এটি আলাদাভাবে approve করতে হবে।**

Safer target:

``` text
BOOT
 ↓
Fan OFF
Motor OFF
 ↓
Sensors + ZC initialize
 ↓
Water logic authorize করলে motor
 ↓
User/MQTT command দিলে fan
```

এটি **বর্তমান source-এর behavior নয়; production target policy**।

------------------------------------------------------------------------

# 25. Watchdog

``` text
WDT = 5 seconds
```

Main loop handles:

``` text
WiFi
OTA
WebServer
MQTT
ZC health
Sensors
Watchdog
```

MQTT reconnect non-blocking।

------------------------------------------------------------------------

# 26. Security audit

  বিষয়              বর্তমান অবস্থা
  ----------------- --------------------------
  Dashboard         আছে
  Dashboard login   নেই
  Admin username    নেই
  Admin password    নেই
  MQTT username     `esp32` default
  MQTT password     `12345678` default
  OTA               আছে
  OTA password      নেই
  HTTPS             নেই
  MQTT TLS          source-এ নেই
  NVS               আছে
  ZC isolation      hardware-dependent
  Motor isolation   external driver required

### Production target

``` text
Dashboard → authentication
API       → authentication
OTA       → authentication
MQTT      → unique credentials + ACL
Network   → trusted LAN/VLAN
Setup AP  → protected commissioning
```

------------------------------------------------------------------------

# 27. Safety rules

Never:

``` text
ESP32 GPIO → mains
ESP32 GPIO → fan wire
ESP32 GPIO → pump wire
ESP32 GPIO → power TRIAC gate directly
PC817 LED → mains directly
```

Use:

``` text
ESP32
 ↓
isolated optocoupler
 ↓
TRIAC driver
 ↓
power TRIAC
 ↓
fan
```

Zero-cross:

``` text
mains
 ↓
rated isolated detector
 ↓
isolation barrier
 ↓
ESP32
```

**Mains circuit breadboard-এ test করা যাবে না।**

------------------------------------------------------------------------

# 28. Commissioning sequence

## A --- ESP32 only

-   flash
-   serial 115200
-   boot
-   verify fan outputs LOW
-   verify motor LOW

## B --- WiFi

-   connect `Jarvis_AP`
-   configure WiFi
-   obtain IP

## C --- Dashboard

``` text
http://ESP32-IP/
```

## D --- MQTT

-   broker settings
-   online availability
-   discovery
-   command/state

## E --- Sensors

Test separately:

``` text
DHT22
BMP280
PIR
Ultrasonic
PZEM
```

## F --- ZC

প্রতি channel:

``` text
ZC = OK
Half-cycle ≈ expected
No false edge storm
```

## G --- Fan

একটি fan:

``` text
OFF
→ 100%
→ 80%
→ 60%
→ 40%
→ minimum
```

তারপর 2, 3, 4।

------------------------------------------------------------------------

# 29. Troubleshooting

## Fan কাজ করছে না

Check:

1.  ZC `OK`
2.  RISING/FALLING profile
3.  speed \> minimum
4.  random-phase optotriac
5.  gate circuit
6.  power TRIAC
7.  fuse/protection

## ZC FAULT

Check:

1.  detector mains input
2.  isolation
3.  pull-up
4.  GPIO34/35/36 external bias
5.  polarity
6.  ESP32-safe output voltage

## Fan hum/noise

সম্ভাব্য:

-   motor/fan phase-angle incompatibility
-   low-speed behavior
-   gate drive
-   TRIAC selection
-   snubber/EMI
-   minimum firing window

## MQTT unavailable

Check:

``` text
WiFi
Broker IP
Port
Username
Password
ACL
```

## HA entity missing

Check:

``` text
MQTT integration
Discovery enabled
homeassistant/# topics
unique_id
broker connection
```

------------------------------------------------------------------------

# 30. Market/component reference

বর্তমান market/distributor references-এ সাধারণত পাওয়া যায়:

### Zero-cross

-   H11AA1-family AC optocoupler
-   H11AA1-compatible variants
-   verified isolated ZC modules

### Phase optotriac

-   MOC3023/MOC3023M class
-   MOC302x family
-   MOC305x family
-   equivalent random-phase drivers

### Power TRIAC

-   BT136-600 class
-   other 600/800 V TRIAC families selected from actual load

**Stock/price/manufacturer suffix পরিবর্তনশীল; final BOM freeze করার আগে
exact datasheet + distributor listing verify করতে হবে।**

------------------------------------------------------------------------

# 31. Source audit findings

## AUDIT-1 --- WiFiManager custom parameter lifecycle

Non-blocking portal-এর custom fields delayed submission-এর ক্ষেত্রে
আলাদাভাবে verify করা দরকার।

**Status: REVIEW REQUIRED**

## AUDIT-2 --- Dashboard authentication

Authentication নেই।

**Status: SECURITY WARNING**

## AUDIT-3 --- OTA authentication

Password নেই।

**Status: SECURITY WARNING**

## AUDIT-4 --- default MQTT password

``` text
12345678
```

**Status: MUST CHANGE**

## AUDIT-5 --- motor restore

Saved motor state restore হতে পারে।

**Status: SAFETY REVIEW**

## AUDIT-6 --- fan restore

Saved non-zero fan speed restore হতে পারে।

**Status: BEHAVIOR REVIEW**

## AUDIT-7 --- HA host/port

MQTT metadata; direct HA API নয়।

**Status: DOCUMENTED**

## AUDIT-8 --- mains component values

Universal values না দেওয়াই সঠিক।

**Status: LOAD-SPECIFIC**

------------------------------------------------------------------------

# 32. Production checklist

## Firmware

-   [ ] Arduino-ESP32 version freeze
-   [ ] libraries freeze
-   [ ] compile clean
-   [ ] ZC ISR clean
-   [ ] WDT test
-   [ ] WiFi reconnect test
-   [ ] MQTT reconnect test
-   [ ] Dashboard test
-   [ ] API test
-   [ ] NVS test
-   [ ] OTA test
-   [ ] HA discovery test

## Security

-   [ ] Dashboard login যোগ করা
-   [ ] strong admin password
-   [ ] OTA authentication
-   [ ] MQTT password change
-   [ ] MQTT ACL
-   [ ] network restriction
-   [ ] setup AP security

## Fan hardware

-   [ ] mains voltage confirmed
-   [ ] mains frequency confirmed
-   [ ] random-phase optotriac confirmed
-   [ ] power TRIAC confirmed
-   [ ] gate network confirmed
-   [ ] fuse confirmed
-   [ ] MOV confirmed
-   [ ] snubber confirmed
-   [ ] heatsink confirmed
-   [ ] creepage confirmed
-   [ ] clearance confirmed
-   [ ] enclosure confirmed

## ZC

-   [ ] four isolated detectors
-   [ ] output voltage verified
-   [ ] GPIO34/35/36 external bias
-   [ ] polarity verified
-   [ ] half-cycle verified
-   [ ] timing offset calibrated

## Motor

-   [ ] driver rated
-   [ ] suppression
-   [ ] protection
-   [ ] boot behavior approved
-   [ ] dry-run protection considered

------------------------------------------------------------------------

# 33. Final truth table

  Feature                            Current source
  ---------------------------------- ---------------------------
  4 fan dashboard                    ✅ আছে
  Fan ON/OFF                         ✅ আছে
  Fan speed                          ✅ আছে
  ZC status                          ✅ আছে
  MQTT                               ✅ আছে
  HA MQTT Discovery                  ✅ আছে
  DHT/BMP/PIR/Ultrasonic/PZEM        ✅ আছে
  Water motor automation             ✅ আছে
  NVS                                ✅ আছে
  WiFiManager                        ✅ আছে
  OTA                                ✅ আছে
  WDT                                ✅ আছে
  Dashboard admin login              ❌ নেই
  Admin username                     ❌ নেই
  Admin password                     ❌ নেই
  OTA password                       ❌ নেই
  Default MQTT password              ⚠️ `12345678`
  Direct HA REST/WebSocket control   ❌ নেই
  Universal mains resistor values    ❌ নেই; hardware-specific
  Universal TRIAC part number        ❌ নেই; load-specific

------------------------------------------------------------------------

# 34. Reference sources

### Home Assistant MQTT

https://www.home-assistant.io/integrations/mqtt/

MQTT Discovery, unique ID, retained discovery এবং availability সম্পর্কে
official documentation।

### Espressif GPIO

https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html

Classic ESP32 GPIO34--39 input-only; GPIO34--39 internal software
pull-up/pull-down ছাড়া।

### H11AA1

https://www.vishay.com/en/product/83608/

### onsemi MOC3023M

https://www.onsemi.com/pdf/datasheet/moc3023m-d.pdf

### PZEM

Exact PZEM-004T version-এর manufacturer/manual অনুসরণ করতে হবে।

------------------------------------------------------------------------

# 35. Final verdict

## Software

**Controlled bench commissioning-এর জন্য architecture ভালো।**

``` text
ESP32
 ↓
isolated ZC
 ↓
phase-angle engine
 ↓
random-phase optotriac
 ↓
power TRIAC
 ↓
fan
```

এর সঙ্গে:

``` text
MQTT
HA Discovery
Dashboard
Sensors
Water automation
NVS
OTA
WDT
```

## কিন্তু "fully production-secure" বলা যাবে না যতক্ষণ:

``` text
1. Dashboard authentication নেই
2. OTA authentication নেই
3. Default MQTT password বদলানো হয়নি
4. WiFiManager custom parameter lifecycle verify হয়নি
5. Motor/fan power-restore policy final হয়নি
6. Exact mains hardware design verified হয়নি
```

------------------------------------------------------------------------

# 36. Golden rule

**Source-এ নেই এমন feature-কে built-in বলা যাবে না।**

**Mains-specific component value আন্দাজ করে লেখা যাবে না।**

**ESP32 side এবং mains side-এর isolation কখনো ভাঙা যাবে না।**

**Final production BOM = exact datasheet + actual mains + actual
fan/motor load + safety design।**
