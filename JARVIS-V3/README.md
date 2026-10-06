# JARVIS AC FAN + WATER CONTROLLER --- FINAL BUILD & WIRING GUIDE

> **Document basis:** exact source scan of
> `JARVIS_AC_FAN_WATER_CONTROLLER_COMPLETE_FIXED.ino` (ESP32 /
> Arduino-ESP32 3.x).
>
> **Safety:** this controller interfaces with hazardous AC mains. The
> ESP32 side must remain isolated from mains. The mains power stage,
> fuse, MOV, snubber, TRIAC, PCB creepage/clearance and enclosure must
> be designed/verified for the actual voltage, frequency and fan current
> by a qualified person.

------------------------------------------------------------------------

## 1. What this firmware is

This firmware is a 4-channel AC ceiling-fan controller with:

-   4 independent **phase-angle TRIAC fan channels**
-   4 independent **isolated zero-cross detector inputs**
-   0--100% logical speed control, with a default minimum of 20%
-   per-fan zero-cross edge profile
-   per-fan zero-cross timing offset from **-2000 to +2000 µs**
-   automatic half-cycle timing measurement
-   DHT22 temperature/humidity
-   BMP280 pressure
-   PIR motion
-   ultrasonic water-level measurement
-   automatic water-motor control
-   PZEM004T v3.x electrical measurements
-   MQTT + Home Assistant MQTT Discovery
-   built-in web dashboard
-   WiFiManager first-time setup
-   Preferences/NVS persistence
-   Arduino OTA
-   task watchdog
-   non-blocking MQTT reconnect

### Important architecture rule

The firmware is **not** a mains dimmer module by itself. The ESP32
drives only the isolated low-voltage side of:

1.  zero-cross detector(s), and
2.  random-phase optotriac driver(s).

The mains power stage remains a separate, properly isolated/high-voltage
section.

------------------------------------------------------------------------

# 2. Hardware architecture

``` text
                         ┌─────────────────────────────┐
                         │          AC MAINS            │
                         │      L / N / PE as used      │
                         └──────────────┬──────────────┘
                                        │
                    ┌───────────────────┴───────────────────┐
                    │                                       │
             ZERO-CROSS SENSE                         FAN POWER PATH
             (ISOLATED ×4)                            (×4 channels)
                    │                                       │
             safe digital output                      Fuse / protection
                    │                                       │
                    ▼                                       ▼
             ESP32 ZC GPIO                           Power TRIAC
          23 / 34 / 35 / 36                              │
                    │                                     ▼
                    │                                  AC FAN
                    │
                    │       ESP32 TRIAC GPIO
                    └────────13 / 14 / 18 / 19────────────┐
                                                         │
                                                  Random-phase
                                                   optotriac
                                                   MOC302x/
                                                   MOC305x/
                                                   equivalent
```

### Separation

**LOW-VOLTAGE / SAFE CONTROL SIDE**

-   ESP32
-   sensors
-   MQTT/Wi-Fi
-   ZC detector transistor/logic output
-   optotriac LED input

**MAINS SIDE**

-   AC line
-   fuse/protection
-   zero-cross detector mains input network
-   random-phase optotriac output
-   power TRIAC
-   fan
-   motor/power wiring

Do not connect an ESP32 GPIO directly to an AC line, fan line, TRIAC
MT1/MT2, or a mains-referenced detector output.

------------------------------------------------------------------------

