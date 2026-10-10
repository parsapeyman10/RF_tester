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

#define FW_VERSION "2.1"

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
// توان خروجی اکسس‌پوینت. اگر در لاگ ESP32 مقدار rssi بدتر از -70 دیدید،
// یعنی فاصله زیاد است و باید روی حداکثر (۲۰.۵) بماند. فقط وقتی بردها
// چسبیده به هم‌اند و هندشیک خراب می‌شود، ۱۲ تا ۱۴ بگذارید.
const float    AP_TX_POWER = 20.5;

// فقط برای عیب‌یابی: اکسس‌پوینت را بدون رمز بالا می‌آورد.
// اگر با این حالت ESP32 وصل شد، مشکل از احراز هویت/رمز است؛
// اگر باز هم وصل نشد، مشکل رادیویی یا تغذیه است.
// (در این حالت باید DATA_AP_PASS در اسکچ ESP32 هم خالی شود)
#define AP_OPEN_TEST false
const unsigned long CLIENT_IDLE_TIMEOUT_MS = 60000;   // قبلاً ۱۰ ثانیه بود
const unsigned long AP_HEALTH_PERIOD_MS = 30000;      // گزارش سلامت هر ۳۰ ثانیه
const uint32_t LOW_HEAP_LIMIT = 6000;                 // آستانه‌ی حافظه‌ی بحرانی

// اگر کلاینتی در جدول اکسس‌پوینت ثبت شده ولی این مدت هیچ داده‌ای نفرستاده،
// آن ثبت «مرده» است. تلنبار شدن این‌ها باعث می‌شود AP پیام ASSOC_TOOMANY
// بدهد و کلاینت واقعی دیگر نتواند وصل شود؛ پس AP بازسازی می‌شود.
const unsigned long STALE_SESSION_MS = 120000;        // ۲ دقیقه

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
    // تعداد تلاش‌هایی که ESP32 برای این سیکل طول کشید (۱، ۲ یا ۳)؛
    // از فیلد «cycle=» خط دریافتی می‌آید و عیناً فوروارد می‌شود
    uint8_t  CycleAttempt;
    // دو سیگنال دیجیتال اضافی (GPIO34=Indicator, GPIO35=Buzzer روی ESP32)؛
    // از فیلدهای «Indicator=» و «Buzzer=» خط دریافتی می‌آیند و عیناً
    // فوروارد می‌شوند. اگر فریمور فرستنده قدیمی‌تر باشد و این فیلدها را
    // نفرستد، مقدار پیش‌فرض NOK/false در نظر گرفته می‌شود.
    bool     Indicator;
    bool     Buzzer;
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
  uint32_t acksFwd = 0;        // تاییدیه‌های سرور که به ESP32 برگردانده شد
  uint32_t waitsSent = 0;      // چند بار «صبر کن» گفتیم چون سرور آماده نبود
};
ReceiverStats stats;
unsigned long lastDataMs = 0;      // آخرین باری که دیتای معتبر رسید

// =====================================================================
//  لینک با سرور (پروتکل نسخه ۲ — انتقال مطمئن سه‌مرحله‌ای)
//
//  قواعد زنجیره (سرور <-> ESP8266 <-> ESP32):
//   1) سرور تا وقتی پورت سریالش باز نشده، هیچ درخواست/اعلامی نمی‌دهد.
//      فقط بعد از اتصال خط «SRV_READY <sid>» می‌فرستد (و هر ۱۰ ثانیه
//      تکرارش می‌کند). تا آن لحظه هیچ داده‌ای به کامپیوتر نمی‌رود و
//      هیچ تاییدی به ESP32 داده نمی‌شود.
//   2) <sid> شناسه‌ی نشست است که یکسان در هر سه لایه حرکت می‌کند:
//      همین‌جا ذخیره و با «READY <sid>» به ESP32 اعلام می‌شود.
//   3) تاییدیه‌ی واقعی فقط از سرور می‌آید («ACK <num>») و عیناً به
//      ESP32 برمی‌گردد. یعنی ESP32 فقط وقتی رکوردش را «رسیده» می‌داند
//      که واقعاً در دیتابیس ثبت شده باشد — نه صرفِ رسیدن به این برد.
//   4) اگر سرور برود (ضربان SRV_READY قطع شود) فوروارد متوقف و با
//      «SRV_LOST» به ESP32 اعلام می‌شود؛ همه‌چیز روی حافظه‌ی خود ESP32
//      سالم می‌ماند و بعد از READY جدید دوباره ارسال می‌شود.
// =====================================================================
bool serverReady = false;             // سرور پشت پورت سریال نشسته و آماده است؟
char srvSid[12] = "";                 // شناسه‌ی نشست سرور (خالی = هنوز ندیده‌ایم)
unsigned long lastSrvReadyMs = 0;     // آخرین ضربان SRV_READY
unsigned long lastSrvPingMs = 0;      // آخرین SRV_HELLO / SRV_PING خودمان
const unsigned long SRV_HEARTBEAT_MS = 30000;  // بی‌ضربانی = سرور رفته
const unsigned long SRV_PING_PERIOD_MS = 10000;

