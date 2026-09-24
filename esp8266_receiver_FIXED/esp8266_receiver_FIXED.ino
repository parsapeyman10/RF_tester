/*
  Project: ESP8266 Robust Industrial AP Receiver - Full Traffic Monitoring
  Description: Port 80, Synchronized with specific snprintf format and OK acknowledgement.
  Engineer: Peyman Parsa
*/

#include <ESP8266WiFi.h>

// =====================================================================
// حالت دیباگ - طبق مستندات فنی: در تولید (Production) باید false باشد
// تا هیچ پرینت اضافه‌ای غیر از خط داده استاندارد روی سریال ارسال نشود
// (این یک منبع باگ جدی بود: پرینت‌های خام قبلی باعث دوبار ثبت شدن
//  هر رکورد در دیتابیس Flask می‌شدند چون همان کلیدواژه NUM= را هم در
//  خط اکو خام و هم در خط [LINE RECV] تکرار می‌کردند)
// =====================================================================
#define DEBUG_ENABLE false

#if DEBUG_ENABLE
#define DBG_PRINT(x) Serial.print(x)
#define DBG_PRINTLN(x) Serial.println(x)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(x)
#define DBG_PRINTLN(x)
#define DBG_PRINTF(...)
#endif

// تنظیمات شبکه و ارتباطی
const char* SSID_NAME = "ESP8266_AP";
const char* PASSWORD  = "12345678";
const int   SERVER_PORT = 80; // پورت به 80 تغییر یافت
const long  SERIAL_BAUD = 115200;
const uint8_t AP_INIT_RETRY = 5;      // طبق مستندات فنی: ۵ بار تلاش مجدد
const unsigned long AP_RETRY_DELAY_MS = 1000;

// ظرفیت بافر ورودی
const int RX_BUFFER_SIZE = 512;
char rxBuffer[RX_BUFFER_SIZE];
int rxIndex = 0;

// ساختار داده‌ای صنعتی
struct WifiData {
    int      NUM;
    float    Temp;
    float    Hum;
    bool     NBCM1, NBCM2, NBCM3, NBCM4;
    uint16_t Year;
    uint8_t  Month, Day, Hour, Minute, Second;
};

WifiData WData;
WiFiServer server(SERVER_PORT);
WiFiClient currentClient;

bool isClientConnected = false;
unsigned long lastClientActivity = 0;
bool lastLineWasEvt = false;
char lastEvtLine[160];

// پروتوتایپ توابع
void initAccessPoint();
bool parseData(char* inputBuffer);
void sendDataToComputer();
void clearRxBuffer();
void stopClient(const char* reason);
bool strToBool(const char* str);

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);

  DBG_PRINTLN("\n\n========================================");
  DBG_PRINTLN("SYSTEM BOOTING... (PORT 80 ACTIVE)");
  DBG_PRINTLN("========================================");

  initAccessPoint();
  server.begin();
  server.setNoDelay(true);

  DBG_PRINT("Server Started on Port: ");
  DBG_PRINTLN(SERVER_PORT);
  DBG_PRINTLN("Monitoring every incoming byte...");
  DBG_PRINTLN("========================================\n");
}

void loop() {
  // مدیریت اتصال کلاینت جدید
  if (server.hasClient()) {
    if (currentClient && currentClient.connected()) {
      currentClient.stop();
    }
    currentClient = server.available();
    isClientConnected = true;
    lastClientActivity = millis();
    clearRxBuffer();
    DBG_PRINTLN("\n[SYSTEM] New client connected.");
  }

  // پردازش داده‌های دریافتی
  if (isClientConnected && currentClient.connected()) {
    if (currentClient.available() > 0) {
      // نکته حیاتی: اکوی کاراکتر به کاراکتر روی سریال حذف شد.
      // این اکو در نسخه قبلی چون خودش هم شامل رشته کامل NUM=... می‌شد،
      // باعث می‌شد سرور Flask هر رکورد را دو بار (یک بار از خط اکوی خام
      // و یک بار از خط [LINE RECV]) در دیتابیس ثبت کند. اکنون فقط در
      // حالت دیباگ و بدون آلوده کردن پارسر سریال چاپ می‌شود.
      while (currentClient.available() > 0) {
        char c = currentClient.read();

#if DEBUG_ENABLE
        Serial.write(c); // اکوی خام فقط در حالت دیباگ
#endif

        if (c == '\n') {
          rxBuffer[rxIndex] = '\0';

          if (rxIndex > 0) {
            DBG_PRINTLN();
            DBG_PRINT("[LINE RECV]: ");
            DBG_PRINTLN(rxBuffer);

            // تحلیل دیتا و بررسی مطابقت با فرمت درخواستی
            if (parseData(rxBuffer)) {
              // ارسال تاییدیه OK به فرستنده (ESP32)
              currentClient.println("OK");
              DBG_PRINTLN("[RESPONSE]: Sent 'OK' to Client (Handshake Complete)");
              // تنها خروجی غیرمشروط به کامپیوتر: همیشه چاپ می‌شود
              // چون Flask دقیقاً منتظر همین یک خط با فرمت NUM=... است
              sendDataToComputer();
            } else {
              // در صورت عدم تطابق فرمت
              currentClient.println("ERR:FORMAT");
              DBG_PRINTLN("[RESPONSE]: Sent 'ERR:FORMAT' to Client");
            }
          }
          clearRxBuffer();
          lastClientActivity = millis();
        } else if (c != '\r' && rxIndex < RX_BUFFER_SIZE - 1) {
          rxBuffer[rxIndex++] = c;
        }
      }
    }

    // بررسی Timeout - طبق مستندات فنی دقیقاً ۱۰ ثانیه (قبلاً به اشتباه ۱۵ ثانیه بود)
    if (millis() - lastClientActivity > 10000) {
      stopClient("Inactivity Timeout (10s)");
    }
  }
  else if (isClientConnected) {
    stopClient("Physical Disconnect");
  }

  yield();
}