# 3. Exact ESP32 pin map from the firmware

  -------------------------------------------------------------------------
  Function                          GPIO Direction         Notes
  ---------------- --------------------- ----------------- ----------------
  PZEM RX                             16 UART RX           `Serial2`

  PZEM TX                             17 UART TX           `Serial2`

  Fan 1 TRIAC                         13 OUTPUT            isolated
  trigger                                                  random-phase
                                                           optotriac LED
                                                           input stage

  Fan 2 TRIAC                         14 OUTPUT            same
  trigger                                                  

  Fan 3 TRIAC                         18 OUTPUT            same
  trigger                                                  

  Fan 4 TRIAC                         19 OUTPUT            same
  trigger                                                  

  Fan 1 ZC                            23 INPUT/interrupt   isolated ZC
                                                           output

  Fan 2 ZC                            34 INPUT/interrupt   input-only;
                                                           external
                                                           pull-up/bias
                                                           required

  Fan 3 ZC                            35 INPUT/interrupt   input-only;
                                                           external
                                                           pull-up/bias
                                                           required

  Fan 4 ZC                            36 INPUT/interrupt   input-only;
                                                           external
                                                           pull-up/bias
                                                           required

  DHT22                               27 digital           DHT data

  PIR                                 26 digital input     motion

  Ultrasonic TRIG                     32 output            trigger

  Ultrasonic ECHO                     33 input             echo

  Water motor                         25 OUTPUT            **do not drive
  control                                                  motor directly**

  I2C SDA                             21 I2C               BMP280

  I2C SCL                             22 I2C               BMP280
  -------------------------------------------------------------------------

Espressif documents GPIO34--39 as input-only and without internal
pull-up/pull-down; therefore GPIO34/35/36 require an appropriate
external bias in the detector output circuit.

------------------------------------------------------------------------

# 4. Fan channel wiring --- exact logical path

Each fan uses the same architecture.

## FAN 1

``` text
ESP32 GPIO13
   │
   ▼
series LED resistor / optotriac input
   │
   ▼
RANDOM-PHASE OPTO-TRIAC
   │
   ▼
power TRIAC gate circuit
   │
   ▼
POWER TRIAC
   │
   ├── AC mains line path ── FAN 1 ── return
   │
   └── protection/snubber/MOV/fuse as designed
```

Zero-cross sensing is a separate isolated circuit:

``` text
AC mains
   │
   ▼
isolated AC zero-cross detector
(H11AA1 class / suitable module / PC817 detector front-end)
   │  isolation barrier
   ▼
ESP32-safe digital output
   │
   ▼
GPIO23
```

The same arrangement repeats:

-   FAN2 trigger → GPIO14; ZC → GPIO34
-   FAN3 trigger → GPIO18; ZC → GPIO35
-   FAN4 trigger → GPIO19; ZC → GPIO36

### Critical rule

The zero-cross detector is **not** the TRIAC driver.

-   ZC detector = tells ESP32 when the AC waveform is around a crossing.
-   random-phase optotriac = receives the ESP32 firing command at the
    desired phase angle.
-   power TRIAC = carries the fan current.

------------------------------------------------------------------------

# 5. Zero-cross detector choices supported by the firmware

The firmware provides six selectable profiles.

  Profile             Intended hardware
  ------------------- ----------------------------------------------------
  AC-OPTO / RISING    AC-input optocoupler output, normal polarity
  AC-OPTO / FALLING   AC-input optocoupler output, inverted polarity
  MODULE / RISING     isolated module with ESP32-safe active-high output
  MODULE / FALLING    isolated module with ESP32-safe active-low output
  PC817 / RISING      PC817 + proper isolated AC/rectifier front-end
  PC817 / FALLING     same, inverted output

### Recommended baseline

A strong, common reference design is:

-   **Vishay H11AA1** for isolated AC zero-cross sensing.
-   Low-voltage collector output with a suitable external pull-up.
-   ESP32 reads the transistor output.
-   Select `AC-OPTO / RISING` or `AC-OPTO / FALLING` according to the
    actual output polarity.

H11AA1 is an AC-input phototransistor optocoupler; Vishay specifies
polarity-insensitive AC input and 5000 VRMS isolation for the device
family.

### PC817 warning

A bare PC817 is **not** an AC-line detector.

Do not connect mains directly to the PC817 LED. A PC817-based detector
needs a correctly designed mains-side rectifier/current-limiting network
and the required isolation distances.

### Module warning

Only use a ready-made zero-cross module when its output side is
explicitly safe for ESP32 3.3 V GPIO operation and the module's
mains-side ratings are suitable.

------------------------------------------------------------------------

# 6. TRIAC driver choice

The firmware requires a **random-phase phototriac driver**.

Suitable families include:

