#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>

// Set timeout to 5 seconds and panic/reboot on timeout
#define WDT_TIMEOUT 5

typedef enum { BOOTING, CONNECTING, WORKING, NOT_CONFIGURED, CONNECTION_FAILED, ERROR } Status;

// Relay pins
const int relayPins[] = {32, 33, 25, 26, 27, 14, 12, 13};                        // GPIO pins for the relays
String relayStates[] = {"OFF", "OFF", "OFF", "OFF", "OFF", "OFF", "OFF", "OFF"}; // Stores relay states

const int ledPin = 23; // status LED on GPIO23
unsigned long previousLedMillis = 0;
Status currentStatus = BOOTING; // Current status of the device

// Make server and WiFiManager global to keep lifetime beyond setup stack
WiFiManager wm;
AsyncWebServer server(80);

// Replaces placeholder with LED state value
String processor(const String& var) {
  if (var == "R1") {
    return relayStates[0];
  } else if (var == "R2") {
    return relayStates[1];
  } else if (var == "R3") {
    return relayStates[2];
  } else if (var == "R4") {
    return relayStates[3];
  } else if (var == "R5") {
    return relayStates[4];
  } else if (var == "R6") {
    return relayStates[5];
  } else if (var == "R7") {
    return relayStates[6];
  } else if (var == "R8") {
    return relayStates[7];
  }

  return String();
}

void handle404(AsyncWebServerRequest* request) {
  Serial.println("404 Not Found: " + request->url());
  request->send(404, "text/plain", "404 Not Found");
}

void handleRoot(AsyncWebServerRequest* request) {
  request->send(LittleFS, "/index.html", String(), false, processor);
}

void handleStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["ip"] = WiFi.localIP().toString();
  JsonArray relays = root.createNestedArray("relays");
  for (int i = 0; i < 8; i++) {
    JsonObject relay = relays.createNestedObject();
    relay["id"] = i + 1;
    relay["state"] = relayStates[i];
  }

  String payload;
  serializeJson(doc, payload);

  request->send(200, "application/json", payload);
}

void setRelayState(int idx, bool on) {
  digitalWrite(relayPins[idx], on ? HIGH : LOW);
  relayStates[idx] = on ? "ON" : "OFF";
}

void setAllRelays(bool on) {
  for (int i = 0; i < 8; i++) {
    setRelayState(i, on);
  }
}

bool applyRelayUpdate(JsonVariant update, String& error) {
  if (update["state"].isNull()) {
    error = "expected state on/off";
    return false;
  }

  bool on = update["state"].as<String>() == "on";

  if (update["relay"].isNull() || update["relay"] == "all") {
    setAllRelays(on);
    return true;
  }

  int idx = update["relay"].as<int>() - 1;
  if (idx < 0 || idx >= 8) {
    error = "relay must be 1-8";
    return false;
  }

  setRelayState(idx, on);
  return true;
}

// REST API: set relay state(s). Body: {"state":"on"|"off"} sets all relays at once;
// {"relay":1-8|"all","state":"on"|"off"} sets a single relay.
// [{"relay":1-8,"state":"on"|"off"}, {"relay":1-8,"state":"on"|"off"},...]
void handleSet(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", R"({"result":"error","message":"invalid JSON"})");
    return;
  }

  Serial.println("Set request received: " + String((char*)data, len));

  String error;
  if (doc.is<JsonArray>()) {
    for (JsonVariant update : doc.as<JsonArray>()) {
      if (!applyRelayUpdate(update, error)) {
        request->send(400, "application/json", String("{\"result\":\"error\",\"message\":\"") + error + "\"}");
        return;
      }
    }
  } else {
    if (!applyRelayUpdate(doc.as<JsonObject>(), error)) {
      request->send(400, "application/json", String("{\"result\":\"error\",\"message\":\"") + error + "\"}");
      return;
    }
  }

  request->send(200, "application/json", R"({"result":"ok"})");
}

