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
#include <esp_wifi.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
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

// سطح جزئیات لاگ:
//   0 = فقط رویدادهای مهم (توصیه‌شده برای کار عادی)
//   1 = معمولی: خلاصه‌ی سلامت و تغییر وضعیت‌ها  (پیش‌فرض)
//   2 = کامل: همه‌چیز، برای عیب‌یابی
#define LOG_LEVEL 1
#define FW_VERSION "2.3"

// --- ساختار واقعی سخت‌افزار -------------------------------------------------
//  دو رله = دو «فرمان» ، دو BCM = دو «دستگاه»
//
//     رله ۱ (پین 2)  = فرمان باز کردن  -> هم‌زمان روی BCM1 و BCM2 اثر می‌گذارد
//     رله ۲ (پین 4)  = فرمان بستن      -> هم‌زمان روی BCM1 و BCM2 اثر می‌گذارد
//
//  بنابراین هر سیکل دو فاز دارد و در هر فاز، فیدبک هر دو دستگاه
//  «در آنِ واحد» مانیتور می‌شود:
//
//     تحریک رله۱ ─► مانیتورینگ (BCM1 و BCM2) ─► قطع رله۱
//     تحریک رله۲ ─► مانیتورینگ (BCM1 و BCM2) ─► قطع رله۲
// -----------------------------------------------------------------------------
#define PHASE_OPEN 0
#define PHASE_CLOSE 1
#define PHASE_COUNT 2
#define DEVICE_COUNT 2

const uint8_t RELAY_PINS[PHASE_COUNT] = { 2, 4 };
const char *PHASE_NAMES[PHASE_COUNT] = { "OPEN", "CLOSE" };
const char *DEVICE_NAMES[DEVICE_COUNT] = { "BCM1", "BCM2" };

// FEEDBACK_PINS[فاز][دستگاه]
const uint8_t FEEDBACK_PINS[PHASE_COUNT][DEVICE_COUNT] = {
  { 13, 16 },  // فاز OPEN : فیدبکِ «باز شد» برای BCM1 و BCM2
  { 15, 17 },  // فاز CLOSE: فیدبکِ «بسته شد» برای BCM1 و BCM2
};

// یک دستگاه وقتی سالم است که هم باز شدنش و هم بسته شدنش تأیید شود.
// اگر false شود، دیدن یکی از دو فاز کافی است.
const bool REQUIRE_BOTH_FEEDBACKS = true;

// --- زمان‌بندی تست هر رله ---
const uint32_t RELAY_SETTLE_MS = 50;        // فاصله‌ی فعال شدن رله تا شروع مانیتورینگ
const uint32_t RELAY_RETRY_GAP_MS = 2000;   // فاصله‌ی بین تلاش‌ها
// فاصله‌ی «تریگ تا تریگ»: از لحظه‌ی فعال شدن رله‌ی اول تا لحظه‌ی فعال شدن
// رله‌ی دوم دقیقاً همین مقدار طول می‌کشد (شامل مدت مانیتورینگ).
const uint32_t PHASE_TRIGGER_INTERVAL_MS = 5000;

// حداقل فاصله‌ی خاموشی بین دو رله؛ اگر مدت مانیتورینگ از بازه‌ی بالا بیشتر
// شود، دست‌کم این مقدار فاصله رعایت می‌شود تا دو رله پشت سر هم نزنند.
const uint32_t PHASE_MIN_GAP_MS = 300;
const uint32_t FEEDBACK_WINDOW_MS = 3000;   // مهلت پاسخ BCM بعد از تریگ
const uint8_t RELAY_MAX_ATTEMPTS = 3;       // تعداد تلاش برای هر رله
const uint32_t PULSE_CONFIRM_MS = 100;      // حداقل مدت HIGH برای معتبر بودن پالس
const uint32_t CYCLE_PERIOD_MS = 120000;    // فاصله‌ی بین سیکل‌ها (۲ دقیقه)

// --- شبکه ---
const char *DATA_AP_SSID = "ESP8266_AP";  // گیرنده‌ی دیتا (سمت کامپیوتر)
const char *DATA_AP_PASS = "12345678";

// اتصال به گیرنده همیشه با همین دو مقدار بالا انجام می‌شود. این دو، سخت‌افزار
// ثابت پروژه‌اند و دلیلی ندارد از NVS خوانده شوند؛ تنظیمات ذخیره‌شده‌ی خراب
// (مثلاً رمز خالی) بزرگ‌ترین منبع «وصل نشدن» بود.
// اگر روزی خواستید از روی پورتال عوضش کنید، این را false کنید.
const bool FORCE_DEFAULT_WIFI = true;
IPAddress serverIP(192, 168, 4, 1);
const int serverPort = 80;

// --- پایداری اتصال ---
// آدرس‌دهی از DHCP خودِ گیرنده گرفته می‌شود؛ ساده‌ترین و مطمئن‌ترین حالت.
// توان فرستنده تطبیقی است: بر اساس قدرت سیگنالی که در اسکن دیده می‌شود
// انتخاب می‌شود. نزدیک که باشیم توان پایین می‌آید (جلوگیری از اشباع گیرنده
// و خراب شدن فریم‌های EAPOL)، دور که باشیم حداکثر توان استفاده می‌شود تا
// برد کم نشود.
//   rssi بهتر از -25  -> 11 dBm    (بردها چسبیده به هم)
//   rssi بهتر از -45  -> 15 dBm    (همان اتاق)
//   غیر این           -> 19.5 dBm  (حداکثر، برای برد زیاد)
#define STA_TX_POWER_NEAR WIFI_POWER_11dBm
#define STA_TX_POWER_MID WIFI_POWER_15dBm
#define STA_TX_POWER_FAR WIFI_POWER_19_5dBm

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;   // مهلت هر تلاش اتصال
const uint32_t WIFI_BACKOFF_MIN_MS = 2000;       // فاصله‌ی تلاش‌ها: از ۲ ثانیه
const uint32_t WIFI_BACKOFF_MAX_MS = 30000;      // تا سقف ۳۰ ثانیه
const uint32_t TCP_KEEPALIVE_MS = 25000;         // PING برای زنده نگه داشتن سوکت
// --- زمان‌بندی واچ‌داگ (بازبینی‌شده) ---
// هر تسک یک «ضربان» دارد. اگر ضربانی در بازه‌ی زیر تکان نخورد یعنی قفل کرده
// و برد کنترل‌شده ری‌استارت می‌شود. مقادیر با سرعت طبیعی هر تسک تنظیم شده‌اند:
//   • شبکه   : هر ≤۲ ثانیه یک ضربان  -> ۹۰ ثانیه سکوت = قفل
//   • رله    : هر سیکل یک ضربان      -> ۳ برابر دوره‌ی سیکل
//   • دیجیتال: در هر پنجره‌ی پایش     -> ۳ برابر دوره‌ی سیکل
//   • سنسور  : هر سیکل یک ضربان      -> ۳ برابر دوره‌ی سیکل
const uint32_t WDT_CHECK_PERIOD_MS = 15000;
const uint32_t WDT_TIMEOUT_NET_MS = 90000;
const uint32_t WDT_TIMEOUT_RELAY_MS = CYCLE_PERIOD_MS * 3;
const uint32_t WDT_TIMEOUT_DIGITAL_MS = CYCLE_PERIOD_MS * 3;
const uint32_t WDT_TIMEOUT_SHT_MS = CYCLE_PERIOD_MS * 3;
const uint32_t LINK_DOWN_RESET_MS = 600000;      // ۱۰ دقیقه قطعی بعد از اتصال موفق
const char *DEVICE_HOSTNAME = "RF-TESTER";

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
// مهلت باز ماندن پورتال SetClock (۲ دقیقه در هر دو حالت).
// اگر کسی وصل شود و ساعت را ست کند و حالت کاری را انتخاب کند، پورتال
// بلافاصله بسته می‌شود و منتظر پایان این زمان نمی‌ماند.
// پورتال در هر بوت باز می‌شود تا همیشه بتوانید با دکمه‌ی «تست رله‌ها»
// خودتان شروع را تأیید کنید. اگر false شود، فقط وقتی باز می‌شود که
// ساعت گرفته نشده باشد.
const bool ALWAYS_OPEN_PORTAL = true;

const uint32_t SETUP_PORTAL_TIMEOUT_MS = 120000;  // وقتی ساعت نامعتبر است
const uint32_t SETUP_PORTAL_GRACE_MS = 120000;    // وقتی RTC از قبل معتبر است

// تا وقتی گوشی به پورتال وصل است، شمارش معکوس متوقف می‌ماند و دستگاه
// منتظر می‌ماند تا خودتان دکمه‌ی شروع را بزنید.
// این عدد فقط یک تور ایمنی است: اگر گوشی وصل بماند ولی هیچ دکمه‌ای زده
// نشود، بعد از این مدت خودکار ادامه می‌دهد. صفر یعنی «بی‌نهایت صبر کن».
const uint32_t SETUP_PORTAL_MAX_WITH_CLIENT_MS = 900000;   // ۱۵ دقیقه
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

// لاگ پرجزئیات فقط در سطح ۲ چاپ می‌شود
#if DEBUG_MODE && (LOG_LEVEL >= 2)
#define VERBOSE_PRINTLN(x) Serial.println(x)
#define VERBOSE_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define VERBOSE_PRINTLN(x)
#define VERBOSE_PRINTF(...)
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
  // چهار نتیجه‌ی تفکیکی هر سیکل (ترتیب بایت‌ها دست‌نخورده است)
  bool BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE;
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
Preferences prefs;  // ذخیره‌ی دائمی تنظیمات وای‌فای در NVS

// تنظیمات وای‌فای که از پورتال گرفته و در حافظه‌ی داخلی ذخیره می‌شوند.
// مزیت: برای عوض کردن SSID/پسورد دیگر لازم نیست کد را دوباره کامپایل کنید.
String cfgDataSsid, cfgDataPass;  // اکسس‌پوینت گیرنده‌ی دیتا (ESP8266)
String cfgTimeSsid, cfgTimePass;  // مودم یا هات‌اسپات گوشی برای گرفتن ساعت
WiFiClient uploadClient;
WebServer setupServer(80);

