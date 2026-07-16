#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <MQTT.h>
#include <DHT.h>

// =====================================================
// KONFIGURASI WIFI DAN HIVEMQ
// =====================================================

const char WIFI_SSID[] = "Metyou @tomoro";
const char WIFI_PASSWORD[] = "TerimaKasih";
const char MQTT_HOST[] = "712b2488276943edbd55e938ca1ba12b.s1.eu.hivemq.cloud";
const int MQTT_PORT = 8883;
const char MQTT_USERNAME[] = "mikail";
const char MQTT_PASSWORD[] = "mikail123";

// =====================================================
// IDENTITAS PERANGKAT
// =====================================================

const char DEVICE_ID[] = "AIRGUARD-01";
const char DEVICE_NAME[] = "ESP8266";
const char FIRMWARE_VERSION[] = "1.0.2"; // Versi update bug-fix

// =====================================================
// KONFIGURASI PIN
// =====================================================

#define DHT_PIN D1
#define DHT_TYPE DHT22

#define MQ135_PIN A0
#define BUZZER_PIN D5

/*
 * Jika buzzer berbunyi saat seharusnya mati,
 * tukar HIGH dan LOW pada dua konstanta berikut.
 */
const uint8_t BUZZER_ACTIVE_LEVEL = HIGH;
const uint8_t BUZZER_INACTIVE_LEVEL = LOW;

// =====================================================
// TOPIC MQTT
// =====================================================

const char TOPIC_TELEMETRY[] = "airguard/AIRGUARD-01/telemetry";
const char TOPIC_STATUS[]    = "airguard/AIRGUARD-01/status";
const char TOPIC_ALERT[]     = "airguard/AIRGUARD-01/alert";
const char TOPIC_COMMAND[]   = "airguard/AIRGUARD-01/command";
const char TOPIC_COMMAND_ACK[] = "airguard/AIRGUARD-01/command/ack";
const char TOPIC_SETTINGS[]  = "airguard/AIRGUARD-01/settings";

// =====================================================
// PENGATURAN MQ-135 (Threshold Disesuaikan untuk Demo)
// =====================================================

const unsigned long MQ_WARMUP_DURATION = 60000;

// Batas berdasarkan selisih dari baseline. (Dibuat variabel agar bisa diubah via Web)
int MQ_MODERATE_DELTA = 10;  
int MQ_POOR_DELTA = 30;       
int MQ_DANGER_DELTA = 60;     

bool autoBuzzerEnabled = true; // Sinkronisasi dengan Web Settings

const int MQ_MAX_DISPLAY_DELTA = 500;

// Konfirmasi agar alarm tidak aktif karena satu lonjakan data.
const int DANGER_CONFIRMATION_COUNT = 3;
const int SAFE_CONFIRMATION_COUNT = 3;

// =====================================================
// INTERVAL
// =====================================================

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long STATUS_INTERVAL = 30000;

const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;

// =====================================================
// OBJECT
// =====================================================

DHT dht(DHT_PIN, DHT_TYPE);
BearSSL::WiFiClientSecure secureClient;
MQTTClient mqttClient(768);

// =====================================================
// DATA SENSOR
// =====================================================

float temperature = NAN;
float humidity = NAN;

int mqRaw = 0;
int mqBaseline = 250;
int mqDelta = 0;
int gasPercentage = 0;

bool dhtAvailable = false;
bool warmupComplete = false;
bool baselineCalibrated = false;

String airStatus = "PEMANASAN";

// =====================================================
// STATUS ALARM DAN BUZZER
// =====================================================

bool dangerActive = false;
bool buzzerOutput = false;
bool pendingDangerAlert = false;

int dangerCounter = 0;
int safeCounter = 0;

unsigned long silenceUntil = 0;
unsigned long buzzerTestUntil = 0;
unsigned long lastBuzzerToggle = 0;

// =====================================================
// COMMAND MQTT
// =====================================================

String pendingCommand = "";
unsigned long pendingCommandDuration = 0;

// =====================================================
// TIMER DAN DIAGNOSTIK
// =====================================================

unsigned long bootTime = 0;
unsigned long lastSensorRead = 0;
unsigned long lastStatusPublish = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMQTTAttempt = 0;

unsigned long publishCounter = 0;
unsigned long wifiReconnectAttempts = 0;
unsigned long mqttReconnectAttempts = 0;

// =====================================================
// UTILITAS
// =====================================================

