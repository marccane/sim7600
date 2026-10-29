// Phase 2 — sleep/wake skeleton
//
// One complete tracker cycle, then deep sleep:
//   wake (RTC timer) → read battery → power modem ON → network → GPS fix
//   → MQTT publish → power modem OFF → deep sleep
//
// No modem.restart(): the modem is powered up via PWRKEY at cycle start
// and powered down via AT+CPOF at cycle end, so between cycles both the
// ESP32 (deep sleep, ~µA) and the modem (power off, ~µA) draw almost nothing.
//
// Libraries: TinyGSM, PubSubClient

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <BluetoothSerial.h>

// On-battery debug: mirror logs over Bluetooth SPP ("bike-01-debug")
BluetoothSerial btLog;

void logln(const String &s) { Serial.println(s); btLog.println(s); }
void logf(const char *fmt, ...) {
  char buf[256];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf); btLog.print(buf);
}

// ── Pins (LilyGO T-SIM7600G-H, per LilyGo-Modem-Series utilities.h) ──
#define PWRKEY_PIN     4    // NPN driver: HIGH = PWRKEY low (≥100ms on, ≥2.5s off)
#define MODEM_TX_PIN  27    // ESP32 TX → modem RXD (via DIP switch 1)
#define MODEM_RX_PIN  26    // ESP32 RX ← modem TXD (via DIP switch 2)
#define DTR_PIN       32    // hold LOW so modem stays awake (not in UART sleep)
#define STATUS_PIN    34    // modem STATUS via level shifter powered by the modem's
                            // 1.8V rail: HIGH = on, FLOATING (not LOW) when off
#define BAT_ADC_PIN   35    // VBAT through 100K/100K divider → ×2
#define SOLAR_ADC_PIN 36    // solar input through 100K/100K divider → ×2
#define LED_PIN       12

// ── Network config ───────────────────────────────────────
const char APN[]  = "orangeworld";
const char USER[] = "orange";
const char PASS[] = "orange";

// ── MQTT (bench broker — test.mosquitto.org unreachable from this LAN) ──
const char MQTT_HOST[] = "broker.emqx.io";
const int  MQTT_PORT   = 1883;
const char DEVICE_ID[] = "bike-01";

// ── Tunables (these become backend-configurable in Phase 3) ──
uint32_t reportIntervalS = 1800;   // production value; bench tests override below
#define GPS_TIMEOUT_MS   60000UL   // max wait for a GPS fix
#define CYCLE_DEADLINE_MS 240000UL // hard cap on the whole active window
#define FW_VERSION       "0.2.0"

// Bench test override: 90s cycles. MUST be removed before deployment.
#define BENCH_TEST_INTERVAL_S 90

// DEBUG: never enter deep sleep (board stays awake, serial/BT stay usable).
// Remove before deployment!
#define DEBUG_NO_SLEEP

HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
PubSubClient   mqtt(gsmClient);

uint32_t cycleStart;
bool deadlineExceeded() { return millis() - cycleStart > CYCLE_DEADLINE_MS; }

// ── Battery ──────────────────────────────────────────────
// Li-ion OCV → percent, rough but good enough for alerts.
int battPercent(float v) {
  if (v >= 4.20) return 100;
  if (v >= 4.06) return 90;
  if (v >= 3.98) return 80;
  if (v >= 3.92) return 70;
  if (v >= 3.87) return 60;
  if (v >= 3.82) return 50;
  if (v >= 3.79) return 40;
  if (v >= 3.77) return 30;
  if (v >= 3.74) return 20;
  if (v >= 3.68) return 10;
  if (v >= 3.45) return 5;
  return 0;
}

float readVolts(int pin) {
  return analogReadMilliVolts(pin) * 2.0 / 1000.0;   // 100K/100K divider
}

// ── Modem power control ─────────────────────────────────
bool modemIsOn = false;   // only trust power-off when we saw it come up

