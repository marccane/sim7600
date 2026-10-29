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

// ── Pins (LilyGO T-SIM7600G-H, per LilyGo-Modem-Series utilities.h) ──
#define PWRKEY_PIN     4    // NPN driver: HIGH 1.2s = PWRKEY pulse (toggle power)
#define MODEM_TX_PIN  27    // ESP32 TX → modem RXD
#define MODEM_RX_PIN  26    // ESP32 RX ← modem TXD
#define DTR_PIN       32    // hold LOW so modem stays awake (not in UART sleep)
#define STATUS_PIN    34    // modem STATUS line (input-only): HIGH = modem on
#define BAT_ADC_PIN   35    // VBAT through 100K/100K divider → ×2
#define SOLAR_ADC_PIN 36    // solar input through 100K/100K divider → ×2
#define LED_PIN       12

// ── Network config ───────────────────────────────────────
const char APN[]  = "orangeworld";
const char USER[] = "orange";
const char PASS[] = "orange";

// ── MQTT (placeholder — VPS broker in Phase 4) ──────────
const char MQTT_HOST[] = "test.mosquitto.org";
const int  MQTT_PORT   = 1883;
const char DEVICE_ID[] = "bike-01";

// ── Tunables (these become backend-configurable in Phase 3) ──
uint32_t reportIntervalS = 1800;   // production value; bench tests override below
#define GPS_TIMEOUT_MS   60000UL   // max wait for a GPS fix
#define CYCLE_DEADLINE_MS 240000UL // hard cap on the whole active window
#define FW_VERSION       "0.2.0"

// Bench test override: 90s cycles. MUST be removed before deployment.
#define BENCH_TEST_INTERVAL_S 90

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
bool modemPowerOn() {
  pinMode(DTR_PIN, OUTPUT);
  digitalWrite(DTR_PIN, LOW);          // keep modem awake (no UART sleep)

  pinMode(PWRKEY_PIN, OUTPUT);
  digitalWrite(PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(PWRKEY_PIN, HIGH);      // NPN pulls PWRKEY low
  delay(1200);                         // SIMCom spec: ≥1s pulse
  digitalWrite(PWRKEY_PIN, LOW);

  // Wait for the AT port (boot takes ~5-10s); re-pulse once if needed
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t t0 = millis();
    while (millis() - t0 < 20000) {
      if (modem.testAT()) return true;
      delay(500);
    }
    Serial.println("[PWR] AT not responding, re-pulsing PWRKEY");
    digitalWrite(PWRKEY_PIN, HIGH); delay(1200); digitalWrite(PWRKEY_PIN, LOW);
  }
  return false;
}

void modemPowerOff() {
  modem.gprsDisconnect();
  bool cpofOk = modem.poweroff();      // AT+CPOF
  delay(3000);

  bool off = !modem.testAT();
  if (!off) {                          // fallback: hardware power toggle
    Serial.println("[PWR] CPOF failed, forcing PWRKEY pulse");
    digitalWrite(PWRKEY_PIN, HIGH); delay(1500); digitalWrite(PWRKEY_PIN, LOW);
    delay(5000);
    off = !modem.testAT();
  }
  Serial.println(off ? "[PWR] modem OFF" : "[PWR] modem FAILED to power off!");
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
  delay(500);
  analogReadResolution(12);
  pinMode(LED_PIN, OUTPUT);
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  cycleStart = millis();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\n[BOOT] wakeup cause %d\n", cause);
#ifdef BENCH_TEST_INTERVAL_S
  reportIntervalS = BENCH_TEST_INTERVAL_S;   // bench override
#endif

  // Battery BEFORE powering the modem (resting voltage, no TX sag)
  float battV = readVolts(BAT_ADC_PIN);
  float solarV = readVolts(SOLAR_ADC_PIN);
  Serial.printf("[BAT] %.2f V (%d%%)  solar %.2f V\n", battV, battPercent(battV), solarV);

  const char *reason = "timer";
  const char *mode   = "normal";
  uint32_t sleepS = reportIntervalS;
  bool skipGps = false;
  bool lastGasp = false;

  // Battery policy: protect the cell, but never die silently
  if (battV < 3.30) {
    skipGps = true; lastGasp = true;
    sleepS = 6 * 3600;                          // hibernate between last gasps
    Serial.println("[BAT] CRITICAL — last-gasp report, then 6h sleep");
  } else if (battV < 3.50) {
    skipGps = true;                             // low: report without GPS
    Serial.println("[BAT] LOW — skipping GPS this cycle");
  }

  bool networkOk = false, gpsOk = false, published = false;
  float lat = 0, lon = 0, acc = 0;

  if (modemPowerOn()) {
    Serial.println("[NET] waiting for network...");
    if (modem.waitForNetwork(min(60000UL, CYCLE_DEADLINE_MS - (millis() - cycleStart)))) {
      networkOk = true;
      Serial.println("[NET] ok, signal: " + String(modem.getSignalQuality()));

      if (modem.gprsConnect(APN, USER, PASS)) {
        Serial.println("[NET] GPRS ok, IP: " + modem.localIP().toString());

        if (!skipGps && !deadlineExceeded()) {
          Serial.println("[GPS] waiting for fix...");
          gpsOk = getGpsFix(lat, lon, acc, GPS_TIMEOUT_MS);
          Serial.println(gpsOk
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
            Serial.println(published ? "[MQTT] published: " + payload
                                     : "[MQTT] publish failed");
            mqtt.disconnect();
          } else {
            Serial.println("[MQTT] connect failed, state " + String(mqtt.state()));
          }
        }
      } else {
        Serial.println("[NET] GPRS failed");
      }
    } else {
      Serial.println("[NET] no network");
    }
  } else {
    Serial.println("[PWR] modem did not power on!");
  }

  Serial.printf("[CYCLE] net=%d gps=%d pub=%d — sleeping %us\n",
                networkOk, gpsOk, published, sleepS);
  modemPowerOff();

  digitalWrite(LED_PIN, published ? HIGH : LOW);
  delay(published ? 200 : 1200);
  digitalWrite(LED_PIN, LOW);

  Serial.printf("[SLEEP] deep sleep for %us\n", sleepS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepS * 1000000ULL);
  esp_deep_sleep_start();   // never returns; wake re-enters setup()
}

void loop() {}
