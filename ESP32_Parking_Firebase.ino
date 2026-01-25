#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h>
#include <map>

// ===== WiFi =====
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// ===== Firebase =====
#define API_KEY ""
#define DATABASE_URL ""

// ===== Ultrasonic Pins =====
#define TRIG_P1 5
#define ECHO_P1 18
#define TRIG_P2 17
#define ECHO_P2 16
#define TRIG_P3 27
#define ECHO_P3 26

#define THRESHOLD_CM 7
#define UPDATE_INTERVAL 3000 // ms
#define READINGS 3           // average 3 readings per sensor

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastUpdate = 0;

// ===== Last known values =====
float lastDistance[3] = {-1, -1, -1};
String lastStatus[3] = {"", "", ""};
int lastAvailable = -1;

// ===== Ultrasonic function with averaging =====
float readDistance(int trigPin, int echoPin) {
  float total = 0;
  int validReadings = 0;

  for (int i = 0; i < READINGS; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, 25000); // 25ms timeout
    if (duration > 0) {
      float d = duration * 0.0343 / 2.0;
      total += d;
      validReadings++;
    }
    delay(10);
  }

  if (validReadings == 0) return -1;
  return total / validReadings;
}

// ===== Time functions =====
void initTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 100000) {
    Serial.print(".");
    delay(500);
    now = time(nullptr);
  }
  Serial.println(" OK");
}

unsigned long getEpochTime() {
  time_t now;
  time(&now);
  return now < 100000 ? 0 : now;
}

// ===== Wi-Fi & Firebase reliability =====
void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost! Reconnecting...");
    WiFi.disconnect();
    WiFi.reconnect();
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500);
      Serial.print(".");
      tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi reconnected!");
    } else {
      Serial.println("\nWiFi reconnect failed, restarting ESP32...");
      ESP.restart();
    }
  }
}

void ensureFirebase() {
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready! Reconnecting...");
    Firebase.reconnectWiFi(true);
    delay(1000);
  }
}

// ===== Firebase helpers =====
bool setFloat(String path, float value) {
  for (int i = 0; i < 3; i++) {
    if (Firebase.RTDB.setFloat(&fbdo, path.c_str(), value)) return true;
    delay(200);
  }
  return false;
}

bool setString(String path, String value) {
  for (int i = 0; i < 3; i++) {
    if (Firebase.RTDB.setString(&fbdo, path.c_str(), value)) return true;
    delay(200);
  }
  return false;
}

bool setInt(String path, int value) {
  for (int i = 0; i < 3; i++) {
    if (Firebase.RTDB.setInt(&fbdo, path.c_str(), value)) return true;
    delay(200);
  }
  return false;
}

// ===== Heartbeat function =====
void sendHeartbeat() {
  ensureWiFi();
  ensureFirebase();

  unsigned long now = getEpochTime();
  if (now == 0) return; // NTP not synced yet

  // Create a small JSON object for the system node
  FirebaseJson json;
  json.set("lastSeen", now);
  json.set("status", "online");

  // Write the whole /system object at once
  if (Firebase.RTDB.setJSON(&fbdo, "/system", &json)) {
    Serial.println("System heartbeat sent");
  } else {
    Serial.printf("System heartbeat failed: %s\n", fbdo.errorReason().c_str());
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_P1, OUTPUT); pinMode(ECHO_P1, INPUT);
  pinMode(TRIG_P2, OUTPUT); pinMode(ECHO_P2, INPUT);
  pinMode(TRIG_P3, OUTPUT); pinMode(ECHO_P3, INPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setAutoReconnect(true);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.println("\nWiFi Connected");

  initTime();

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = "";
  auth.user.password = "";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (!Firebase.signUp(&config, &auth, "", "")) {
    Serial.printf("SignUp Error: %s\n", config.signer.signupError.message.c_str());
  } else Serial.println("Firebase anonymous auth OK");

  while (!Firebase.ready()) delay(100);
  Firebase.RTDB.setString(&fbdo, "/test", "ESP32_CONNECTED");
}

// ===== Main loop =====
void loop() {
  // Heartbeat independent of parking updates
  sendHeartbeat();

  unsigned long currentMillis = millis();
  if (currentMillis - lastUpdate < UPDATE_INTERVAL) return;
  lastUpdate = currentMillis;

  // ===== Read distances =====
  float d[3];
  d[0] = readDistance(TRIG_P1, ECHO_P1);
  d[1] = readDistance(TRIG_P2, ECHO_P2);
  d[2] = readDistance(TRIG_P3, ECHO_P3);

  bool occ[3];
  String status[3];
  int available = 0;

  for (int i = 0; i < 3; i++) {
    occ[i] = (d[i] > 0 && d[i] < THRESHOLD_CM);
    status[i] = occ[i] ? "Occupied" : "Available";
    if (!occ[i]) available++;
  }

  // Update Firebase only if values changed
  bool updated = false;
  for (int i = 0; i < 3; i++) {
    if (abs(d[i] - lastDistance[i]) > 0.5) {
      setFloat("/parking/P" + String(i+1) + "/distance", d[i]);
      lastDistance[i] = d[i];
      updated = true;
    }
    if (status[i] != lastStatus[i]) {
      setString("/parking/P" + String(i+1) + "/status", status[i]);
      lastStatus[i] = status[i];
      updated = true;
    }
  }

  if (available != lastAvailable) {
    setInt("/parking/availableCount", available);
    lastAvailable = available;
    updated = true;
  }

  unsigned long now = getEpochTime();
  if (updated && now > 0) {
    setInt("/parking/lastUpdate", now);
    Serial.println("Firebase parking updated");
  }
}
