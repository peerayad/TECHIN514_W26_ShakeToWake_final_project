/**
 * ============================================================
 *  ESP-NOW SENDER — XIAO ESP32C3 + ADXL345
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>

// ─── ✅ Updated MAC address ─────────────────────────────────
uint8_t receiverAddress[] = {0x58, 0x8C, 0x81, 0xA0, 0xBC, 0x30};

// ─── LED ────────────────────────────────────────────────────
#define LED_PIN           D3
#define MOTION_THRESHOLD  0.5f
#define LED_BLINK_MS      300

// ─── ADXL345 ───────────────────────────────────────────────
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);

// ─── Data struct ────────────────────────────────────────────
typedef struct struct_message {
  float x;
  float y;
  float z;
} struct_message;

struct_message data;

// ─── Motion tracking ───────────────────────────────────────
float prevX = 0, prevY = 0, prevZ = 0;
bool firstReading = true;
unsigned long ledOnUntil = 0;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(D4, D5);
  Wire.setClock(100000);
  if (!accel.begin()) {
    Serial.println("[ERROR] ADXL345 not found!");
    while (1) {
      digitalWrite(LED_PIN, HIGH); delay(100);
      digitalWrite(LED_PIN, LOW);  delay(100);
    }
  }
  accel.setRange(ADXL345_RANGE_2_G);
  Serial.println("[OK] ADXL345 ready");

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println("Sender ready");
}

void loop() {
  digitalWrite(LED_PIN, millis() < ledOnUntil ? HIGH : LOW);

  static unsigned long lastLoop = 0;
  if (millis() - lastLoop < 200) return;
  lastLoop = millis();

  sensors_event_t event;
  accel.getEvent(&event);

  data.x = event.acceleration.x;
  data.y = event.acceleration.y;
  data.z = event.acceleration.z;

  if (firstReading) {
    prevX = data.x; prevY = data.y; prevZ = data.z;
    firstReading = false;
  } else {
    float dx = fabsf(data.x - prevX);
    float dy = fabsf(data.y - prevY);
    float dz = fabsf(data.z - prevZ);
    if (dx > MOTION_THRESHOLD || dy > MOTION_THRESHOLD || dz > MOTION_THRESHOLD) {
      ledOnUntil = millis() + LED_BLINK_MS;
      Serial.printf("[MOTION] dX=%.2f dY=%.2f dZ=%.2f\n", dx, dy, dz);
    }
    prevX = data.x; prevY = data.y; prevZ = data.z;
  }

  esp_now_send(receiverAddress, (uint8_t *)&data, sizeof(data));
  Serial.printf("Sending X=%.2f  Y=%.2f  Z=%.2f\n", data.x, data.y, data.z);
}