volatile int currentGlobalID = 0;
volatile WifiData globalSystemState;

// نتیجه‌ی خام آخرین پنجره‌ی مانیتورینگ:  fbSeen[فاز][دستگاه]
volatile bool fbSeen[PHASE_COUNT][DEVICE_COUNT] = { { false, false }, { false, false } };

// پرچم‌های پورتال تنظیم ساعت
volatile bool portalTimeSet = false;
volatile bool portalModeChosen = false;
volatile bool portalWantsDataView = false;

// --- ضربان تسک‌ها برای ناظر پایداری ---
volatile uint32_t hbRelay = 0, hbDigital = 0, hbSht = 0, hbNet = 0;

// آمار پایداری شبکه
volatile uint32_t wifiDropCount = 0;   // چند بار لینک قطع شده
volatile uint8_t lastDropReason = 0;   // آخرین دلیل قطعی
const char *relayPhaseText = "بوت";    // الان در چه مرحله‌ای هستیم
volatile uint32_t sdWriteFailures = 0; // شکست‌های پیاپی نوشتن روی SD
const uint32_t SD_FAIL_LIMIT = 5;      // بعد از این تعداد، ری‌استارت
uint32_t lastTxMillis = 0;             // آخرین باری که چیزی روی سوکت فرستادیم

// Prototypes
void TaskHealthMonitor(void *pv);
void loadConfig();
void saveWifiConfig(const String &dSsid, const String &dPass,
                    const String &tSsid, const String &tPass);
bool syncTimeFromNtp();
void runSetupPortal(bool timeAlreadyValid);
bool rtcTimeLooksValid();
void wifiService();
void handleSerialCommands();
void waitForDataLink(uint32_t timeoutMs);
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void scanForDataAp();
static const char *wifiReasonText(uint8_t reason);
void saveToSD(const WifiData &data);