-   onsemi MOC3020/MOC3021/MOC3022/MOC3023
-   MOC3051/MOC3052 family
-   Vishay VOT8121 family or an equivalent random-phase phototriac
    driver

The exact part must be selected according to the mains voltage, required
trigger current, power TRIAC gate requirements, isolation, dv/dt and
thermal design.

## DO NOT use

Do **not** substitute a zero-cross phototriac such as:

-   MOC306x family
-   other zero-cross-only phototriac drivers

for phase-angle speed control. A zero-cross driver deliberately waits
for the waveform crossing and therefore cannot provide the arbitrary
firing angle required by this firmware.

------------------------------------------------------------------------

# 7. Power TRIAC stage

The source code does not lock the design to one power TRIAC because the
correct device depends on:

-   mains voltage (120/127/220/230/240 V)
-   fan rated current
-   startup/inrush behavior
-   dv/dt
-   di/dt
-   thermal environment
-   heatsink
-   enclosure
-   required safety certification

Therefore the power TRIAC must be selected from its datasheet for the
real load.

The power stage normally requires, as applicable:

-   appropriately rated fuse
-   surge protection/MOV
-   RC snubber if required by the TRIAC/load design
-   gate resistor network
-   thermal management
-   safe PCB creepage/clearance
-   touch-safe enclosure
-   suitable wire and terminal ratings

**Never infer these values from the ESP32 firmware.**

------------------------------------------------------------------------

# 8. Mains wiring concept

For each fan channel, the final mains topology is conceptually:

``` text
AC LINE
  │
  ├── protection / fuse
  │
  ▼
power TRIAC switching path
  │
  ▼
FAN
  │
  ▼
AC return / neutral
```

The zero-cross detector senses the mains through its own isolated input
network.

The exact resistor values, fuse rating, MOV voltage, snubber values and
TRIAC selection must be calculated from the actual mains specification
and component datasheets. The firmware cannot determine those values.

**Do not prototype the mains side on a breadboard.**

------------------------------------------------------------------------

# 9. Fan phase-angle operation

The firmware assumes:

-   default mains frequency: **50 Hz**
-   nominal half-cycle: **10 ms**
-   timer tick: **20 µs**
-   TRIAC gate pulse: **120 µs**
-   early firing margin: **250 µs**
-   late firing margin: **500 µs**

It also measures the observed half-cycle when valid zero-cross timing is
available.

Logical relationship:

``` text
Speed 100%  → fire near the start of the half-cycle
Speed 50%   → fire around the middle of the usable window
Speed 20%   → fire late in the usable window
Speed 0%    → no gate request
```

This is **phase-angle control**, not DC PWM.

------------------------------------------------------------------------

# 10. Zero-cross profile configuration

The dashboard stores, per fan:

-   detector profile
-   edge polarity
-   timing offset

Offset range:

``` text
-2000 µs ... 0 ... +2000 µs
```

Start with:

``` text
Profile: actual detector polarity
Offset: 0 µs
```

Only tune the offset after observing real ZC timing.

------------------------------------------------------------------------

# 11. Sensors and peripherals

## DHT22

``` text
ESP32 GPIO27 ← DHT22 DATA
ESP32 3.3V   → VCC
ESP32 GND    → GND
```

Use the pull-up arrangement required by the sensor/module you actually
have.

## PIR

``` text
PIR OUT → GPIO26
PIR GND → ESP32 GND
PIR VCC → supply according to module specification
```

Do not assume every PIR module accepts 3.3 V or outputs a 3.3 V-safe
level; verify the exact module.

## Ultrasonic sensor

``` text
TRIG → GPIO32
ECHO → GPIO33
```

**Important:** if the ultrasonic module outputs 5 V on ECHO, do not
connect that directly to an ESP32 GPIO. Use a suitable level
shifter/divider.

## BMP280

``` text
SDA → GPIO21
SCL → GPIO22
VCC/GND → according to module/device rating
I2C address expected by firmware: 0x76
```

## PZEM004T

``` text
PZEM RX ↔ ESP32 GPIO16
PZEM TX ↔ ESP32 GPIO17
Serial2: 9600, 8N1
```

