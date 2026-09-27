/**
 * Project : ESP32 Industrial Controller  (V2.0)
 * Engineer: Peyman Parsa
 *
 * تغییرات نسبت به V1.7.1 :
 *  1) تست ترتیبی رله‌ها: اول رله ۱ تریگ می‌شود و فقط فیدبک BCM متناظرِ خودش
 *     بررسی می‌شود، بعد از تمام شدن کارِ رله ۱ نوبت رله ۲ می‌رسد.
 *  2) هر سیکل، تاریخ/ساعت (RTC) + دما + رطوبت را کنار نتیجه‌ی BCM ها ذخیره می‌کند.
 *  3) در بوت، ساعت و تاریخ به‌صورت خودکار گرفته می‌شود:
 *        الف) اتصال به مودم/هات‌اسپات گوشی و گرفتن زمان از NTP
 *        ب) اگر نشد، خودش هات‌اسپات «SetClock» را بالا می‌آورد و صفحه‌ی وب،
 *           ساعت خودِ گوشی را خودکار می‌فرستد (بدون نیاز به اینترنت)
 *        ج) اگر آن هم نشد، ورود دستی از همان صفحه
 *
 *  رفع باگ‌های نسخه‌ی قبل:
 *   - در حالت نمایش دیتا، اکسس‌پوینت اصلاً بالا نمی‌آمد (WIFI_OFF بود)
 *   - حلقه‌ی کلاینت هات‌اسپات بدون vTaskDelay بود (ریسک Task Watchdog)
 *   - ساخت مسیر فایل با file.name() در core 2.x مسیر تکراری می‌ساخت -> SD.remove شکست می‌خورد
 *   - بعد از شکست اتصال اولیه، هیچ تلاش مجددی برای اتصال به ESP8266 انجام نمی‌شد
 */

#include <Adafruit_SHT31.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "SD.h"
#include "SPI.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// =====================================================================
//                            USER CONFIG
// =====================================================================
#define DEBUG_MODE 1  // برای Production صفر شود

// --- نگاشت رله -> BCM متناظر -------------------------------------------------
// فرض پیش‌فرض: هر رله مربوط به یک دستگاه است.
//   رله ۱ (پین 2)  ->  BCM1 : فیدبک Open = پین 13 , فیدبک Close = پین 15
//   رله ۲ (پین 4)  ->  BCM2 : فیدبک Open = پین 16 , فیدبک Close = پین 17
// اگر نگاشت سخت‌افزاری شما فرق دارد، فقط همین جدول را عوض کنید.
struct ChannelConfig {
  const char *name;
  uint8_t relayPin;
  uint8_t fbOpenPin;   // فیدبک «باز شد»
  uint8_t fbClosePin;  // فیدبک «بسته شد»
};

const ChannelConfig CHANNELS[] = {
  { "BCM1", 2, 13, 15 },
  { "BCM2", 4, 16, 17 },
};
const int CHANNEL_COUNT = sizeof(CHANNELS) / sizeof(CHANNELS[0]);

// اگر true باشد، موفقیت یعنی هر دو فیدبک (Open و Close) دیده شوند.
// اگر false باشد، دیدن حداقل یکی کافی است.
const bool REQUIRE_BOTH_FEEDBACKS = true;

// --- زمان‌بندی تست هر رله ---
const uint32_t RELAY_PULSE_MS = 800;        // مدت تریگ رله
const uint32_t FEEDBACK_WINDOW_MS = 3000;   // مهلت پاسخ BCM بعد از تریگ
const uint8_t RELAY_MAX_ATTEMPTS = 3;       // تعداد تلاش برای هر رله
const bool ENABLE_HAMMERING = true;         // ضربه‌های کوتاه در صورت گیر کردن
const uint8_t HAMMER_COUNT = 5;
const uint32_t HAMMER_ON_MS = 50;
const uint32_t HAMMER_OFF_MS = 100;
const uint32_t PULSE_CONFIRM_MS = 100;      // حداقل مدت HIGH برای معتبر بودن پالس
const uint32_t CYCLE_PERIOD_MS = 120000;    // فاصله‌ی بین سیکل‌ها (۲ دقیقه)

// --- شبکه ---
const char *DATA_AP_SSID = "ESP8266_AP";  // گیرنده‌ی دیتا (سمت کامپیوتر)
const char *DATA_AP_PASS = "12345678";
IPAddress serverIP(192, 168, 4, 1);
const int serverPort = 80;

// شبکه‌هایی که برای گرفتن ساعت از NTP امتحان می‌شوند (مودم یا هات‌اسپات گوشی).
// SSID و پسورد خودتان را اینجا بگذارید. خالی بودنش اشکالی ندارد؛
// در آن صورت مستقیم سراغ روش «ساعتِ گوشی از طریق صفحه‌ی وب» می‌رود.
struct WifiCred {
  const char *ssid;
  const char *pass;
};
const WifiCred TIME_NETWORKS[] = {
  // { "MyModem",      "modem-password" },
  // { "iPhone-Peyman", "12345678"      },
};
const int TIME_NETWORK_COUNT = sizeof(TIME_NETWORKS) / sizeof(TIME_NETWORKS[0]);

const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.google.com";
// تهران: UTC+3:30 بدون ساعت تابستانی
const char *TIMEZONE_TZ = "<+0330>-3:30";

const char *SETUP_AP_SSID = "SetClock";
const char *SETUP_AP_PASS = "12345678";
const uint32_t SETUP_PORTAL_TIMEOUT_MS = 120000;  // ۲ دقیقه فرصت برای گوشی
const uint32_t STA_CONNECT_TIMEOUT_MS = 15000;

// =====================================================================
//                         DEBUG HELPERS
// =====================================================================
#if DEBUG_MODE
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
#endif