// بافر دریافت خطوط سریال از سرور
const int SRV_RX_SIZE = 96;
char srvRx[SRV_RX_SIZE];
int srvRxLen = 0;

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
void handleServerLine(const char* line);
void serviceServerLink();
void notifyClientServerState();

/** وقتی ESP32 (یا هر کلاینتی) به اکسس‌پوینت می‌پیوندد */
void handleStationConnected(const WiFiEventSoftAPModeStationConnected &evt) {
  stats.sessions++;
  lastDataMs = millis();   // به کلاینت تازه فرصت می‌دهیم
  ST_PRINTF("[AP] کلاینت وصل شد: %s (مجموع نشست‌ها: %u)\n",
            macToString(evt.mac).c_str(), stats.sessions);
}

void handleStationDisconnected(const WiFiEventSoftAPModeStationDisconnected &evt) {
  ST_PRINTF("[AP] کلاینت جدا شد: %s\n", macToString(evt.mac).c_str());
}

void setup() {
  // بافر دریافت سریال را قبل از begin بزرگ می‌کنیم: حالا علاوه بر داده،
  // خطوط کنترلی لینک (SRV_READY/ACK) هم از این مسیر می‌آیند
  Serial.setRxBufferSize(512);
  Serial.begin(SERIAL_BAUD);
  delay(1000);

  ST_PRINTF("\n[BOOT] ESP8266 Receiver FW %s\n", FW_VERSION);

  // --- دست‌دادن با سرور (لینک v2) ---
  // خودمان را معرفی می‌کنیم تا اگر سرور از قبل پشت پورت نشسته، بلافاصله
  // SRV_READY و شناسه‌ی نشست را بگیریم. (قاعده ۱: سرور تا پورت سریالش
  // باز نشده هیچ چیزی نمی‌فرستد، پس شروع گفتگو با ماست)
  Serial.println("SRV_HELLO");
  lastSrvPingMs = millis();

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
  // لینک سریال با سرور: خواندن SRV_READY/ACK/SRV_PONG + ضربان و تشخیص قطع
  serviceServerLink();

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
    // بلافاصله به ESP32 می‌گوییم سرور آماده است یا نه تا بی‌جهت نفرستد
    notifyClientServerState();
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
              lastDataMs = millis();
            }
            // پرسش وضعیت از سمت ESP32: «سرور آماده‌ای؟»
            else if (strcmp(rxBuffer, "HELLO") == 0) {
              if (serverReady) {
                currentClient.printf("READY %s\n", srvSid);
              } else {
                currentClient.println("WAIT");
              }
              lastDataMs = millis();
            }
            // تحلیل دیتا و بررسی مطابقت با فرمت درخواستی
            else if (parseData(rxBuffer)) {
              if (!serverReady) {
                // گیت قاعده ۱: سرور پشت پورت نیست. خط به کامپیوتر نمی‌رود
                // و «هیچ» تاییدیه‌ای نمی‌گیرد تا ESP32 بداند باید نگهش دارد
                // و دوباره بفرستد. (پاسخ OK قبلی حذف شد — تایید واقعی فقط
                // ACK سرور است که بعداً فوروارد می‌شود)
                currentClient.println("WAIT");
                stats.waitsSent++;
                lastDataMs = millis();   // کلاینت زنده است؛ فقط سرور غایب است
              } else {
                stats.linesOk++;
                lastDataMs = millis();
                // ارسال به کامپیوتر — تاییدیه به ESP32 «فقط» وقتی برمی‌گردد
                // که سرور رکورد را واقعاً ثبت کرده باشد (ACK از مسیر
                // handleServerLine فوروارد می‌شود)
                sendDataToComputer();
              }
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

    // ۲) نشست‌های مرده: کلاینت در جدول هست ولی داده‌ای نمی‌آید
    if (stations > 0 && lastDataMs > 0 &&
        (millis() - lastDataMs) > STALE_SESSION_MS) {
      ST_PRINTF("[HEALTH] %u نشست مرده (%lu ثانیه بدون داده) -> بازسازی AP\n",
                stations, (millis() - lastDataMs) / 1000);
      stats.apRestarts++;
      lastDataMs = millis();
      initAccessPoint();
    }

    // ۳) گاهی SoftAP بدون اینکه پایین بیاید دیگر کسی را نمی‌پذیرد
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

    // ۴) محافظ حافظه: با هیپ خیلی کم، پشته‌ی شبکه ناپایدار می‌شود
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
      WiFi.setOutputPower(AP_TX_POWER);   // بعضی نسخه‌های SDK بعد از softAP ریست می‌کنند
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
  char indStr[10] = "NOK", buzStr[10] = "NOK";  // اگر فریمور قدیمی‌تر این دو فیلد را نفرستد
  int num, yr, mon, day, hr, min, sec;
  int cyc = 1;   // اگر ESP32 قدیمی بدون فیلد cycle= باشد، پیش‌فرض ۱

  // تطابق کامل با فرمت snprintf ارسالی شما (فیلدهای cycle= و
  // Indicator=/Buzzer= در انتها اضافه شده‌اند)
  int itemsParsed = sscanf(inputBuffer, 
    "NUM=%d,BCM1_OPEN=%9[^,],BCM1_CLOSE=%9[^,],BCM2_OPEN=%9[^,],BCM2_CLOSE=%9[^,],"
    "Temp=%14[^,],Humidity=%14[^,],Date=%d-%d-%d,Time=%d:%d:%d,cycle=%d,"
    "Indicator=%9[^,],Buzzer=%9[^,\r\n]",
    &num, bcm1, bcm2, bcm3, bcm4, tStr, hStr, &yr, &mon, &day, &hr, &min, &sec, &cyc,
    indStr, buzStr
  );

  // ۱۳ = فریمور خیلی قدیمی (نه cycle، نه Indicator/Buzzer)
  // ۱۴ = فریمور میانی (فقط cycle=، بدون Indicator/Buzzer)
  // ۱۶ = فریمور فعلی (cycle= + Indicator= + Buzzer=)
  if (itemsParsed == 13 || itemsParsed == 14 || itemsParsed == 16) {
    WData.NUM = num;
    WData.BCM1_OPEN  = strToBool(bcm1);
    WData.BCM1_CLOSE = strToBool(bcm2);
    WData.BCM2_OPEN  = strToBool(bcm3);
    WData.BCM2_CLOSE = strToBool(bcm4);
    WData.Temp = atof(tStr); 
    WData.Hum = atof(hStr);
    WData.Year = yr; WData.Month = mon; WData.Day = day;
    WData.Hour = hr; WData.Minute = min; WData.Second = sec;
    if (cyc < 1) cyc = 1;
    if (cyc > 3) cyc = 3;
    WData.CycleAttempt = (uint8_t)cyc;
    WData.Indicator = strToBool(indStr);
    WData.Buzzer = strToBool(buzStr);
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
    "Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d,"
    "cycle=%u,Indicator=%s,Buzzer=%s\n",
    WData.NUM,
    WData.BCM1_OPEN ? "OK" : "NOK",
    WData.BCM1_CLOSE ? "OK" : "NOK",
    WData.BCM2_OPEN ? "OK" : "NOK",
    WData.BCM2_CLOSE ? "OK" : "NOK",
    WData.Temp, WData.Hum,
    WData.Year, WData.Month, WData.Day,
    WData.Hour, WData.Minute, WData.Second,
    (unsigned)WData.CycleAttempt,
    WData.Indicator ? "OK" : "NOK",
    WData.Buzzer ? "OK" : "NOK"
  );
}

