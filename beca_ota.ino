/*
  ================================================================
  BỂ CÁ IoT v4.0.2 — CÓ BÁO CÁO TIẾN TRÌNH OTA LÊN WEB THỰC TẾ
  ================================================================
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPUpdate.h>

// ── THÔNG TIN KẾT NỐI ─────────────────────────────────────────
#define FB_HOST   "beca3-43b1a-default-rtdb.firebaseio.com"
#define FB_AUTH   "cQTYc06GyuyKeAltqJ7nqxfBdzIFUds2vUr5ax0a"
#define TG_TOKEN  "8618680216:AAEts4r3-QDhvX3bvieQ-eUshlJGpv5v9IA"
#define TG_CHAT   "7335775840"

// ── PHIÊN BẢN ─────────────────────────────────────────────────
String currentVersion = "4.0.3"; // Đã nâng cấp lên 4.0.3

// ── CHÂN GPIO ─────────────────────────────────────────────────
#define PIN_TDS   34
#define PIN_TURB  35
#define PIN_PUMP  25
#define PIN_LIGHT 26
#define I2C_SDA   21
#define I2C_SCL   22

// ── CHU KỲ (ms) ───────────────────────────────────────────────
#define SEND_REALTIME_MS   2000   
#define SEND_HISTORY_MS   10000   
#define READ_CONTROL_MS    5000   
#define WIFI_CHECK_MS     30000   
#define ADC_N                64    
#define HTTP_TIMEOUT_MS   5000    

// ── BUFFER LƯU DỮ LIỆU KHI MẤT WIFI ─────────────────────────
#define OFFLINE_BUFFER_SIZE 120

struct SensorRecord {
  float  tds;
  float  turb;
  bool   pump;
  bool   light;
  unsigned long ts;       
  char   t[10];           
};

SensorRecord offlineBuffer[OFFLINE_BUFFER_SIZE];
int  bufferHead  = 0;
int  bufferCount = 0;     
bool wasOffline  = false; 

// ── TRẠNG THÁI ────────────────────────────────────────────────
float tdsTh   = 350.0f;
float turbTh  =  40.0f;
int   ctrlMode  = 0;
int   lightMode = 0;
int   onHour    = 7;
int   offHour   = 22;
bool  pump      = false;
bool  light     = false;
bool  alTDS     = false;
bool  alTurb    = false;
unsigned long tAlTDS  = 0;
unsigned long tAlTurb = 0;

#define ALERT_COOLDOWN_MS  (60UL * 1000UL) // 1 phút

bool notifMuted = false;
unsigned long lastSendRT   = 0;
unsigned long lastSendHist = 0;
unsigned long lastReadCtrl = 0;
unsigned long lastWifiChk  = 0;
unsigned long lastFlushBuf = 0;
float lastTDS       = 0;   
float lastTurb      = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcdOk = false;

// ══════════════════════════════════════════════════════════════
// TIỆN ÍCH
// ══════════════════════════════════════════════════════════════

const char* timeStr() {
  static char b[12];
  struct tm t;
  if (!getLocalTime(&t)) return "--:--:--";
  snprintf(b, sizeof(b), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return b;
}

unsigned long getTS() {
  time_t n; time(&n);
  return (unsigned long)n;
}

bool lightSched() {
  struct tm t;
  if (!getLocalTime(&t)) return false;
  return (t.tm_hour >= onHour && t.tm_hour < offHour);
}

// ══════════════════════════════════════════════════════════════
// ĐỌC ADC TRỰC TIẾP
// ══════════════════════════════════════════════════════════════

int adcAvg(int pin) {
  long s = 0;
  for (int i = 0; i < ADC_N; i++) {
    s += analogRead(pin);
    delayMicroseconds(500);
  }
  return (int)(s / ADC_N);
}

float toTDS(int raw) {
  float v = (raw / 4095.0f) * 3.3f;
  return v * 303.0f;
}

float toTurb(int raw) {
  return constrain(100.0f - (raw / 4095.0f) * 100.0f, 0.0f, 100.0f);
}

// ══════════════════════════════════════════════════════════════
// FIREBASE
// ══════════════════════════════════════════════════════════════

bool fbPutJson(const char* path, const char* json) {
  if (WiFi.status() != WL_CONNECTED) return false;
  char url[220];
  snprintf(url, sizeof(url), "https://%s%s.json?auth=%s", FB_HOST, path, FB_AUTH);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient h;
  h.begin(client, url);
  h.addHeader("Content-Type", "application/json");
  h.setTimeout(HTTP_TIMEOUT_MS);
  int code = h.PUT(json);
  h.end();
  return (code == 200);
}

bool fbGetJson(const char* path, String& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  char url[220];
  snprintf(url, sizeof(url), "https://%s%s.json?auth=%s", FB_HOST, path, FB_AUTH);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient h;
  h.begin(client, url);
  h.setTimeout(HTTP_TIMEOUT_MS);
  int code = h.GET();
  if (code == 200) { out = h.getString(); h.end(); return true; }
  h.end(); return false;
}

// ══════════════════════════════════════════════════════════════
// TELEGRAM
// ══════════════════════════════════════════════════════════════

void tgSend(const char* msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  char url[160];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TG_TOKEN);
  StaticJsonDocument<512> doc;
  doc["chat_id"]    = TG_CHAT;
  doc["text"]       = msg;
  doc["parse_mode"] = "HTML";
  char payload[512];
  serializeJson(doc, payload, sizeof(payload));
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient h;
  h.begin(client, url);
  h.addHeader("Content-Type", "application/json");
  h.setTimeout(HTTP_TIMEOUT_MS);
  int code = h.POST(payload);
  h.end();
  if (code != 200) {
    delay(1000);
    h.begin(client, url);
    h.addHeader("Content-Type", "application/json");
    h.setTimeout(HTTP_TIMEOUT_MS);
    h.POST(payload);
    h.end();
  }
}

// ══════════════════════════════════════════════════════════════
// OTA BÁO CÁO TIẾN TRÌNH %
// ══════════════════════════════════════════════════════════════

void performOTA(String url) {
  Serial.println("[OTA] Bắt đầu cập nhật: " + url);
  if (lcdOk) { lcd.clear(); lcd.print("Dang cap nhat..."); }
  tgSend("⏳ <b>Đang cập nhật Firmware OTA...</b>\nKhông rút điện!");

  // Báo cho Web biết đã bắt đầu bước 2 (Đang tải file)
  fbPutJson("/ota_status", "{\"status\":2, \"progress\":0}");

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Xóa URL trên Firebase ngay trước khi cập nhật để tránh kẹt vòng lặp nếu nạp nhầm file cũ
  fbPutJson("/config/otaUrl", "\"\"");

  // Kích hoạt cơ chế báo cáo tiến độ bằng cách gửi dữ liệu % lên Firebase
  httpUpdate.onProgress([](int cur, int total) {
    int percent = (cur * 100) / total;
    static int lastPercent = 0;
    
    // Gửi mỗi khi tăng 10% để tránh quá tải tốc độ xử lý của vi điều khiển
    if (percent - lastPercent >= 10 || percent == 100) {
      lastPercent = percent;
      
      char json[50];
      snprintf(json, sizeof(json), "{\"status\":2, \"progress\":%d}", percent);
      fbPutJson("/ota_status", json); 
      
      Serial.printf("[OTA] Progress: %d%%\n", percent);
      
      if (lcdOk) {
        lcd.setCursor(0, 1);
        lcd.printf("Tien trinh: %3d%%", percent);
      }
    }
  });

  t_httpUpdate_return ret = httpUpdate.update(client, url);
  
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      tgSend("❌ Cập nhật OTA thất bại! Giữ phiên bản cũ.");
      fbPutJson("/ota_status", "{\"status\":-1, \"progress\":0}"); // Báo lỗi cho Web
      // Xóa URL trên Firebase để tránh bị vòng lặp vô tận (cập nhật thất bại liên tục)
      fbPutJson("/config/otaUrl", "\"\"");
      if (lcdOk) { lcd.clear(); lcd.print("Loi OTA Update!"); delay(2000); }
      break;
    case HTTP_UPDATE_OK:
      // Firmware nạp thành công, báo Web trạng thái hoàn tất (Bước 5)
      fbPutJson("/ota_status", "{\"status\":3, \"progress\":100}");
      break; 
    default: break;
  }
}

// ══════════════════════════════════════════════════════════════
// WIFI WATCHDOG
// ══════════════════════════════════════════════════════════════

void wifiCheck() {
  if (millis() - lastWifiChk < WIFI_CHECK_MS) return;
  lastWifiChk = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!wasOffline) {
      wasOffline = true;
      Serial.println(F("[WiFi] Mất kết nối! Sẽ thử lại mỗi 30s"));
      if (lcdOk) { lcd.clear(); lcd.print("Offline - Buffer"); lcd.setCursor(0,1); lcd.print("dang luu du lieu"); }
    }
    WiFi.reconnect();
    Serial.println(F("[WiFi] Đang thử kết nối lại..."));
  } else if (wasOffline) {
    wasOffline = false;
    Serial.println(F("[WiFi] Đã kết nối lại!"));
    if (lcdOk) { lcd.clear(); lcd.print("WiFi restored!"); }

    char msg[300];
    snprintf(msg, sizeof(msg),
      "✅ <b>ESP32 đã kết nối lại WiFi!</b>\n"
      "━━━━━━━━━━━━━━━━\n"
      "🕐 Lúc: %s\n"
      "📦 Đang đẩy <b>%d bản ghi</b> offline lên Firebase...",
      timeStr(), bufferCount);
    tgSend(msg);

    lastFlushBuf = 0;
  }
}

// ══════════════════════════════════════════════════════════════
// BUFFER OFFLINE
// ══════════════════════════════════════════════════════════════

void bufferSave(float tds, float turb) {
  if (bufferCount >= OFFLINE_BUFFER_SIZE) {
    Serial.println(F("[Buffer] Đầy! Ghi đè bản ghi cũ nhất"));
  } else {
    bufferCount++;
  }
  SensorRecord& r  = offlineBuffer[bufferHead];
  r.tds   = tds;
  r.turb  = turb;
  r.pump  = pump;
  r.light = light;
  r.ts    = getTS();
  strncpy(r.t, timeStr(), sizeof(r.t) - 1);
  bufferHead = (bufferHead + 1) % OFFLINE_BUFFER_SIZE;
  Serial.printf("[Buffer] Lưu: TDS=%.1f Turb=%.1f (%d/%d)\n",
    tds, turb, bufferCount, OFFLINE_BUFFER_SIZE);
}

void bufferFlush() {
  if (bufferCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  int oldest = (bufferHead - bufferCount + OFFLINE_BUFFER_SIZE) % OFFLINE_BUFFER_SIZE;
  SensorRecord& r = offlineBuffer[oldest];
  char hPath[36], hJson[200];
  snprintf(hPath, sizeof(hPath), "/History/%lu", r.ts);
  snprintf(hJson, sizeof(hJson),
    "{\"tds\":%.1f,\"turb\":%.1f,\"pump\":%s,\"light\":%s,"
    "\"ts\":%lu,\"t\":\"%s\",\"offline\":true}",
    r.tds, r.turb,
    r.pump  ? "true" : "false",
    r.light ? "true" : "false",
    r.ts, r.t);
  if (fbPutJson(hPath, hJson)) {
    bufferCount--;
    Serial.printf("[Buffer] Đã đẩy bản ghi ts=%lu, còn lại: %d\n", r.ts, bufferCount);
  }
  
  if (bufferCount == 0) {
    Serial.println(F("[Buffer] Flush hoàn tất!"));
    char msg[150];
    snprintf(msg, sizeof(msg),
      "📤 <b>Đã đồng bộ xong dữ liệu offline!</b>\n🕐 %s", timeStr());
    tgSend(msg);
  }
}

// ══════════════════════════════════════════════════════════════
// ĐỌC CONFIG TỪ FIREBASE VÀ KÍCH HOẠT OTA
// ══════════════════════════════════════════════════════════════

void readControl() {
  if (millis() - lastReadCtrl < READ_CONTROL_MS) return;
  lastReadCtrl = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  String body;
  if (!fbGetJson("/config", body)) return;

  StaticJsonDocument<768> doc; 
  if (deserializeJson(doc, body)) return;
  ctrlMode   = doc["ControlMode"]  | ctrlMode;
  lightMode  = doc["LightMode"]    | lightMode;
  onHour     = doc["onHour"]       | onHour;
  offHour    = doc["offHour"]      | offHour;
  tdsTh      = doc["tdsTh"]        | tdsTh;
  turbTh     = doc["turbTh"]       | turbTh;
  if (doc.containsKey("notifMuted")) {
    notifMuted = doc["notifMuted"].as<bool>();
  }

  if (doc.containsKey("otaUrl") && doc.containsKey("otaVersion")) {
    String otaVer = doc["otaVersion"].as<String>();
    String otaUrl = doc["otaUrl"].as<String>();
    // Lệnh phát hiện phiên bản mới từ giao diện Web
    if (otaVer != currentVersion && otaUrl.startsWith("https")) {
      performOTA(otaUrl);
    }
  }
}

// ══════════════════════════════════════════════════════════════
// ĐIỀU KHIỂN PUMP & LIGHT
// ══════════════════════════════════════════════════════════════

void updatePump(float tds, float turb) {
  if      (ctrlMode == 1) pump = true;
  else if (ctrlMode == 2) pump = false;
  else                    pump = (tds > tdsTh || turb > turbTh);
  digitalWrite(PIN_PUMP, pump);
}

void updateLight() {
  if      (lightMode == 1) light = true;
  else if (lightMode == 2) light = false;
  else                     light = lightSched();
  digitalWrite(PIN_LIGHT, light);
}

// ══════════════════════════════════════════════════════════════
// GỬI DỮ LIỆU LÊN FIREBASE
// ══════════════════════════════════════════════════════════════

void sendRealtime(float tds, float turb) {
  char json[380];
  snprintf(json, sizeof(json),
    "{\"TDS\":%.1f,\"Turbidity\":%.1f,"
    "\"PumpStatus\":%s,\"LightStatus\":%s,"
    "\"ControlMode\":%d,\"LightMode\":%d,"
    "\"Timestamp\":%lu,\"TimeStr\":\"%s\","
    "\"sensorOk\":true,\"offline\":%s}",
    tds, turb,
    pump  ? "true" : "false",
    light ? "true" : "false",
    ctrlMode, lightMode,
    getTS(), timeStr(),
    wasOffline ? "true" : "false");
  fbPutJson("/realtime", json);
}

void sendHistory(float tds, float turb) {
  char hPath[32], hJson[180];
  unsigned long t = getTS();
  snprintf(hPath, sizeof(hPath), "/History/%lu", t);
  snprintf(hJson, sizeof(hJson),
    "{\"tds\":%.1f,\"turb\":%.1f,\"pump\":%s,\"light\":%s,"
    "\"ts\":%lu,\"t\":\"%s\",\"offline\":false}",
    tds, turb,
    pump  ? "true" : "false",
    light ? "true" : "false",
    t, timeStr());
  fbPutJson(hPath, hJson);
}

// ══════════════════════════════════════════════════════════════
// CẢNH BÁO TELEGRAM
// ══════════════════════════════════════════════════════════════

void checkAlerts(float tds, float turb) {
  unsigned long now = millis();
  char msg[300];

  if (tds > tdsTh) {
    if (!notifMuted && (!alTDS || now - tAlTDS > ALERT_COOLDOWN_MS)) {
      snprintf(msg, sizeof(msg),
        "🚨 <b>CẢNH BÁO BỂ CÁ!</b>\n"
        "━━━━━━━━━━━━━━━━\n"
        "📊 <b>TDS vượt ngưỡng!</b>\n"
        "📈 Hiện tại: <b>%.0f ppm</b> | Ngưỡng: %.0f ppm\n"
        "🕐 Lúc: %s\n"
        "💡 Kiểm tra và thay nước!\n"
        "🔕 Tắt thông báo trên Web để ngừng nhận tin.",
        tds, tdsTh, timeStr());
      tgSend(msg);
      tAlTDS = now; alTDS = true;
    }
  } else if (alTDS) {
    alTDS = false;
    if (!notifMuted) {
      snprintf(msg, sizeof(msg),
        "✅ <b>TDS đã về bình thường</b>\n"
        "📊 Hiện tại: <b>%.0f ppm</b> (ngưỡng %.0f ppm)\n"
        "🕐 Lúc: %s\n",
        tds, tdsTh, timeStr());
      tgSend(msg);
    }
  }

  if (turb > turbTh) {
    if (!notifMuted && (!alTurb || now - tAlTurb > ALERT_COOLDOWN_MS)) {
      snprintf(msg, sizeof(msg),
        "🚨 <b>CẢNH BÁO BỂ CÁ!</b>\n"
        "━━━━━━━━━━━━━━━━\n"
        "🌊 <b>Độ đục vượt ngưỡng!</b>\n"
        "📈 Hiện tại: <b>%.0f%%</b> | Ngưỡng: %.0f%%\n"
        "🕐 Lúc: %s\n"
        "💡 Bật máy bơm lọc nước!\n"
        "🔕 Tắt thông báo trên Web để ngừng nhận tin.",
        turb, turbTh, timeStr());
      tgSend(msg);
      tAlTurb = now; alTurb = true;
    }
  } else if (alTurb) {
    alTurb = false;
    if (!notifMuted) {
      snprintf(msg, sizeof(msg),
        "✅ <b>Độ đục đã về bình thường</b>\n"
        "🌊 Hiện tại: <b>%.0f%%</b> (ngưỡng %.0f%%)\n"
        "🕐 Lúc: %s\n",
        turb, turbTh, timeStr());
      tgSend(msg);
    }
  }
}

// ══════════════════════════════════════════════════════════════
// LCD
// ══════════════════════════════════════════════════════════════

void showLCD(float tds, float turb) {
  if (!lcdOk) return;
  char row0[17], row1[17];
  snprintf(row0, sizeof(row0), "TDS:%-4d Tu:%-3d%%", (int)tds, (int)turb);
  const char* wifiSt = (WiFi.status() == WL_CONNECTED) ? "OK" : "--";
  snprintf(row1, sizeof(row1), "B:%-3s L:%-3s W:%s", 
           pump ? "ON" : "OFF", 
           light ? "ON" : "OFF", 
           wifiSt);
  lcd.setCursor(0, 0); lcd.print(row0);
  lcd.setCursor(0, 1); lcd.print(row1);
}

// ══════════════════════════════════════════════════════════════
// GHI CONFIG MẶC ĐỊNH LÊN FIREBASE KHI RESET (NẾU CẦN)
// ══════════════════════════════════════════════════════════════

void writeDefaultConfigIfNeeded() {
  String body;
  if (fbGetJson("/config/tdsTh", body)) {
    if (body != "null" && body.length() > 0) {
      Serial.println(F("[Config] Đã có config trên Firebase, không ghi đè"));
      return;
    }
  }
  Serial.println(F("[Config] Chưa có config, ghi mặc định"));
  char json[200];
  snprintf(json, sizeof(json),
    "{\"ControlMode\":0,\"LightMode\":0,"
    "\"onHour\":%d,\"offHour\":%d,"
    "\"tdsTh\":%.1f,\"turbTh\":%.1f,"
    "\"notifMuted\":false}",
    onHour, offHour, tdsTh, turbTh);
  fbPutJson("/config", json);
}

// ══════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n╔══════════════════════════════╗"));
  Serial.println(F("║  BỂ CÁ IoT v4.0.3 KHỞI ĐỘNG  ║"));
  Serial.println(F("╚══════════════════════════════╝"));

  pinMode(PIN_PUMP,  OUTPUT); digitalWrite(PIN_PUMP,  LOW);
  pinMode(PIN_LIGHT, OUTPUT); digitalWrite(PIN_LIGHT, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(0x27);
  lcdOk = (Wire.endTransmission() == 0);
  if (lcdOk) {
    lcd.init(); lcd.backlight();
    lcd.print("Be Ca IoT v4.0.3");
    lcd.setCursor(0,1); lcd.print("Tim WiFi...");
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  bool res = wm.autoConnect("BE_CA_IOT");
  if (!res) {
    Serial.println(F("[WiFi] Lỗi! Khởi động lại..."));
    if (lcdOk) { lcd.clear(); lcd.print("Loi WiFi! Reset"); }
    delay(3000);
    ESP.restart();
  }
  Serial.println("[WiFi] Kết nối thành công! IP: " + WiFi.localIP().toString());
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  int attempt = 0;
  while (!getLocalTime(&t) && attempt++ < 10) delay(500);
  writeDefaultConfigIfNeeded();
  lastReadCtrl = 0;
  readControl();

  char boot[400];
  snprintf(boot, sizeof(boot),
    "🎉 <b>BỂ CÁ IoT v%s ONLINE</b>\n"
    "━━━━━━━━━━━━━━━━\n"
    "🌐 IP: <b>%s</b> | WiFi: %s\n"
    "🕐 Giờ: %s\n"
    "━━━━━━━━━━━━━━━━\n"
    "✅ Sẵn sàng giám sát!",
    currentVersion.c_str(),
    WiFi.localIP().toString().c_str(),
    WiFi.SSID().c_str(),
    timeStr());
  tgSend(boot);

  if (lcdOk) { delay(1500); lcd.clear(); lcd.print("San sang!"); }
}

// ══════════════════════════════════════════════════════════════
// LOOP CHÍNH
// ══════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();
  wifiCheck();

  if (!wasOffline && bufferCount > 0 && WiFi.status() == WL_CONNECTED) {
    if (now - lastFlushBuf >= 500) {
      lastFlushBuf = now;
      bufferFlush();
    }
  }

  if (now - lastSendRT < SEND_REALTIME_MS) return;
  lastSendRT = millis();
  int rawTDS  = adcAvg(PIN_TDS);
  int rawTurb = adcAvg(PIN_TURB);

  float currentTDS = toTDS(rawTDS);
  float currentTurb = toTurb(rawTurb);
  
  if (lastTDS == 0 && lastTurb == 0) {
    lastTDS = currentTDS;
    lastTurb = currentTurb;
  } else {
    // Low-pass filter (Exponential Moving Average)
    lastTDS = 0.8 * lastTDS + 0.2 * currentTDS;
    lastTurb = 0.8 * lastTurb + 0.2 * currentTurb;
  }

  Serial.printf("[Sensor] TDS raw=%d → %.1fppm | Turb raw=%d → %.1f%%\n",
    rawTDS, lastTDS, rawTurb, lastTurb);
  readControl();
  updatePump(lastTDS, lastTurb);
  updateLight();

  if (WiFi.status() == WL_CONNECTED && !wasOffline) {
    sendRealtime(lastTDS, lastTurb);
    if (now - lastSendHist >= SEND_HISTORY_MS) {
      lastSendHist = millis();
      sendHistory(lastTDS, lastTurb);
    }
    checkAlerts(lastTDS, lastTurb);
  } else {
    if (now - lastSendHist >= SEND_HISTORY_MS) {
      lastSendHist = millis();
      bufferSave(lastTDS, lastTurb);
    }
  }

  showLCD(lastTDS, lastTurb);
}