// =====================================================================
//                       RTC PCF8563 (I2C 0x51)
// =====================================================================
#define RTC_ADDRESS 0x51

class Rtc_Pcf8563 {
public:
  void initClock() {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x00);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
      Serial.println("[RTC] Error: Failed to communicate with RTC!");
    }
  }

  void setTime(uint8_t hour, uint8_t minute, uint8_t second) {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x02);
    Wire.write(decToBcd(second));
    Wire.write(decToBcd(minute));
    Wire.write(decToBcd(hour));
    Wire.endTransmission();
  }

  void setDate(uint8_t day, uint8_t weekday, uint8_t month, uint8_t century, uint8_t year) {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x05);
    Wire.write(decToBcd(day));
    Wire.write(decToBcd(weekday));
    Wire.write(decToBcd(month) | (century << 7));
    Wire.write(decToBcd(year));
    Wire.endTransmission();
  }

  /** ست کردن کامل تاریخ و ساعت با سال چهاررقمی */
  void setDateTime(int year4, uint8_t month, uint8_t day,
                   uint8_t hour, uint8_t minute, uint8_t second) {
    setDate(day, weekdayOf(year4, month, day), month, 0, (uint8_t)(year4 % 100));
    setTime(hour, minute, second);
  }

  /** خواندن همه‌ی رجیسترها در یک رفت‌وبرگشت */
  bool read() {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x02);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(RTC_ADDRESS, 7) < 7) return false;

    _second = bcdToDec(Wire.read() & 0x7F);
    _minute = bcdToDec(Wire.read() & 0x7F);
    _hour = bcdToDec(Wire.read() & 0x3F);
    _day = bcdToDec(Wire.read() & 0x3F);
    _weekday = bcdToDec(Wire.read() & 0x07);
    uint8_t mRaw = Wire.read();
    _month = bcdToDec(mRaw & 0x1F);
    _year = bcdToDec(Wire.read());
    return true;
  }

  uint8_t getSecond() { return _second; }
  uint8_t getMinute() { return _minute; }
  uint8_t getHour() { return _hour; }
  uint8_t getDay() { return _day; }
  uint8_t getMonth() { return _month; }
  uint8_t getYear() { return _year; }  // دو رقمی

  String isoString() {
    char buf[24];
    snprintf(buf, sizeof(buf), "20%02u-%02u-%02u %02u:%02u:%02u",
             _year, _month, _day, _hour, _minute, _second);
    return String(buf);
  }

private:
  static uint8_t decToBcd(uint8_t v) { return ((v / 10) << 4) + (v % 10); }
  static uint8_t bcdToDec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }

  // الگوریتم Zeller برای محاسبه‌ی روز هفته (0=یکشنبه)
  static uint8_t weekdayOf(int y, uint8_t m, uint8_t d) {
    if (m < 3) {
      m += 12;
      y -= 1;
    }
    int k = y % 100, j = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (uint8_t)((h + 6) % 7);
  }

  uint8_t _hour = 0, _minute = 0, _second = 0;
  uint8_t _day = 1, _weekday = 0, _month = 1, _year = 0;
};

// =====================================================================
//                         DATA STRUCTURE
// ساختار دقیقاً مثل قبل است تا با پارسر ESP8266 و با
// STRUCT_FORMAT = '<iff????iBBBBB' (۲۵ بایت) در app.py سازگار بماند.
// =====================================================================
#pragma pack(1)
struct WifiData {
  int NUM;
  float Temp;
  float Hum;
  bool NBCM1, NBCM2, NBCM3, NBCM4;
  int Year;
  uint8_t Month, Day, Hour, Minute, Second;
};
#pragma pack()

enum WiFiOperationMode {
  MODE_CLIENT_UPLOAD = 0,  // حالت نرمال: اتصال به گیرنده و آپلود
  MODE_HOTSPOT_VIEW = 1    // حالت دیباگ: هات‌اسپات و نمایش دیتا
};

// =====================================================================
//                       GLOBALS / RTOS HANDLES
// =====================================================================
QueueHandle_t xDataQueue;
SemaphoreHandle_t xSDMutex;
SemaphoreHandle_t xGlobalStateMutex;
EventGroupHandle_t xSystemEvents;

#define SD_CS_PIN 5

#define BIT_START_DIGITAL_MONITORING (1UL << 0)
#define BIT_STOP_DIGITAL_MONITORING (1UL << 1)
#define BIT_START_SHT_READ (1UL << 2)
#define BIT_DIGITAL_READ_COMPLETE (1UL << 3)
#define BIT_SHT_READ_COMPLETE (1UL << 4)
#define BIT_NETWORK_BOOT_COMPLETE (1UL << 5)
#define BIT_REQUEST_AP_DATA_VIEW (1UL << 6)
#define BIT_WIFI_PERMIT (1UL << 7)

Adafruit_SHT31 sht31 = Adafruit_SHT31();
Rtc_Pcf8563 rtc;
WiFiClient uploadClient;
WebServer setupServer(80);

volatile int currentGlobalID = 0;
volatile WifiData globalSystemState;

// نتیجه‌ی خام آخرین پنجره‌ی مانیتورینگ (ایندکس = ترتیب پین‌ها در CHANNELS)
volatile bool fbOpenSeen[4] = { false, false, false, false };
volatile bool fbCloseSeen[4] = { false, false, false, false };

// پرچم‌های پورتال تنظیم ساعت
volatile bool portalTimeSet = false;
volatile bool portalModeChosen = false;
volatile bool portalWantsDataView = false;

