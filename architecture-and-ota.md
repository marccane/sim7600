# Architecture & OTA Updates

How the tracker firmware (`phase3-ota/`), the server backend and the developer
tooling fit together, and how to ship firmware to a device over the cellular
link. For the product goals and roadmap, see `tracker-design.md`.

All hostnames, credentials and tokens below are placeholders. The real values
live only in `tools/secrets.env`, which is gitignored.

---

## 1. Big picture

```
 ┌───────────────────────────────┐   cellular (LTE/3G)   ┌──────────────────────────────────────┐
 │ LilyGO T-SIM7600 (the device) │                       │ Server (any small VPS)               │
 │                               │  MQTT  :1883          │                                      │
 │  ESP32 ──UART── SIM7600 modem │ ────────────────────► │  Mosquitto  (password + ACL)         │
 │   • wake cycle (deep sleep)   │  telemetry, logs      │     │                                │
 │   • RTC log buffer            │                       │     └─► logger service ─► log file   │
 │   • two OTA app slots         │  HTTP  :8080 (GET)    │                                      │
 │                               │ ◄──────────────────── │  static file server                  │
 │                               │  manifest + image     │   www/fw/<token>/manifest.txt        │
 └───────────────────────────────┘                       │   www/fw/<token>/fw-<build>.bin      │
                                                         └──────────────▲───────────────────────┘
                                                                        │ scp / ssh
                                                         ┌──────────────┴───────────────────────┐
                                                         │ Developer PC                         │
                                                         │  tools/deploy_ota.sh  (build+publish)│
                                                         │  tools/mqtt_listen.py (watch device) │
                                                         └──────────────────────────────────────┘
```

Design choices:

- **The device always initiates.** It sleeps most of the time and can't be reached
  while asleep, so instead of having updates *pushed* to it, it *pulls* them: after
  each report it asks the server whether there's a new build.
- **MQTT for data, HTTP for images.** Telemetry and logs are small messages, which
  suits MQTT. Firmware images are static files that are easy to serve and stream
  over HTTP.
- **The server stays simple.** It's Mosquitto, Python's built-in `http.server` and
  a small Python MQTT logger, with no database and no app server.

---

## 2. The device

### 2.1 Hardware notes that matter for the firmware

| Item | Detail |
|---|---|
| Modem UART | ESP32 RX = GPIO26, TX = GPIO27, 115200 baud (`modemSerial.begin(...)` is required) |
| PWRKEY | GPIO4 via an NPN transistor (GPIO HIGH → PWRKEY low). Low ≥100 ms = **power on**, low ≥2.5 s = **power off** |
| STATUS | GPIO34, through a level shifter powered by the modem. It floats when the modem is off, so don't rely on it |
| DTR | GPIO32, held LOW so the modem doesn't enter UART sleep |
| Battery ADC | GPIO35 via a 100K/100K divider. Reads about 0.28 V whenever USB power is present (the battery is switched off that node) |
| Flash | 4 MB, partition scheme **`min_spiffs`**: two 1.9 MB app slots (OTA) + small SPIFFS |

### 2.2 The wake cycle

Every wake runs the same sequence from `setup()`, then sleeps:

```
boot ─► restore RTC log, arm watchdog, check OTA state (probation? rolled back?)
     ─► read battery (before the modem draws current)
     ─► modem power-on (1 s PWRKEY pulse, up to 2 tries, ~20 s to AT-ready)
     ─► network registration ─► data connection ─► GPS fix (bounded)
     ─► MQTT: publish telemetry ─► confirm new build (if on probation) ─► publish logs
     ─► OTA check: fetch manifest, install new build if needed
     ─► modem power-off (AT+CPOF, PWRKEY fallback)
     ─► reboot into new build  |  deep sleep until next report
```

Safety rails:

- **Cycle deadline:** a hard cap on the active window (OTA has its own timeout).
- **Task watchdog (180 s):** fed between steps and during downloads. A hung cycle
  resets the board.
- **Battery policy:** below 3.5 V the cycle skips GPS. Below 3.3 V it sends a
  last-gasp report and sleeps 6 h. No OTA starts below 3.6 V.