/** نتیجه‌ی پاک‌سازی کارت حافظه */
struct SdEraseResult {
  bool ok = false;
  uint32_t files = 0;
  uint32_t bytes = 0;
};
SdEraseResult eraseSdData();
static String dayFilePath(int y, int m, int d);
static String posPathOf(const String &datPath);
static uint32_t readUploadPos(const String &datPath);
static void writeUploadPos(const String &datPath, uint32_t pos);
static String baseNameOf(const char *rawName);
static String pickDayFile(bool oldest);
static int lastRecordIdOnSD();
void formatRecordLine(const WifiData &d, char *out, size_t outSize);
void sendRecord(WiFiClient &cl, const WifiData &d);
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
  DEBUG_PRINTF("\n[BOOT] ESP32 Industrial Controller FW %s\n", FW_VERSION);

  Wire.begin();
  delay(200);

  // ---------- تنظیمات پایداری وای‌فای ----------
  WiFi.persistent(false);        // ننوشتن روی فلش در هر اتصال (عمر فلش + سرعت)
  WiFi.setAutoReconnect(true);   // اتصال مجدد خودکار توسط خود استک
  WiFi.setSleep(false);          // خاموش کردن Modem-Sleep: مهم‌ترین علت قطعی‌های لحظه‌ای
  WiFi.setHostname(DEVICE_HOSTNAME);

  xDataQueue = xQueueCreate(20, sizeof(WifiData));
  xSDMutex = xSemaphoreCreateMutex();
  xGlobalStateMutex = xSemaphoreCreateMutex();
  xSystemEvents = xEventGroupCreate();

  WiFi.onEvent(onWiFiEvent);  // با اطلاعات دلیل قطعی
  loadConfig();
  rtc.initClock();

  // ---------------- SD & شماره‌ی رکورد ----------------
  if (xSemaphoreTake(xSDMutex, portMAX_DELAY)) {
    if (!SD.begin(SD_CS_PIN)) {
      Serial.println("[SD] Critical Error: SD Card not detected!");
    } else {
      DEBUG_PRINTLN("[SD] Card OK.");
      if (!SD.exists("/data")) SD.mkdir("/data");

      // مدل ذخیره‌سازی بهینه: به‌جای «یک فایل برای هر رکورد»، هر روز یک فایل
      // /data/YYYYMMDD.dat که رکوردهای ۲۵ بایتی پشت سر هم به آن append می‌شوند.
      // آخرین شماره‌ی رکورد = NUM آخرین رکورد جدیدترین فایل.
      currentGlobalID = lastRecordIdOnSD();
      int lastSaved = getNextPersistentID();
      if (lastSaved > currentGlobalID) currentGlobalID = lastSaved;
      DEBUG_PRINTF("[SD] Resuming from ID %d\n", currentGlobalID);
      saveNextPersistentID(currentGlobalID);
    }
    xSemaphoreGive(xSDMutex);
  }

  // ---------------- گرفتن خودکار تاریخ و ساعت ----------------
  // مرحله ۱: NTP از طریق مودم یا هات‌اسپات گوشی
  DEBUG_PRINTLN("[BOOT] مرحله ۱/۳ : گرفتن ساعت از NTP (حداکثر چند ثانیه)");
  bool timeOk = syncTimeFromNtp();

  // مرحله ۲/۳: پورتال محلی — صفحه‌ی وب ساعتِ گوشی را خودکار می‌فرستد،
  // و اگر کسی وصل نشد، با ساعت فعلی RTC ادامه می‌دهیم.
  if (!timeOk || ALWAYS_OPEN_PORTAL) {
    DEBUG_PRINTLN("[BOOT] مرحله ۲/۳ : پورتال SetClock — منتظر دکمه‌ی «تست رله‌ها»");
    runSetupPortal(timeOk || rtcTimeLooksValid());
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
    DEBUG_PRINTLN("[BOOT] مرحله ۳/۳ : اتصال به گیرنده (حداکثر ۲۰ ثانیه)");
    waitForDataLink(20000);
  }

  // ---------------- سنسور و پین‌ها ----------------
  if (!sht31.begin(0x44)) Serial.println("[SHT31] Error: sensor not found!");

  for (int p = 0; p < PHASE_COUNT; p++) {
    pinMode(RELAY_PINS[p], OUTPUT);
    digitalWrite(RELAY_PINS[p], LOW);
    for (int d = 0; d < DEVICE_COUNT; d++) {
      pinMode(FEEDBACK_PINS[p][d], INPUT_PULLDOWN);
    }
  }

  xEventGroupSetBits(xSystemEvents, BIT_NETWORK_BOOT_COMPLETE);

  // =============================================================
  //  تقسیم هسته‌ها برای ران‌تایم دائمی و بدون کرش
  //
  //  Core 1 (هسته‌ی کاربردی) : کارهای زمان‌بندی‌شده و GPIO
  //      - DigiRead  اولویت 6  : خواندن فیدبک، حساس‌ترین بخش زمانی
  //      - RelayCtrl اولویت 5  : تریگ رله‌ها
  //      - SHTRead   اولویت 3  : I2C
  //
  //  Core 0 (هسته‌ی شبکه)     : هرچه ممکن است بلاک شود
  //      - WiFiConn  اولویت 2  : TCP/SD، کنار خود درایور وای‌فای
  //      - Health    اولویت 1  : ناظر سلامت
  //
  //  دلیل: وقتی تسک شبکه روی همان هسته‌ی رله بود، بلاک شدن TCP
  //  باعث لرزش زمان‌بندی پالس‌ها و در بدترین حالت ریست واچ‌داگ می‌شد.
  // =============================================================
  xTaskCreatePinnedToCore(TaskDigitalRead, "DigiRead", 4096, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(TaskRelayControl, "RelayCtrl", 6144, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(TaskReadSHT, "SHTRead", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskInternalWiFiConnection, "WiFiConn", 10240, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskHealthMonitor, "Health", 3072, NULL, 1, NULL, 0);

  DEBUG_PRINTLN("[BOOT] همه‌ی تسک‌ها شروع شدند؛ سیکل اول تا چند ثانیه‌ی دیگر.");
}

void loop() {
  vTaskDelete(NULL);
}

// =====================================================================
//                       TIME: NTP  +  LOCAL PORTAL
// =====================================================================
// =====================================================================
//        تنظیمات وای‌فای در حافظه‌ی داخلی (NVS) — «اتصال آسان»
//  یک بار از روی پورتال وارد می‌شود و برای همیشه می‌ماند؛ نیازی به
//  کامپایل دوباره برای عوض کردن SSID یا پسورد نیست.
// =====================================================================
void loadConfig() {
  prefs.begin("rfcfg", true);  // read-only
  // شبکه‌ی ساعت همیشه از NVS خوانده می‌شود (کاربر از پورتال واردش می‌کند)
  cfgTimeSsid = prefs.getString("tssid", "");
  cfgTimePass = prefs.getString("tpass", "");

  if (FORCE_DEFAULT_WIFI) {
    cfgDataSsid = DATA_AP_SSID;
    cfgDataPass = DATA_AP_PASS;
    DEBUG_PRINTLN("[CFG] اکسس‌پوینت دیتا از مقادیر ثابت کد استفاده می‌کند");
  } else {
    cfgDataSsid = prefs.getString("dssid", DATA_AP_SSID);
    cfgDataPass = prefs.getString("dpass", DATA_AP_PASS);
  }
  prefs.end();

  // طول رمز چاپ می‌شود (نه خودش) تا اگر خالی یا ناقص ذخیره شده باشد، معلوم شود
  DEBUG_PRINTF("[CFG] data-AP='%s' passLen=%u | time-AP='%s' passLen=%u\n",
               cfgDataSsid.c_str(), (unsigned)cfgDataPass.length(),
               cfgTimeSsid.length() ? cfgTimeSsid.c_str() : "(none)",
               (unsigned)cfgTimePass.length());

  if (cfgDataPass.length() < 8) {
    DEBUG_PRINTLN("[CFG] هشدار: رمز ذخیره‌شده کوتاه‌تر از ۸ کاراکتر است -> "
                  "به مقدار پیش‌فرض برمی‌گردیم");
    cfgDataSsid = DATA_AP_SSID;
    cfgDataPass = DATA_AP_PASS;
  }
}

void saveWifiConfig(const String &dSsid, const String &dPass,
                    const String &tSsid, const String &tPass) {
  prefs.begin("rfcfg", false);
  if (dSsid.length()) {
    prefs.putString("dssid", dSsid);
    cfgDataSsid = dSsid;
    // رمز فقط وقتی نوشته می‌شود که واقعاً وارد شده باشد؛ فیلد خالی نباید
    // رمز قبلی را پاک کند (منبع باگِ «SSID درست ولی auth مدام fail»)
    if (dPass.length()) {
      prefs.putString("dpass", dPass);
      cfgDataPass = dPass;
    }
  }
  prefs.putString("tssid", tSsid);
  prefs.putString("tpass", tPass);
  cfgTimeSsid = tSsid;
  cfgTimePass = tPass;
  prefs.end();
  DEBUG_PRINTLN("[CFG] WiFi settings saved to NVS.");
}

bool rtcTimeLooksValid() {
  if (!rtc.read()) return false;
  uint8_t y = rtc.getYear(), mo = rtc.getMonth(), d = rtc.getDay();
  return (y >= 24 && y <= 99) && (mo >= 1 && mo <= 12) && (d >= 1 && d <= 31);
}

/** تلاش برای اتصال به شبکه‌های کاربر و گرفتن زمان از NTP */
bool syncTimeFromNtp() {
  // لیست تلاش: اول شبکه‌ای که کاربر از پورتال ذخیره کرده، بعد لیست کامپایلی
  String ssids[1 + 8];
  String passes[1 + 8];
  int n = 0;

  if (cfgTimeSsid.length()) {
    ssids[n] = cfgTimeSsid;
    passes[n] = cfgTimePass;
    n++;
  }
  for (int i = 0; i < TIME_NETWORK_COUNT && n < 9; i++) {
    ssids[n] = String(TIME_NETWORKS[i].ssid);
    passes[n] = String(TIME_NETWORKS[i].pass);
    n++;
  }

  if (n == 0) {
    DEBUG_PRINTLN("[TIME] No time-network saved; will ask the phone instead.");
    return false;
  }

  for (int i = 0; i < n; i++) {
    DEBUG_PRINTF("[TIME] Trying SSID '%s' ...\n", ssids[i].c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssids[i].c_str(), passes[i].c_str());

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
    for (int k = 0; k < 20; k++) {  // حداکثر ~۱۰ ثانیه
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

<div class="card"><h2>تنظیم وای‌فای (ذخیره‌ی دائمی)</h2>
<small>شبکه‌ای که برای گرفتن ساعت (اینترنت) استفاده می‌شود:</small>
<input id="tssid" placeholder="نام شبکه / SSID">
<input id="tpass" type="password" placeholder="رمز عبور">
<small>اکسس‌پوینت گیرنده‌ی دیتا (ESP8266):</small>
<input id="dssid" placeholder="ESP8266_AP">
<input id="dpass" type="password" placeholder="12345678">
<button class="ghost" onclick="saveWifi()">ذخیره در حافظه‌ی دستگاه</button>
<button class="ghost" onclick="scanWifi()">اسکن شبکه‌های اطراف</button>
<div id="scan"><small></small></div></div>

<div class="card"><h2>کارت حافظه</h2>
<small id="sdinfo">پاک کردن همه‌ی رکوردهای ذخیره‌شده روی کارت. برگشت‌ناپذیر است.</small>
<button class="ghost" onclick="formatSd()">پاک‌سازی کارت حافظه</button></div>

<div class="card"><h2>شروع کار</h2>
<small id="waitmsg">دستگاه منتظر شماست؛ تا این دکمه را نزنید تستی شروع نمی‌شود.</small>
<button onclick="mode(0)" style="font-size:17px;padding:16px">▶ تست رله‌ها را شروع کن</button>
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
function saveWifi(){
  var q='dssid='+encodeURIComponent(document.getElementById('dssid').value)
       +'&dpass='+encodeURIComponent(document.getElementById('dpass').value)
       +'&tssid='+encodeURIComponent(document.getElementById('tssid').value)
       +'&tpass='+encodeURIComponent(document.getElementById('tpass').value);
  fetch('/savewifi?'+q).then(r=>r.text()).then(t=>{
    document.getElementById('msg').innerHTML='<span class="ok">'+t+'</span>';});}
function scanWifi(){
  document.getElementById('scan').innerHTML='<small>در حال اسکن…</small>';
  fetch('/scan').then(r=>r.text()).then(t=>{
    document.getElementById('scan').innerHTML='<small>'+t+'</small>';});}
function formatSd(){
  if(!confirm('همه‌ی رکوردهای روی کارت حافظه پاک می‌شوند. مطمئن هستید؟'))return;
  document.getElementById('sdinfo').textContent='در حال پاک‌سازی…';
  fetch('/formatsd?confirm=YES').then(r=>r.text()).then(t=>{
    document.getElementById('sdinfo').textContent=t;
    document.getElementById('msg').innerHTML='<span class="ok">'+t+'</span>';});}
function loadCfg(){fetch('/cfg').then(r=>r.json()).then(c=>{
  document.getElementById('dssid').value=c.dssid; document.getElementById('tssid').value=c.tssid;});}
cur(); loadCfg(); sendNow();  // ارسال خودکار ساعت گوشی به محض باز شدن صفحه
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

void handlePortalCfg() {
  String json = "{\"dssid\":\"" + cfgDataSsid + "\",\"tssid\":\"" + cfgTimeSsid + "\"}";
  setupServer.send(200, "application/json", json);
}

void handlePortalSaveWifi() {
  saveWifiConfig(setupServer.arg("dssid"), setupServer.arg("dpass"),
                 setupServer.arg("tssid"), setupServer.arg("tpass"));
  setupServer.send(200, "text/plain; charset=utf-8", "تنظیمات وای‌فای ذخیره شد");
}

void handlePortalScan() {
  // اسکن نیاز به رابط STA دارد؛ اگر در حالت فقط-AP اسکن کنیم ممکن است
  // اکسس‌پوینت لحظه‌ای بیفتد و گوشی از پورتال پرت شود.
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  String out = "";
  for (int i = 0; i < n && i < 15; i++) {
    out += WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)<br>";
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_AP);
  if (!out.length()) out = "شبکه‌ای پیدا نشد";
  setupServer.send(200, "text/html; charset=utf-8", out);
}

void handlePortalFormatSd() {
  if (setupServer.arg("confirm") != "YES") {
    setupServer.send(400, "text/plain; charset=utf-8", "تأیید لازم است");
    return;
  }

  SdEraseResult r = eraseSdData();
  char msg[160];
  if (r.ok) {
    snprintf(msg, sizeof(msg),
             "کارت پاک شد: %u فایل (%u کیلوبایت). شماره‌ی رکورد از ۱ شروع می‌شود.",
             (unsigned)r.files, (unsigned)(r.bytes / 1024));
  } else {
    snprintf(msg, sizeof(msg), "پاک‌سازی ناموفق بود (کارت مشغول یا در دسترس نیست)");
  }
  setupServer.send(r.ok ? 200 : 500, "text/plain; charset=utf-8", msg);
}

void handlePortalMode() {
  int m = setupServer.arg("v").toInt();
  portalWantsDataView = (m == 1);
  portalModeChosen = true;
  setupServer.send(200, "text/plain; charset=utf-8",
                   m == 1 ? "حالت نمایش دیتا انتخاب شد"
                          : "تست رله‌ها شروع شد — می‌توانید این صفحه را ببندید");
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
  setupServer.on("/cfg", handlePortalCfg);
  setupServer.on("/savewifi", handlePortalSaveWifi);
  setupServer.on("/scan", handlePortalScan);
  setupServer.on("/formatsd", handlePortalFormatSd);
  setupServer.onNotFound(handlePortalRoot);  // Captive-portal-ish
  setupServer.begin();

  DEBUG_PRINTF("[PORTAL] SSID '%s' -> http://192.168.1.1/\n", SETUP_AP_SSID);

  portalTimeSet = timeAlreadyValid;
  portalModeChosen = false;
  portalWantsDataView = false;

  // در هر دو حالت ۲ دقیقه فرصت داده می‌شود تا با گوشی وصل شوید.
  // به‌محض اینکه ساعت ست شد و حالت کاری انتخاب شد، پورتال زودتر بسته می‌شود.
  const uint32_t portalLimit =
      timeAlreadyValid ? SETUP_PORTAL_GRACE_MS : SETUP_PORTAL_TIMEOUT_MS;
  DEBUG_PRINTF("[PORTAL] waiting up to %u s\n", (unsigned)(portalLimit / 1000));

  uint32_t start = millis();
  uint32_t lastTick = 0;
  uint32_t firstClientMs = 0;   // اولین باری که گوشی وصل شد
  bool clientSeen = false;

  for (;;) {
    setupServer.handleClient();

    // ---- خروج فقط با فشردن دکمه‌ی حالت کاری روی گوشی ----
    if (portalTimeSet && portalModeChosen) {
      delay(400);  // فرصت رسیدن پاسخ آخر به مرورگر
      DEBUG_PRINTLN("[PORTAL] دکمه‌ی شروع روی گوشی زده شد -> ادامه");
      break;
    }

    bool clientConnected = (WiFi.softAPgetStationNum() > 0);

    if (clientConnected) {
      // گوشی وصل است -> تایمر متوقف می‌شود و منتظر دکمه‌ی شروع می‌مانیم
      if (!clientSeen) {
        clientSeen = true;
        firstClientMs = millis();
        lastTick = 0;
        DEBUG_PRINTLN("[PORTAL] کلاینت وصل شد؛ شمارش معکوس متوقف شد. "
                      "دستگاه منتظر دکمه‌ی «تست رله‌ها» در http://192.168.1.1 است.");
      }
      start = millis();  // تایمر عملاً فریز می‌شود

      uint32_t waiting = millis() - firstClientMs;
      if (waiting / 15000 != lastTick) {
        lastTick = waiting / 15000;
        DEBUG_PRINTF("[PORTAL] منتظر دکمه‌ی «تست رله‌ها»... (%u ثانیه)\n",
                     (unsigned)(waiting / 1000));
      }

      // تور ایمنی: اگر گوشی وصل ماند ولی هیچ دکمه‌ای زده نشد
      if (SETUP_PORTAL_MAX_WITH_CLIENT_MS > 0 &&
          waiting > SETUP_PORTAL_MAX_WITH_CLIENT_MS) {
        DEBUG_PRINTLN("[PORTAL] گوشی وصل بود ولی دکمه‌ای زده نشد -> ادامه‌ی خودکار");
        break;
      }

    } else {
      // کسی وصل نیست -> همان شمارش معکوس عادی
      if (clientSeen) {
        clientSeen = false;
        lastTick = 0;
        DEBUG_PRINTLN("[PORTAL] گوشی قطع شد؛ شمارش معکوس از نو شروع شد");
      }

      uint32_t elapsed = millis() - start;
      if (elapsed / 5000 != lastTick) {
        lastTick = elapsed / 5000;
        DEBUG_PRINTF("[PORTAL] منتظر گوشی... %u ثانیه دیگر ادامه می‌دهیم (SSID: %s)\n",
                     (unsigned)((portalLimit - elapsed) / 1000), SETUP_AP_SSID);
      }
      if (elapsed >= portalLimit) break;
    }

    delay(2);
  }

  if (portalWantsDataView) {
    xEventGroupSetBits(xSystemEvents, BIT_REQUEST_AP_DATA_VIEW);
    DEBUG_PRINTLN("[PORTAL] حالت: نمایش دیتا");
  } else {
    xEventGroupClearBits(xSystemEvents, BIT_REQUEST_AP_DATA_VIEW);
    DEBUG_PRINTLN("[PORTAL] حالت: تست رله‌ها");
  }

  setupServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
}

/** اتصال (یا اتصال مجدد) به اکسس‌پوینت گیرنده‌ی دیتا */
// =====================================================================
//                   مدیریت لینک وای‌فای (صنعتی)
//
//  یک ماشین حالت ساده و قابل پیش‌بینی:
//
//      DOWN ──(اسکن + اتصال)──► UP
//        ▲                        │
//        └────(قطعی/تایم‌اوت)─────┘
//
//  قواعد:
//   • هیچ‌وقت دو تلاش اتصال هم‌پوشانی نمی‌کنند.
//   • فاصله‌ی تلاش‌ها نمایی است (۲ ► ۴ ► ۸ ► ۱۶ ► ۳۰ ثانیه) تا نه شبکه را
//     شخم بزنیم و نه بعد از یک قطعی لحظه‌ای دیر برگردیم.
//   • قبل از هر اتصال، کانال و BSSID دقیق با اسکن پیدا می‌شود.
//   • آمار لینک (اتصال‌ها، شکست‌ها، قطعی‌ها، مدت آپ‌تایم لینک) نگه داشته
//     می‌شود و در گزارش سلامت چاپ می‌شود.
// =====================================================================
struct LinkStats {
  uint32_t connects = 0;     // چند بار موفق وصل شده
  uint32_t failures = 0;     // چند تلاش ناموفق
  uint32_t upSinceMs = 0;    // لینک از چه زمانی بالاست
  uint32_t downSinceMs = 0;  // لینک از چه زمانی پایین است
  uint32_t backoffMs = WIFI_BACKOFF_MIN_MS;
  uint32_t nextTryMs = 0;
};

// نام «link» عمداً استفاده نشده: در unistd.h تابعی به همین نام وجود دارد
// و کامپایلر آن را به‌عنوان تعریف دوباره‌ی یک موجودیت دیگر رد می‌کند.
LinkStats wifiLink;

/** یک تلاش اتصال؛ بلاک‌کننده ولی کراندار. فقط از تسک شبکه صدا زده می‌شود. */
static bool wifiConnectOnce() {
  static uint8_t failStreak = 0;
  VERBOSE_PRINTF("[NET] اتصال به '%s' (passLen=%u) ...\n",
                 cfgDataSsid.c_str(), (unsigned)cfgDataPass.length());

  // هر ۶ شکست، درایور کاملاً خاموش و روشن می‌شود تا از حالت گیرکرده
  // (که خودش را به شکل هندشیک ناموفق نشان می‌دهد) بیرون بیاید.
  if (failStreak > 0 && failStreak % 6 == 0) {
    VERBOSE_PRINTLN("[NET] راه‌اندازی مجدد کامل درایور وای‌فای");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);
  }

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(STA_TX_POWER_FAR);   // پیش‌فرض: حداکثر برد (در اسکن تنظیم می‌شود)
  WiFi.setAutoReconnect(false);   // خودِ ما مدیریت می‌کنیم؛ تلاش‌های موازی ممنوع

  // کانال و BSSID دقیق را پیدا کن تا اتصال، کورکورانه روی همه‌ی کانال‌ها نباشد
  int32_t channel = 0;
  uint8_t bssid[6];
  bool haveBssid = false;
  int32_t bestRssi = -127;
  int bestEnc = -1;

  int found = WiFi.scanNetworks(false, true, false, 200);
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == cfgDataSsid && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      bestEnc = (int)WiFi.encryptionType(i);
      channel = WiFi.channel(i);
      memcpy(bssid, WiFi.BSSID(i), 6);
      haveBssid = true;
    }
  }
  WiFi.scanDelete();

  // ---- توان فرستنده متناسب با فاصله ----
  if (bestRssi > -25) {
    WiFi.setTxPower(STA_TX_POWER_NEAR);
    VERBOSE_PRINTLN("[NET] بردها خیلی نزدیک‌اند -> توان کم");
  } else if (bestRssi > -45) {
    WiFi.setTxPower(STA_TX_POWER_MID);
  } else {
    WiFi.setTxPower(STA_TX_POWER_FAR);
  }

  // BSSID کهنه یا کانال عوض‌شده هم می‌تواند هندشیک را خراب کند؛
  // بعد از سه شکست، اتصال ساده (بدون قید کانال) امتحان می‌شود.
  if (failStreak >= 3) haveBssid = false;

  if (haveBssid) {
    VERBOSE_PRINTF("[NET] AP پیدا شد: ch=%d rssi=%d enc=%d\n",
                   (int)channel, (int)bestRssi, (int)bestEnc);

    // سیگنال خیلی قوی = بردها بیش از حد به هم نزدیک‌اند
    if (bestRssi > -25) {
      DEBUG_PRINTF("[NET] هشدار: سیگنال بیش از حد قوی است (%d dBm). "
                   "بردها را کمی از هم دور کنید.\n", (int)bestRssi);
    }
    // 3 = WPA2_PSK ، 2 = WPA_PSK/TKIP ، 0 = باز
    if (bestEnc != 3 && bestEnc != 0) {
      DEBUG_PRINTF("[NET] هشدار: اکسس‌پوینت WPA2 خالص نیست (enc=%d) — "
                   "همین می‌تواند هندشیک را خراب کند.\n", (int)bestEnc);
    }
  }

  // =============================================================
  //  اتصال دومرحله‌ای — ریشه‌ی «4WAY_HANDSHAKE_TIMEOUT»
  //
  //  هسته‌ی ESP32 نسخه‌ی ۳ (IDF 5.x) به‌صورت پیش‌فرض PMF را «capable»
  //  اعلام می‌کند و آستانه‌ی احراز هویت را هم خودش می‌گذارد. اکسس‌پوینت
  //  ESP8266 (NONOS SDK) اصلاً PMF ندارد؛ نتیجه این می‌شود که مذاکره
  //  وسط مبادله‌ی چهارمرحله‌ای گیر می‌کند و تایم‌اوت می‌خورد — با رمز
  //  کاملاً درست.
  //
  //  برای همین ابتدا با connect=false تنظیمات ساخته می‌شود، بعد PMF
  //  خاموش و آستانه روی WPA2 ثابت می‌شود و بعد اتصال شروع می‌شود.
  // =============================================================
  if (haveBssid) {
    if (cfgDataPass.length() == 0) {
      WiFi.begin(cfgDataSsid.c_str(), (const char *)nullptr, channel, bssid, false);
    } else {
      WiFi.begin(cfgDataSsid.c_str(), cfgDataPass.c_str(), channel, bssid, false);
    }
  } else {
    VERBOSE_PRINTLN("[NET] AP در اسکن نبود؛ اتصال عادی امتحان می‌شود");
    if (cfgDataPass.length() == 0) {
      WiFi.begin(cfgDataSsid.c_str(), (const char *)nullptr, 0, nullptr, false);
    } else {
      WiFi.begin(cfgDataSsid.c_str(), cfgDataPass.c_str(), 0, nullptr, false);
    }
  }

  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = false;   // ESP8266 اصلاً PMF ندارد
    conf.sta.pmf_cfg.required = false;
    conf.sta.threshold.authmode =
        (cfgDataPass.length() == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    conf.sta.scan_method = WIFI_FAST_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }
  esp_wifi_connect();

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    failStreak = 0;
    return true;
  }
  failStreak++;
  return false;
}