The PZEM's voltage/current measurement wiring is a separate electrical
measurement circuit. Follow the exact PZEM model's safety and wiring
instructions.

## Water motor

``` text
ESP32 GPIO25 → motor switching driver → motor
```

GPIO25 must **never** directly drive a mains pump/motor. Use an
appropriately rated relay/contactor/solid-state stage with the required
suppression and isolation.

------------------------------------------------------------------------

# 12. Water-level logic

Default configuration:

``` text
Tank empty distance = 110 cm
Tank full distance  = 10 cm

Motor start when water <= 15%
Motor stop  when water >= 98%
```

The ultrasonic distance is mapped between the configured empty/full
distances.

If empty and full distance are accidentally equal, firmware forces the
full distance to one centimeter below empty to avoid a divide-by-zero
style mapping condition.

------------------------------------------------------------------------

# 13. MQTT configuration

Default firmware values:

``` text
MQTT host: homeassistant.local
MQTT port: 1883
MQTT user: esp32
MQTT password: 12345678
```

**Change the default password before deployment.**

MQTT is used for:

### Fan command

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

### Fan state

``` text
jarvis/status/sf1
...
jarvis/status/sf4
```

### Fan speed state

``` text
jarvis/status/sf1/speed
...
jarvis/status/sf4/speed
```

### Zero-cross health

``` text
jarvis/status/sf1/zc
...
jarvis/status/sf4/zc
```

Payload:

``` text
OK
FAULT
```

### Water / sensors

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

### Motor

``` text
jarvis/motor/cmd
jarvis/status/motor
```

### Water calibration

``` text
jarvis/settings/empty/set
jarvis/settings/full/set
```

------------------------------------------------------------------------

# 14. Home Assistant MQTT Discovery

The firmware publishes discovery under:

``` text
homeassistant/...
```

It creates:

-   4 fan entities
-   temperature sensor
-   humidity sensor
-   water percentage sensor
-   voltage sensor
-   current sensor
-   power sensor
-   pressure sensor
-   PIR binary sensor
-   water motor switch
-   4 zero-cross health binary sensors

The HA entities are associated with the configured Jarvis device
ID/name.

### Important

The `haHost` and `haPort` fields are stored and shown in configuration,
but the actual entity discovery/control transport in this firmware is
MQTT. The firmware does not use the HA host as a direct REST API
connection.

------------------------------------------------------------------------

# 15. First-time WiFi + MQTT + HA configuration

## Step 1 --- flash the firmware

Use:

-   ESP32 board package: Arduino-ESP32 3.x
-   correct ESP32 board
-   required libraries:
    -   WiFi
    -   WebServer
    -   WiFiManager
    -   PubSubClient
    -   ArduinoOTA
    -   Preferences
    -   Wire
    -   Adafruit BMP280
    -   PZEM004Tv30
    -   DHT
    -   ESP32 watchdog support

The exact library versions should be frozen in your project before
production.

## Step 2 --- first boot

The firmware starts a WiFiManager access point:

``` text
SSID: Jarvis_AP
```

Connect your phone/PC to that AP.

WiFiManager then provides its configuration page.

## Step 3 --- enter WiFi

Select the real 2.4 GHz WiFi network used by the ESP32 and enter its
password.

## Step 4 --- enter MQTT

Enter:

``` text
MQTT Host/IP
MQTT Port
MQTT Username
MQTT Password
```

Typical local MQTT:

``` text
Host: Home Assistant IP or MQTT broker hostname
Port: 1883
```

If using a broker on another machine, use that broker's LAN
address/name, not necessarily the Home Assistant address.

## Step 5 --- enter HA fields

Enter:

``` text
Home Assistant Host/IP
Home Assistant Port
HA Device Name
HA Device ID
```

Again, discovery is actually performed through MQTT in this firmware.

## Step 6 --- save

After the network is connected, the device should appear on the LAN.

Open the ESP32's IP address in a browser:

``` text
http://ESP32_IP/
Dashboard Authentication
├── Username: admin
├── Password: 1234567
├── Login session/cookie
├── /              → login required
├── /api/status    → login required
├── /api/fan      → login required
├── /save          → login required
└── /logout        → logout
```