bool timerStillActive(unsigned long endTime) {
  return static_cast<int32_t>(endTime - millis()) > 0;
}

String boolToJson(bool value) {
  return value ? "true" : "false";
}

String nullableFloat(float value, bool available) {
  if (!available || isnan(value)) {
    return "null";
  }
  return String(value, 1);
}

int extractJsonInt(const String& payload, const String& key, int defaultVal) {
  int keyIndex = payload.indexOf("\"" + key + "\"");
  if (keyIndex < 0) return defaultVal;
  int colonIndex = payload.indexOf(":", keyIndex);
  if (colonIndex < 0) return defaultVal;
  int commaIndex = payload.indexOf(",", colonIndex);
  int braceIndex = payload.indexOf("}", colonIndex);
  int endIndex = (commaIndex > 0 && commaIndex < braceIndex) ? commaIndex : braceIndex;
  if (endIndex < 0) return defaultVal;
  String valStr = payload.substring(colonIndex + 1, endIndex);
  valStr.trim();
  return valStr.toInt();
}

bool extractJsonBool(const String& payload, const String& key, bool defaultVal) {
  int keyIndex = payload.indexOf("\"" + key + "\"");
  if (keyIndex < 0) return defaultVal;
  int colonIndex = payload.indexOf(":", keyIndex);
  if (colonIndex < 0) return defaultVal;
  int commaIndex = payload.indexOf(",", colonIndex);
  int braceIndex = payload.indexOf("}", colonIndex);
  int endIndex = (commaIndex > 0 && commaIndex < braceIndex) ? commaIndex : braceIndex;
  if (endIndex < 0) return defaultVal;
  String valStr = payload.substring(colonIndex + 1, endIndex);
  valStr.trim();
  valStr.toLowerCase();
  if (valStr == "true" || valStr == "1") return true;
  if (valStr == "false" || valStr == "0") return false;
  return defaultVal;
}

// =====================================================
// KONTROL BUZZER
// =====================================================

void setBuzzer(bool active) {
  buzzerOutput = active;
  digitalWrite(BUZZER_PIN, active ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL);
}

void updateBuzzer() {
  unsigned long now = millis();

  // Prioritas pertama: pengujian buzzer manual.
  if (timerStillActive(buzzerTestUntil)) {
    setBuzzer(true);
    return;
  }

  // Buzzer sedang disenyapkan.
  if (timerStillActive(silenceUntil)) {
    setBuzzer(false);
    return;
  }

  // Buzzer berbunyi cepat ketika kondisi bahaya.
  if (dangerActive) {
    if (!autoBuzzerEnabled) {
      setBuzzer(false);
      return;
    }
    if (now - lastBuzzerToggle >= 250) {
      lastBuzzerToggle = now;
      setBuzzer(!buzzerOutput);
    }
    return;
  }

  setBuzzer(false);
}

// =====================================================
// PEMBACAAN MQ-135
// =====================================================

int readMQ135Average(int sampleCount = 10) {
  long total = 0;
  for (int i = 0; i < sampleCount; i++) {
    total += analogRead(MQ135_PIN);
    delay(10);
    yield();
  }
  return static_cast<int>(total / sampleCount);
}

void calibrateMQ135() {
  Serial.println();
  Serial.println(F("[MQ135] Memulai kalibrasi baseline."));
  Serial.println(F("[MQ135] Pastikan kondisi udara sedang bersih."));

  long total = 0;
  const int sampleCount = 50;

  for (int i = 0; i < sampleCount; i++) {
    total += analogRead(MQ135_PIN);
    delay(20);
    yield();

    if (mqttClient.connected()) {
      mqttClient.loop();
    }
  }

  mqBaseline = static_cast<int>(total / sampleCount);
  baselineCalibrated = true;

  Serial.print(F("[MQ135] Baseline baru: "));
  Serial.println(mqBaseline);
}

// =====================================================
// KLASIFIKASI KUALITAS UDARA
// =====================================================

String determineAirStatus(int delta) {
  if (delta < MQ_MODERATE_DELTA) {
    return "BAIK";
  }
  if (delta < MQ_POOR_DELTA) {
    return "SEDANG";
  }
  if (delta < MQ_DANGER_DELTA) {
    return "BURUK";
  }
  return "BAHAYA";
}