/**
 * سرویس لینک — هر بار که تسک شبکه بیدار می‌شود صدا زده می‌شود.
 * هم در حالت عادی و هم وسط سیکل رله اجرا می‌شود (به SD دست نمی‌زند).
 */
/**
 * فرمان‌های سریال (در ترمینال Arduino تایپ کنید و Enter بزنید):
 *     FORMAT SD    -> پاک‌سازی کامل کارت حافظه
 *     STATUS       -> نمایش وضعیت لحظه‌ای
 */
void handleSerialCommands() {
  static String buf;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      buf.trim();
      if (buf.equalsIgnoreCase("FORMAT SD")) {
        Serial.println("[CMD] پاک‌سازی کارت حافظه ...");
        eraseSdData();
      } else if (buf.equalsIgnoreCase("STATUS")) {
        Serial.printf("[CMD] مرحله=%s | wifi=%s | رکورد بعدی=%d | heap=%uk\n",
                      relayPhaseText,
                      WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                      currentGlobalID + 1, (unsigned)(ESP.getFreeHeap() / 1024));
      } else if (buf.length()) {
        Serial.println("[CMD] فرمان‌ها: FORMAT SD | STATUS");
      }
      buf = "";
    } else if (buf.length() < 40) {
      buf += c;
    }
  }
}