The built-in dashboard opens.

------------------------------------------------------------------------

# 16. Dashboard access

The firmware has an HTTP dashboard on port 80.

Open:

``` text
http://<ESP32-IP>/
```

The dashboard provides:

-   Fan 1--4 ON/OFF
-   speed control
-   zero-cross health
-   temperature
-   humidity
-   water %
-   motor state
-   voltage
-   current
-   power
-   pressure
-   WiFi RSSI
-   MQTT connection state
-   MQTT configuration
-   HA configuration
-   fan speed limits
-   startup speed
-   tank distances
-   motor thresholds
-   per-fan ZC profile
-   per-fan ZC timing offset

Save settings with **SAVE CONFIGURATION**.

------------------------------------------------------------------------

# 17. Persistent configuration

Preferences/NVS namespace:

``` text
jarvis_sys
```

Stored configuration includes:

-   MQTT host
-   MQTT port
-   MQTT username
-   MQTT password
-   HA host
-   HA port
-   HA name
-   HA device ID
-   fan minimum
-   fan maximum
-   fan startup speed
-   tank empty distance
-   tank full distance
-   motor start %
-   motor stop %
-   ZC profile per fan
-   ZC offset per fan
-   fan speed state
-   motor state

------------------------------------------------------------------------

# 18. OTA

Hostname:

``` text
jarvis-fan-system
```

OTA is initialized after network setup.

Before OTA starts, the firmware requests all fan outputs to stop.

**Production recommendation:** add authenticated OTA credentials and
disable OTA on untrusted networks before field deployment.

------------------------------------------------------------------------

# 19. Watchdog and responsiveness

The firmware uses the ESP32 task watchdog with a 5-second timeout.

The main loop services:

``` text
WiFi task
OTA
WebServer
MQTT
ZC health
sensor task
watchdog
```

The MQTT reconnect path is non-blocking.

The WiFiManager portal is configured for non-blocking operation.

------------------------------------------------------------------------

# 20. What MUST NOT be used

### Never use these as the phase-angle driver

``` text
MOC306x
zero-cross-only phototriac drivers
```

### Never do this

``` text
ESP32 GPIO → mains
ESP32 GPIO → fan
ESP32 GPIO → pump
ESP32 GPIO → power TRIAC gate directly
PC817 LED → mains directly
bare optocoupler → mains without a rated input network
```

### Never assume

-   a random online TRIAC board is isolated
-   a ZC module output is 3.3 V safe
-   a 5 V ultrasonic ECHO is ESP32 safe
-   a relay module is suitable for an inductive pump
-   a TRIAC rated for resistive load is automatically suitable for every
    ceiling fan
-   an optotriac marked "zero cross" can perform phase-angle control

------------------------------------------------------------------------

# 21. Safety checklist before first mains power

1.  Keep the ESP32/control PCB physically separated from the mains
    section.
2.  Verify isolation barrier and creepage/clearance.
3.  Verify fuse/protection.
4.  Verify TRIAC voltage/current/dv/dt/di/dt ratings.
5.  Verify optotriac isolation and trigger current.
6.  Verify the ZC detector input network.
7.  Verify GPIO34/35/36 external pull-up/bias.
8.  Verify the motor driver is independently rated.
9.  Verify every sensor voltage.
10. Test with mains disconnected first.
11. Test ZC logic using an isolated low-voltage test setup where
    possible.
12. Test TRIAC gate pulses before connecting a real fan.
13. Use a properly protected enclosure.
14. Do not work on the energized mains circuit.

------------------------------------------------------------------------

# 22. Recommended commissioning sequence

### Stage A --- ESP32 only

-   Flash firmware.
-   Open Serial Monitor at 115200.
-   Confirm boot.
-   Confirm no TRIAC gate output is active at startup.

### Stage B --- WiFi

-   Connect `Jarvis_AP`.
-   Configure WiFi.
-   Confirm ESP32 obtains an IP.

### Stage C --- Dashboard

-   Open `http://ESP32_IP/`.
-   Confirm status page.
-   Confirm configuration page.

