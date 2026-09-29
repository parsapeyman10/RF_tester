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

// =====================================================================
// خطوط وضعیت (سلامت، اتصال کلاینت، خطاها) مستقل از حالت دیباگ چاپ می‌شوند.
// این‌ها با کلیدواژه‌ی NUM= شروع نمی‌شوند، پس پارسر سخت‌گیرِ app.py آن‌ها را
// نادیده می‌گیرد و هیچ رکورد اضافه‌ای ثبت نمی‌شود — ولی شما همیشه می‌بینید
// که گیرنده در چه حالی است. برای سکوت کامل، false کنید.
// =====================================================================
#define STATUS_ENABLE true

#if STATUS_ENABLE
#define ST_PRINTLN(x) Serial.println(x)
#define ST_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define ST_PRINTLN(x)
#define ST_PRINTF(...)
#endif

#define FW_VERSION "2.0"

// تنظیمات شبکه و ارتباطی
const char* SSID_NAME = "ESP8266_AP";
const char* PASSWORD  = "12345678";
const int   SERVER_PORT = 80; // پورت به 80 تغییر یافت
const long  SERIAL_BAUD = 115200;
const uint8_t AP_INIT_RETRY = 5;      // طبق مستندات فنی: ۵ بار تلاش مجدد
const unsigned long AP_RETRY_DELAY_MS = 1000;

// --- پایداری اکسس‌پوینت ---
const uint8_t  AP_CHANNEL = 6;            // کانال ثابت (کمترین تداخل معمول)
const uint8_t  AP_MAX_CLIENTS = 8;        // سخت‌گیری بی‌دلیل نکنیم
// توان خروجی: حداکثرِ ۲۰.۵ وقتی دو برد کنار هم روی میز هستند باعث اشباع
// گیرنده‌ی طرف مقابل و خراب شدن فریم‌های EAPOL می‌شود؛ نتیجه‌اش دقیقاً
// «4WAY_HANDSHAKE_TIMEOUT» است. ۱۴ dBm برای چند ده متر کافی است.
const float    AP_TX_POWER = 14.0;

// فقط برای عیب‌یابی: اکسس‌پوینت را بدون رمز بالا می‌آورد.
// اگر با این حالت ESP32 وصل شد، مشکل از احراز هویت/رمز است؛
// اگر باز هم وصل نشد، مشکل رادیویی یا تغذیه است.
// (در این حالت باید DATA_AP_PASS در اسکچ ESP32 هم خالی شود)
#define AP_OPEN_TEST false
const unsigned long CLIENT_IDLE_TIMEOUT_MS = 60000;   // قبلاً ۱۰ ثانیه بود
const unsigned long AP_HEALTH_PERIOD_MS = 30000;      // گزارش سلامت هر ۳۰ ثانیه
const uint32_t LOW_HEAP_LIMIT = 6000;                 // آستانه‌ی حافظه‌ی بحرانی

// ظرفیت بافر ورودی
const int RX_BUFFER_SIZE = 512;
char rxBuffer[RX_BUFFER_SIZE];
int rxIndex = 0;
bool overflowed = false;   // خط ورودی از ظرفیت بافر رد شد؟

// ساختار داده‌ای صنعتی
struct WifiData {
    int      NUM;
    float    Temp;
    float    Hum;
    // چهار نتیجه‌ی تفکیکی: باز/بسته برای هر یک از دو دستگاه
    bool     BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE;
    uint16_t Year;
    uint8_t  Month, Day, Hour, Minute, Second;
};

WifiData WData;
WiFiServer server(SERVER_PORT);
WiFiClient currentClient;

bool isClientConnected = false;
unsigned long lastClientActivity = 0;

// --- آمار کارکرد (مثل سمت ESP32) ---
struct ReceiverStats {
  uint32_t linesOk = 0;        // رکوردهای معتبر تحویل‌شده به کامپیوتر
  uint32_t linesBad = 0;       // خطوطی که فرمتشان درست نبود
  uint32_t pings = 0;          // keep-alive های پاسخ داده‌شده
  uint32_t sessions = 0;       // چند بار کلاینت وصل شده
  uint32_t apRestarts = 0;     // چند بار AP بازسازی شده
};
ReceiverStats stats;

// هندلرهای رویداد اکسس‌پوینت (باید سراسری بمانند)
WiFiEventHandler onStationConnectedHandler;
WiFiEventHandler onStationDisconnectedHandler;

static String macToString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// پروتوتایپ توابع
void initAccessPoint();
bool parseData(char* inputBuffer);
void sendDataToComputer();
void clearRxBuffer();
void stopClient(const char* reason);
bool strToBool(const char* str);