void wifiService() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLink.upSinceMs == 0) {
      wifiLink.upSinceMs = millis();
      wifiLink.downSinceMs = 0;
      wifiLink.connects++;
      wifiLink.backoffMs = WIFI_BACKOFF_MIN_MS;
      DEBUG_PRINTF("[NET] لینک بالا آمد. IP: %s  RSSI: %d dBm  ch=%d\n",
                   WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    }

    // سوکت را بین دو سیکل زنده نگه می‌داریم
    if (uploadClient.connected() && millis() - lastTxMillis > TCP_KEEPALIVE_MS) {
      uploadClient.println("PING");
      lastTxMillis = millis();
      uint32_t t0 = millis();
      while (millis() - t0 < 500) {
        if (uploadClient.available()) {
          uploadClient.readStringUntil('\n');  // PONG
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    return;
  }

  // ---- لینک پایین است ----
  if (wifiLink.upSinceMs != 0) {
    wifiLink.upSinceMs = 0;
    wifiLink.downSinceMs = millis();
    if (uploadClient.connected()) uploadClient.stop();
  }
  if (wifiLink.downSinceMs == 0) wifiLink.downSinceMs = millis();

  if (millis() < wifiLink.nextTryMs) return;  // هنوز نوبت تلاش بعدی نشده

  if (wifiConnectOnce()) {
    wifiLink.backoffMs = WIFI_BACKOFF_MIN_MS;
    wifiLink.nextTryMs = 0;
    return;
  }

  wifiLink.failures++;
  // فقط تلاش‌های اول، پنجم، دهم... گزارش می‌شوند
  if (wifiLink.failures <= 2 || wifiLink.failures % 5 == 0) {
    DEBUG_PRINTF("[NET] وصل نشد (تلاش %u، دلیل %u %s) — تلاش بعدی %u ثانیه دیگر\n",
                 (unsigned)wifiLink.failures, lastDropReason,
                 wifiReasonText(lastDropReason),
                 (unsigned)(wifiLink.backoffMs / 1000));
  }

  // هر پنج شکست، یک اسکن تشخیصی کامل
  if (wifiLink.failures % 5 == 0) scanForDataAp();

  wifiLink.nextTryMs = millis() + wifiLink.backoffMs;
  wifiLink.backoffMs = (wifiLink.backoffMs * 2 > WIFI_BACKOFF_MAX_MS)
                     ? WIFI_BACKOFF_MAX_MS
                     : wifiLink.backoffMs * 2;
}

/** برای استفاده در setup: تا سقف مشخصی منتظر بالا آمدن لینک می‌ماند */
void waitForDataLink(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    wifiService();
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("[NET] فعلاً بدون شبکه ادامه می‌دهیم؛ دیتا روی SD می‌ماند.");
  }
}

/** ترجمه‌ی کد خطای قطعی وای‌فای به متن خوانا */
static const char *wifiReasonText(uint8_t reason) {
  switch (reason) {
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE (احراز هویت منقضی شد)";
    case 4: return "ASSOC_EXPIRE";
    case 5: return "ASSOC_TOOMANY (ظرفیت AP پر است)";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT (رمز اشتباه)";
    case 201: return "NO_AP_FOUND (AP دیده نمی‌شود)";
    case 202: return "AUTH_FAIL (رمز اشتباه)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default: return "?";
  }
}

/** اسکن تشخیصی: آیا اکسس‌پوینت هدف اصلاً در هوا هست؟ */
void scanForDataAp() {
  int n = WiFi.scanNetworks();
  bool found = false;

  for (int i = 0; i < n; i++) {
    bool isTarget = (WiFi.SSID(i) == cfgDataSsid);
    if (isTarget) {
      found = true;
      DEBUG_PRINTF("[SCAN] %s دیده شد: ch=%d rssi=%d enc=%d\n",
                   cfgDataSsid.c_str(), WiFi.channel(i), WiFi.RSSI(i),
                   (int)WiFi.encryptionType(i));
    }
    // فهرست کامل شبکه‌ها فقط در حالت عیب‌یابی کامل
    VERBOSE_PRINTF("[SCAN]   %-20s ch=%2d rssi=%4d\n",
                   WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  }
  WiFi.scanDelete();

  if (found) {
    DEBUG_PRINTLN("[SCAN] AP در دسترس است -> مشکل رمز یا گم شدن فریم‌های هندشیک است.");
    DEBUG_PRINTLN("[SCAN] بررسی کنید: ۱) رمز دو طرف یکی باشد  ۲) بردها ~۱ متر "
                  "فاصله داشته باشند  ۳) تغذیه‌ی ESP8266 پایدار باشد");
  } else {
    DEBUG_PRINTF("[SCAN] '%s' در هوا نیست! برد گیرنده روشن است؟ فاصله زیاد است؟\n",
                 cfgDataSsid.c_str());
  }
}

/** لاگ رویدادهای وای‌فای — برای اینکه دلیل قطعی‌ها معلوم شود */
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      DEBUG_PRINTLN("[NET] لینک وای‌فای برقرار شد");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      DEBUG_PRINTF("[NET] IP گرفت: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      wifiDropCount++;
      lastDropReason = info.wifi_sta_disconnected.reason;

      // به‌جای چاپ هر قطعی (که پنجره را پر می‌کرد) حداکثر هر ۳۰ ثانیه
      // یک خلاصه چاپ می‌شود.
      static uint32_t lastReport = 0;
      static uint32_t reportedAt = 0;
      if (millis() - lastReport > 30000) {
        uint32_t since = wifiDropCount - reportedAt;
        reportedAt = wifiDropCount;
        lastReport = millis();
        DEBUG_PRINTF("[NET] %u قطعی در ۳۰ ثانیه‌ی اخیر | دلیل: %u %s\n",
                     (unsigned)since, lastDropReason, wifiReasonText(lastDropReason));
      }
      break;
    }
    default:
      break;
  }
}

// =====================================================================
//     TASK: تست ترتیبی رله‌ها  (اول رله ۱ و BCM1 ، بعد رله ۲ و BCM2)
// =====================================================================

/** پنجره‌ی مانیتورینگ را باز می‌کند و پرچم‌های خام قبلی را صفر می‌کند */
static void beginFeedbackWindow() {
  for (int p = 0; p < PHASE_COUNT; p++)
    for (int d = 0; d < DEVICE_COUNT; d++) fbSeen[p][d] = false;

  // باگ: اگر بیت STOP از پنجره‌ی قبلی باقی مانده باشد، تسک خواندن به‌محض
  // شروع، پنجره را می‌بندد و هیچ پالسی دیده نمی‌شود (همه‌چیز NOK می‌شود).
  xEventGroupClearBits(xSystemEvents,
                       BIT_STOP_DIGITAL_MONITORING | BIT_DIGITAL_READ_COMPLETE);
  xEventGroupSetBits(xSystemEvents, BIT_START_DIGITAL_MONITORING);
}

