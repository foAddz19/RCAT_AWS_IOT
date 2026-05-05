#include <Arduino.h>

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <time.h> 
#include <esp_task_wdt.h> 

#define WDT_TIMEOUT 15 

// ==========================================
// 1. ตั้งค่า WiFi และ Blynk
// ==========================================
char auth[] = "ddK-Ajpk4_jZuDXjb6XHDkyZXJBG3Ilk";
char ssid[] = "RCAT_WIFI";
char pass[] = "";
char blynk_server[] = "blynk.iot-cm.com";
int blynk_port = 8080;

// ==========================================
// 2. ตั้งค่า LINE Messaging API
// ==========================================
//String CHANNEL_ACCESS_TOKEN = "3mJoHZiH+ow3h6xojFO827IDg50wTs93t0bLJA4LOc7YQLMs71Vl4Rh3Md3zALXyIVfmvCzyBparvVUCyUdzdJGpM6Uk748FDtdVSr4OGIEUU1BgY8orhk+K/n4J5t5a7hs/sIyUqHLS+Sr8KiYsewdB04t89/1O/w1cDnyilFU=";
String CHANNEL_ACCESS_TOKEN = "b18LOY9tkC0FYPDZu3yz87NS+Ti/VknOoaA3s0aYtGjEOaHqbaso+kbLbmr0z4v8ZoNFhdECdPetZFDxGQUrqr/MHDu4fy5mgHr3G7kgfc+HtTSdMNXB6zGwm7jubCH3nb0qYN1vXsIIKigvKZBtZAdB04t89/1O/w1cDnyilFU=";
//String USER_ID = "U5a67c095c1607c2958b46c3c9a934a7f"; // ID กลุ่ม RCAT
//String USER_ID = "C686e1a5f25a18d0e39087ad80a712ca4"; // ID กลุ่ม test
String USER_ID = "C8c932708ed6f7c597727ecea20199f10"; //rcat smart 
// ==========================================
// 3. ตั้งค่า RS485
// ==========================================
#define RXD2 16
#define TXD2 17
HardwareSerial RS485(2);

// ==========================================
// 4. ตั้งค่าเวลา (NTP)
// ==========================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; 
const int   daylightOffset_sec = 0;

// ตัวแปรเก็บค่าสภาพอากาศทั้ง 8 ค่า
float windSpeed = 0, windDir = 0, temp = 0, hum = 0, press = 0, rain = 0, solar = 0, pm25 = 0;

unsigned long lastBlynkUpdate = 0; 
unsigned long lastWifiCheck = 0;
int lastReportedDay = -1; 
bool isTimeSynced = false;

// ดึงเวลา
String getDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "กำลังซิงค์เวลา...";
  }
  isTimeSynced = true; 
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m/%Y เวลา %H:%M น.", &timeinfo);
  return String(timeStringBuff);
}

// ป้องกัน Error ใน JSON
String escapeJsonString(String input) {
  String output = input;
  output.replace("\n", "\\n");
  output.replace("\"", "\\\"");
  return output;
}

// ส่ง LINE
void sendLinePush(String msg) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://api.line.me/v2/bot/message/push");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + CHANNEL_ACCESS_TOKEN);

    String escapedMsg = escapeJsonString(msg);
    String payload = "{\"to\":\"" + USER_ID + "\",\"messages\":[{\"type\":\"text\",\"text\":\"" + escapedMsg + "\"}]}";

    int code = http.POST(payload);
    Serial.print("[LINE] Status Code: "); Serial.println(code);
    http.end();
  }
}

