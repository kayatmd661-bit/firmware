# JARVIS AC FAN + WATER CONTROLLER — PRODUCTION-READY FIRMWARE

## Source
`JARVIS_AC_FAN_WATER_CONTROLLER_PRODUCTION_READY.ino`

This build preserves the existing 4-fan AC phase-angle architecture, MQTT topics, Home Assistant discovery, dashboard, sensors, water-level logic, Preferences/NVS, OTA, watchdog, and WiFiManager workflow. Only required reliability/safety hardening was added.

## Production-hardening changes

1. **Fan OFF state is now persistent.** An OFF command writes speed `0` to NVS instead of leaving the previous ON speed stored.
2. **MQTT reconnect no longer restores outputs.** Saved outputs are restored only during boot. Reconnecting MQTT cannot unexpectedly restart a fan that was turned OFF.
3. **Water motor is safe on reboot.** The previous motor state remains stored in NVS, but the physical motor starts OFF after reboot. Normal water-level logic must re-authorize it.
4. **WiFiManager custom parameters are persistent objects.** This fixes the non-blocking portal lifecycle so MQTT/HA fields are captured when the user actually submits the form.
5. **MQTT/HA ports are validated.** Invalid port values are constrained to `1..65535`.
6. **Tank distance configuration is normalized.** Full distance is kept below empty distance.
7. **Motor thresholds are normalized.** Start/stop percentages are constrained and ordered safely.
8. **Dashboard/API authentication added.** Browser/API access uses HTTP Basic authentication with username `admin` and the configured MQTT password. Do not leave the default MQTT password in production.
9. **OTA authentication added.** ArduinoOTA uses the configured MQTT password as its OTA password. A newly changed MQTT password takes effect for OTA after reboot because ArduinoOTA password configuration is applied before `ArduinoOTA.begin()`.
10. **Default-password warning added.** Serial output warns if the default MQTT password `12345678` is still active.
11. **Arduino-ESP32 3.x ISR declaration warning is fixed.** ISR prototypes are declared without `IRAM_ATTR`; only the definitions use `IRAM_ATTR`.

## Output-state behavior

### Fans
Fan speed/state is stored in NVS. Example:

- FAN1 = 60%
- FAN2 = OFF
- FAN3 = 80%
- FAN4 = OFF

After power loss and reboot, those logical fan states are restored. The phase-angle engine only fires the TRIAC when a valid zero-cross event is received.

### Water motor
The last motor state is stored, but the physical motor is forced OFF at boot. This prevents an unexpected pump start after a reboot. The ultrasonic water-level task can subsequently turn it ON/OFF according to the configured thresholds.

## Dashboard authentication

Open:

`http://<ESP32-IP>/`

Credentials:

- Username: `admin`
- Password: the configured MQTT password

The same protection applies to `/api/status`, `/api/fan`, and `/save`.

## First-time WiFiManager setup

Connect to the `Jarvis_AP` access point when the device cannot connect to its saved Wi-Fi network. Configure:

- Wi-Fi SSID/password
- MQTT host/port/user/password
- Home Assistant host/port/name/device ID

The MQTT/HA custom fields are stored through Preferences/NVS.

## AC fan architecture

```text
AC MAINS
   |
   +---- isolated zero-cross detector ----> ESP32 ZC GPIO
   |
   +---- power TRIAC <---- random-phase optotriac <---- ESP32 TRIAC GPIO
                                      |
                                     FAN
```

The ESP32 must only touch the isolated low-voltage side.

### Zero-cross
Supported firmware profiles:

- AC-OPTO / RISING
- AC-OPTO / FALLING
- MODULE / RISING
- MODULE / FALLING
- PC817 AC DETECTOR / RISING
- PC817 AC DETECTOR / FALLING

H11AA1-class AC-input optocouplers are a suitable common reference. A bare PC817 must not be connected directly to mains AC; it requires an appropriate isolated AC/rectifier/current-limiting front end.

### TRIAC driver
Use a **random-phase** optotriac driver for phase-angle control, such as MOC302x/MOC305x/VOT8121-class devices where the exact part is suitable for the selected power TRIAC and gate requirements.

Do **not** replace it with a zero-cross-only driver such as the MOC306x family for arbitrary phase-angle speed control.

### Power stage
The actual power TRIAC, gate resistor, snubber, MOV, fuse, thermal design, PCB creepage/clearance, enclosure, conductor rating, and mains protection must be selected for the actual mains voltage/frequency and fan current by a qualified person.

## GPIO map

| Function | GPIO |
|---|---:|
| PZEM RX | 16 |
| PZEM TX | 17 |
| FAN1 TRIAC | 13 |
| FAN2 TRIAC | 14 |
| FAN3 TRIAC | 18 |
| FAN4 TRIAC | 19 |
| FAN1 ZC | 23 |
| FAN2 ZC | 34 |
| FAN3 ZC | 35 |
| FAN4 ZC | 36 |
| DHT22 | 27 |
| PIR | 26 |
| Ultrasonic TRIG | 32 |
| Ultrasonic ECHO | 33 |
| Water motor driver | 25 |
| BMP280 SDA | 21 |
| BMP280 SCL | 22 |

GPIO34/35/36 are input-only and do not provide an internal pull-up; the isolated ZC detector output must provide the required external bias.

## Home Assistant / MQTT

The existing MQTT topics and HA MQTT Discovery structure are preserved. The device publishes availability and discovery payloads after MQTT connection and reports fan, sensor, motor, and zero-cross status.

## Production commissioning order

1. Upload and test with **all mains power disconnected**.
2. Verify Wi-FiManager and dashboard.
3. Verify MQTT and HA Discovery.
4. Verify sensor readings.
5. Verify each ZC input using the intended isolated detector.
6. Verify each TRIAC trigger stage at low-risk test conditions.
7. Commission **one fan channel at a time** with appropriate mains protection.
8. Confirm minimum/maximum speed behavior and ZC health.
9. Test reboot/power-loss behavior.
10. Only then enable all four channels.

## Important limitation

“Production-ready firmware” does **not** mean the mains hardware is automatically safe. The software cannot certify the mains detector, optotriac, power TRIAC, fuse, MOV, snubber, PCB layout, isolation distance, thermal design, or enclosure. Those must be physically verified for the actual installation.

## Toolchain basis

The firmware targets Arduino-ESP32 3.x and uses the current 3.x timer API. ESP32 Preferences/NVS is appropriate for small persistent configuration/state values and survives restart/power loss. Arduino-ESP32 WebServer supports Basic/Digest authentication, and ArduinoOTA supports password authentication.