### Stage D --- MQTT

-   Enter broker settings.
-   Confirm MQTT connected.
-   Confirm Home Assistant discovery.

### Stage E --- sensors

Test individually:

-   DHT22
-   BMP280
-   PIR
-   ultrasonic
-   PZEM

### Stage F --- ZC

With the correct isolated detector installed:

-   verify each fan's ZC state becomes `OK`
-   verify half-cycle is near the expected mains timing
-   verify no false edge storm

### Stage G --- TRIAC channel

Test one fan channel at a time.

Start at:

``` text
OFF
→ 100%
→ 80%
→ 60%
→ 40%
→ minimum
```

Observe:

-   smoothness
-   hum/buzz
-   temperature of TRIAC
-   ZC health
-   gate behavior
-   fan startup

Then repeat for channels 2--4.

------------------------------------------------------------------------

# 23. Troubleshooting

## Fan does not respond

Check:

1.  ZC status is `OK`.
2.  Correct ZC edge profile is selected.
3.  Fan speed is above the configured minimum.
4.  Random-phase optotriac is used.
5.  Power TRIAC gate circuit is correct.
6.  Power TRIAC is correctly rated.
7.  Fuse/protection is intact.

## ZC always FAULT

Check:

1.  detector has mains-side input
2.  detector is actually isolated
3.  low-voltage output has correct pull-up
4.  GPIO34/35/36 have external pull-up/bias
5.  correct RISING/FALLING profile
6.  detector output does not exceed ESP32 GPIO voltage

## Fan is noisy at low speed

This is a load/control issue, not necessarily a software failure.
Ceiling-fan motors can behave differently with phase-angle control.

Check:

-   fan compatibility
-   minimum speed
-   firing window
-   ZC timing offset
-   TRIAC choice
-   snubber design
-   EMI/inductive-load behavior

## MQTT not connecting

Check:

-   broker IP/hostname
-   port
-   username/password
-   broker ACL
-   WiFi RSSI
-   broker availability

## Home Assistant entities missing

Check:

-   MQTT integration is working
-   broker accepts `homeassistant/...`
-   discovery is enabled
-   MQTT connection is established
-   unique IDs are not conflicting

------------------------------------------------------------------------

# 24. Source-code audit findings --- important

The source is structurally complete, but the audit found several items
that should be treated as **production checks**, not ignored.

## AUDIT-1 --- WiFiManager custom-parameter lifecycle / save path

`startWiFiManagerPortal()` creates `WiFiManagerParameter` objects as
local variables, registers them with WiFiManager, then returns while the
portal can continue in non-blocking mode.

The code copies their values immediately after `autoConnect()` returns.
In non-blocking portal operation, the user may submit the portal later.

**Status: \[REVIEW REQUIRED\]**

This should be tested/fixed before relying on the WiFiManager page to
save MQTT/HA custom fields after a delayed portal submission.

The dashboard `/save` path is the safer configuration mechanism after
the device is reachable.

## AUDIT-2 --- Dashboard has no authentication

The built-in WebServer exposes:

``` text
/
 /api/status
 /api/fan
 /save
```

on HTTP port 80 without a username/password layer.

**Status: \[SECURITY WARNING\]**

Anyone who can reach the device can potentially change fan
state/configuration.

Use only on a trusted LAN during development, or add authentication
before production.

## AUDIT-3 --- OTA has no authentication in this source

`ArduinoOTA.begin()` is used without an OTA password.

**Status: \[SECURITY WARNING\]**

Add authenticated OTA or restrict OTA to a trusted maintenance network
before deployment.

## AUDIT-4 --- default MQTT password is weak

Source default:

``` text
12345678
```

**Status: \[MUST CHANGE\]**

Change it before deployment.

## AUDIT-5 --- saved motor state can restore ON after reboot

`restoreOutputs()` reads the saved motor state and can set GPIO25 HIGH
at startup.

**Status: \[SAFETY REVIEW REQUIRED\]**

For a water pump, a power restoration event can therefore restore a
previous ON state. Decide whether the production system should always
boot the motor OFF and require the water logic to re-authorize it.