// ดักฟัง RS485 (Smart Filter)
void listenRS485() {
  if (RS485.available()) {
    delay(60); 
    uint8_t buf[128];
    int len = 0;
    while (RS485.available() && len < 128) {
      buf[len++] = RS485.read();
    }

    for (int i = 0; i <= len - 5; i++) {
      if (buf[i+1] == 0x03 && buf[i+2] == 0x02) {
        
        int currentID = buf[i];
        int rawData = (buf[i+3] << 8) | buf[i+4];
        float val = rawData / 10.0; 

        // คัดแยกเข้าตัวแปรตาม ID
        if (currentID == 0x01) windSpeed = val;
        else if (currentID == 0x02) windDir = rawData; 
        else if (currentID == 0x06) pm25 = val; 
       
        // ☀️ อัปเดตแสงแดดเป็น ID 5 ตามที่คุณบัญชาหาเจอ!
        else if (currentID == 0x05) solar = rawData; 
        
      
        else if (currentID == 0x04) rain = rawData;
        
        // Smart Filter สำหรับ ID 3 (อุณหภูมิ, ความชื้น, ความกดอากาศ)
        else if (currentID == 0x03) {
          if (val > 500.0) press = val;       
          else if (val >= 45.0 && val <= 100.0) hum = val;         
          else if (val >= -10.0 && val < 45.0) temp = val;        
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  RS485.begin(9600, SERIAL_8N1, RXD2, TXD2); 
  
  // 🛡️ ตั้งค่า Watchdog Timer รูปแบบใหม่ (รองรับ ESP32 Core 3.x)
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WDT_TIMEOUT * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
  #else
    esp_task_wdt_init(WDT_TIMEOUT, true);
  #endif
  esp_task_wdt_add(NULL);
  
  WiFi.begin(ssid, pass);
  Serial.println("\n[SYSTEM] Connecting to WiFi...");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Blynk.config(auth, blynk_server, blynk_port);
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.connect(); //addz
  }
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  Serial.println("🟢 [ ระบบออนไลน์ AWS RCAT ]\n");
  /*
  String startupMsg = "🟢 [ ระบบออนไลน์ AWS RCAT ]\n";
  startupMsg += "บอร์ด ESP32 เริ่มทำงานแล้ว!\n";
  startupMsg += "📅 อัปเดตเมื่อ: " + getDateTime() + "\n";
  startupMsg += "📡 โหมด: Ultimate Filter (8 Params)\n";
  startupMsg += "🛡️ ระบบความปลอดภัย: Watchdog Timer [ON]\n";
  startupMsg += "✨ พร้อมรายงานเวลา 18:00 น.\n";
  startupMsg += "วิทยาลัยเกษตรและเทคโนโลยีร้อยเอ็ด";
  sendLinePush(startupMsg);
  */
}

void loop() {
  esp_task_wdt_reset();

  if (millis() - lastWifiCheck > 10000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Disconnected. Trying to reconnect...");
      WiFi.disconnect();
      WiFi.reconnect();
    }
    lastWifiCheck = millis();
  }

  if (Blynk.connected()) {
    Blynk.run();    
  } else if (WiFi.status() == WL_CONNECTED) {
    Blynk.connect(); 
  }

  listenRS485();  

  if (millis() - lastBlynkUpdate > 3000) {
    Serial.println("\n>>>>>> [ สภาพอากาศปัจจุบัน ] <<<<<<");
    Serial.print("🕒 อัปเดตล่าสุด: "); Serial.println(getDateTime());
    Serial.printf("🌡 อุณหภูมิ:   %.1f °C\n", temp);
    Serial.printf("💧 ความชื้น:   %.1f %%\n", hum);
    Serial.printf("📊 ความกด:     %.1f hPa\n", press);
    Serial.printf("🌪 ลม(เร็ว):   %.1f m/s\n", windSpeed);
    Serial.printf("🧭 ลม(ทิศ):    %.1f°\n", windDir); 
    Serial.printf("🌧 ฝน:         %.1f mm\n", rain);
    Serial.printf("☀️ แสง:        %.0f W/m2\n", solar);
    Serial.printf("🌫 PM2.5:      %.0f ug/m3\n", pm25);
    Serial.println("------------------------------------");
    
    if (Blynk.connected()) {
      Blynk.virtualWrite(V0, windSpeed);
      Blynk.virtualWrite(V1, windDir); 
      Blynk.virtualWrite(V2, temp);
      Blynk.virtualWrite(V3, hum);
      Blynk.virtualWrite(V4, press);
      Blynk.virtualWrite(V5, rain);
      Blynk.virtualWrite(V6, solar); // ส่งค่า ID 5 ขึ้น Blynk แล้ว
      Blynk.virtualWrite(V7, pm25);
    }
    lastBlynkUpdate = millis();
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_hour == 18 && timeinfo.tm_min == 0 && timeinfo.tm_mday != lastReportedDay) {
      String msg = "🌤️ [ สรุปสภาพอากาศ AWS RCAT ] 🌤️\n";
      msg += "📅 " + getDateTime() + "\n";
      msg += "━━━━━━━━━━━━━\n";
      msg += "🌡️ อุณหภูมิ : " + String(temp, 1) + " °C\n";
      msg += "💧 ความชื้น : " + String(hum, 1) + " %\n";
      msg += "📊 ความกด : " + String(press, 1) + " hPa\n";
      msg += "━━━━━━━━━━━━━\n";
      msg += "🌪️ ลม (เร็ว) : " + String(windSpeed, 1) + " m/s\n";
      msg += "🧭 ลม (ทิศ) : " + String(windDir, 1) + "°\n"; 
      msg += "🌧️ ปริมาณฝน : " + String(rain, 1) + " mm\n";
      msg += "━━━━━━━━━━━━━\n";
      msg += "☀️ แสงแดด : " + String(solar, 0) + " W/m²\n";
      msg += "🌫️ PM2.5 : " + String(pm25, 0) + " µg/m³\n";
      msg += "━━━━━━━━━━━━━\n";
      msg += "🤖 แจ้งเตือนอัตโนมัติจากระบบ\n";
      msg += "วิทยาลัยเกษตรและเทคโนโลยีร้อยเอ็ด";
      
      sendLinePush(msg);
      lastReportedDay = timeinfo.tm_mday; 
    }
  }
}