void setup() {
  currentStatus = BOOTING;

  Serial.begin(115200);
  ArduinoOTA.setHostname("esp32-ota-device"); // Set network device name
  // ArduinoOTA.setPassword("your_secret_password"); // Optional password protection

  // Initialize relay pins and set all relays to OFF
  for (int i = 0; i < 8; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  // Initialize status LED
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // Initialize LittleFS Filesystem (format if the partition has never been written / is corrupted)
  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting LittleFS");
    currentStatus = ERROR;
    return;
  }

  // Optional: Reset saved settings for testing purposes
  // wm.resetSettings();

  // Automatically connects using saved credentials.
  // If connection fails, it starts an AP named "ESP32_Config_AP" with password "12345678"
  currentStatus = CONNECTING;
  bool success = wm.autoConnect("Relay8_Config_AP", "12345678");
  if (!success) {
    Serial.println("Failed to connect or hit timeout.");
    // ESP.restart(); // Optional: restart and try again
    currentStatus = CONNECTION_FAILED;
  } else {
    // If you reach here, you are connected to the local Wi-Fi router
    Serial.println("Connected to Wi-Fi successfully!");
    Serial.print("Local IP Address: ");
    Serial.println(WiFi.localIP());

    // Route for root / web page
    server.on("/", AsyncWebRequestMethod::HTTP_GET, handleRoot);

    // Route to load style.css file
    server.serveStatic("/bootstrap.min.css", LittleFS, "/bootstrap.min.css");
    server.serveStatic("/bootstrap.min.js", LittleFS, "/bootstrap.min.js");

    // REST API: get relay states
    server.on("/status", AsyncWebRequestMethod::HTTP_POST, handleStatus);

    // REST API: set relay states (single relay or all-at-once)
    server.on("/set", AsyncWebRequestMethod::HTTP_POST, [](AsyncWebServerRequest* request) {}, nullptr, handleSet);

    server.onNotFound(handle404); // Handle 404 errors

    // Start server
    server.begin();

    // Initialize OTA now that Wi-Fi is connected
    ArduinoOTA.begin();

    currentStatus = WORKING;
  }
}

void loop() {
  static int ledToggles = 0; // number of toggles done in the current blink burst
  static bool ledPaused = false;

  // Feed the watchdog
  esp_task_wdt_reset();

  ArduinoOTA.handle();

  unsigned long currentMillis = millis();
  // Your main code runs here once connected
  if (currentStatus == BOOTING) {
    // Blink the status LED 2 Hz
    if (currentMillis - previousLedMillis >= 250) { // 2 Hz = 500 ms period, so toggle every 250 ms
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
    }
  } else if (currentStatus == CONNECTING) {
    // Blink the status LED 2 Hz then off for 1 second
    if (ledPaused) {
      // Keep the LED off for 1 second before starting the next burst
      if (currentMillis - previousLedMillis >= 1000) {
        previousLedMillis = currentMillis;
        ledPaused = false;
        ledToggles = 0;
        digitalWrite(ledPin, HIGH); // start next burst
      }
    } else if (currentMillis - previousLedMillis >= 250) { // 2 Hz = 500 ms period, toggle every 250 ms
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
      ledToggles++;

      if (ledToggles >= 4) { // 2 complete blinks, then pause
        digitalWrite(ledPin, LOW);
        ledPaused = true;
      }
    }
  } else if (currentStatus == WORKING) {
    // Turn on the status LED for 100 ms then off for 2 seconds
    if (ledPaused) {
      // Keep the LED off for 1 second before starting the next burst
      if (currentMillis - previousLedMillis >= 1000) {
        previousLedMillis = currentMillis;
        ledPaused = false;
        ledToggles = 0;
        digitalWrite(ledPin, HIGH); // start next burst
      }
    } else if (currentMillis - previousLedMillis >= 100) {
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
      ledToggles++;

      if (ledToggles >= 4) { // 2 complete blinks, then pause
        digitalWrite(ledPin, LOW);
        ledPaused = true;
      }
    }
  } else if (currentStatus == NOT_CONFIGURED) {
    // Blink the status LED 0,5Hz
    if (currentMillis - previousLedMillis >= 2000) { // 0.5 Hz = 2000 ms period
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
    }
  } else if (currentStatus == CONNECTION_FAILED) {
    // Keep LED on for 1 second then blink 250ms
    if (ledPaused) {
      // Keep the LED off for 1 second before starting the next burst
      if (currentMillis - previousLedMillis >= 1000) {
        previousLedMillis = currentMillis;
        ledPaused = false;
        ledToggles = 0;
        digitalWrite(ledPin, HIGH); // start next burst
      }
    } else if (currentMillis - previousLedMillis >= 250) { // 2 Hz = 500 ms period, toggle every 250 ms
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
      ledToggles++;

      if (ledToggles >= 4) { // 2 complete blinks, then pause
        digitalWrite(ledPin, LOW);
        ledPaused = true;
      }
    }
  } else if (currentStatus == ERROR) {
    // Blink LED 1Hz
    if (currentMillis - previousLedMillis >= 1000) { // 1 Hz = 1000 ms period
      previousLedMillis = currentMillis;
      digitalWrite(ledPin, !digitalRead(ledPin)); // Toggle LED state
    }
  }
}