void updateDangerState() {
  if (airStatus == "BAHAYA") {
    dangerCounter++;
    safeCounter = 0;

    if (dangerCounter >= DANGER_CONFIRMATION_COUNT && !dangerActive) {
      dangerActive = true;
      pendingDangerAlert = true;
      Serial.println(F("[ALERT] Kondisi kualitas udara BAHAYA."));
    }
    return;
  }

  dangerCounter = 0;

  if (dangerActive) {
    safeCounter++;
    if (safeCounter >= SAFE_CONFIRMATION_COUNT) {
      dangerActive = false;
      safeCounter = 0;
      Serial.println(F("[ALERT] Kondisi udara kembali di bawah batas bahaya."));
    }
  }
}

// =====================================================
// PEMBACAAN SENSOR
// =====================================================

void printSensorData() {
  Serial.println();
  Serial.println(F("========== DATA SENSOR =========="));
  if (dhtAvailable) {
    Serial.print(F("Suhu        : ")); Serial.print(temperature, 1); Serial.println(F(" C"));
    Serial.print(F("Kelembapan  : ")); Serial.print(humidity, 1);    Serial.println(F(" %"));
  } else {
    Serial.println(F("DHT22       : GAGAL DIBACA"));
  }
  Serial.print(F("MQ-135 Raw  : ")); Serial.println(mqRaw);
  Serial.print(F("Baseline    : ")); Serial.println(mqBaseline);
  Serial.print(F("Selisih     : ")); Serial.println(mqDelta);
  Serial.print(F("Level       : ")); Serial.print(gasPercentage); Serial.println(F(" %"));
  Serial.print(F("Status      : ")); Serial.println(airStatus);
  Serial.print(F("Buzzer      : ")); Serial.println(buzzerOutput ? "ON" : "OFF");
  Serial.print(F("Wi-Fi RSSI  : ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
  Serial.println(F("================================="));
}

void readSensors() {
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  dhtAvailable = !isnan(temperature) && !isnan(humidity);
  mqRaw = readMQ135Average();

  if (!warmupComplete) {
    airStatus = "PEMANASAN";
    mqDelta = 0;
    gasPercentage = 0;
    dangerCounter = 0;
    safeCounter = 0;
    dangerActive = false;
    printSensorData();
    return;
  }

  mqDelta = mqRaw - mqBaseline;
  if (mqDelta < 0) {
    mqDelta = 0;
  }

  gasPercentage = map(mqDelta, 0, MQ_MAX_DISPLAY_DELTA, 0, 100);
  gasPercentage = constrain(gasPercentage, 0, 100);

  airStatus = determineAirStatus(mqDelta);
  updateDangerState();
  printSensorData();
}

// =====================================================
// PAYLOAD TELEMETRY
// =====================================================

String createTelemetryPayload() {
  String payload;
  payload.reserve(650);
  payload += "{";
  payload += "\"device_id\":\""; payload += DEVICE_ID; payload += "\",";
  payload += "\"device_name\":\""; payload += DEVICE_NAME; payload += "\",";
  payload += "\"firmware_version\":\""; payload += FIRMWARE_VERSION; payload += "\",";
  payload += "\"temperature\":"; payload += nullableFloat(temperature, dhtAvailable); payload += ",";
  payload += "\"humidity\":"; payload += nullableFloat(humidity, dhtAvailable); payload += ",";
  payload += "\"gas_raw\":"; payload += String(mqRaw); payload += ",";
  payload += "\"gas_baseline\":"; payload += String(mqBaseline); payload += ",";
  payload += "\"gas_delta\":"; payload += String(mqDelta); payload += ",";
  payload += "\"gas_percentage\":"; payload += String(gasPercentage); payload += ",";
  payload += "\"air_status\":\""; payload += airStatus; payload += "\",";
  payload += "\"buzzer_status\":"; payload += boolToJson(buzzerOutput); payload += ",";
  payload += "\"buzzer_silenced\":"; payload += boolToJson(timerStillActive(silenceUntil)); payload += ",";
  payload += "\"danger_active\":"; payload += boolToJson(dangerActive); payload += ",";
  payload += "\"dht_available\":"; payload += boolToJson(dhtAvailable); payload += ",";
  payload += "\"warmup_complete\":"; payload += boolToJson(warmupComplete); payload += ",";
  payload += "\"wifi_connected\":"; payload += boolToJson(WiFi.status() == WL_CONNECTED); payload += ",";
  payload += "\"mqtt_connected\":"; payload += boolToJson(mqttClient.connected()); payload += ",";
  payload += "\"rssi\":"; payload += String(WiFi.RSSI()); payload += ",";
  payload += "\"uptime\":"; payload += String(millis() / 1000); payload += ",";
  payload += "\"free_heap\":"; payload += String(ESP.getFreeHeap()); payload += ",";
  payload += "\"publish_counter\":"; payload += String(publishCounter); payload += ",";
  payload += "\"wifi_reconnect_attempts\":"; payload += String(wifiReconnectAttempts); payload += ",";
  payload += "\"mqtt_reconnect_attempts\":"; payload += String(mqttReconnectAttempts);
  payload += "}";
  return payload;
}

// =====================================================
// PAYLOAD STATUS TANPA LOKASI
// =====================================================

String createStatusPayload(const char* status) {
  String payload;
  payload.reserve(350);
  payload += "{";
  payload += "\"device_id\":\""; payload += DEVICE_ID; payload += "\",";
  payload += "\"device_name\":\""; payload += DEVICE_NAME; payload += "\",";
  payload += "\"firmware_version\":\""; payload += FIRMWARE_VERSION; payload += "\",";
  payload += "\"status\":\""; payload += status; payload += "\",";
  payload += "\"ip_address\":\""; payload += WiFi.localIP().toString(); payload += "\",";
  payload += "\"rssi\":"; payload += String(WiFi.RSSI()); payload += ",";
  payload += "\"uptime\":"; payload += String(millis() / 1000); payload += ",";
  payload += "\"free_heap\":"; payload += String(ESP.getFreeHeap()); payload += ",";
  payload += "\"wifi_reconnect_attempts\":"; payload += String(wifiReconnectAttempts); payload += ",";
  payload += "\"mqtt_reconnect_attempts\":"; payload += String(mqttReconnectAttempts);
  payload += "}";
  return payload;
}

// =====================================================
// PUBLISH MQTT
// =====================================================

void publishTelemetry() {
  if (!mqttClient.connected()) {
    Serial.println(F("[TELEMETRY] Gagal: MQTT tidak terhubung."));
    return;
  }

  String payload = createTelemetryPayload();
  bool success = mqttClient.publish(TOPIC_TELEMETRY, payload, false, 1);

  if (success) {
    publishCounter++;
    Serial.print(F("[TELEMETRY] Publish berhasil #"));
    Serial.println(publishCounter);
  } else {
    Serial.println(F("[TELEMETRY] Publish gagal."));
  }
}

void publishDeviceStatus() {
  if (!mqttClient.connected()) {
    return;
  }

  const char* currentStatus;
  if (!warmupComplete) {
    currentStatus = "WARMING_UP";
  } else if (!dhtAvailable) {
    currentStatus = "SENSOR_ERROR";
  } else {
    currentStatus = "ONLINE";
  }

  String payload = createStatusPayload(currentStatus);
  bool success = mqttClient.publish(TOPIC_STATUS, payload, true, 1);

  Serial.print(F("[STATUS] Publish: "));
  Serial.println(success ? "BERHASIL" : "GAGAL");
}

void publishDangerAlert() {
  if (!mqttClient.connected() || !pendingDangerAlert) {
    return;
  }

  // Perbaikan Bug: Membatasi pengiriman ulang agar tidak flooding jika gagal/pending
  static unsigned long lastAlertAttempt = 0;
  unsigned long now = millis();
  if (now - lastAlertAttempt < 5000) { 
    return; // Coba lagi maksimal 5 detik sekali
  }
  lastAlertAttempt = now;

  String payload;
  payload.reserve(450);
  payload += "{";
  payload += "\"device_id\":\""; payload += DEVICE_ID; payload += "\",";
  payload += "\"event_type\":\"AIR_QUALITY\",";
  payload += "\"event_status\":\"TRIGGERED\",";
  payload += "\"severity\":\"BAHAYA\",";
  payload += "\"message\":\"Terdeteksi peningkatan gas atau asap\",";
  payload += "\"gas_raw\":"; payload += String(mqRaw); payload += ",";
  payload += "\"gas_baseline\":"; payload += String(mqBaseline); payload += ",";
  payload += "\"gas_delta\":"; payload += String(mqDelta); payload += ",";
  payload += "\"gas_percentage\":"; payload += String(gasPercentage); payload += ",";
  payload += "\"temperature\":"; payload += nullableFloat(temperature, dhtAvailable); payload += ",";
  payload += "\"humidity\":"; payload += nullableFloat(humidity, dhtAvailable); payload += ",";
  payload += "\"buzzer_status\":"; payload += boolToJson(buzzerOutput);
  payload += "}";

  bool success = mqttClient.publish(TOPIC_ALERT, payload, false, 1);

  if (success) {
    pendingDangerAlert = false;
    Serial.println(F("[ALERT] Peringatan bahaya berhasil dikirim."));
  } else {
    Serial.println(F("[ALERT] Peringatan bahaya gagal dikirim."));
  }
}

void publishCommandAck(const String& command, bool success, const String& message) {
  if (!mqttClient.connected()) {
    return;
  }

  String payload;
  payload.reserve(300);
  payload += "{";
  payload += "\"device_id\":\""; payload += DEVICE_ID; payload += "\",";
  payload += "\"command\":\""; payload += command; payload += "\",";
  payload += "\"success\":"; payload += boolToJson(success); payload += ",";
  payload += "\"message\":\""; payload += message; payload += "\"";
  payload += "}";

  mqttClient.publish(TOPIC_COMMAND_ACK, payload, false, 1);
}

// =====================================================
// MENERIMA COMMAND MQTT
// =====================================================

unsigned long extractDuration(const String& payload, unsigned long defaultDuration) {
  int durationPosition = payload.indexOf("duration");
  if (durationPosition < 0) {
    return defaultDuration;
  }
  int colonPosition = payload.indexOf(':', durationPosition);
  if (colonPosition < 0) {
    return defaultDuration;
  }
  unsigned long parsedDuration = payload.substring(colonPosition + 1).toInt();
  if (parsedDuration == 0) {
    return defaultDuration;
  }
  return constrain(parsedDuration, 1000UL, 60000UL);
}

void messageReceived(String& topic, String& payload) {
  Serial.println();
  Serial.print(F("[MQTT] Pesan diterima di Topic: "));  Serial.println(topic);
  Serial.print(F("[MQTT] Payload: ")); Serial.println(payload);

  // Jika pesan dari topik pengaturan (Settings)
  if (topic == String(TOPIC_SETTINGS)) {
    MQ_MODERATE_DELTA = extractJsonInt(payload, "threshold_moderate", MQ_MODERATE_DELTA);
    MQ_POOR_DELTA = extractJsonInt(payload, "threshold_poor", MQ_POOR_DELTA);
    MQ_DANGER_DELTA = extractJsonInt(payload, "threshold_danger", MQ_DANGER_DELTA);
    autoBuzzerEnabled = extractJsonBool(payload, "auto_buzzer", autoBuzzerEnabled);
    Serial.println(F("[SETTINGS] Pengaturan baru berhasil diterapkan dari Web."));
    return;
  }

  String normalizedPayload = payload;
  normalizedPayload.toUpperCase();

  if (normalizedPayload.indexOf("TEST_BUZZER") >= 0) {
    pendingCommand = "TEST_BUZZER";
    pendingCommandDuration = extractDuration(payload, 3000);
  } else if (normalizedPayload.indexOf("SILENCE_BUZZER") >= 0) {
    pendingCommand = "SILENCE_BUZZER";
    pendingCommandDuration = extractDuration(payload, 30000);
  } else if (normalizedPayload.indexOf("RESET_ALARM") >= 0) {
    pendingCommand = "RESET_ALARM";
  } else if (normalizedPayload.indexOf("CALIBRATE_BASELINE") >= 0) {
    pendingCommand = "CALIBRATE_BASELINE";
  } else if (normalizedPayload.indexOf("GET_STATUS") >= 0) {
    pendingCommand = "GET_STATUS";
  } else {
    pendingCommand = "UNKNOWN_COMMAND";
  }
}

void processPendingCommand() {
  if (pendingCommand.length() == 0) {
    return;
  }

  String command = pendingCommand;
  pendingCommand = "";

  if (command == "TEST_BUZZER") {
    buzzerTestUntil = millis() + pendingCommandDuration;
    publishCommandAck(command, true, "Pengujian buzzer dimulai");
    Serial.println(F("[BUZZER] Pengujian dimulai."));
    return;
  }

  if (command == "SILENCE_BUZZER") {
    silenceUntil = millis() + pendingCommandDuration;
    publishCommandAck(command, true, "Buzzer disenyapkan sementara");
    Serial.println(F("[BUZZER] Buzzer disenyapkan sementara."));
    return;
  }

  if (command == "RESET_ALARM") {
    dangerActive = false;
    dangerCounter = 0;
    safeCounter = 0;
    pendingDangerAlert = false;
    silenceUntil = 0;
    buzzerTestUntil = 0;
    setBuzzer(false);
    publishCommandAck(command, true, "Alarm berhasil direset");
    Serial.println(F("[ALERT] Alarm direset."));
    return;
  }

  if (command == "CALIBRATE_BASELINE") {
    calibrateMQ135();
    publishCommandAck(command, true, "Baseline MQ-135 berhasil diperbarui");
    return;
  }

  if (command == "GET_STATUS") {
    publishDeviceStatus();
    publishCommandAck(command, true, "Status perangkat telah dikirim");
    return;
  }

  publishCommandAck(command, false, "Command tidak dikenali");
}

// =====================================================
// WIFI
// =====================================================

void startWiFiConnection() {
  Serial.println();
  Serial.print(F("[WIFI] Menghubungkan ke: "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (now - lastWiFiAttempt < WIFI_RETRY_INTERVAL) {
    return;
  }

  lastWiFiAttempt = now;
  wifiReconnectAttempts++;
  Serial.print(F("[WIFI] Reconnect percobaan #"));
  Serial.println(wifiReconnectAttempts);
  WiFi.reconnect();
}

// =====================================================
// MQTT HIVEMQ
// =====================================================

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (mqttClient.connected()) {
    return;
  }

  unsigned long now = millis();
  if (now - lastMQTTAttempt < MQTT_RETRY_INTERVAL) {
    return;
  }

  lastMQTTAttempt = now;
  mqttReconnectAttempts++;

  String clientId = String(DEVICE_ID) + "-" + String(ESP.getChipId(), HEX);
  Serial.println();
  Serial.print(F("[MQTT] Menghubungkan sebagai: "));
  Serial.println(clientId);

  bool connected = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);

  if (!connected) {
    Serial.print(F("[MQTT] Koneksi gagal. Error: "));
    Serial.print(mqttClient.lastError());
    Serial.print(F(" | Return code: "));
    Serial.println(mqttClient.returnCode());
    return;
  }

  Serial.println(F("[MQTT] HiveMQ berhasil terhubung."));
  bool subCmd = mqttClient.subscribe(TOPIC_COMMAND, 1);
  bool subSet = mqttClient.subscribe(TOPIC_SETTINGS, 1);
  Serial.print(F("[MQTT] Subscribe Command & Settings: "));
  Serial.println((subCmd && subSet) ? "BERHASIL" : "GAGAL");
  publishDeviceStatus();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  bootTime = millis();
  Serial.println();
  Serial.println(F("================================"));
  Serial.println(F("AIRGUARD IoT"));
  Serial.println(F("ESP8266 + DHT22 + MQ-135"));
  Serial.println(F("================================"));

  pinMode(BUZZER_PIN, OUTPUT);
  setBuzzer(false);

  dht.begin();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  startWiFiConnection();
  secureClient.setInsecure();

  mqttClient.begin(MQTT_HOST, MQTT_PORT, secureClient);
  mqttClient.onMessage(messageReceived);
  mqttClient.setOptions(30, true, 10000);

  mqttClient.setWill(TOPIC_STATUS,
      "{\"device_id\":\"AIRGUARD-01\",\"device_name\":\"ESP8266\",\"status\":\"OFFLINE\"}",
      true, 1);
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  unsigned long now = millis();
  maintainWiFiConnection();

  if (WiFi.status() == WL_CONNECTED) {
    static bool wifiConnectedPrinted = false;
    if (!wifiConnectedPrinted) {
      wifiConnectedPrinted = true;
      Serial.println();
      Serial.println(F("[WIFI] Berhasil terhubung."));
      Serial.print(F("[WIFI] IP: ")); Serial.println(WiFi.localIP());
      Serial.print(F("[WIFI] RSSI: ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
    }
    connectMQTT();
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  delay(10);
  processPendingCommand();

  if (!warmupComplete && now - bootTime >= MQ_WARMUP_DURATION) {
    warmupComplete = true;
    Serial.println();
    Serial.println(F("[MQ135] Pemanasan awal selesai."));
    if (!baselineCalibrated) {
      calibrateMQ135();
    }
    publishDeviceStatus();
  }

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    updateBuzzer();
    publishTelemetry();
  }

  if (now - lastStatusPublish >= STATUS_INTERVAL) {
    lastStatusPublish = now;
    publishDeviceStatus();
  }

  publishDangerAlert();
  updateBuzzer();
}