// =====================================================================
//  لینک v2 — خط کامل دریافت‌شده از سرور روی سریال
//  (SRV_READY <sid> / ACK <num> / SRV_PONG <sid>)
// =====================================================================
void handleServerLine(const char* line) {
  if (strncmp(line, "SRV_READY", 9) == 0) {
    char sid[12] = "";
    if (sscanf(line, "SRV_READY %11s", sid) == 1 && sid[0] != '\0') {
      strlcpy(srvSid, sid, sizeof(srvSid));
    }
    bool wasReady = serverReady;
    serverReady = true;
    lastSrvReadyMs = millis();          // ضربان زنده است
    if (!wasReady) {
      ST_PRINTF("[LINK] سرور آماده است (نشست %s)\n", srvSid);
      notifyClientServerState();        // خبر دادن به ESP32
    }
  }
  else if (strncmp(line, "ACK", 3) == 0) {
    int num = -1;
    if (sscanf(line, "ACK %d", &num) == 1) {
      // جریان ACK هم دلیل زنده بودن لینک است (هنگام آپلود، ضربان
      // SRV_READY ممکن است بین رکوردها فرصت ارسال نیابد)
      lastSrvReadyMs = millis();
      if (isClientConnected && currentClient.connected()) {
        currentClient.printf("ACK %d\n", num);
        stats.acksFwd++;
      }
    }
  }
  else if (strncmp(line, "SRV_PONG", 8) == 0) {
    lastSrvReadyMs = millis();
  }
  else if (strncmp(line, "CFG", 3) == 0) {
    // کانال عبوری ساده: هر خط «CFG ...» که از سرور می‌آید (مثلاً تنظیم
    // از‌راه‌دورِ CYCLE_PERIOD_MS / RELAY_RETRY_GAP_MS) عیناً، بدون پردازش
    // یا اعتبارسنجی محتوا، به ESP32 فوروارد می‌شود. ESP32 خودش قالب و
    // بازه‌ی مجاز مقادیر را چک می‌کند؛ ESP8266 فقط لوله‌ی انتقال است.
    if (isClientConnected && currentClient.connected()) {
      currentClient.printf("%s\n", line);
      ST_PRINTF("[LINK] پیکربندی به ESP32 فوروارد شد: %s\n", line);
    } else {
      ST_PRINTF("[LINK] ESP32 وصل نیست؛ CFG فعلاً رد شد (با SRV_READY بعدی دوباره امتحان می‌شود): %s\n", line);
    }
  }
  else {
    DBG_PRINTF("[LINK] خط ناشناخته از سرور: %s\n", line);
  }
}

