# Bike Tracker — Design Document

LilyGO T-SIM7600G-H as a hidden, low-power bike locator, controlled from a VPS backend.

## 1. Goals

- Hidden in the bike frame, "set and forget" — no manual on/off.
- Reports location on a schedule; caches GPS fixes in flash when offline and flushes them when back online.
- Remote command & control from a VPS: change config, trigger emergency mode, request a live fix, OTA firmware.
- Low power: ~1 month on the 18650, extended by the board's solar input.

## 2. Hardware reality

| Fact | Implication |
|---|---|
| SIM7600G-H is **4G LTE Cat-4**, not 5G | Fine for tiny payloads; "G" = global bands |
| **No onboard accelerometer/gyroscope** | Motion detection needs an add-on (decided later) |
| **Battery ADC on GPIO35** | Battery voltage monitoring is free |
| **Solar ADC on GPIO36 + solar charging input** | Solar keeps the 18650 topped up when sealed |
| **Flight Mode pin (GPIO25)** | Low-power modem state between wakes |
| Free I2C: SDA 21 / SCL 22 | Reserved for future accelerometer |
| 1500 mAh 18650 | Small; solar + ~1 month target makes this workable |

## 3. Power budget (~1 month target)

1500 mAh / 720 h ≈ **2 mA average budget**. Achievable with the modem fully powered off between wakes (not just slept — `AT+CSCLK=1` still draws ~1–2 mA).

| State | Current |
|---|---|
| ESP32 deep sleep | ~10–150 µA |
| SIM7600 powered off (PWRKEY) | ~0–50 µA |
| Modem active (idle/registered) | ~20–50 mA |
| Modem TX burst | up to 2 A (ms) |
| GPS active | ~30–50 mA |

A single wake cycle (attach + TLS + GPS) costs ~1–2 mAh. At a 30-min report cadence that's ~75 mAh/day → ~20 days, plus solar top-up → comfortably ~1 month. Motion-gating (later) will extend this further.

## 4. Architecture

```
┌─────────────────────────┐         ┌──────────────────────────────┐
│  LilyGO T-SIM7600G-H    │  MQTT   │  VPS                          │
│  (hidden in frame)      │  (TLS)  │  Mosquitto broker (TLS)       │
│  ESP32 ──UART── SIM7600 │ ──────► │  FastAPI app server           │
│   ├─ GPS (via modem)    │         │  SQLite                       │
│   ├─ Battery ADC (35)   │         │  Web dashboard (map)          │
│   └─ Flash (offline q)  │         │  Push alerts (ntfy/Telegram)  │
└─────────────────────────┘         └──────────────────────────────┘
```

**Protocol: MQTT over TLS** (not HTTP) — bidirectional (server can push commands), tiny overhead, QoS + retained messages + Last Will.

## 5. Wake cycle (state machine)

```
DEEP SLEEP (modem OFF, ESP32 deep sleep)
  → wake on RTC timer (or future: motion interrupt)
  → power modem (PWRKEY) + wait for attach
  → try GPS fix (bounded, gps_timeout_s)
  → connect MQTT (TLS)          ← ALWAYS, even without a GPS fix
  → send telemetry + heartbeat + flush offline queue
  → receive commands (config / mode / OTA / locate_now)
  → power off modem
  → deep sleep
```

Two independent, backend-configurable timers:

1. **`report_interval_s`** — normal report cadence (motion-gated later).
2. **`heartbeat_interval_s`** — guaranteed "still alive" ping every N hours, even with no motion and no GPS fix, so a silent device is distinguishable from a dead one.

Every wake is bounded by a hard timeout so a stuck modem can't drain the battery.

## 6. Modes

| Mode | Cadence | Trigger |
|---|---|---|
| Normal | `report_interval_s` | default |
| Emergency/Lost | `emergency_interval_s` (e.g. 60 s) | server command or geofence breach |
| Heartbeat | `heartbeat_interval_s` | no motion/GPS for a long time |

## 7. Features

**In scope:**
- Low-power sleep/wake cycle
- Offline GPS caching to flash + flush on reconnect
- Backend command & control (VPS)
- Emergency/lost mode (increased frequency)
- OTA firmware updates (signed, A/B rollback)
- Remote config changes
- "Still alive" heartbeat
- Battery monitoring + low-battery alert (GPIO35)
- Geofencing (server-side)
- Cell-tower fallback position when GPS fails
- Watchdog + self-recovery
- Time sync (network / `AT+CCLK`) for correct offline timestamps
- Diagnostics ring buffer uploaded with telemetry

**Deferred:**
- Motion detection (accelerometer add-on — decide later)
- Tamper/removal detection (skipped: hidden-in-frame install makes it redundant; motion sensor covers theft)

## 8. MQTT topics

```
devices/{id}/telemetry    ← device → server (location, battery, signal)
devices/{id}/status       ← device → server (LWT: online/offline)
devices/{id}/commands     ← server → device (config, mode, OTA)
devices/{id}/ack          ← device → server (command results)
devices/{id}/ota          ← server → device (firmware chunks)
```

### Telemetry payload