void initAccessPoint() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP);

  bool apOK = false;
  for (uint8_t attempt = 1; attempt <= AP_INIT_RETRY; attempt++) {
    if (WiFi.softAP(SSID_NAME, PASSWORD)) {
      apOK = true;
      DBG_PRINTF("AP READY - SSID: %s | IP: %s\n", SSID_NAME, WiFi.softAPIP().toString().c_str());
      break;
    }
    DBG_PRINTF("[AP] Attempt %d/%d failed, retrying...\n", attempt, AP_INIT_RETRY);
    delay(AP_RETRY_DELAY_MS);
  }

  // طبق مستندات فنی: در صورت شکست نهایی، ریست سخت‌افزاری برای بازگشت به حالت پایدار
  if (!apOK) {
    DBG_PRINTLN("[AP] Critical: Failed after all retries. Restarting device...");
    delay(200);
    ESP.restart();
  }
}

void stopClient(const char* reason) {
  if (isClientConnected) {
    currentClient.stop();
    isClientConnected = false;
    DBG_PRINT("\n[SYSTEM] Connection Closed. Reason: ");
    DBG_PRINTLN(reason);
  }
}

void clearRxBuffer() { 
  rxIndex = 0; 
  rxBuffer[0] = '\0'; 
}

bool strToBool(const char* str) {
  return (strcmp(str, "1") == 0 || strcmp(str, "OK") == 0 || strcmp(str, "true") == 0);
}

bool parseData(char* inputBuffer) {
  // رویداد سوییچ: EVT,CH=...,STATE=...,Date=...,Time=...
  // ACK می‌دهیم و خط خام را برای Flask نگه می‌داریم
  if (strncmp(inputBuffer, "EVT,", 4) == 0) {
    lastLineWasEvt = true;
    strncpy(lastEvtLine, inputBuffer, sizeof(lastEvtLine) - 1);
    lastEvtLine[sizeof(lastEvtLine) - 1] = '\0';
    return true;
  }

  char bcm1[10], bcm2[10], bcm3[10], bcm4[10];
  char tStr[15], hStr[15];
  int num, yr, mon, day, hr, min, sec;

  // تطابق کامل با فرمت snprintf ارسالی شما
  int itemsParsed = sscanf(inputBuffer,
    "NUM=%d,NBCM1=%9[^,],NBCM2=%9[^,],NBCM3=%9[^,],NBCM4=%9[^,],Temp=%14[^,],Humidity=%14[^,],Date=%d-%d-%d,Time=%d:%d:%d",
    &num, bcm1, bcm2, bcm3, bcm4, tStr, hStr, &yr, &mon, &day, &hr, &min, &sec
  );

  if (itemsParsed == 13) {
    lastLineWasEvt = false;
    WData.NUM = num;
    WData.NBCM1 = strToBool(bcm1);
    WData.NBCM2 = strToBool(bcm2);
    WData.NBCM3 = strToBool(bcm3);
    WData.NBCM4 = strToBool(bcm4);
    WData.Temp = atof(tStr); 
    WData.Hum = atof(hStr);
    WData.Year = yr; WData.Month = mon; WData.Day = day;
    WData.Hour = hr; WData.Minute = min; WData.Second = sec;
    return true;
  }
  return false;
}

// =====================================================================
// این تابع تنها پل ارتباطی بین ESP8266 و سرور Flask (روی سریال) است.
// نسخه‌ی قبلی این خط را به فرمت انسان‌خوان "[LOG]: ID:.. | BCMs:.."
// می‌فرستاد که هیچ‌گاه با پارسر Flask (که دنبال کلیدهای
// NUM=,NBCM1=,...,Date=,Time= می‌گردد) مطابقت نداشت؛ یعنی داده هرگز
// وارد دیتابیس نمی‌شد. اکنون دقیقاً همان فرمت استاندارد پروژه ارسال
// می‌شود که هم با sscanf ورودی و هم با parse_industrial_line در app.py
// سازگار است.
// =====================================================================
void sendDataToComputer() {
  if (lastLineWasEvt) {
    // عبور خط رویداد به Flask همان‌طور که آمد
    Serial.println(lastEvtLine);
    return;
  }
  Serial.printf(
    "NUM=%d,NBCM1=%s,NBCM2=%s,NBCM3=%s,NBCM4=%s,Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d\n",
    WData.NUM,
    WData.NBCM1 ? "OK" : "NOK",
    WData.NBCM2 ? "OK" : "NOK",
    WData.NBCM3 ? "OK" : "NOK",
    WData.NBCM4 ? "OK" : "NOK",
    WData.Temp, WData.Hum,
    WData.Year, WData.Month, WData.Day,
    WData.Hour, WData.Minute, WData.Second
  );
}
