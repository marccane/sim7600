// Phase 1 — network + GPS + MQTT publish
// Evolves sim7600.ino: adds a GPS fix and replaces the HTTP GET with an MQTT publish.
// Libraries needed: TinyGSM, PubSubClient
//
// NOTE: this is a one-shot test (no deep sleep yet — that's Phase 2).
//       modem.restart() is fine here, but will be replaced by PWRKEY power
//       control in the real sleep/wake firmware.

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <PubSubClient.h>

// ── Pins (LilyGO T-SIM7600G-H) ──────────────────────────
#define MODEM_TX     27
#define MODEM_RX     26
#define MODEM_PWRKEY  4
#define MODEM_STATUS 34
#define LED_PIN      12

// ── Network config — change APN to match your SIM card ──
const char APN[]  = "orangeworld";
const char USER[] = "orange";
const char PASS[] = "orange";

// ── MQTT (placeholder — point at your VPS broker in Phase 4) ──
const char MQTT_HOST[] = "test.mosquitto.org";   // public test broker for now
const int  MQTT_PORT   = 1883;                   // 8883 + TLS in Phase 5
const char DEVICE_ID[] = "bike-01";

HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
PubSubClient   mqtt(gsmClient);

// ── Power on the modem via PWRKEY ───────────────────────
void powerOnModem() {
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(3000);
}

// ── Block until a GPS fix or timeout ─────────────────────
bool getGpsFix(float &lat, float &lon, float &acc, uint32_t timeoutMs) {
  modem.enableGPS();
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    float speed, alt, accuracy;
    int vsat, usat, year, month, day, hour, min, sec;
    if (modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                     &year, &month, &day, &hour, &min, &sec)) {
      acc = accuracy;
      return true;
    }
    delay(1000);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  powerOnModem();
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Initializing modem...");
  modem.restart();
  Serial.println("Modem: " + modem.getModemInfo());

  Serial.println("Waiting for network...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println("Network failed — check SIM/antenna");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  Serial.println("Network OK, signal: " + String(modem.getSignalQuality()));

  Serial.println("Connecting GPRS...");
  if (!modem.gprsConnect(APN, USER, PASS)) {
    Serial.println("GPRS failed — check APN");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  Serial.println("GPRS OK, IP: " + modem.localIP().toString());

  // ── GPS fix ────────────────────────────────────────────
  float lat = 0, lon = 0, acc = 0;
  Serial.println("Getting GPS fix (up to 60s)...");
  if (getGpsFix(lat, lon, acc, 60000)) {
    Serial.printf("GPS: %.6f, %.6f (acc %.0fm)\n", lat, lon, acc);
  } else {
    Serial.println("No GPS fix (timeout) — publishing without location");
  }

  // ── MQTT publish ───────────────────────────────────────
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  Serial.println("Connecting MQTT...");
  if (mqtt.connect(DEVICE_ID)) {
    Serial.println("MQTT connected");

    String topic = String("devices/") + DEVICE_ID + "/telemetry";
    String payload = String("{\"lat\":") + String(lat, 6) +
                     String(",\"lon\":") + String(lon, 6) +
                     String(",\"acc_m\":") + String((int)acc) +
                     String(",\"signal\":") + String(modem.getSignalQuality()) +
                     String(",\"fw\":\"0.1.0\"}");

    mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println("Published to " + topic + ":");
    Serial.println(payload);
  } else {
    Serial.println("MQTT failed, state: " + String(mqtt.state()));
  }

  modem.gprsDisconnect();
  Serial.println("Done.");

  // Blink LED to signal success
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void loop() {}