/** وقتی ESP32 (یا هر کلاینتی) به اکسس‌پوینت می‌پیوندد */
void handleStationConnected(const WiFiEventSoftAPModeStationConnected &evt) {
  stats.sessions++;
  ST_PRINTF("[AP] کلاینت وصل شد: %s (مجموع نشست‌ها: %u)\n",
            macToString(evt.mac).c_str(), stats.sessions);
}

void handleStationDisconnected(const WiFiEventSoftAPModeStationDisconnected &evt) {
  ST_PRINTF("[AP] کلاینت جدا شد: %s\n", macToString(evt.mac).c_str());
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);

  ST_PRINTF("\n[BOOT] ESP8266 Receiver FW %s\n", FW_VERSION);

  onStationConnectedHandler = WiFi.onSoftAPModeStationConnected(&handleStationConnected);
  onStationDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected(&handleStationDisconnected);

  DBG_PRINTLN("\n\n========================================");
  DBG_PRINTLN("SYSTEM BOOTING... (PORT 80 ACTIVE)");
  DBG_PRINTLN("========================================");

  initAccessPoint();
  server.begin();
  server.setNoDelay(true);

  ST_PRINTF("[BOOT] AP='%s' ch=%u port=%d  |  آماده‌ی دریافت\n",
            SSID_NAME, AP_CHANNEL, SERVER_PORT);
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

          if (overflowed) {
            currentClient.println("ERR:TOOLONG");
            DBG_PRINTLN("[RESPONSE]: line too long, discarded");
          } else if (rxIndex > 0) {
            DBG_PRINTLN();
            DBG_PRINT("[LINE RECV]: ");
            DBG_PRINTLN(rxBuffer);

            // Keep-alive: ESP32 وقتی مدتی دیتا ندارد PING می‌فرستد تا
            // سوکت زنده بماند. نباید به‌عنوان خطای فرمت جواب داده شود.
            if (strcmp(rxBuffer, "PING") == 0) {
              currentClient.println("PONG");
              stats.pings++;
            }
            // تحلیل دیتا و بررسی مطابقت با فرمت درخواستی
            else if (parseData(rxBuffer)) {
              // ارسال تاییدیه OK به فرستنده (ESP32)
              currentClient.println("OK");
              stats.linesOk++;
              DBG_PRINTLN("[RESPONSE]: Sent 'OK' to Client (Handshake Complete)");
              // تنها خروجی غیرمشروط به کامپیوتر: همیشه چاپ می‌شود
              // چون Flask دقیقاً منتظر همین یک خط با فرمت NUM=... است
              sendDataToComputer();
            } else {
              // در صورت عدم تطابق فرمت
              currentClient.println("ERR:FORMAT");
              stats.linesBad++;
              ST_PRINTF("[WARN] خط با فرمت نامعتبر رد شد (مجموع: %u)\n", stats.linesBad);
            }
          }
          clearRxBuffer();
          lastClientActivity = millis();
        } else if (c != '\r') {
          if (rxIndex < RX_BUFFER_SIZE - 1) {
            rxBuffer[rxIndex++] = c;
          } else {
            // سرریز بافر: خط را باطل کن تا نیمه‌کاره پارس نشود
            overflowed = true;
          }
        }
      }
    }

    // بستن اتصالِ بی‌کار. قبلاً ۱۰ ثانیه بود و چون ESP32 بین دو سیکل حدود
    // دو دقیقه ساکت است، هر بار سوکت بسته می‌شد و دوباره وصل می‌شد؛ همین
    // «قطع و وصل شدن مداوم» را می‌ساخت. حالا اتصال بازِ بی‌کار حفظ می‌شود.
    if (millis() - lastClientActivity > CLIENT_IDLE_TIMEOUT_MS) {
      stopClient("Inactivity Timeout");
    }
  }
  else if (isClientConnected) {
    stopClient("Physical Disconnect");
  }

  // =====================================================================
  //  نگهداری و گزارش سلامت (هم‌تراز با ناظر سلامت ESP32)
  // =====================================================================
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > AP_HEALTH_PERIOD_MS) {
    lastHealth = millis();
    uint8_t stations = WiFi.softAPgetStationNum();
    uint32_t heap = ESP.getFreeHeap();

    ST_PRINTF("[HEALTH] ssid=%s ch=%u clients=%u | ok=%u bad=%u ping=%u "
              "sessions=%u apRst=%u | heap=%u up=%lus\n",
              SSID_NAME, WiFi.channel(), stations,
              stats.linesOk, stats.linesBad, stats.pings,
              stats.sessions, stats.apRestarts,
              heap, millis() / 1000);

    // ۱) اگر اکسس‌پوینت پایین آمده باشد، دوباره بالا می‌آید
    if (WiFi.getMode() != WIFI_AP || WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
      ST_PRINTLN("[HEALTH] AP پایین است -> راه‌اندازی مجدد");
      stats.apRestarts++;
      initAccessPoint();
    }

    // ۲) گاهی SoftAP بدون اینکه پایین بیاید دیگر کسی را نمی‌پذیرد
    static uint8_t noClientRounds = 0;
    if (stations == 0) {
      noClientRounds++;
      if (noClientRounds >= 10) {   // ۵ دقیقه
        ST_PRINTLN("[HEALTH] ۵ دقیقه بدون کلاینت -> AP تازه‌سازی می‌شود");
        noClientRounds = 0;
        stats.apRestarts++;
        initAccessPoint();
      }
    } else {
      noClientRounds = 0;
    }

    // ۳) محافظ حافظه: با هیپ خیلی کم، پشته‌ی شبکه ناپایدار می‌شود
    if (heap < LOW_HEAP_LIMIT) {
      ST_PRINTF("[HEALTH] حافظه بحرانی (%u) -> ریست کنترل‌شده\n", heap);
      delay(200);
      ESP.restart();
    }
  }

  yield();
}