bool modemPowerOn() {
  pinMode(DTR_PIN, OUTPUT);
  digitalWrite(DTR_PIN, LOW);          // keep modem awake (no UART sleep)
  pinMode(STATUS_PIN, INPUT);

  // Already running? (ESP32 reset while the modem kept power, DIP 4 auto-on)
  if (modem.testAT(2000)) {
    modemIsOn = true;
    logln("[PWR] modem already on");
    return true;
  }

  // SIM7600: PWRKEY low ≥100 ms powers on, low ≥2.5 s powers off — so a
  // 1 s pulse is always an ON command, never a shutdown.
  for (int attempt = 1; attempt <= 2; attempt++) {
    logf("[PWR] PWRKEY pulse #%d, batt=%.2f V\n", attempt, readVolts(BAT_ADC_PIN));
    digitalWrite(PWRKEY_PIN, HIGH);    // NPN pulls PWRKEY low
    delay(1000);
    digitalWrite(PWRKEY_PIN, LOW);

    uint32_t t0 = millis();
    while (millis() - t0 < 30000) {    // power-on → AT ready takes ~10-20 s
      if (modem.testAT(5000)) {
        modemIsOn = true;
        logf("[PWR] modem up %lus after pulse #%d\n", (millis() - t0) / 1000, attempt);
        return true;
      }
      logf("[PWR] t+%lus status=%d batt=%.2f V\n", (millis() - t0) / 1000,
           digitalRead(STATUS_PIN), readVolts(BAT_ADC_PIN));
    }
  }

  logf("[PWR] modem did not power on\n");
  return false;
}

void modemPowerOff() {
  if (!modemIsOn) {                    // modem never came up — don't poke it
    Serial.println("[PWR] modem never powered on, skipping power-off");
    return;
  }
  modem.gprsDisconnect();
  modem.poweroff();                    // AT+CPOF
  delay(3000);
  modemIsOn = !modem.testAT() ? false : true;
  if (modemIsOn) {                     // fallback: hardware power toggle
    Serial.println("[PWR] CPOF failed, forcing PWRKEY pulse");
    digitalWrite(PWRKEY_PIN, HIGH); delay(1500); digitalWrite(PWRKEY_PIN, LOW);
    delay(5000);
    modemIsOn = modem.testAT();
  }
  Serial.println(modemIsOn ? "[PWR] modem FAILED to power off!" : "[PWR] modem OFF");
}

// ── GPS ──────────────────────────────────────────────────
bool getGpsFix(float &lat, float &lon, float &acc, uint32_t timeoutMs) {
  modem.enableGPS();
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (deadlineExceeded()) break;
    float speed, alt, accuracy;
    int vsat, usat, year, month, day, hour, min, sec;
    if (modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                     &year, &month, &day, &hour, &min, &sec)) {
      acc = accuracy;
      modem.disableGPS();
      return true;
    }
    delay(2000);
  }
  modem.disableGPS();
  return false;
}

