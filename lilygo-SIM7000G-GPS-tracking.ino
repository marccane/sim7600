#include <WiFi.h>

// Replace with your network credentials (SSID and Password)
const char* ssid = "*****";
const char* password = "*****";

// WiFi server on port 80
WiFiServer server(80);

// Define the ADC pin
const int adcPin = 35;

void setup() {
  // Start Serial monitor
  Serial.begin(115200);

  // Connect to Wi-Fi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  
  // Wait until the ESP32 is connected to the WiFi network
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  // Start the server
  server.begin();
}

void loop() {
  // Check if a client has connected
  WiFiClient client = server.available();

  if (client) {
    Serial.println("New Client Connected.");
    
    // While the client is connected, continuously send ADC values
    while (client.connected()) {
      // Read ADC value from pin 35
      int adcValue = analogRead(adcPin);
      
      // Send the ADC value to the client
      client.print("ADC Value: ");
      client.println(adcValue);

      // Also print to Serial monitor
      // Serial.print("ADC Value: ");
      // Serial.println(adcValue);

      // Delay for 1 second
      delay(1000);
    }
    
    // Close the connection when the client disconnects
    client.stop();
    Serial.println("Client Disconnected.");
  }
}