```json
{
  "ts": 1730000000,
  "lat": 40.4168, "lon": -3.7038, "acc_m": 12,
  "batt_v": 3.9, "batt_pct": 82,
  "signal": 22,
  "mode": "normal",
  "reason": "timer | heartbeat | command",
  "queued": 3,
  "fw": "0.1.0"
}
```

### Commands

```json
{ "cmd": "set_config", "config": { ... } }
{ "cmd": "set_mode",   "mode": "emergency" }
{ "cmd": "locate_now" }
{ "cmd": "ota", "url": "...", "sha256": "...", "version": "0.2.0" }
{ "cmd": "reboot" }
```

## 9. Config schema (stored in flash, editable via backend)

```json
{
  "device_id": "bike-01",
  "report_interval_s": 1800,
  "heartbeat_interval_s": 21600,
  "emergency_interval_s": 60,
  "gps_timeout_s": 60,
  "gps_min_accuracy_m": 50,
  "mqtt_host": "mqtt.example.com",
  "mqtt_port": 8883,
  "apn": "orangeworld",
  "apn_user": "orange",
  "apn_pass": "orange",
  "mode": "normal",
  "geofence": { "lat": 0, "lon": 0, "radius_m": 500 },
  "battery_low_v": 3.3,
  "motion_enabled": false
}
```

## 10. Security

- TLS everywhere (MQTT + OTA).
- Per-device client certificate/token (revocable per unit).
- Signed OTA with A/B rollback.
- No plaintext secrets in flash (keys in eFuse where possible).

## 11. Backend (VPS)

- **Mosquitto** — MQTT broker, TLS, per-device ACLs.
- **FastAPI** app server — subscribes to telemetry → SQLite; exposes REST + dashboard; publishes commands; tracks online/offline via LWT.
- **Web dashboard** — single-page map (Leaflet/MapLibre) with history, battery, signal, and buttons (Emergency, Locate now, OTA, Config).
- **Alerts** — ntfy.sh / Telegram push on geofence breach, low battery, device offline.

## 12. Development roadmap

1. **Phase 1 — data path** ✅ **DONE** (tested 2026-10-12): modem init → network (signal 25) → GPRS → GPS fix (2 m accuracy) → MQTT publish to `devices/bike-01/telemetry` via `test.mosquitto.org`. Sketch: `phase1/phase1.ino`. Flash via `/dev/ttyACM0` (CH343 USB-UART — the ESP32 USB-C port, not the modem's).
2. **Phase 2 — sleep/wake skeleton** ✅ **DONE** (2026-11-02): deep sleep with a 30 min RTC timer verified on the device (`phase3-ota/`, branch `production`, v0.4.0). Still open: measure real current draw.

Done ahead of plan (2026-10-29 … 11-02, see `architecture-and-ota.md`): pull-based OTA over cellular with A/B rollback, probation and blacklist; task watchdog; remote logs (RTC ring buffer → `devices/{id}/log`); own Mosquitto on the VPS with per-device password + ACL; GPS with XTRA assistance and remote sky diagnostics.

Remaining work, in order (decided 2026-11-02):

3. **Milestone 1 — command channel + remote config** ✅ **DONE** (2026-11-02, v0.5.0, `tools/set_config.py`; apply, reject and live update verified on the device): retained `devices/{id}/config` read on every wake, validated, persisted in NVS, acknowledged on `devices/{id}/ack`; a VPS-side tool to set it. Also lets the bench switch between 30 min and fast cycles without an OTA.
4. **Milestone 2 — emergency mode** ✅ **DONE** (2026-11-02: 30 s reports, cleared live within a second): on `mode: emergency` the modem, GPS and MQTT stay up and report every `emergency_interval_s`; falls back to slower reports at a battery floor; exits as soon as the config is cleared. Latency to start = up to one `report_interval_s` (modem is off while asleep).
5. **Milestone 3 — data quality + offline queue**: `ts` (GPS / network time), HDOP + satellites instead of PDOP in `acc_m`, offline fix queue in flash with `queued`, cell-tower ID for server-side fallback, modem-echo fix.
6. **Milestone 4 — backend v1**: FastAPI + SQLite ingest, map with track, buttons (Emergency / Locate / Config), ntfy alerts (missed reports, low battery, geofence).
7. **Milestone 5 — security**: MQTT over TLS, HTTPS or signed OTA images, per-device credentials provisioned into NVS.
8. **Milestone 6 — field readiness**: current measurement, solar charging, long-duration run; motion sensor if/when added.

## 13. Decisions log

| Decision | Choice |
|---|---|
| Battery target | ~1 month |
| Motion detection | Decide later (firmware supports it, build timer-only first); re-confirmed 2026-11-02 |
| Next priority (2026-11-02) | Command channel + emergency mode, before backend and security |
| Report cadence during testing | 5 min (`report_interval_s=300` via remote config) until a motion sensor exists; no solar, cells swapped by hand |
| Dev device | The one board stays on the bench: `main` builds OTA to it, branch `production` records the last deployed state. Stable/dev OTA channels once a second board exists |
| Backend | Python + FastAPI + Mosquitto + SQLite |
| Dashboard | Web + push alerts (ntfy/Telegram) |
| Mounting | Sealed/hidden + solar |
| Protocol | MQTT over TLS |
| Tamper detection | Skipped |