// ── One full cycle ───────────────────────────────────────
void setup() {
  Serial.begin(115200);
  btLog.begin("bike-01-debug");  // pair from the PC to watch logs on battery
  delay(500);
  analogReadResolution(12);

  pinMode(PWRKEY_PIN, OUTPUT);
  digitalWrite(PWRKEY_PIN, LOW);       // NPN off → PWRKEY released
  pinMode(LED_PIN, OUTPUT);
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  cycleStart = millis();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  logf("\n[BOOT] wakeup cause %d, build %s %s\n", cause, __DATE__, __TIME__);
#ifdef BENCH_TEST_INTERVAL_S
  reportIntervalS = BENCH_TEST_INTERVAL_S;   // bench override
#endif

  // Battery BEFORE powering the modem (resting voltage, no TX sag)
  float battV = readVolts(BAT_ADC_PIN);
  float solarV = readVolts(SOLAR_ADC_PIN);
  logf("[BAT] %.2f V (%d%%)  solar %.2f V\n", battV, battPercent(battV), solarV);

  const char *reason = "timer";
  const char *mode   = "normal";
  uint32_t sleepS = reportIntervalS;
  bool skipGps = false;
  bool lastGasp = false;

  // DEBUG build: tag the report so bench publishes are recognisable.
#ifdef DEBUG_NO_SLEEP
  logln("[DEBUG] debug build: no deep sleep, reason=debug");
  reason = "debug";
#endif

  // Battery policy: protect the cell, but never die silently.
  // With USB VBUS present the battery is switched off the ADC node (Q7), which
  // then reads ~0.28 V — that's "on USB", not a flat cell.
  if (battV < 1.0) {
    logln("[BAT] no reading (USB powered) — battery policy skipped");
  } else if (battV < 3.30) {
    skipGps = true; lastGasp = true;
    sleepS = 6 * 3600;                          // hibernate between last gasps
    logln("[BAT] CRITICAL — last-gasp report, then 6h sleep");
  } else if (battV < 3.50) {
    skipGps = true;                             // low: report without GPS
    logln("[BAT] LOW — skipping GPS this cycle");
  }

  bool networkOk = false, gpsOk = false, published = false;
  float lat = 0, lon = 0, acc = 0;

  if (modemPowerOn()) {
    if (!modem.init()) logln("[PWR] modem.init() failed (SIM not ready?)");
    logln("[PWR] modem: " + modem.getModemInfo());
    logln("[NET] waiting for network...");
    if (modem.waitForNetwork(min(60000UL, CYCLE_DEADLINE_MS - (millis() - cycleStart)))) {
      networkOk = true;
      logln("[NET] ok, signal: " + String(modem.getSignalQuality()));

      if (modem.gprsConnect(APN, USER, PASS)) {
        logln("[NET] GPRS ok, IP: " + modem.localIP().toString());

        if (!skipGps && !deadlineExceeded()) {
          logln("[GPS] waiting for fix...");
          gpsOk = getGpsFix(lat, lon, acc, GPS_TIMEOUT_MS);
          logln(gpsOk
            ? "[GPS] fix: " + String(lat, 6) + ", " + String(lon, 6) + " (acc " + String(acc, 0) + "m)"
            : "[GPS] no fix (timeout)");
        }

        if (!deadlineExceeded()) {
          mqtt.setServer(MQTT_HOST, MQTT_PORT);
          mqtt.setSocketTimeout(15);
          if (mqtt.connect(DEVICE_ID)) {
            String payload =
              String("{\"lat\":")    + (gpsOk ? String(lat, 6) : "null") +
              ",\"lon\":"            + (gpsOk ? String(lon, 6) : "null") +
              ",\"acc_m\":"          + (gpsOk ? String(acc, 0) : "null") +
              ",\"batt_v\":"         + String(battV, 2) +
              ",\"batt_pct\":"       + String(battPercent(battV)) +
              ",\"solar_v\":"        + String(solarV, 2) +
              ",\"signal\":"         + String(modem.getSignalQuality()) +
              ",\"mode\":\""         + String(mode) + "\"" +
              ",\"reason\":\""       + String(reason) + "\"" +
              ",\"last_gasp\":"      + (lastGasp ? "true" : "false") +
              ",\"fw\":\""           + FW_VERSION + "\"}";
            String topic = String("devices/") + DEVICE_ID + "/telemetry";
            published = mqtt.publish(topic.c_str(), payload.c_str());
            logln(published ? "[MQTT] published: " + payload
                                     : "[MQTT] publish failed");
            mqtt.disconnect();
          } else {
            logln("[MQTT] connect failed, state " + String(mqtt.state()));
          }
        }
      } else {
        logln("[NET] GPRS failed");
      }
    } else {
      logln("[NET] no network");
    }
  } else {
    logln("[PWR] modem did not power on!");
  }

  logf("[CYCLE] net=%d gps=%d pub=%d — sleeping %us\n",
                networkOk, gpsOk, published, sleepS);
  modemPowerOff();

  digitalWrite(LED_PIN, published ? HIGH : LOW);
  delay(published ? 200 : 1200);
  digitalWrite(LED_PIN, LOW);

  logf("[SLEEP] deep sleep for %us\n", sleepS);
  Serial.flush(); btLog.flush(); delay(300);
#ifdef DEBUG_NO_SLEEP
  logf("[DEBUG] sleep SKIPPED (debug mode). Batt now: %.2f V, solar %.2f V\n",
       readVolts(BAT_ADC_PIN), readVolts(SOLAR_ADC_PIN));
  // Debug idle: stream battery readings, keep serial/BT alive, then restart
  // to run the next cycle (stands in for the deep-sleep timer wake).
  uint32_t idleStart = millis();
  while (millis() - idleStart < sleepS * 1000UL) {
    static uint32_t last = 0;
    if (millis() - last > 5000) {
      last = millis();
      logf("[DBG] batt %.2f V | solar %.2f V\n",
           readVolts(BAT_ADC_PIN), readVolts(SOLAR_ADC_PIN));
    }
    delay(200);
  }
  logln("[DEBUG] restarting for next cycle");
  Serial.flush(); btLog.flush(); delay(300);
  esp_restart();
#else
  btLog.end();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepS * 1000000ULL);
  esp_deep_sleep_start();   // never returns; wake re-enters setup()
#endif
}

void loop() {}