static void endFeedbackWindow() {
  xEventGroupSetBits(xSystemEvents, BIT_STOP_DIGITAL_MONITORING);
  xEventGroupWaitBits(xSystemEvents, BIT_DIGITAL_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
}

/**
 * یک فاز کامل:  تحریک رله ► مانیتورینگ هم‌زمانِ هر دو BCM ► قطع رله
 * نتیجه در got[phase][device] جمع می‌شود (تجمعی است و پاک نمی‌شود).
 */
static void runPhase(int phase, bool got[PHASE_COUNT][DEVICE_COUNT],
                     uint32_t *triggeredAtMs = nullptr) {
  uint8_t pin = RELAY_PINS[phase];

  digitalWrite(pin, HIGH);                          // 1) تحریک رله
  if (triggeredAtMs) *triggeredAtMs = millis();     // لحظه‌ی دقیق تریگ
  vTaskDelay(pdMS_TO_TICKS(RELAY_SETTLE_MS));       //    پایدار شدن کنتاکت
  beginFeedbackWindow();                            // 2) مانیتورینگ فعال
  vTaskDelay(pdMS_TO_TICKS(FEEDBACK_WINDOW_MS));    // 3) زمان مجاز
  endFeedbackWindow();                              // 4) مانیتورینگ غیرفعال
  digitalWrite(pin, LOW);                           // 5) قطع رله

  for (int d = 0; d < DEVICE_COUNT; d++) {
    if (fbSeen[phase][d]) got[phase][d] = true;
  }

  DEBUG_PRINTF("[PHASE %s] %s:%s  %s:%s\n",
               PHASE_NAMES[phase],
               DEVICE_NAMES[0], fbSeen[phase][0] ? "YES" : "NO",
               DEVICE_NAMES[1], fbSeen[phase][1] ? "YES" : "NO");

  // تشخیص خطای سیم‌کشی: در فاز باز کردن نباید فیدبکِ «بسته شد» بیاید و برعکس.
  // این پالس‌ها عمداً به نتیجه اضافه نمی‌شوند، فقط هشدار داده می‌شود.
  int other = (phase == PHASE_OPEN) ? PHASE_CLOSE : PHASE_OPEN;
  for (int d = 0; d < DEVICE_COUNT; d++) {
    if (fbSeen[other][d]) {
      DEBUG_PRINTF("[PHASE %s] هشدار: در این فاز فیدبک %s از %s دیده شد (سیم‌کشی؟)\n",
                   PHASE_NAMES[phase], PHASE_NAMES[other], DEVICE_NAMES[d]);
    }
  }
}

/** آیا کار این دستگاه تمام است؟ */
static bool deviceDone(const bool got[PHASE_COUNT][DEVICE_COUNT], int d) {
  return REQUIRE_BOTH_FEEDBACKS
           ? (got[PHASE_OPEN][d] && got[PHASE_CLOSE][d])
           : (got[PHASE_OPEN][d] || got[PHASE_CLOSE][d]);
}

/**
 * یک سیکل کامل تست:
 *
 *      تحریک رله باز کردن  ► مانیتورینگ BCM1 و BCM2 ► قطع مانیتورینگ ► قطع رله
 *      تحریک رله بستن      ► مانیتورینگ BCM1 و BCM2 ► قطع مانیتورینگ ► قطع رله
 *
 * این دو فاز به هم وابسته‌اند و توالی دارند، پس در هر تکرار **هر دو** فرمان
 * حتماً داده می‌شوند؛ هیچ فازی رد نمی‌شود.
 *
 * کل سیکل حداکثر ۳ بار تکرار می‌شود. اگر بعد از یک سیکل هر دو دستگاه هم باز
 * شدن و هم بسته شدن را تأیید کرده باشند، تکرار بعدی انجام نمی‌شود.
 */
static void runTestCycle(bool got[PHASE_COUNT][DEVICE_COUNT]) {
  for (int p = 0; p < PHASE_COUNT; p++)
    for (int d = 0; d < DEVICE_COUNT; d++) got[p][d] = false;

  for (uint8_t attempt = 1; attempt <= RELAY_MAX_ATTEMPTS; attempt++) {
    relayPhaseText = "سیکل تست";
    DEBUG_PRINTF("\n[TEST] ===== سیکل %u/%u =====\n", attempt, RELAY_MAX_ATTEMPTS);

    // --- در هر سیکل، هر دو فرمان به ترتیب داده می‌شوند ---
    for (int phase = 0; phase < PHASE_COUNT; phase++) {
      uint32_t triggeredAt = 0;
      runPhase(phase, got, &triggeredAt);

      // فاصله فقط بین دو فاز معنی دارد، نه بعد از فاز آخر
      if (phase < PHASE_COUNT - 1) {
        uint32_t elapsed = millis() - triggeredAt;
        uint32_t waitMs = (elapsed < PHASE_TRIGGER_INTERVAL_MS)
                            ? (PHASE_TRIGGER_INTERVAL_MS - elapsed)
                            : 0;
        if (waitMs < PHASE_MIN_GAP_MS) waitMs = PHASE_MIN_GAP_MS;

        VERBOSE_PRINTF("[PHASE] %u ms تا تریگ رله‌ی بعدی\n", (unsigned)waitMs);
        vTaskDelay(pdMS_TO_TICKS(waitMs));
      }
    }

    // --- جمع‌بندی این سیکل ---
    bool allDone = true;
    for (int d = 0; d < DEVICE_COUNT; d++) {
      DEBUG_PRINTF("[TEST] %s -> Open:%s Close:%s %s\n",
                   DEVICE_NAMES[d],
                   got[PHASE_OPEN][d] ? "YES" : "NO",
                   got[PHASE_CLOSE][d] ? "YES" : "NO",
                   deviceDone(got, d) ? "(OK)" : "(ناقص)");
      if (!deviceDone(got, d)) allDone = false;
    }

    if (allDone) {
      DEBUG_PRINTF("[TEST] هر دو دستگاه OK در سیکل %u — تکرار لازم نیست\n", attempt);
      break;
    }

    if (attempt < RELAY_MAX_ATTEMPTS) {
      DEBUG_PRINTLN("[TEST] نتیجه ناقص -> کل سیکل دوباره تکرار می‌شود");
      vTaskDelay(pdMS_TO_TICKS(RELAY_RETRY_GAP_MS));
    }
  }

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
    hbRelay++;
    relayPhaseText = "شروع سیکل";
    DEBUG_PRINTLN("\n[CYCLE] ===== Started =====");
    xEventGroupClearBits(xSystemEvents, BIT_WIFI_PERMIT);

    // نتیجه‌ی تفکیکی: برای هر دستگاه، هم «باز شد» و هم «بسته شد»
    bool got[PHASE_COUNT][DEVICE_COUNT];
    runTestCycle(got);

    // ---- خواندن دما و رطوبت و ساعت ----
    xEventGroupClearBits(xSystemEvents, BIT_SHT_READ_COMPLETE);
    xEventGroupSetBits(xSystemEvents, BIT_START_SHT_READ);
    xEventGroupWaitBits(xSystemEvents, BIT_SHT_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));

    // ---- ثبت رکورد ----
    if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(1000))) {
      currentGlobalID++;
      globalSystemState.NUM = currentGlobalID;
      // نگاشت چهار فیلد پروتکل به چهار نتیجه‌ی تفکیکی این سیکل:
      //   BCM1_OPEN / BCM1_CLOSE  و  BCM2_OPEN / BCM2_CLOSE
      globalSystemState.BCM1_OPEN = got[PHASE_OPEN][0];
      globalSystemState.BCM1_CLOSE = got[PHASE_CLOSE][0];
      globalSystemState.BCM2_OPEN = got[PHASE_OPEN][1];
      globalSystemState.BCM2_CLOSE = got[PHASE_CLOSE][1];

      WifiData snapshot;
      memcpy(&snapshot, (const void *)&globalSystemState, sizeof(WifiData));
      xSemaphoreGive(xGlobalStateMutex);

      DEBUG_PRINTF("[CYCLE] #%d  %s[open:%s close:%s]  %s[open:%s close:%s]  "
                   "T=%.2f H=%.2f  @ %04d-%02d-%02d %02d:%02d:%02d\n",
                   snapshot.NUM,
                   DEVICE_NAMES[0], snapshot.BCM1_OPEN ? "OK" : "NOK",
                                    snapshot.BCM1_CLOSE ? "OK" : "NOK",
                   DEVICE_NAMES[1], snapshot.BCM2_OPEN ? "OK" : "NOK",
                                    snapshot.BCM2_CLOSE ? "OK" : "NOK",
                   snapshot.Temp, snapshot.Hum,
                   snapshot.Year, snapshot.Month, snapshot.Day,
                   snapshot.Hour, snapshot.Minute, snapshot.Second);

      if (xQueueSend(xDataQueue, (void *)&snapshot, pdMS_TO_TICKS(100)) != pdPASS) {
        // صف پر است -> همین‌جا مستقیم روی SD بنویس تا رکورد گم نشود
        DEBUG_PRINTLN("[CYCLE] صف پر بود؛ ذخیره‌ی مستقیم روی SD");
        if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(2000))) {
          saveToSD(snapshot);
          xSemaphoreGive(xSDMutex);
        } else {
          sdWriteFailures++;
        }
      }
      saveNextPersistentID(currentGlobalID);
    } else {
      DEBUG_PRINTLN("[CYCLE] Error: state mutex busy, record skipped.");
    }

    xEventGroupSetBits(xSystemEvents, BIT_WIFI_PERMIT);
    relayPhaseText = "انتظار تا سیکل بعد";
    vTaskDelayUntil(&lastWake, period);
  }
}