## AUDIT-6 --- saved fan speeds can restore active fans

`restoreOutputs()` restores each saved fan speed.

**Status: \[BEHAVIOR REVIEW REQUIRED\]**

This means a previously active fan can resume after reboot if its saved
speed is non-zero and ZC timing is available.

## AUDIT-7 --- `haHost` / `haPort` are configuration metadata, not a direct HA API path

The firmware's actual HA integration is MQTT discovery/control.

**Status: \[DOCUMENTED\]**

Do not interpret these fields as proof of a direct Home Assistant
REST/WebSocket connection.

## AUDIT-8 --- mains component values are intentionally not hard-coded

The firmware does not specify one universal:

-   fuse
-   MOV
-   snubber
-   power TRIAC
-   mains resistor network

because those values depend on the real mains/load.

**Status: \[CORRECT DESIGN CHOICE\]**

Do not invent universal resistor/fuse values from this README.

------------------------------------------------------------------------

# 25. Manufacturer/component reference

### Zero-cross detector

**Vishay --- H11AA1**

-   AC/polarity-insensitive input
-   phototransistor output
-   intended for isolated AC detection
-   published isolation rating is 5000 VRMS for the cited device family

Official reference: https://www.vishay.com/en/product/83608/

### Random-phase optotriac

**onsemi --- MOC3020/MOC3021/MOC3022/MOC3023 family**

The MOC3023M datasheet identifies the family as a **6-pin DIP
Random-Phase** optocoupler/phototriac driver.

Official reference: https://www.onsemi.com/pdf/datasheet/moc3023m-d.pdf

### ESP32

Use an ESP32 module/board compatible with the pin map in the firmware.

Espressif's documentation confirms GPIO34--39 are input-only and do not
have internal pull-up/pull-down circuitry on the classic ESP32.

Official reference:
https://documentation.espressif.com/esp32_datasheet_en.pdf

------------------------------------------------------------------------

# 26. Final architecture summary

``` text
                         HOME ASSISTANT
                              │
                              │ MQTT
                              ▼
                         MQTT BROKER
                              │
                              ▼
                    ┌───────────────────┐
                    │       ESP32       │
                    │  Jarvis Controller│
                    ├───────────────────┤
                    │ WiFiManager        │
                    │ Dashboard          │
                    │ MQTT               │
                    │ OTA                │
                    │ NVS                │
                    │ Watchdog           │
                    │ ZC timing          │
                    │ Phase-angle engine │
                    └───────┬───────────┘
                            │
             ┌──────────────┼──────────────┐
             │              │              │
         ZC ×4          TRIAC ×4        Sensors
             │              │              │
       isolated AC       random-phase    DHT/PIR/
       detectors         optotriac       BMP/US/PZEM
                            │
                         power TRIAC
                            │
                         FAN ×4

                       WATER CONTROL
                            │
                      GPIO25 driver
                            │
                         MOTOR
```

------------------------------------------------------------------------

# 27. Final verdict

### Firmware architecture

**GOOD / READY FOR CONTROLLED BENCH COMMISSIONING**

The source contains the intended 4-channel ZC-synchronized phase-angle
architecture, MQTT/HA discovery, dashboard, persistence, sensors, water
control, OTA and watchdog.

### Before real mains deployment

The following must be closed:

-   [ ] WiFiManager non-blocking custom-parameter save path
    verified/fixed
-   [ ] dashboard authentication/security decision
-   [ ] OTA authentication/security decision
-   [ ] default MQTT password changed
-   [ ] motor power-on behavior explicitly approved
-   [ ] fan power-on restore behavior explicitly approved
-   [ ] exact mains voltage/frequency confirmed
-   [ ] exact zero-cross circuit verified
-   [ ] exact random-phase optotriac verified
-   [ ] exact power TRIAC verified
-   [ ] fuse/MOV/snubber/thermal/creepage design verified
-   [ ] one-channel mains commissioning completed before four-channel
    operation

This README is an architecture/commissioning guide, not a substitute for
the component datasheets or electrical safety certification of the mains
power stage.