// Prototypes
bool syncTimeFromNtp();
void runSetupPortal(bool timeAlreadyValid);
bool rtcTimeLooksValid();
void connectToDataAp(uint32_t timeoutMs);
void saveToSD(const WifiData &data);
int getNextPersistentID();
void saveNextPersistentID(int id);
void TaskRelayControl(void *pv);
void TaskReadSHT(void *pv);
void TaskDigitalRead(void *pv);
void TaskInternalWiFiConnection(void *pv);

// =====================================================================
//                                SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  DEBUG_PRINTLN("\n[BOOT] Industrial Controller V2.0");

  Wire.begin();
  delay(200);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

  xDataQueue = xQueueCreate(20, sizeof(WifiData));
  xSDMutex = xSemaphoreCreateMutex();
  xGlobalStateMutex = xSemaphoreCreateMutex();
  xSystemEvents = xEventGroupCreate();

  rtc.initClock();

  // ---------------- SD & شماره‌ی رکورد ----------------
  if (xSemaphoreTake(xSDMutex, portMAX_DELAY)) {
    if (!SD.begin(SD_CS_PIN)) {
      Serial.println("[SD] Critical Error: SD Card not detected!");
    } else {
      DEBUG_PRINTLN("[SD] Card OK.");
      if (!SD.exists("/data")) SD.mkdir("/data");

      int maxFileID = 0;
      bool filesFound = false;
      File root = SD.open("/data");
      if (root) {
        File f = root.openNextFile();
        while (f) {
          String fn = String(f.name());
          int slash = fn.lastIndexOf('/');
          if (slash >= 0) fn = fn.substring(slash + 1);
          int underscore = fn.lastIndexOf('_');
          if (underscore != -1 && fn.endsWith(".dat")) {
            filesFound = true;
            int id = fn.substring(underscore + 1, fn.length() - 4).toInt();
            if (id > maxFileID) maxFileID = id;
          }
          f.close();
          f = root.openNextFile();
        }
        root.close();
      }

      int lastSaved = getNextPersistentID();
      currentGlobalID = filesFound ? max(maxFileID, lastSaved) : lastSaved;
      DEBUG_PRINTF("[SD] Resuming from ID %d\n", currentGlobalID);
      saveNextPersistentID(currentGlobalID);
    }
    xSemaphoreGive(xSDMutex);
  }

  // ---------------- گرفتن خودکار تاریخ و ساعت ----------------
  // مرحله ۱: NTP از طریق مودم یا هات‌اسپات گوشی
  bool timeOk = syncTimeFromNtp();

  // مرحله ۲/۳: پورتال محلی — صفحه‌ی وب ساعتِ گوشی را خودکار می‌فرستد،
  // و اگر کسی وصل نشد، با ساعت فعلی RTC ادامه می‌دهیم.
  if (!timeOk) {
    DEBUG_PRINTLN("[TIME] NTP failed -> opening SetClock portal");
    runSetupPortal(rtcTimeLooksValid());
    timeOk = rtcTimeLooksValid();
  } else {
    // حتی وقتی ساعت از NTP گرفته شد، کاربر ممکن است بخواهد مود را عوض کند؛
    // پورتال کوتاه فقط وقتی باز می‌شود که ساعت معتبر نباشد.
    DEBUG_PRINTLN("[TIME] RTC synced from NTP.");
  }

  rtc.read();
  DEBUG_PRINT("[TIME] Current RTC: ");
  DEBUG_PRINTLN(rtc.isoString());

  // ---------------- اتصال به گیرنده‌ی دیتا ----------------
  if (!(xEventGroupGetBits(xSystemEvents) & BIT_REQUEST_AP_DATA_VIEW)) {
    connectToDataAp(30000);
  }

  // ---------------- سنسور و پین‌ها ----------------
  if (!sht31.begin(0x44)) Serial.println("[SHT31] Error: sensor not found!");

  for (int i = 0; i < CHANNEL_COUNT; i++) {
    pinMode(CHANNELS[i].relayPin, OUTPUT);
    digitalWrite(CHANNELS[i].relayPin, LOW);
    pinMode(CHANNELS[i].fbOpenPin, INPUT_PULLDOWN);
    pinMode(CHANNELS[i].fbClosePin, INPUT_PULLDOWN);
  }

  xEventGroupSetBits(xSystemEvents, BIT_NETWORK_BOOT_COMPLETE);

  xTaskCreatePinnedToCore(TaskDigitalRead, "DigiRead", 4096, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(TaskRelayControl, "RelayCtrl", 4096, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(TaskReadSHT, "SHTRead", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskInternalWiFiConnection, "WiFiConn", 8192, NULL, 2, NULL, 1);

  DEBUG_PRINTLN("[BOOT] Tasks started.");
}

void loop() {
  vTaskDelete(NULL);
}

// =====================================================================
//                       TIME: NTP  +  LOCAL PORTAL
// =====================================================================
bool rtcTimeLooksValid() {
  if (!rtc.read()) return false;
  uint8_t y = rtc.getYear(), mo = rtc.getMonth(), d = rtc.getDay();
  return (y >= 24 && y <= 99) && (mo >= 1 && mo <= 12) && (d >= 1 && d <= 31);
}

/** تلاش برای اتصال به شبکه‌های کاربر و گرفتن زمان از NTP */
bool syncTimeFromNtp() {
  if (TIME_NETWORK_COUNT == 0) {
    DEBUG_PRINTLN("[TIME] No time-networks configured, skipping NTP.");
    return false;
  }

  for (int i = 0; i < TIME_NETWORK_COUNT; i++) {
    DEBUG_PRINTF("[TIME] Trying SSID '%s' ...\n", TIME_NETWORKS[i].ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(TIME_NETWORKS[i].ssid, TIME_NETWORKS[i].pass);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
      delay(250);
      DEBUG_PRINT(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
      DEBUG_PRINTLN(" failed.");
      WiFi.disconnect(true);
      continue;
    }

    DEBUG_PRINTF("\n[TIME] Connected (%s). Asking NTP...\n", WiFi.localIP().toString().c_str());
    configTzTime(TIMEZONE_TZ, NTP_SERVER_1, NTP_SERVER_2);

    struct tm tmNow;
    bool got = false;
    for (int k = 0; k < 20; k++) {           // حداکثر ~۱۰ ثانیه
      if (getLocalTime(&tmNow, 500)) {
        got = (tmNow.tm_year + 1900) >= 2024;
        if (got) break;
      }
      delay(100);
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(300);

    if (got) {
      rtc.setDateTime(tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                      tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
      DEBUG_PRINTF("[TIME] NTP OK -> %04d-%02d-%02d %02d:%02d:%02d\n",
                   tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                   tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
      return true;
    }
    DEBUG_PRINTLN("[TIME] NTP did not answer on this network.");
  }
  return false;
}

static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="fa" dir="rtl"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>تنظیم ساعت کنترلر</title><style>
body{background:#0b1220;color:#e8eefc;font-family:Tahoma,sans-serif;margin:0;padding:18px}
.card{background:#131c2e;border:1px solid #22304a;border-radius:14px;padding:16px;margin-bottom:14px}
h2{margin:0 0 10px;font-size:17px}
button{border:0;border-radius:10px;padding:12px 16px;font-size:15px;font-weight:700;
 background:linear-gradient(135deg,#3b82f6,#22d3ee);color:#06101f;width:100%;margin-top:8px}
.ghost{background:#1b2740;color:#e8eefc}
input{width:100%;padding:10px;border-radius:8px;border:1px solid #22304a;background:#0a1120;color:#fff;margin-top:6px}
.ok{color:#22c55e}.err{color:#ef4444}small{color:#8fa3c4}
</style></head><body>
<div class="card"><h2>ساعت دستگاه</h2>
<div id="cur">در حال خواندن…</div>
<div id="msg"><small>در حال ارسال ساعت گوشی…</small></div>
<button onclick="sendNow()">همگام‌سازی با ساعت همین گوشی</button></div>

<div class="card"><h2>ورود دستی</h2>
<input id="man" type="datetime-local" step="1">
<button class="ghost" onclick="sendManual()">ثبت دستی</button></div>

<div class="card"><h2>حالت کاری</h2>
<button onclick="mode(0)">شروع کار عادی (تست رله‌ها)</button>
<button class="ghost" onclick="mode(1)">حالت نمایش دیتا (بدون تست)</button></div>

<script>
function two(n){return String(n).padStart(2,'0')}
function q(d){return 'y='+d.getFullYear()+'&mo='+(d.getMonth()+1)+'&d='+d.getDate()+
 '&h='+d.getHours()+'&mi='+d.getMinutes()+'&s='+d.getSeconds()}
function cur(){fetch('/now').then(r=>r.text()).then(t=>document.getElementById('cur').textContent=t)}
function sendNow(){
  fetch('/settime?'+q(new Date())).then(r=>r.text()).then(t=>{
    document.getElementById('msg').innerHTML='<span class="ok">'+t+'</span>';cur();})
  .catch(e=>document.getElementById('msg').innerHTML='<span class="err">خطا</span>');}
function sendManual(){
  var v=document.getElementById('man').value; if(!v)return;
  sendDate(new Date(v));}
function sendDate(d){fetch('/settime?'+q(d)).then(r=>r.text()).then(t=>{
  document.getElementById('msg').innerHTML='<span class="ok">'+t+'</span>';cur();});}
function mode(m){fetch('/mode?v='+m).then(r=>r.text()).then(t=>{
  document.getElementById('msg').innerHTML='<span class="ok">'+t+'</span>';});}
cur(); sendNow();           // ارسال خودکار ساعت گوشی به محض باز شدن صفحه
</script></body></html>)HTML";

void handlePortalRoot() {
  setupServer.send_P(200, "text/html; charset=utf-8", PORTAL_PAGE);
}

void handlePortalNow() {
  rtc.read();
  setupServer.send(200, "text/plain; charset=utf-8", rtc.isoString());
}

void handlePortalSetTime() {
  if (!setupServer.hasArg("y") || !setupServer.hasArg("mo") || !setupServer.hasArg("d")) {
    setupServer.send(400, "text/plain; charset=utf-8", "پارامتر ناقص");
    return;
  }
  int y = setupServer.arg("y").toInt();
  int mo = setupServer.arg("mo").toInt();
  int d = setupServer.arg("d").toInt();
  int h = setupServer.arg("h").toInt();
  int mi = setupServer.arg("mi").toInt();
  int s = setupServer.arg("s").toInt();

  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) {
    setupServer.send(400, "text/plain; charset=utf-8", "مقدار نامعتبر");
    return;
  }

  rtc.setDateTime(y, mo, d, h, mi, s);
  portalTimeSet = true;
  DEBUG_PRINTF("[PORTAL] RTC set to %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
  setupServer.send(200, "text/plain; charset=utf-8", "ساعت ثبت شد");
}

void handlePortalMode() {
  int m = setupServer.arg("v").toInt();
  portalWantsDataView = (m == 1);
  portalModeChosen = true;
  setupServer.send(200, "text/plain; charset=utf-8",
                   m == 1 ? "حالت نمایش دیتا انتخاب شد" : "حالت کار عادی انتخاب شد");
}

/**
 * هات‌اسپات محلی برای گرفتن ساعت از گوشی (بدون اینترنت) و انتخاب مود.
 * به محض اینکه ساعت ست شد و مود انتخاب شد، پورتال بسته می‌شود.
 */
void runSetupPortal(bool timeAlreadyValid) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASS);

  setupServer.on("/", handlePortalRoot);
  setupServer.on("/now", handlePortalNow);
  setupServer.on("/settime", handlePortalSetTime);
  setupServer.on("/mode", handlePortalMode);
  setupServer.onNotFound(handlePortalRoot);  // Captive-portal-ish
  setupServer.begin();

  DEBUG_PRINTF("[PORTAL] SSID '%s' -> http://192.168.1.1/\n", SETUP_AP_SSID);

  portalTimeSet = timeAlreadyValid;
  portalModeChosen = false;
  portalWantsDataView = false;

  uint32_t start = millis();
  while (millis() - start < SETUP_PORTAL_TIMEOUT_MS) {
    setupServer.handleClient();
    // وقتی هم ساعت آمد و هم مود انتخاب شد، دیگر منتظر نمی‌مانیم
    if (portalTimeSet && portalModeChosen) {
      delay(400);  // فرصت ارسال پاسخ آخر به مرورگر
      break;
    }
    delay(2);
  }

  if (portalWantsDataView) {
    xEventGroupSetBits(xSystemEvents, BIT_REQUEST_AP_DATA_VIEW);
    DEBUG_PRINTLN("[PORTAL] Mode: DATA VIEW");
  } else {
    xEventGroupClearBits(xSystemEvents, BIT_REQUEST_AP_DATA_VIEW);
    DEBUG_PRINTLN("[PORTAL] Mode: NORMAL RUN");
  }

  setupServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
}

/** اتصال (یا اتصال مجدد) به اکسس‌پوینت گیرنده‌ی دیتا */
void connectToDataAp(uint32_t timeoutMs) {
  DEBUG_PRINTF("[NET] Connecting to %s ...\n", DATA_AP_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(DATA_AP_SSID, DATA_AP_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
    DEBUG_PRINT(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINTF("\n[NET] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DEBUG_PRINTLN("\n[NET] Not available now; data will be buffered on SD.");
  }
}

// =====================================================================
//     TASK: تست ترتیبی رله‌ها  (اول رله ۱ و BCM1 ، بعد رله ۲ و BCM2)
// =====================================================================

/** پنجره‌ی مانیتورینگ را باز می‌کند و پرچم‌های قبلی را صفر می‌کند */
static void beginFeedbackWindow() {
  for (int i = 0; i < 4; i++) {
    fbOpenSeen[i] = false;
    fbCloseSeen[i] = false;
  }
  xEventGroupClearBits(xSystemEvents, BIT_DIGITAL_READ_COMPLETE);
  xEventGroupSetBits(xSystemEvents, BIT_START_DIGITAL_MONITORING);
}

static void endFeedbackWindow() {
  xEventGroupSetBits(xSystemEvents, BIT_STOP_DIGITAL_MONITORING);
  xEventGroupWaitBits(xSystemEvents, BIT_DIGITAL_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
}

static void pulseRelay(uint8_t pin, uint32_t ms) {
  digitalWrite(pin, HIGH);
  vTaskDelay(pdMS_TO_TICKS(ms));
  digitalWrite(pin, LOW);
}

static bool channelSucceeded(int ch) {
  bool o = fbOpenSeen[ch];
  bool c = fbCloseSeen[ch];
  return REQUIRE_BOTH_FEEDBACKS ? (o && c) : (o || c);
}

/** یک کانال (یک رله + BCM متناظرش) را کامل تست می‌کند */
static bool testChannel(int ch) {
  const ChannelConfig &cfg = CHANNELS[ch];
  DEBUG_PRINTF("\n[TEST] ---- %s (relay pin %u) ----\n", cfg.name, cfg.relayPin);

  for (uint8_t attempt = 1; attempt <= RELAY_MAX_ATTEMPTS; attempt++) {
    DEBUG_PRINTF("[TEST] %s attempt %u/%u\n", cfg.name, attempt, RELAY_MAX_ATTEMPTS);

    // ۱) پنجره‌ی شنود فیدبک باز شود، بعد رله تریگ شود
    beginFeedbackWindow();
    pulseRelay(cfg.relayPin, RELAY_PULSE_MS);

    // ۲) مهلت پاسخ BCM
    vTaskDelay(pdMS_TO_TICKS(FEEDBACK_WINDOW_MS));
    endFeedbackWindow();

    bool openSeen = fbOpenSeen[ch];
    bool closeSeen = fbCloseSeen[ch];
    DEBUG_PRINTF("[TEST] %s feedback -> Open:%s Close:%s\n",
                 cfg.name, openSeen ? "YES" : "NO", closeSeen ? "YES" : "NO");

    if (channelSucceeded(ch)) {
      DEBUG_PRINTF("[TEST] %s OK\n", cfg.name);
      return true;
    }

    // ۳) نیمه‌کاره (مثلاً باز شد ولی بسته نشد) -> چکش‌کاری
    bool jammed = (openSeen != closeSeen);
    if (ENABLE_HAMMERING && jammed) {
      DEBUG_PRINTF("[TEST] %s JAM -> hammering %ux\n", cfg.name, HAMMER_COUNT);
      beginFeedbackWindow();
      for (uint8_t k = 0; k < HAMMER_COUNT; k++) {
        digitalWrite(cfg.relayPin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(HAMMER_ON_MS));
        digitalWrite(cfg.relayPin, LOW);
        vTaskDelay(pdMS_TO_TICKS(HAMMER_OFF_MS));
      }
      vTaskDelay(pdMS_TO_TICKS(FEEDBACK_WINDOW_MS));
      endFeedbackWindow();

      if (channelSucceeded(ch)) {
        DEBUG_PRINTF("[TEST] %s recovered after hammering\n", cfg.name);
        return true;
      }
    }

    if (attempt < RELAY_MAX_ATTEMPTS) vTaskDelay(pdMS_TO_TICKS(2000));
  }

  DEBUG_PRINTF("[TEST] %s FAILED\n", cfg.name);
  return false;
}

void TaskRelayControl(void *pv) {
  const TickType_t period = pdMS_TO_TICKS(CYCLE_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  xEventGroupWaitBits(xSystemEvents, BIT_NETWORK_BOOT_COMPLETE, pdFALSE, pdTRUE, portMAX_DELAY);

  // در حالت «نمایش دیتا» اصلاً نباید رله‌ای زده شود
  if (xEventGroupGetBits(xSystemEvents) & BIT_REQUEST_AP_DATA_VIEW) {
    DEBUG_PRINTLN("[RELAY] Data-view mode: relay task disabled.");
    xEventGroupSetBits(xSystemEvents, BIT_WIFI_PERMIT);
    vTaskDelete(NULL);
  }

  for (;;) {
    DEBUG_PRINTLN("\n[CYCLE] ===== Started =====");
    xEventGroupClearBits(xSystemEvents, BIT_WIFI_PERMIT);

    bool result[4] = { false, false, false, false };

    // ---- تست ترتیبی: اول کانال ۱ تا آخر، بعد کانال ۲ ----
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
      result[ch] = testChannel(ch);
      vTaskDelay(pdMS_TO_TICKS(1000));  // فاصله‌ی بین دو کانال
    }

    // ---- خواندن دما و رطوبت و ساعت ----
    xEventGroupClearBits(xSystemEvents, BIT_SHT_READ_COMPLETE);
    xEventGroupSetBits(xSystemEvents, BIT_START_SHT_READ);
    xEventGroupWaitBits(xSystemEvents, BIT_SHT_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));

    // ---- ثبت رکورد ----
    if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(1000))) {
      currentGlobalID++;
      globalSystemState.NUM = currentGlobalID;
      globalSystemState.NBCM1 = result[0];
      globalSystemState.NBCM2 = result[1];
      globalSystemState.NBCM3 = (CHANNEL_COUNT > 2) ? result[2] : false;
      globalSystemState.NBCM4 = (CHANNEL_COUNT > 3) ? result[3] : false;

      WifiData snapshot;
      memcpy(&snapshot, (const void *)&globalSystemState, sizeof(WifiData));
      xSemaphoreGive(xGlobalStateMutex);

      DEBUG_PRINTF("[CYCLE] #%d  %s=%s  %s=%s  T=%.2f H=%.2f  @ %04d-%02d-%02d %02d:%02d:%02d\n",
                   snapshot.NUM,
                   CHANNELS[0].name, snapshot.NBCM1 ? "OK" : "NOK",
                   CHANNELS[1].name, snapshot.NBCM2 ? "OK" : "NOK",
                   snapshot.Temp, snapshot.Hum,
                   snapshot.Year, snapshot.Month, snapshot.Day,
                   snapshot.Hour, snapshot.Minute, snapshot.Second);

      xQueueSend(xDataQueue, (void *)&snapshot, pdMS_TO_TICKS(100));
      saveNextPersistentID(currentGlobalID);
    } else {
      DEBUG_PRINTLN("[CYCLE] Error: state mutex busy, record skipped.");
    }

    xEventGroupSetBits(xSystemEvents, BIT_WIFI_PERMIT);
    vTaskDelayUntil(&lastWake, period);
  }
}

// =====================================================================
//        TASK: خواندن فیدبک‌های دیجیتال در طول پنجره‌ی مانیتورینگ
// =====================================================================
void TaskDigitalRead(void *pv) {
  const int pinCount = CHANNEL_COUNT * 2;
  uint8_t pins[8];
  for (int i = 0; i < CHANNEL_COUNT; i++) {
    pins[2 * i] = CHANNELS[i].fbOpenPin;
    pins[2 * i + 1] = CHANNELS[i].fbClosePin;
  }

  bool lastState[8];
  uint32_t highSince[8];
  bool confirmed[8];

  for (;;) {
    xEventGroupWaitBits(xSystemEvents, BIT_START_DIGITAL_MONITORING, pdTRUE, pdFALSE, portMAX_DELAY);

    for (int i = 0; i < pinCount; i++) {
      lastState[i] = digitalRead(pins[i]);
      highSince[i] = 0;
      confirmed[i] = false;
    }

    bool active = true;
    while (active) {
      for (int i = 0; i < pinCount; i++) {
        bool now = digitalRead(pins[i]);
        if (now && !lastState[i]) {
          highSince[i] = millis();
        } else if (now && lastState[i]) {
          if (highSince[i] != 0 && !confirmed[i] && (millis() - highSince[i] >= PULSE_CONFIRM_MS)) {
            confirmed[i] = true;
          }
        } else if (!now && lastState[i]) {
          highSince[i] = 0;
        }
        lastState[i] = now;
      }

      if (xEventGroupGetBits(xSystemEvents) & BIT_STOP_DIGITAL_MONITORING) {
        xEventGroupClearBits(xSystemEvents, BIT_STOP_DIGITAL_MONITORING);
        active = false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < CHANNEL_COUNT; i++) {
      if (confirmed[2 * i]) fbOpenSeen[i] = true;
      if (confirmed[2 * i + 1]) fbCloseSeen[i] = true;
    }

    xEventGroupSetBits(xSystemEvents, BIT_DIGITAL_READ_COMPLETE);
  }
}

// =====================================================================
//              TASK: دما/رطوبت (میانگین‌گیری) + تاریخ و ساعت
// =====================================================================
void TaskReadSHT(void *pv) {
  const int SAMPLE_COUNT = 10;
  const int SAMPLE_DELAY_MS = 20;
  const float MAX_TEMP_JUMP = 5.0f;
  const float MAX_HUM_JUMP = 15.0f;

  static float lastValidTemp = 25.0f;
  static float lastValidHum = 50.0f;

  for (;;) {
    xEventGroupWaitBits(xSystemEvents, BIT_START_SHT_READ, pdTRUE, pdFALSE, portMAX_DELAY);

    float sumT = 0, sumH = 0;
    int valid = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
      float t = sht31.readTemperature();
      float h = sht31.readHumidity();
      if (!isnan(t) && !isnan(h)) {
        sumT += t;
        sumH += h;
        valid++;
      }
      vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));
    }

    if (valid > 0) {
      float avgT = sumT / valid;
      float avgH = sumH / valid;

      bool sane = (avgT > -20 && avgT < 85) && (avgH >= 0 && avgH <= 100);
      if (sane) {
        float dT = avgT - lastValidTemp;
        float dH = avgH - lastValidHum;
        if (fabs(dT) > MAX_TEMP_JUMP) avgT = lastValidTemp + (dT > 0 ? MAX_TEMP_JUMP : -MAX_TEMP_JUMP);
        if (fabs(dH) > MAX_HUM_JUMP) avgH = lastValidHum + (dH > 0 ? MAX_HUM_JUMP : -MAX_HUM_JUMP);

        if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(500))) {
          globalSystemState.Temp = avgT;
          globalSystemState.Hum = avgH;
          xSemaphoreGive(xGlobalStateMutex);
        }
        lastValidTemp = avgT;
        lastValidHum = avgH;
      } else {
        DEBUG_PRINTLN("[SHT] Average out of physical range!");
      }
    } else {
      DEBUG_PRINTLN("[SHT] Sensor failed all samples!");
    }

    // --- تاریخ و ساعت همین لحظه از RTC ---
    if (rtc.read()) {
      int y4 = 2000 + rtc.getYear();
      bool valid_time = (rtc.getYear() >= 24) && (rtc.getMonth() >= 1 && rtc.getMonth() <= 12)
                        && (rtc.getDay() >= 1 && rtc.getDay() <= 31);
      if (valid_time && xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(500))) {
        globalSystemState.Year = y4;
        globalSystemState.Month = rtc.getMonth();
        globalSystemState.Day = rtc.getDay();
        globalSystemState.Hour = rtc.getHour();
        globalSystemState.Minute = rtc.getMinute();
        globalSystemState.Second = rtc.getSecond();
        xSemaphoreGive(xGlobalStateMutex);
      }
    }

    xEventGroupSetBits(xSystemEvents, BIT_SHT_READ_COMPLETE);
  }
}

// =====================================================================
//                     SD HELPERS
// =====================================================================
int getNextPersistentID() {
  int id = 0;
  if (SD.exists("/last_id.txt")) {
    File f = SD.open("/last_id.txt", FILE_READ);
    if (f) {
      id = f.parseInt();
      f.close();
    }
  }
  return id;
}

void saveNextPersistentID(int id) {
  File f = SD.open("/last_id.txt", FILE_WRITE);
  if (f) {
    f.print(id);
    f.close();
  }
}

void saveToSD(const WifiData &data) {
  char filename[40];
  snprintf(filename, sizeof(filename), "/data/%04d%02d%02d_%d.dat",
           data.Year, data.Month, data.Day, data.NUM);
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    f.write((const uint8_t *)&data, sizeof(WifiData));
    f.close();
    DEBUG_PRINTF("[SD] Logged %s\n", filename);
  } else {
    DEBUG_PRINTLN("[SD] Critical: could not write file!");
  }
}

/** نام فایل را بدون توجه به نسخه‌ی core به مسیر کامل تبدیل می‌کند */
static String fullDataPath(const char *rawName) {
  String n = String(rawName);
  if (n.startsWith("/")) return n;
  return "/data/" + n;
}

void sendDataFile(WiFiClient &cl, const String &filePath) {
  File f = SD.open(filePath, FILE_READ);
  if (!f) return;
  WifiData d;
  if (f.read((uint8_t *)&d, sizeof(WifiData)) == sizeof(WifiData)) {
    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"ID\":%d,\"T\":%.2f,\"H\":%.2f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
             d.NUM, d.Temp, d.Hum, d.NBCM1, d.NBCM2,
             d.Year, d.Month, d.Day, d.Hour, d.Minute, d.Second);
    cl.println(buf);
  }
  f.close();
}

// =====================================================================
//        TASK: ذخیره/آپلود (حالت کلاینت)  یا  نمایش دیتا (هات‌اسپات)
// =====================================================================
void TaskInternalWiFiConnection(void *pv) {
  xEventGroupWaitBits(xSystemEvents, BIT_NETWORK_BOOT_COMPLETE, pdFALSE, pdTRUE, portMAX_DELAY);

  WifiData q;
  WiFiServer debugServer(80);
  bool debugServerStarted = false;
  bool apStarted = false;
  uint32_t lastReconnectTry = 0;

  for (;;) {
    int mode = (xEventGroupGetBits(xSystemEvents) & BIT_REQUEST_AP_DATA_VIEW)
                 ? MODE_HOTSPOT_VIEW
                 : MODE_CLIENT_UPLOAD;

    switch (mode) {

      // ---------------------------------------------------------------
      case MODE_CLIENT_UPLOAD: {
        if (debugServerStarted) {
          debugServer.stop();
          debugServerStarted = false;
        }

        xEventGroupWaitBits(xSystemEvents, BIT_WIFI_PERMIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // الف) هر چه در صف است روی SD ذخیره شود
        while (xQueueReceive(xDataQueue, &q, pdMS_TO_TICKS(10)) == pdPASS) {
          if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
            saveToSD(q);
            xSemaphoreGive(xSDMutex);
          }
        }

        // ب) اگر شبکه قطع است، هر ۲۰ ثانیه دوباره تلاش کن (باگ نسخه‌ی قبل)
        if (WiFi.status() != WL_CONNECTED) {
          if (millis() - lastReconnectTry > 20000) {
            lastReconnectTry = millis();
            connectToDataAp(8000);
          }
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
        }

        // ج) Store & Forward : یک فایل در هر دور
        if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(100))) {
          File root = SD.open("/data");
          if (root) {
            File file = root.openNextFile();
            if (file) {
              String fPath = fullDataPath(file.name());
              if (fPath.endsWith(".dat")) {
                WifiData stored;
                bool readOk = (file.read((uint8_t *)&stored, sizeof(WifiData)) == sizeof(WifiData));
                file.close();

                if (readOk && uploadClient.connect(serverIP, serverPort)) {
                  // این فرمت باید دقیقاً با sscanf سمت ESP8266 و با
                  // parse_industrial_line در app.py یکی بماند (۱۳ فیلد)
                  char buf[300];
                  snprintf(buf, sizeof(buf),
                           "NUM=%d,NBCM1=%s,NBCM2=%s,NBCM3=%s,NBCM4=%s,Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d",
                           stored.NUM,
                           stored.NBCM1 ? "OK" : "NOK",
                           stored.NBCM2 ? "OK" : "NOK",
                           stored.NBCM3 ? "OK" : "NOK",
                           stored.NBCM4 ? "OK" : "NOK",
                           stored.Temp, stored.Hum,
                           stored.Year, stored.Month, stored.Day,
                           stored.Hour, stored.Minute, stored.Second);
                  uploadClient.println(buf);

                  uint32_t t0 = millis();
                  bool ack = false;
                  while (millis() - t0 < 3000) {
                    if (uploadClient.available() &&
                        uploadClient.readStringUntil('\n').indexOf("OK") != -1) {
                      ack = true;
                      break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                  }

                  if (ack) {
                    SD.remove(fPath);
                    DEBUG_PRINTF("[UPLOAD] ACK -> removed %s\n", fPath.c_str());
                  } else {
                    DEBUG_PRINTLN("[UPLOAD] No ACK, keeping file.");
                  }
                  uploadClient.stop();
                }
              } else {
                file.close();
              }
            }
            root.close();
          }
          xSemaphoreGive(xSDMutex);
        }
        break;
      }

      // ---------------------------------------------------------------
      case MODE_HOTSPOT_VIEW: {
        // باگ نسخه‌ی قبل: اینجا وای‌فای خاموش بود و سرور روی هیچ شبکه‌ای بالا نمی‌آمد
        if (!apStarted) {
          WiFi.disconnect(true);
          WiFi.mode(WIFI_AP);
          WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1),
                            IPAddress(255, 255, 255, 0));
          WiFi.softAP("RF_TESTER", "12345678");
          apStarted = true;
          DEBUG_PRINTLN("[VIEW] AP 'RF_TESTER' up on 192.168.1.1");
        }
        if (!debugServerStarted) {
          debugServer.begin();
          debugServer.setNoDelay(true);
          debugServerStarted = true;
          DEBUG_PRINTLN("[VIEW] TCP server on port 80 (sync / sync10)");
        }

        WiFiClient remote = debugServer.available();
        if (remote) {
          DEBUG_PRINTLN("[VIEW] Client connected.");
          uint32_t lastActivity = millis();

          while (remote.connected() && (millis() - lastActivity < 30000)) {
            if (remote.available()) {
              lastActivity = millis();
              String cmd = remote.readStringUntil('\n');
              cmd.trim();

              if (cmd.equalsIgnoreCase("sync")) {
                if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                  File root = SD.open("/data");
                  String lastFile = "";
                  int maxID = -1;
                  if (root) {
                    File e = root.openNextFile();
                    while (e) {
                      String p = fullDataPath(e.name());
                      if (p.endsWith(".dat")) {
                        int u = p.lastIndexOf('_');
                        int id = p.substring(u + 1, p.length() - 4).toInt();
                        if (id > maxID) {
                          maxID = id;
                          lastFile = p;
                        }
                      }
                      e.close();
                      e = root.openNextFile();
                    }
                    root.close();
                  }
                  if (lastFile.length()) sendDataFile(remote, lastFile);
                  else remote.println("NO_DATA");
                  xSemaphoreGive(xSDMutex);
                }
              } else if (cmd.equalsIgnoreCase("sync10")) {
                if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                  File root = SD.open("/data");
                  String ring[10];
                  int count = 0;
                  if (root) {
                    File e = root.openNextFile();
                    while (e) {
                      String p = fullDataPath(e.name());
                      if (p.endsWith(".dat")) {
                        ring[count % 10] = p;
                        count++;
                      }
                      e.close();
                      e = root.openNextFile();
                    }
                    root.close();
                  }
                  int start = (count > 10) ? (count % 10) : 0;
                  int items = (count > 10) ? 10 : count;

                  remote.println("[");
                  for (int i = 0; i < items; i++) {
                    int idx = (start + i) % 10;
                    if (ring[idx].length()) {
                      sendDataFile(remote, ring[idx]);
                      if (i < items - 1) remote.print(",");
                    }
                  }
                  remote.println("]");
                  xSemaphoreGive(xSDMutex);
                }
              } else if (cmd.length()) {
                remote.println("ERR:CMD");
              }
            }
            vTaskDelay(pdMS_TO_TICKS(20));  // باگ نسخه‌ی قبل: busy-loop بدون تاخیر
          }
          remote.stop();
          DEBUG_PRINTLN("[VIEW] Client disconnected.");
        }
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
