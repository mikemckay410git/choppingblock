#include <WiFi.h>
#include <esp_now.h>

// MAC of Player 1 (replace with your actual address)
uint8_t masterAddress[] = { 0x78, 0x1C, 0x3C, 0xB8, 0xD5, 0xA9 };

typedef struct struct_message {
  uint8_t  playerId;    // = 2
  uint8_t  action;      // 1 = heartbeat, 2 = sensor-update
  uint16_t cap[5];      // five capacitive channels
  uint16_t shock;       // shock reading
} struct_message;

struct_message myData;

unsigned long lastSendTime = 0;
const unsigned long sendInterval = 500; // ms

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);
  sensorsInit();

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while (true);
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    while (true);
  }

  myData.playerId = 2;
  Serial.println("Player 2 ready");
}

void loop() {
  sensorsLoop();

  unsigned long now = millis();
  if (now - lastSendTime >= sendInterval) {
    myData.action = 2; // sensor update
    for (int i = 0; i < 5; i++) {
      myData.cap[i] = getCapacitiveValue(i);
    }
    myData.shock = getShockValue();

    esp_err_t result = esp_now_send(masterAddress,
                                    (uint8_t*)&myData,
                                    sizeof(myData));
    if (result == ESP_OK) {
      Serial.println("Sensor update sent");
    } else {
      Serial.println("Send error");
    }
    lastSendTime = now;
  }
}