void initAccessPoint() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP);

  // مهم‌ترین تنظیمات پایداری:
  //  - خاموش کردن حالت خواب مودم (منبع اصلی قطع و وصل شدن‌های لحظه‌ای)
  //  - توان خروجی کامل
  //  - کانال ثابت به‌جای انتخاب خودکار
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(AP_TX_POWER);
  // توجه: setPhyMode(11N) عمداً حذف شد. اجبار AP به حالت N باعث می‌شود
  // بعضی کلاینت‌ها (از جمله ESP32 در شرایط خاص) اصلاً associate نشوند.
  // حالت پیش‌فرض b/g/n سازگارترین است.
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                    IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));

  bool apOK = false;
  for (uint8_t attempt = 1; attempt <= AP_INIT_RETRY; attempt++) {
#if AP_OPEN_TEST
    bool started = WiFi.softAP(SSID_NAME, nullptr, AP_CHANNEL, false, AP_MAX_CLIENTS);
    ST_PRINTLN("[AP] حالت عیب‌یابی: اکسس‌پوینت بدون رمز بالا آمد");
#else
    bool started = WiFi.softAP(SSID_NAME, PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);
#endif
    if (started) {
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
  overflowed = false;
}

bool strToBool(const char* str) {
  return (strcmp(str, "1") == 0 || strcmp(str, "OK") == 0 || strcmp(str, "true") == 0);
}

bool parseData(char* inputBuffer) {
  char bcm1[10], bcm2[10], bcm3[10], bcm4[10];
  char tStr[15], hStr[15];
  int num, yr, mon, day, hr, min, sec;

  // تطابق کامل با فرمت snprintf ارسالی شما
  int itemsParsed = sscanf(inputBuffer, 
    "NUM=%d,BCM1_OPEN=%9[^,],BCM1_CLOSE=%9[^,],BCM2_OPEN=%9[^,],BCM2_CLOSE=%9[^,],"
    "Temp=%14[^,],Humidity=%14[^,],Date=%d-%d-%d,Time=%d:%d:%d",
    &num, bcm1, bcm2, bcm3, bcm4, tStr, hStr, &yr, &mon, &day, &hr, &min, &sec
  );

  if (itemsParsed == 13) {
    WData.NUM = num;
    WData.BCM1_OPEN  = strToBool(bcm1);
    WData.BCM1_CLOSE = strToBool(bcm2);
    WData.BCM2_OPEN  = strToBool(bcm3);
    WData.BCM2_CLOSE = strToBool(bcm4);
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
// NUM=,BCM1_OPEN=,...,Date=,Time= می‌گردد) مطابقت نداشت؛ یعنی داده هرگز
// وارد دیتابیس نمی‌شد. اکنون دقیقاً همان فرمت استاندارد پروژه ارسال
// می‌شود که هم با sscanf ورودی و هم با parse_industrial_line در app.py
// سازگار است.
// =====================================================================
void sendDataToComputer() {
  Serial.printf(
    "NUM=%d,BCM1_OPEN=%s,BCM1_CLOSE=%s,BCM2_OPEN=%s,BCM2_CLOSE=%s,"
    "Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d\n",
    WData.NUM,
    WData.BCM1_OPEN ? "OK" : "NOK",
    WData.BCM1_CLOSE ? "OK" : "NOK",
    WData.BCM2_OPEN ? "OK" : "NOK",
    WData.BCM2_CLOSE ? "OK" : "NOK",
    WData.Temp, WData.Hum,
    WData.Year, WData.Month, WData.Day,
    WData.Hour, WData.Minute, WData.Second
  );
}
