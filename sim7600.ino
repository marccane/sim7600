// LilyGO SIM7600G-H — HTTP GET request via cellular
// Libraries needed: TinyGSM, ArduinoHttpClient

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>

// ── Pins (LilyGO T-SIM7600G-H) ──────────────────────────
#define MODEM_TX     27
#define MODEM_RX     26
#define MODEM_PWRKEY  4
#define MODEM_STATUS 34
#define LED_PIN      12

// ── Network config — change APN to match your SIM card ──
const char APN[]  = "orangeworld";   // e.g. "hologram", "iot.1nce.net"
const char USER[] = "orange";
const char PASS[] = "orange";

// ── Request target ───────────────────────────────────────
const char HOST[] = "example.com";
const int  PORT   = 80;
const char PATH[] = "/";

HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
HttpClient     http(gsmClient, HOST, PORT);

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // Power on modem
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(3000);

  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Initializing modem...");
  modem.restart();

  String info = modem.getModemInfo();
  Serial.println("Modem: " + info);

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

  // ── GET request ─────────────────────────────────────────
  Serial.println("\nGET " + String(HOST) + String(PATH));
  http.connectionKeepAlive();
  int err = http.get(PATH);
  if (err != 0) {
    Serial.println("Request failed: " + String(err));
    return;
  }

  int status = http.responseStatusCode();
  Serial.println("Status: " + String(status));

  String body = http.responseBody();
  Serial.println("Body:\n" + body);

  http.stop();
  modem.gprsDisconnect();

  // Blink LED to signal success
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void loop() {}