// =====================================================================
//        TASK: خواندن فیدبک‌های دیجیتال در طول پنجره‌ی مانیتورینگ
// =====================================================================
void TaskDigitalRead(void *pv) {
  // هر چهار پین فیدبک هم‌زمان مانیتور می‌شوند (هر دو BCM در آنِ واحد)
  const int pinCount = PHASE_COUNT * DEVICE_COUNT;
  uint8_t pins[PHASE_COUNT * DEVICE_COUNT];
  for (int p = 0; p < PHASE_COUNT; p++)
    for (int d = 0; d < DEVICE_COUNT; d++)
      pins[p * DEVICE_COUNT + d] = FEEDBACK_PINS[p][d];

  bool lastState[8];
  uint32_t highSince[8];
  bool confirmed[8];

  for (;;) {
    xEventGroupWaitBits(xSystemEvents, BIT_START_DIGITAL_MONITORING, pdTRUE, pdFALSE, portMAX_DELAY);
    hbDigital++;

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

    for (int p = 0; p < PHASE_COUNT; p++)
      for (int d = 0; d < DEVICE_COUNT; d++)
        if (confirmed[p * DEVICE_COUNT + d]) fbSeen[p][d] = true;

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
    hbSht++;

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

// =====================================================================
//   ذخیره‌سازی بهینه روی SD  (یک فایل در روز، نه یک فایل برای هر رکورد)
//
//   چرا؟ روی FAT هر فایل حداقل یک کلاستر (۴ تا ۳۲ کیلوبایت) جا می‌گیرد.
//   با رکورد ۲۵ بایتی یعنی بیش از ۹۹٪ فضا هدر می‌رفت و پیمایش پوشه هم
//   با زیاد شدن فایل‌ها به‌شدت کند می‌شد.
//
//   ساختار:
//     /data/YYYYMMDD.dat  -> رکوردهای ۲۵ بایتی پشت سر هم (append)
//     /data/YYYYMMDD.pos  -> آفست بایتیِ رکورد بعدی که باید آپلود شود
//
//   فایلِ .dat دقیقاً همان فرمتی است که صفحه‌ی /upload_dat در app.py
//   می‌خواند (len % 25 == 0)، پس آپلود دستی هم بدون تغییر کار می‌کند.
// =====================================================================
const size_t REC_SIZE = sizeof(WifiData);

static String dayFilePath(int y, int m, int d) {
  char buf[32];
  snprintf(buf, sizeof(buf), "/data/%04d%02d%02d.dat", y, m, d);
  return String(buf);
}

static String posPathOf(const String &datPath) {
  return datPath.substring(0, datPath.length() - 4) + ".pos";
}

static uint32_t readUploadPos(const String &datPath) {
  String pp = posPathOf(datPath);
  if (!SD.exists(pp)) return 0;
  File f = SD.open(pp, FILE_READ);
  if (!f) return 0;
  uint32_t v = (uint32_t)f.parseInt();
  f.close();
  return v;
}

static void writeUploadPos(const String &datPath, uint32_t pos) {
  File f = SD.open(posPathOf(datPath), FILE_WRITE);
  if (f) {
    f.print(pos);
    f.close();
  }
}

/** فقط نام فایل (بدون مسیر) را برمی‌گرداند؛ مستقل از نسخه‌ی core */
static String baseNameOf(const char *rawName) {
  String n = String(rawName);
  int slash = n.lastIndexOf('/');
  return (slash >= 0) ? n.substring(slash + 1) : n;
}

/** قدیمی‌ترین (یا جدیدترین) فایل روزانه‌ی موجود. نام فایل‌ها تاریخی است پس
    ترتیب الفبایی = ترتیب زمانی. */
static String pickDayFile(bool oldest) {
  File root = SD.open("/data");
  if (!root) return "";
  String best = "";
  File e = root.openNextFile();
  while (e) {
    String name = baseNameOf(e.name());
    if (name.endsWith(".dat")) {
      if (best.length() == 0 ||
          (oldest ? (name < best) : (name > best))) {
        best = name;
      }
    }
    e.close();
    e = root.openNextFile();
  }
  root.close();
  return best.length() ? ("/data/" + best) : "";
}

/** NUM آخرین رکورد ذخیره‌شده (برای ادامه‌ی شماره‌گذاری بعد از ریست) */
static int lastRecordIdOnSD() {
  String newest = pickDayFile(false);
  if (!newest.length()) return 0;
  File f = SD.open(newest, FILE_READ);
  if (!f) return 0;
  int id = 0;
  size_t size = f.size();
  if (size >= REC_SIZE) {
    f.seek((size / REC_SIZE - 1) * REC_SIZE);
    WifiData d;
    if (f.read((uint8_t *)&d, REC_SIZE) == (int)REC_SIZE) id = d.NUM;
  }
  f.close();
  return id;
}

/**
 * ذخیره‌ی رکورد روی SD — «دیتا هیچ‌وقت نباید گم شود»
 *
 * سه لایه محافظت:
 *   ۱) اگر باز کردن فایل شکست خورد، یک بار SD دوباره مقداردهی و تلاش می‌شود
 *   ۲) بعد از نوشتن، اندازه‌ی نوشته‌شده بررسی می‌شود
 *   ۳) شکست‌های پیاپی شمرده می‌شوند؛ ناظر سلامت بعد از SD_FAIL_LIMIT بار
 *      برد را ری‌استارت می‌کند
 */
// =====================================================================
//                    پاک‌سازی (فرمت) کارت حافظه
//
//  توجه: کتابخانه‌ی SD در هسته‌ی ESP32 «فرمت واقعی FAT» ندارد. کاری که
//  اینجا انجام می‌شود پاک کردن کامل محتواست: همه‌ی فایل‌های پوشه‌ی /data،
//  فایل آفست‌ها و شمارنده‌ی شماره‌ی رکورد. نتیجه از نظر کاربردی همان
//  «کارت خالی» است. اگر واقعاً فرمت سطح‌پایین لازم دارید، کارت را روی
//  کامپیوتر با FAT32 فرمت کنید.
//
//  ایمنی: فقط با تأیید صریح اجرا می‌شود و در حین اجرا قفل SD گرفته
//  می‌شود تا با نوشتن رکوردها تداخل نکند.
// =====================================================================
SdEraseResult eraseSdData() {
  SdEraseResult res;

  if (!xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(5000))) {
    Serial.println("[SD] پاک‌سازی انجام نشد: کارت مشغول است");
    return res;
  }

  Serial.println("[SD] شروع پاک‌سازی کارت حافظه ...");

  // ۱) همه‌ی فایل‌های پوشه‌ی داده
  File root = SD.open("/data");
  if (root) {
    File e = root.openNextFile();
    while (e) {
      String path = String(e.name());
      if (!path.startsWith("/")) path = "/data/" + path;
      uint32_t sz = e.size();
      e.close();

      if (SD.remove(path)) {
        res.files++;
        res.bytes += sz;
      } else {
        Serial.printf("[SD] حذف نشد: %s\n", path.c_str());
      }
      e = root.openNextFile();
    }
    root.close();
  }

  // ۲) شمارنده‌ی شماره‌ی رکورد
  if (SD.exists("/last_id.txt") && SD.remove("/last_id.txt")) res.files++;

  // ۳) ساخت دوباره‌ی ساختار پوشه‌ها
  if (!SD.exists("/data")) SD.mkdir("/data");

  // ۴) شماره‌گذاری از صفر
  currentGlobalID = 0;
  saveNextPersistentID(0);
  sdWriteFailures = 0;

  res.ok = true;
  xSemaphoreGive(xSDMutex);

  Serial.printf("[SD] پاک‌سازی تمام شد: %u فایل (%u کیلوبایت) حذف شد، "
                "شماره‌ی رکورد از ۱ شروع می‌شود\n",
                (unsigned)res.files, (unsigned)(res.bytes / 1024));
  return res;
}

void saveToSD(const WifiData &data) {
  String path = dayFilePath(data.Year, data.Month, data.Day);

  for (int attempt = 0; attempt < 2; attempt++) {
    File f = SD.open(path, FILE_APPEND);
    if (!f) f = SD.open(path, FILE_WRITE);  // اولین رکورد امروز

    if (f) {
      size_t written = f.write((const uint8_t *)&data, REC_SIZE);
      f.flush();
      f.close();

      if (written == REC_SIZE) {
        sdWriteFailures = 0;
        VERBOSE_PRINTF("[SD] ذخیره شد #%d -> %s\n", data.NUM, path.c_str());
        return;
      }
      DEBUG_PRINTF("[SD] نوشتن ناقص بود (%u از %u بایت)\n",
                   (unsigned)written, (unsigned)REC_SIZE);
    }

    if (attempt == 0) {
      DEBUG_PRINTLN("[SD] تلاش دوم: مقداردهی مجدد کارت حافظه");
      SD.end();
      delay(50);
      if (!SD.begin(SD_CS_PIN)) DEBUG_PRINTLN("[SD] مقداردهی مجدد ناموفق بود");
      if (!SD.exists("/data")) SD.mkdir("/data");
    }
  }

  sdWriteFailures++;
  DEBUG_PRINTF("[SD] بحرانی: رکورد #%d ذخیره نشد (شکست پیاپی: %u)\n",
               data.NUM, (unsigned)sdWriteFailures);
}

/**
 * تنها جایی که فرمت خروجی دیتا ساخته می‌شود.
 * دقیقاً همان خطی که:
 *   - از طریق ESP8266 روی سریال به app.py می‌رسد
 *   - و در «حالت دیتا» به هر کلاینتی (گوشی یا کامپیوتر) داده می‌شود
 * یعنی گوشی و سیستم عیناً یک فرمت می‌بینند و هر دو مستقیم به app.py
 * (تابع parse_industrial_line) خورانده می‌شوند.
 */
void formatRecordLine(const WifiData &d, char *out, size_t outSize) {
  snprintf(out, outSize,
           "NUM=%d,BCM1_OPEN=%s,BCM1_CLOSE=%s,BCM2_OPEN=%s,BCM2_CLOSE=%s,"
           "Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d",
           d.NUM,
           d.BCM1_OPEN ? "OK" : "NOK",
           d.BCM1_CLOSE ? "OK" : "NOK",
           d.BCM2_OPEN ? "OK" : "NOK",
           d.BCM2_CLOSE ? "OK" : "NOK",
           d.Temp, d.Hum,
           d.Year, d.Month, d.Day,
           d.Hour, d.Minute, d.Second);
}

void sendRecord(WiFiClient &cl, const WifiData &d) {
  char buf[300];
  formatRecordLine(d, buf, sizeof(buf));
  cl.println(buf);
}

/** n رکورد آخرِ جدیدترین فایل روزانه را می‌خواند (جدید -> قدیم) */
static int readLastRecords(WifiData *out, int maxCount) {
  String newest = pickDayFile(false);
  if (!newest.length()) return 0;
  File f = SD.open(newest, FILE_READ);
  if (!f) return 0;

  size_t total = f.size() / REC_SIZE;
  int n = (int)((total < (size_t)maxCount) ? total : (size_t)maxCount);
  for (int i = 0; i < n; i++) {
    f.seek((total - 1 - i) * REC_SIZE);
    if (f.read((uint8_t *)&out[i], REC_SIZE) != (int)REC_SIZE) {
      f.close();
      return i;
    }
  }
  f.close();
  return n;
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

  for (;;) {
    hbNet++;
    handleSerialCommands();   // FORMAT SD / STATUS
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

        // الف) ذخیره‌سازی اولویت مطلق دارد و به اجازه‌ی شبکه ربطی ندارد.
        //     (قبلاً پشت انتظارِ اجازه بود و اگر سیکل طول می‌کشید، رکوردها
        //      در صف می‌ماندند.)
        while (xQueueReceive(xDataQueue, &q, pdMS_TO_TICKS(10)) == pdPASS) {
          if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(2000))) {
            saveToSD(q);
            xSemaphoreGive(xSDMutex);
          } else {
            // نتوانستیم قفل بگیریم؛ رکورد را برمی‌گردانیم تا گم نشود
            xQueueSendToFront(xDataQueue, (void *)&q, pdMS_TO_TICKS(100));
            DEBUG_PRINTLN("[SD] قفل آزاد نشد؛ رکورد در صف ماند");
            break;
          }
        }

        // ب) کارهای شبکه فقط با اجازه (تا با زمان‌بندی رله تداخل نکند)
        EventBits_t permit = xEventGroupWaitBits(xSystemEvents, BIT_WIFI_PERMIT,
                                                 pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
        if (!(permit & BIT_WIFI_PERMIT)) {
          wifiService();   // سیکل رله در جریان است؛ فقط لینک را نگه می‌داریم
          break;
        }

        // ب) اگر شبکه قطع است، هر ۲۰ ثانیه دوباره تلاش کن (باگ نسخه‌ی قبل)
        wifiService();
        if (WiFi.status() != WL_CONNECTED) {
          vTaskDelay(pdMS_TO_TICKS(500));
          break;
        }

        // ج) Store & Forward با آفست:
        //    قدیمی‌ترین فایل روزانه را برمی‌داریم، از روی آفستِ ذخیره‌شده
        //    رکورد بعدی را می‌خوانیم و می‌فرستیم. بعد از ACK فقط آفست جلو
        //    می‌رود (نه حذف فایل) -> نوشتن روی SD خیلی کمتر و امن‌تر می‌شود.
        if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(100))) {
          String dayFile = pickDayFile(true);
          if (dayFile.length()) {
            uint32_t pos = readUploadPos(dayFile);
            File f = SD.open(dayFile, FILE_READ);
            if (f) {
              size_t fileSize = f.size();

              if (pos + REC_SIZE <= fileSize) {
                WifiData stored;
                f.seek(pos);
                bool readOk = (f.read((uint8_t *)&stored, REC_SIZE) == (int)REC_SIZE);
                f.close();

                // اتصال باز نگه داشته می‌شود؛ فقط اگر قطع بود دوباره وصل می‌شویم.
                // (قبلاً برای هر رکورد یک اتصال جدید باز و بسته می‌شد که هم
                //  کند بود و هم روی ESP8266 مدام «client connected/disconnected»
                //  تولید می‌کرد.)
                bool linkReady = uploadClient.connected();
                if (!linkReady) {
                  linkReady = uploadClient.connect(serverIP, serverPort);
                  if (linkReady) {
                    uploadClient.setNoDelay(true);
                    VERBOSE_PRINTLN("[UPLOAD] اتصال TCP برقرار شد");
                  }
                }

                if (readOk && linkReady) {
                  // این فرمت باید دقیقاً با sscanf سمت ESP8266 و با
                  // parse_industrial_line در app.py یکی بماند (۱۳ فیلد)
                  char buf[300];
                  formatRecordLine(stored, buf, sizeof(buf));  // فرمت واحد پروژه
                  uploadClient.println(buf);
                  lastTxMillis = millis();

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
                    pos += REC_SIZE;
                    writeUploadPos(dayFile, pos);
                    VERBOSE_PRINTF("[UPLOAD] #%d sent, offset -> %u/%u\n",
                                 stored.NUM, (unsigned)pos, (unsigned)fileSize);
                  } else {
                    DEBUG_PRINTLN("[UPLOAD] No ACK, will retry same record.");
                    uploadClient.stop();  // اتصال مشکوک -> دور بعد تازه باز شود
                  }
                }
              } else {
                f.close();
                // فایل کامل آپلود شده؛ اگر مربوط به امروز نیست پاکش کن
                rtc.read();
                String today = dayFilePath(2000 + rtc.getYear(), rtc.getMonth(), rtc.getDay());
                if (dayFile != today) {
                  SD.remove(posPathOf(dayFile));
                  SD.remove(dayFile);
                  DEBUG_PRINTF("[UPLOAD] %s fully uploaded -> removed\n", dayFile.c_str());
                }
              }
            }
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

              // ---------------------------------------------------------
              //  «حالت دیتا» — پاسخ برای هر وسیله‌ای که وصل شود یکسان است
              //  (گوشی، لپ‌تاپ، اسکریپت پایتون ... فرقی نمی‌کند)
              //  خروجی: خط(های) NUM=... که مستقیماً خوراک app.py هستند،
              //          و در انتها یک خط "END".
              // ---------------------------------------------------------
              if (cmd.equalsIgnoreCase("sync")) {
                if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                  WifiData one;
                  int n = readLastRecords(&one, 1);
                  if (n == 1) sendRecord(remote, one);
                  else remote.println("NO_DATA");
                  remote.println("END");
                  xSemaphoreGive(xSDMutex);
                }
              } else if (cmd.equalsIgnoreCase("sync10")) {
                if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                  WifiData last10[10];
                  int n = readLastRecords(last10, 10);
                  if (n == 0) {
                    remote.println("NO_DATA");
                  } else {
                    for (int i = n - 1; i >= 0; i--) sendRecord(remote, last10[i]);
                  }
                  remote.println("END");
                  xSemaphoreGive(xSDMutex);
                }
              } else if (cmd.equalsIgnoreCase("syncall")) {
                // کل فایل روزِ جاری: برای وقتی که می‌خواهید همه‌چیز را
                // یک‌جا به app.py بدهید
                if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                  String newest = pickDayFile(false);
                  int sent = 0;
                  if (newest.length()) {
                    File f = SD.open(newest, FILE_READ);
                    if (f) {
                      WifiData d;
                      while (f.read((uint8_t *)&d, REC_SIZE) == (int)REC_SIZE) {
                        sendRecord(remote, d);
                        sent++;
                        if ((sent % 20) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                      }
                      f.close();
                    }
                  }
                  if (sent == 0) remote.println("NO_DATA");
                  remote.println("END");
                  xSemaphoreGive(xSDMutex);
                }
              } else if (cmd.equalsIgnoreCase("format")) {
                remote.println("ERR:CONFIRM  (برای پاک‌سازی بنویسید: format CONFIRM)");
                remote.println("END");
              } else if (cmd.equalsIgnoreCase("format CONFIRM")) {
                SdEraseResult r = eraseSdData();
                if (r.ok) {
                  remote.printf("SD_ERASED files=%u kb=%u\n",
                                (unsigned)r.files, (unsigned)(r.bytes / 1024));
                } else {
                  remote.println("ERR:SD_BUSY");
                }
                remote.println("END");
              } else if (cmd.equalsIgnoreCase("info")) {
                rtc.read();
                remote.printf("DEVICE=RF_TESTER,FW=2.2,TIME=%s,HEAP=%u\n",
                              rtc.isoString().c_str(), (unsigned)ESP.getFreeHeap());
                remote.println("END");
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

// =====================================================================
//   ناظر سلامت — ران‌تایم دائمی
//
//   هر ۳۰ ثانیه:
//     • حافظه‌ی آزاد و کمترین حافظه‌ی تجربه‌شده را لاگ می‌کند
//     • ته‌مانده‌ی استک هر تسک را چاپ می‌کند (هشدار قبل از سرریز)
//     • ضربان تسک‌ها را می‌سنجد؛ اگر تسک شبکه یا رله برای مدت طولانی
//       هیچ پیشرفتی نداشته باشد، برد را کنترل‌شده ری‌استارت می‌کند تا
//       دستگاه در حالت نیمه‌مرده باقی نماند.
//
//   عمداً از esp_task_wdt استفاده نشده چون امضای آن بین نسخه‌های
//   core 2.x و 3.x فرق دارد و کد را غیرقابل‌کامپایل می‌کند.
// =====================================================================
void TaskHealthMonitor(void *pv) {
  const TickType_t period = pdMS_TO_TICKS(WDT_CHECK_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  // آخرین مقدار و آخرین زمانی که هر ضربان تغییر کرده است
  struct Beat {
    const char *name;
    volatile uint32_t *counter;
    uint32_t timeoutMs;
    uint32_t lastValue;
    uint32_t lastChangeMs;
  };

  Beat beats[] = {
    { "شبکه", &hbNet, WDT_TIMEOUT_NET_MS, 0, millis() },
    { "رله", &hbRelay, WDT_TIMEOUT_RELAY_MS, 0, millis() },
    { "دیجیتال", &hbDigital, WDT_TIMEOUT_DIGITAL_MS, 0, millis() },
    { "سنسور", &hbSht, WDT_TIMEOUT_SHT_MS, 0, millis() },
  };
  const int beatCount = sizeof(beats) / sizeof(beats[0]);

  // در حالت نمایش دیتا فقط تسک شبکه زنده است
  bool viewMode = (xEventGroupGetBits(xSystemEvents) & BIT_REQUEST_AP_DATA_VIEW) != 0;

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    size_t freeHeap = ESP.getFreeHeap();
    size_t minHeap = ESP.getMinFreeHeap();

    // یک خط خلاصه (سطح ۱) — جزئیات کامل فقط در سطح ۲
    DEBUG_PRINTF("[OK] %s | wifi=%s%d | سیکل=%u | heap=%uk | up=%lus\n",
                 relayPhaseText,
                 WiFi.status() == WL_CONNECTED ? "UP " : "DOWN ",
                 WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                 (unsigned)hbRelay,
                 (unsigned)(freeHeap / 1024),
                 (unsigned long)(millis() / 1000));

    VERBOSE_PRINTF("[HEALTH] heapMin=%u | hb D:%u S:%u N:%u | drops=%u sdErr=%u | "
                   "connects=%u failures=%u\n",
                   (unsigned)minHeap,
                   (unsigned)hbDigital, (unsigned)hbSht, (unsigned)hbNet,
                   (unsigned)wifiDropCount, (unsigned)sdWriteFailures,
                   (unsigned)wifiLink.connects, (unsigned)wifiLink.failures);

    if (freeHeap < 20000) DEBUG_PRINTLN("[HEALTH] هشدار: حافظه کم است!");

    // ---------------- واچ‌داگ ضربان تسک‌ها ----------------
    for (int i = 0; i < beatCount; i++) {
      // در حالت نمایش دیتا، تسک‌های رله/دیجیتال/سنسور عمداً وجود ندارند
      if (viewMode && i > 0) continue;

      uint32_t now = *(beats[i].counter);
      if (now != beats[i].lastValue) {
        beats[i].lastValue = now;
        beats[i].lastChangeMs = millis();
        continue;
      }

      uint32_t silent = millis() - beats[i].lastChangeMs;
      if (silent > beats[i].timeoutMs) {
        Serial.printf("[WDT] تسک «%s» %lu ثانیه است پیشرفتی ندارد -> ری‌استارت\n",
                      beats[i].name, (unsigned long)(silent / 1000));
        Serial.flush();
        delay(200);
        ESP.restart();
      } else if (silent > beats[i].timeoutMs / 2) {
        DEBUG_PRINTF("[WDT] هشدار: «%s» %lu ثانیه ساکت است (آستانه %lu ثانیه)\n",
                     beats[i].name, (unsigned long)(silent / 1000),
                     (unsigned long)(beats[i].timeoutMs / 1000));
      }
    }

    // ---------------- قطعی طولانی لینک ----------------
    if (!viewMode && wifiLink.connects > 0 && wifiLink.downSinceMs != 0 &&
        millis() - wifiLink.downSinceMs > LINK_DOWN_RESET_MS) {
      Serial.println("[WDT] لینک بعد از اتصال موفق، طولانی قطع مانده -> ری‌استارت");
      Serial.flush();
      delay(200);
      ESP.restart();
    }

    // ---------------- خطای مکرر کارت حافظه ----------------
    if (sdWriteFailures >= SD_FAIL_LIMIT) {
      Serial.println("[WDT] نوشتن روی SD مکرراً شکست خورد -> ری‌استارت");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
  }
}