### 2.3 Remote logs

Every log line goes to Serial **and** to a 4 KB buffer in RTC memory
(`RTC_NOINIT_ATTR`). Each line is prefixed with seconds since boot.

- RTC memory survives software resets, panics, watchdog resets and deep sleep.
  It does **not** survive a power-on reset (detected with a magic number).
- After a successful MQTT connect, the buffer is published to `devices/<id>/log`
  in chunks of ≤1000 bytes, split at line ends. The SIM7600 accepts at most
  1500 bytes per `AT+CIPSEND`, and TinyGSM does not split writes. What was sent is
  removed from the buffer.
- Anything logged after the upload (OTA progress, power-off, a crash) stays in the
  buffer and is published by the **next** cycle. That's how you get the logs of a
  cycle that crashed or never reached the network.
- If the buffer fills up, the oldest lines are dropped.

The buffer is a single struct, so it stays at the same RTC address across builds.
That lets a new build upload the old build's last lines, and vice versa.

### 2.4 MQTT topics

| Topic | Direction | Content |
|---|---|---|
| `devices/<id>/telemetry` | device → server | JSON: position, battery, signal, `fw`, `build`, `boot` (reset reason), `ota` (last OTA event) |
| `devices/<id>/log` | device → server | plain-text log chunks |

The device uses its id as both MQTT client id and username. The broker's ACL
(`pattern readwrite devices/%u/#`) confines each device to its own subtree.
A separate backend/bench user may read `devices/#`.

---

## 3. How OTA works

### 3.1 Build identity

Every image published by `tools/deploy_ota.sh` carries a unique **build id**,
compiled in as `FW_BUILD`:

```
<UTC yyyymmdd-hhmmss>-<git short hash>[+]      e.g. 20250101-120000-1a2b3c4+
```

A trailing `+` means the sketch had uncommitted changes. An image built any other
way (Arduino IDE, plain `arduino-cli compile`) gets `FW_BUILD "dev"`. **Dev builds
report normally but never update themselves**, so a firmware you flashed by hand
for testing isn't silently replaced.

### 3.2 The manifest

The server holds one small text file per device fleet:

```
www/fw/<token>/manifest.txt
    version=20250101-120000-1a2b3c4
    file=fw-20250101-120000-1a2b3c4.bin
    size=350384
    md5=0123456789abcdef0123456789abcdef
```

`<token>` is a random directory name (32 hex characters). Images contain the
device's MQTT credentials, so they must not be downloadable from a guessable URL.
Empty `index.html` files in `www/` and `www/fw/` stop the server from listing
directories.

### 3.3 Update flow on the device

After a **successful telemetry publish** (proof that the network path works), and
only for non-dev builds with enough battery:

1. `GET /fw/<token>/manifest.txt`. If `version` equals the running `FW_BUILD`, it
   logs `[OTA] up to date` and stops.
2. If this version has already been rolled back twice, it logs `[OTA] skipping …`
   and stops (see §3.5).
3. Otherwise it downloads `GET /fw/<token>/<file>` and streams it straight into the
   **inactive** app slot with the ESP32 `Update` library. It doesn't wait for the
   whole file; it writes 1 KB at a time as it arrives.
   - `TINY_GSM_RX_BUFFER` is raised to 1024 bytes. At the default 64 bytes,
     downloads crawl.
   - The download is aborted on a 30 s stall or after 7 min in total.
4. `Update.end()` checks the MD5 and that the image is a valid ESP32 app, then
   marks the new slot as the next boot partition.
5. The device records the version as being *tried* (NVS namespace `ota`), powers
   the modem off, waits 15 s for it to finish shutting down, and reboots into the
   new build.

On a bench device, a ~350 KB image downloads in about 40 s (~8.7 KB/s, limited by
the 115200-baud modem UART). Speed depends on signal quality.

### 3.4 Probation and rollback

The ESP-IDF bootloader in the Arduino-ESP32 3.x core is built with app rollback
enabled. The firmware takes control of when to confirm a new image:

```cpp
extern "C" bool verifyRollbackLater() { return true; }   // don't auto-confirm at boot
```

A freshly installed build therefore boots **on probation**
(`ESP_OTA_IMG_PENDING_VERIFY`):

| What happens to the new build | Result |
|---|---|
| Publishes telemetry successfully | `esp_ota_mark_app_valid_cancel_rollback()`; it is now the permanent build (`[OTA] new build confirmed`) |
| Runs a full cycle but can't publish | Calls `esp_ota_mark_app_invalid_rollback_and_reboot()` itself |
| Crashes, hangs (watchdog) or resets before confirming | The bootloader sees an unconfirmed image and boots the previous build |

### 3.5 Avoiding a rollback loop

When the previous build boots again, it notices that the build it *tried* isn't
the one running. It records that version as **bad** and increments a failure
counter, then reports `"ota":"rolled back <build>"` in telemetry. A bad build is
retried **once**, in case the failure was just a network blip. After two failures
it's skipped. Publishing any newer build clears this.

### 3.6 Gotcha: the old build performs the reboot

The reboot into a new build, and the power-off sequence before it, is run by the
**old** firmware. A fix to that code path only takes effect on the update *after*
the one that ships it. Keep this in mind when you check whether such a fix worked.

---

## 4. Setting it up

### 4.1 Developer PC

- `arduino-cli` with the **esp32 core 3.x** (tested with 3.3.7). Its bootloader has
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- Libraries: **TinyGSM**, **PubSubClient**, **ArduinoHttpClient**.
- SSH access (key-based) to the server.
- Copy `tools/secrets.env.example` to `tools/secrets.env` and fill it in.
  `deploy_ota.sh` generates the gitignored `phase3-ota/secrets.h` from it on every
  run.

### 4.2 Server

Any Linux host with a public address works. The device must be able to reach TCP
**1883** (MQTT) and **8080** (HTTP), so open them in any firewall.

**Firmware directory** (as a normal user):

```bash
mkdir -p ~/bike-backend/{www/fw/<token>,logs,tools,mosquitto}
touch ~/bike-backend/www/index.html ~/bike-backend/www/fw/index.html
```

**Mosquitto.** Install the package, then add a listener with auth:

```conf
# /etc/mosquitto/conf.d/bike.conf
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/bike.passwd
acl_file /etc/mosquitto/bike.acl
```

```conf
# /etc/mosquitto/bike.acl
pattern readwrite devices/%u/#
user dev
topic readwrite devices/#
```

```bash
mosquitto_passwd -c -b bike.passwd <device-id> '<device-password>'
mosquitto_passwd    -b bike.passwd dev         '<dev-password>'
sudo install -m 640 -g mosquitto bike.passwd bike.acl /etc/mosquitto/
sudo systemctl restart mosquitto
```

Note: `allow_anonymous false` affects every client of that broker. Check nothing
else relies on anonymous access first.

**Services** (systemd *user* units, so no root is needed to manage them):

```ini
# ~/.config/systemd/user/bike-http.service
[Service]
ExecStart=/usr/bin/python3 -m http.server 8080 --directory %h/bike-backend/www
Restart=always
[Install]
WantedBy=default.target
```

```ini
# ~/.config/systemd/user/bike-mqttlog.service
[Service]
# listener.env (chmod 600): MQTT_HOST=localhost, MQTT_USER, MQTT_PASS, MQTT_TOPIC=devices/#, LOG_FILE=<path>
EnvironmentFile=%h/bike-backend/listener.env
ExecStart=/usr/bin/python3 -u %h/bike-backend/tools/mqtt_listen.py
Restart=always
[Install]
WantedBy=default.target
```

```bash
scp tools/mqtt_listen.py <server>:bike-backend/tools/
systemctl --user daemon-reload
systemctl --user enable --now bike-http bike-mqttlog
sudo loginctl enable-linger <user>      # keep user services running without a login session
```

### 4.3 First flash (USB, once)

