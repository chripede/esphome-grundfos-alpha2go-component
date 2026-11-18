# ESPHome component: Grundfos ALPHA2 GO (BLE)

English translation of [CDank/esphome-grundfos-alpha2go-component](https://github.com/CDank/esphome-grundfos-alpha2go-component). Code, Home Assistant texts, comments and docs translated; protocol logic unchanged.

External ESPHome component `grundfos_alpha2go` for the **ALPHA2 GO**. ESPHome's built-in `alpha3` component doesn't fit:
it requests an object for flow/head (93/289) that only the original ALPHA3 has, and expects the ALPHA3 reply identifiers.

**Status:** Tested on an ALPHA2 GO; reading and writing (controls) work. Every 10 s four replies arrive with no timeouts or CRC errors, and all values appear in Home Assistant.
Compiles with ESPHome 2026.10.2 (ESP-IDF and Arduino). The test pump's firmware version is unknown.
The protocol comes from the Grundfos GO APK and the capture in `Lasse-source/esphome-grundfos-alpha2-go`; the connection sequence is taken from there.

## Setup

```yaml
external_components:
  - source: github://chrissnow/esphome-grundfos-alpha2go-component@main
    components: [grundfos_alpha2go]

esp32_ble_tracker:

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"   # pump MAC (name in scan: "ALPHA2 GO")
    id: pump

sensor:
  - platform: grundfos_alpha2go
    ble_client_id: pump
    flow: {name: "Pump Flow"}
    head: {name: "Pump Head"}
    power: {name: "Pump Power"}
    media_temperature: {name: "Pump Media Temperature"}
```

A full example with every sensor is in [`example/grundfos-alpha2go.yaml`](example/grundfos-alpha2go.yaml).

## Sensors

Readings from the test pump (heating circuit, continuous operation):

| Option | Unit | Object / field | Example |
|---|---|---|---|
| `flow` | m³/h | 93/290 @24 | 1.69 |
| `head` | m | 93/290 @28 | 2.11 |
| `voltage` | V | 87/69 @0 | 223 |
| `dc_voltage` | V | 87/69 @4 | 309 |
| `current` | A | 87/69 @8 | 0.164 |
| `power` | W | 87/69 @12 | 21.6 |
| `speed` | rpm | 87/69 @20 | 3452 |
| `aux_temperature` | °C | 87/69 @32 (electronics) | 62 |
| `starts` | – | 93/1 @0 | 158 |
| `operating_hours` | h | 93/1 @8 | 4858 |
| `power_on_hours` | h | 93/1 @24 | 4858 |
| `media_temperature` | °C | 93/300 @0 | 38.0 |
| `pcb_temperature` | °C | 93/300 @4 | 59.6 |
| `controlbox_temperature` | °C | 93/300 @8 | 50.5 |
| `energy` | kWh | 87/1 @0 (J ÷ 3.6·10⁶) | 77.47 |
| `operation_mode` / `operation_mode_text` | code / text | 86/7 @1 | 0 / "Auto" |
| `control_mode` / `control_mode_text` | code / text | 86/7 @2 | 1 / "Proportional pressure" |
| `setpoint_head` | m | 86/7 @3 (Pa ÷ 9806.65) | 2.50 (proportional pressure) |
| `setpoint_flow` | m³/h | 86/7 @3 (m³/s × 3600) | 1.00 (constant flow) |
| `setpoint_speed` | rpm | 86/7 @3 | 4060 (constant speed) |
| `setpoint_actual` | m | 86/8 @3 (Pa ÷ 9806.65) | 2.09–2.11 (point on the curve) |
| `setpoint_actual_speed` | rpm | 86/8 @3 when realised mode is constant speed | 2455 (external PWM) |
| `control_source` | text | 86/8 @0 | "Panel / app", "External PWM" (123), "AutoNight", "Air venting" |
| `setpoint` | raw SI | 86/7 @3 | 24 509.8 Pa / 0.000278 m³/s |
| `night_setback_enabled` | binary | 90/8 @0 | off (as in the app) |
| `night_mode` | binary | 90/7 @0 (currently in night mode) | off |
| `air_detection_enabled` | binary | 88/600 @7 | on (as in the app) |
| `air_detections` | count | 88/604 @1 (detections after 168 operating hours) | 0 |
| `venting_active` / `venting_status` | binary / text | 88/612 @1 and 86/8 @0 | "Inactive", "Pump only", "System and pump (manual/automatic)" |
| `connected` | binary (connectivity) | – | Connected once the link is encrypted and the pump is being polled; Disconnected after a drop. Useful because HA switches keep their last state after a disconnect |

The settings (90/8, 88/600, 88/604) are only read on every 6th poll (about once a minute at 10 s) and right after connecting. Night mode and venting are read on every poll.

`energy` is the pump's own counter (like "Accumulated power consumption" in the app), not a value integrated from power.

**Setpoint:** The pump reports it in SI units, depending on the control mode. Because an HA sensor needs a fixed unit there are separate sensors:
`setpoint_head` applies to pressure modes (constant/proportional pressure, constant/proportional differential pressure, AutoAdapt), `setpoint_flow` to constant flow and FlowAdapt, `setpoint_speed` to constant speed. The others are then "unknown" (NaN).
`setpoint_actual` is the instantaneous target the pump is regulating to: under proportional pressure, the point on the curve at the current flow, close to the actual head.
Verified: proportional pressure (Pa), both AutoAdapt variants (Pa), constant flow (m³/s) and constant speed (rpm); Pa is assumed for constant pressure without AutoAdapt.
In the GO app AutoAdapt is switched on inside "Prop. pressure" or "Constant pressure". The pump reports it as its own control mode: 13 = proportional pressure with AutoAdapt (on HK1: 41 138 Pa = 4.19 m, actual 3.2 m), 14 = constant pressure with AutoAdapt (29 412 Pa = 3.00 m, actual 2.99 m). The profile names "Radiator"/"Underfloor" refer to these two curve types.
Under AutoAdapt 86/8 reports the same value as 86/7, so `setpoint_actual` shows the setpoint there, not the instantaneous point on the curve.

**Stop** (operation mode 1): 86/6 and 86/7 keep the control mode and setpoint of the last setting; 86/8 reports constant speed at 0. Flow, head, power and speed go to 0; in the operation history (93/1) operating time stops counting but power-on time continues.

Codes for `operation_mode`: 0 = Auto, 1 = Stop, 2 = Min, 3 = Max, 4 = User defined, 7 = Hand, 8 = Standby.
Codes for `control_mode`: 0 = Constant pressure, 1 = Proportional pressure, 2 = Constant speed, 8 = Constant flow, 13 = Proportional pressure (AutoAdapt), 14 = Constant pressure (AutoAdapt); other codes from the profile (5, 10, 15 …) aren't offered for the ALPHA2 GO by the GO app.
The `_text` sensors give the same as English text. The log also shows 86/6 (local request) and 86/8 for comparison.

**Home Assistant categories** (default, overridable per entity with `entity_category:`):
- **Sensors:** flow, head, power, speed, media temperature, energy, operation and control mode as text, night mode, venting active/status.
- **Diagnostic:** voltages, current, electronics temperatures, counters (starts, hours, air detections), numeric codes and raw value, setpoint sensors, current target, night setback / air detection enabled.
- **Control:** pump switch.
- **Config:** control mode, setpoints, night setback, air detection, start/stop system venting.

Only what is configured is requested. Replies are identified by object type, not version, so it doesn't matter whether the pump reports type 256 as v1, v2 or v3.
On a connection drop all sensors go to `NaN`.

## Getting started

1. Adapt `example/grundfos-alpha2go.yaml` (`pump_mac`, Wi-Fi).
2. Flash. **On the first connection press the Bluetooth button on the pump**, otherwise you get `auth fail reason=82`. The bond is then stored in the ESP32's flash and survives restarts and OTA updates: on the test pump the ESP reconnected after several OTA updates without another button press.
   **Reconnecting:** After a disconnect the ESP's BLE stack can lose the bond from RAM even though it's still in flash. The ESP then starts a new pairing, which the pump rejects with `reason=82`, repeating until the next reboot. The component detects this (reason 82, bond present in flash) and restarts the BLE stack (`Restarting BLE stack to reload bonds from NVS`, at most every 30 s), after which it reconnects without a button press. The restart affects all BLE functions on this ESP: connections to other pumps are dropped for about 8 s and then re-established automatically (all pumps' bonds are reloaded). The 30 s lockout is shared by all pumps. Other components (Wi-Fi, UART …) keep running.
3. Close the GO app on your phone. The pump accepts only one client.

Per-connection sequence: encryption → write CCCD → app's initial query → queries one after another (one at a time, 30 ms gap, 1 s timeout).

Multiple pumps: a separate `ble_client` and `grundfos_alpha2go` block per pump; pair each pump individually with its Bluetooth button.

## Protocol: field layout (verified on hardware)

All values big endian, floats IEEE-754. `NaN` means the pump doesn't supply that field.
Field names come from a GENI device profile included as a test file in the Grundfos GO app (`test_geni_profile_30_4_1.xml`); objects 87/1, 87/69, 93/1, 93/290 and 93/300 match the ALPHA2 GO in ID and type.

**93/290, type 565 v2, 36 bytes**

| Offset | Content | ALPHA2 GO |
|---|---|---|
| @0 | Flow in m³/s | ✅ (×3600 = @24) |
| @4 | Pump pressure in Pa | ✅ (÷9806.65 = @28) |
| @8 … @20 | Inlet/outlet pressure, media/differential temperature | `NaN` |
| @24 | Flow in m³/h | ✅ → `flow` |
| @28 | Head in m | ✅ → `head` |
| @32 | kv value | ✅ (log only) |

**87/69, type 256 v3, 41 bytes**

| Offset | Content | ALPHA2 GO |
|---|---|---|
| @0 / @4 | Mains voltage / DC link voltage | ✅ |
| @8 / @12 | Current / power | ✅ |
| @16 | DC power | ✅ (≈ @12, log only) |
| @20 | Speed | ✅ |
| @24 / @28 | Converter / inverter temperature | `NaN` |
| @32 | Electronics temperature | ✅ |
| @36 | PWM | not decoded |

**93/1, type 248 v2, 28 bytes**

| Offset | Content | Example |
|---|---|---|
| @0 u32 | Total starts | 158 |
| @4 / @6 u16 | Starts last hour / last 24 h | 0 / 0 |
| @8 u32 | Total operating time (s) | 4858 h |
| @12 u32 | Operating time last 24 h (s) | 23.6 h |
| @16 u32 | Run time since last start (s) | 27.4 days |
| @20 u32 | Number of power-ons | 157 |
| @24 u32 | Total power-on time (s) | 4858 h |

Only @0, @8 and @24 are available as sensors; the other fields appear in the VERBOSE log.

**93/300, type 534 v2, 13 bytes** (not 16)

| Offset | Content | ALPHA2 GO |
|---|---|---|
| @0 | Media temperature | ✅ |
| @4 | PCB temperature | ✅ |
| @8 | Control box temperature | ✅ |
| @12 (1 byte) | `steady_state` (bool) | ✅ |

**87/1, type 232 v1 `OverallEnergyConsumption`, 26 bytes (for one pump)**

| Offset | Content | ALPHA2 GO |
|---|---|---|
| @0 | Total energy, double in J | ✅ 278 880 350 J = 77.47 kWh (+220 J per 10 s at 22 W) |
| @8 | Total power, float in W | ✅ 22.04 |
| @12 | Number of pumps, u16 | ✅ 1 |
| @14 | Per pump: power (float, W), energy (double, J) | ✅ same as @8 / @0 |

**86/6, 86/7, 86/8, type 303 v1 `OperationStatusRequest`, 7 bytes**

| Offset | Content | ALPHA2 GO |
|---|---|---|
| @0 | `control_source` (0 = unused, 1 = panel, 2 = bus, 104 = AutoNight, …) | ✅ 86/6: 0, 86/7 and 86/8: 1 |
| @1 | `operation_mode` | ✅ 0 = Auto |
| @2 | `control_mode` | ✅ 1 = proportional pressure, 8 = constant flow |
| @3 | `set_point`, float | ✅ Pa, m³/s or rpm depending on control mode |

86/6 is the local request (app/button), 86/7 the prioritised request after weighing local/remote/alarm, 86/8 the request actually realised.
Under proportional pressure 86/6 and 86/7 hold the configured setpoint (24 509.8 Pa = 2.50 m), 86/8 the instantaneous point on the curve (≈ 20 500 Pa = 2.09 m, varies with flow). Under constant flow all three are equal.

**Settings objects** (verified on the ALPHA2 GO; values on HK1: 88/600 = 50 / 1000 / 150 / 5 / enabled, matching the app)

| Object | Type | Content |
|---|---|---|
| 90/8 | 333 `AutoNightEnable` | @0 bool: night setback enabled |
| 90/7 | 324 `AutoNightStatus` | @0 bool: currently in night mode |
| 88/600 | 600 `AirDetectionConfiguration`, 8 bytes | @0 u16 speed change, @2 u16 pressure change, @4 u16 sensitivity, @6 u8 ignore time after start, @7 bool enabled |
| 88/604 | 984 `AirDetectCounterStorage`, 2 bytes | @0 u8 detections in first 24 operating hours, @1 u8 after 168 operating hours |
| 88/612 | 971 `SystemAirVentingCommandAndStatus`, 5 bytes | @0 command (0 stop, 1 start manual), @1 state (0 inactive, 1 manual, 2 automatic), @2 u16 progress, @4 phase (0 idle, 1–4 phase 0–3) |

"Vent pump only" (10 min) does **not** appear in 88/612 but in 86/8 as control source 111 (AirVenting) at constant speed 5225 rpm; 86/6 and 86/7 stay unchanged. 88/612 belongs to "Vent system and pump". `venting_active`/`venting_status` evaluate both.

## Troubleshooting

With `logger` at `VERBOSE` for `grundfos_alpha2go` the component logs every reply with type, version and length, plus all raw fields (floats from 565 and 256, hex dump of 248 and 534, decoded values of 232). For 87/1, 86/6–8, 90/7–8 and 88/600–612 the raw notifications (`raw RX`) are also shown, including replies the parser discards. 86/6–8, 90/7–8 and 88/600–612 are already logged decoded at `DEBUG`, and timeouts with the object number (`Reply timeout (90/8)`).

```yaml
logger:
  level: VERBOSE
  initial_level: DEBUG
  runtime_tag_levels: true
  logs:
    grundfos_alpha2go: VERBOSE
    esp32_ble: INFO
    esp32_ble_client: INFO
    ble_client: INFO
```

For connection problems the component logs every disconnect with its reason (`Disconnected (reason 0x08 …)`: 0x08 radio timeout, 0x13 disconnected by the pump, 0x16 disconnected by the ESP), every encryption with key status, and the number of bonds in flash. Messages from the BLE stack itself appear with tags like `BT_APPL`, `BT_BTC` or `BT_SMP`; lines such as `remove bond` or `keep bond after pairing fail` show when the stack drops a bond.

## Tests

PC unit tests for CRC, request frames, reply identifiers and reassembly of fragmented notifications. The flow/head check uses real values from the hardware log.

```bash
g++ -std=c++17 -I components/grundfos_alpha2go tests/test_geni.cpp -o t && ./t
```

With MSVC (Developer Command Prompt):

```bat
cl /std:c++17 /EHsc /I components\grundfos_alpha2go tests\test_geni.cpp /Fe:t.exe && t.exe
```

## Controls (write access)

The controls write over the same BLE connection. The displayed state always comes from the pump: after every write the component immediately reads back operation status, night setback, air detection and venting. Changes made in the GO app or on the pump show up by the next poll cycle at the latest. The ESP writes nothing at boot. While disconnected from the pump the component reports "no state" for all entities: sensors, selects and numbers show "unknown" in Home Assistant. Switches keep showing their last state because HA's ESPHome integration currently ignores that report for switches; use the `connected` binary sensor to tell whether they're current. The ESPHome API has no per-entity "unavailable".

```yaml
switch:
  - platform: grundfos_alpha2go
    grundfos_alpha2go_id: pump_go    # id of the component (sensor: platform: grundfos_alpha2go)
    pump:
      name: "Pump"
    night_setback:
      name: "Pump Night Setback"
    air_detection:
      name: "Pump Air Detection"

select:
  - platform: grundfos_alpha2go
    control_mode:
      name: "Pump Control Mode"

number:
  - platform: grundfos_alpha2go
    setpoint_head:                   # proportional / constant pressure
      name: "Pump Setpoint Head"
      mode: box
    setpoint_flow:                   # constant flow
      name: "Pump Setpoint Flow"
    setpoint_speed:                  # constant speed
      name: "Pump Setpoint Speed"

button:
  - platform: grundfos_alpha2go
    venting_start:
      name: "Pump Start System Venting"
    venting_stop:
      name: "Pump Stop System Venting"
```

`grundfos_alpha2go_id` can be omitted if there is only one pump.

| Entity | Effect | Telegram | Verified on HK1 |
|---|---|---|---|
| `switch.pump` | On = operation mode Auto, Off = Stop. State from 86/6. | Class 3 START (6) / STOP (5) | ✅ |
| `switch.night_setback` | Night setback (AutoNight) on/off | SET 90/8 | ✅ |
| `switch.air_detection` | Continuous air detection on/off. The other 88/600 values are written back unchanged. | SET 88/600 (25 bytes) | ✅ |
| `select.control_mode` | Proportional pressure, constant pressure, both with AutoAdapt, constant flow, constant speed | SET 86/6 | ✅ all six |
| `number.setpoint_head` | Setpoint in m, proportional and constant pressure only | SET 86/6 | ✅ |
| `number.setpoint_flow` | Setpoint in m³/h, constant flow only | SET 86/6 | ✅ |
| `number.setpoint_speed` | Setpoint in rpm, constant speed only | SET 86/6 | ✅ |
| `button.venting_start` / `venting_stop` | Manually start / abort system venting | SET 88/612 | ✅ |

Notes:
- **Setpoints:** Only the number matching the active control mode applies. The others show "unknown" and input there is discarded with a warning in the log. Under AutoAdapt the pump sets the setpoint itself: `setpoint_head` displays it but can't be changed.
- **Limits:** The default limits (0.5–6 m, 0.1–3.5 m³/h, 1000–5500 rpm) are defaults and depend on the pump type. Adjust with `min_value`, `max_value` and `step`.
- **Changing control mode:** If the unit changes, the last read setpoint in that unit is sent along. If there isn't one yet, defaults apply: 2 m, 1 m³/h or 3000 rpm. Switching between proportional and constant pressure keeps the setpoint. Leaving AutoAdapt uses our own last read pressure setpoint, not AutoAdapt's.
- **Rejected commands:** If the pump rejects a command, `rejected by pump` with the status code appears in the log, and the entity snaps back to the real state on read-back.
- STOP/START and all SETs change the **local** request (86/6), exactly like the GO app. The app can change everything back normally afterwards.

### Write protocol (determined on HK1)

- **Class 3 (commands):** `27 05 E7 F8 03 81 <cmd> CRC`. The reply `24 04 F8 E7 03 00 CRC` means OK. The ALPHA2 GO doesn't know the profile's NIGHT_REDUCT_E/D commands (66/67) and replies `03 81 <cmd>` = ACK 10 (unknown ID).
- **Class 10 SET:** `27 LE E7 F8 0A <0x80|n> <ID> <SubHi> <SubLo> <TypeHi> <TypeLo> <Ver> 00 00 <DataLen> <Data> CRC`. That's the object header from GET replies without the leading status byte. The reply `0A 01 <status>` carries a status code as its first data byte (`GeniClass10StatusCode` in the app):

| Request | Status |
|---|---|
| SET with object header without status byte | `00` = OK |
| GET/SET on an unknown object (90/9999) | `04` = object not found |
| SET with a leading `00` before the type, or with no object header | `04` |
| OS bits 01 (`0x40`) | `01` = unknown operation |
| OS bits 11 (INFO, `0xC3`) | APDU ACK 11 = operation not allowed (`0A C1 5A`) |

- **Long frames:** A BLE write holds 20 bytes. Longer frames (86/6: 24 bytes, 88/612: 22 bytes, 88/600: 25 bytes) are sent in 20-byte pieces; the pump reassembles them.
- **Diagnostics:** `id(pump_go).send_raw_class10({0x03, 0x5A, 0x00, 0x08});` sends any Class 10 APDU, e.g. from a template button. The reply is checked and logged.

## Open questions

- 86/9 (`temporary_operation_control_obj`) contains at rest source 0, Auto, constant speed, 5225 rpm — exactly the pump-venting speed. Probably the preset for "vent pump only"; not yet written.
- "Vent pump only" can't be started. No object for it can be found in the app; the function is only visible as control source 111 in 86/8.
- Untested: other firmware versions, other ALPHA2 GO variants.

Licence: MIT. Independent project, not affiliated with Grundfos.