// =====================================================================
//  لینک v2 — سرویس دوره‌ای: خواندن ورودی سریال + ضربان + تشخیص قطع
// =====================================================================
void serviceServerLink() {
  // ۱) خواندن همه‌ی بایت‌های موجود و سهم‌گذاری خطوط
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (srvRxLen > 0) {
        srvRx[srvRxLen] = '\0';
        handleServerLine(srvRx);
        srvRxLen = 0;
      }
    } else if (srvRxLen < SRV_RX_SIZE - 1) {
      srvRx[srvRxLen++] = c;
    } else {
      // سرریز: خط را دور بریز تا با خط بعدی قاطی نشود
      srvRxLen = 0;
      DBG_PRINTLN("[LINK] خط سریال سرور بلندتر از بافر؛ دور ریخته شد");
    }
  }

  // ۲) تشخیص قطع سرور: مدتی هیچ نشانه‌ی حیاتی نیامده است
  if (serverReady && millis() - lastSrvReadyMs > SRV_HEARTBEAT_MS) {
    serverReady = false;
    srvSid[0] = '\0';
    ST_PRINTF("[LINK] ضربان سرور قطع شد (> %lus) -> فوروارد متوقف\n",
              SRV_HEARTBEAT_MS / 1000);
    if (isClientConnected && currentClient.connected()) {
      currentClient.println("SRV_LOST");
    }
  }

  // ۳) وقتی سرور آماده نیست هر ۱۰ ثانیه یادآوری می‌کنیم؛ سرور به
  //    SRV_PING جواب SRV_PONG و به SRV_HELLO جواب SRV_READY می‌دهد
  if (!serverReady && millis() - lastSrvPingMs > SRV_PING_PERIOD_MS) {
    lastSrvPingMs = millis();
    Serial.println("SRV_PING");
  }
}

// وضعیت جاری لینک سرور را به کلاینت TCP (ESP32) اعلام می‌کند
void notifyClientServerState() {
  if (!isClientConnected || !currentClient.connected()) return;
  if (serverReady) {
    currentClient.printf("READY %s\n", srvSid);
  } else {
    currentClient.println("WAIT");
  }
}