The partition table can only be changed over USB. Plug the board's **ESP32** USB
port in (not the modem's) and run:

```bash
tools/deploy_ota.sh --usb /dev/ttyACM0      # or the /dev/serial/by-id/... path
```

This builds the firmware, publishes it to the server **and** flashes the same
image, so the device starts out "up to date". After that, every update can go over
the air.

---

## 5. Shipping an update

```bash
tools/deploy_ota.sh            # build + publish; devices install it on their next cycle
tools/deploy_ota.sh --build    # build only, publish nothing
```

What the script does:

1. Writes `secrets.h` and `build_id.h`, then compiles with `PartitionScheme=min_spiffs`.
   It deletes `build_id.h` afterwards, so later hand builds are `dev`.
2. Uploads `fw-<build>.bin` **first**, then swaps `manifest.txt` in atomically
   (upload as `.tmp`, then `mv`). A device never sees a manifest that points at a
   missing or partly uploaded image.
3. Keeps the three newest images on the server and deletes the rest.

Then watch it land:

```bash
tools/mqtt_listen.py                        # on the PC; reads tools/secrets.env, appends to tracker.log
ssh <server> tail -f bike-backend/logs/tracker.log   # or read the server-side log
```

A healthy update looks like this (across two `log` messages, since the install
lines are uploaded by the next cycle):

```
[OTA] <old> -> <new> (350384 bytes), downloading
[OTA] 88064/350384 bytes, 10s
...
[OTA] installed <new> in 39s (8.7 KB/s), rebooting into it
[BOOT] reset=sw ... build=<new> slot=app1
[OTA] new build on probation until it publishes
[MQTT] published: {... "build":"<new>" ...}
[OTA] new build confirmed
```

The slot alternates between `app0` and `app1` with each update.

**Rolling back on purpose:** publish the older build again with `deploy_ota.sh`.
"Different from what's running" counts as new, so this works for downgrades too.

---

## 6. Troubleshooting

| Symptom (log / telemetry) | Meaning / fix |
|---|---|
| Nothing on `devices/<id>/#` | Check broker reachability (port 1883 open?), credentials in `secrets.env`, and the device's USB serial output |
| `[MQTT] connect failed, state -2` | TCP connect failed: broker down or port 1883 blocked |
| `[MQTT] connect failed, state 4/5` | Bad credentials / not authorised: the device id must exist in the password file |
| `[OTA] manifest fetch failed` | Port 8080 blocked, server down, wrong `OTA_TOKEN`, or the manifest not uploaded |
| `[OTA] image request failed` | The image named in the manifest is missing, or its size doesn't match |
| `[OTA] failed at N/M bytes: …` | Download stalled or MD5 mismatch. The device retries next cycle; nothing was changed |
| `"ota":"rolled back <build>"` | The new build crashed or couldn't publish. Read the `log` topic: its last lines before the rollback are there |
| `[OTA] skipping <build>: rolled back twice` | Blacklisted. Fix the bug and publish a new build |
| Device never updates | It's a `dev` build (flashed by hand): reflash with `deploy_ota.sh --usb` |
| `PWRKEY pulse #2` needed on every boot | The modem was still shutting down. Give it more time after `AT+CPOF` before powering it on |
| Battery reads ~0.28 V | The board is on USB power, not a flat cell |

---

## 7. Security: what this does and doesn't protect

This setup is meant for development. Before deploying devices you don't control
physically:

| Current | Risk | Production fix |
|---|---|---|
| Plain HTTP + MD5 | MD5 catches corruption, not tampering. Anyone who can alter the traffic or the server's files can install firmware | Sign images and verify the signature on the device (ESP32 secure boot v2 / signed app images); serve over TLS |
| Plain MQTT | Credentials and telemetry (including location) cross the network unencrypted | MQTT over TLS (port 8883) |
| MQTT password compiled into the image | Anyone holding an image can impersonate that device | Per-device credentials stored in NVS / flash encryption, not in the shared image |
| Unguessable download path | Security by obscurity only | Authenticated downloads (e.g. a signed short-lived URL) |

Also remove the bench-only settings before deployment: `DEBUG_NO_SLEEP` and
`BENCH_TEST_INTERVAL_S` in `phase3-ota.ino`.
