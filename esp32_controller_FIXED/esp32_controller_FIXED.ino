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
#include <esp_log.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
#include "SD.h"
#include <LittleFS.h>
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

// -----------------------------------------------------------------------------
//  دو سیگنال دیجیتال اضافی (همیشه خوانده و فرستاده می‌شوند، بدون سوییچ
//  سمت فرم‌ور؛ تصمیم «ذخیره در دیتابیس یا نه» کاملاً سمت سرور است):
//    GPIO34 = Indicator : سیگنال سطح (high-side)، دقیقاً مثل ۴ سیگنال
//                         BCM بالا با همان الگوریتم debounce خوانده می‌شود.
//    GPIO35 = Buzzer    : پالس مربعی تأییدیه‌ی بازر (۵۰۰هرتز..۴کیلوهرتز)؛
//                         چون فرکانسش بسیار بالاتر از پولینگ ۱۰ میلی‌ثانیه‌ای
//                         TaskDigitalRead است، با وقفه‌ی سخت‌افزاری (CHANGE)
//                         لبه‌شماری می‌شود.
//  هر دو پایه روی ESP32 «ورودی‌خالص» هستند (بدون پول‌آپ/پول‌داون داخلی)؛
//  مقاومت pull مناسب باید روی سخت‌افزار بیرونی تعبیه شده باشد.
// -----------------------------------------------------------------------------
const uint8_t PIN_INDICATOR = 34;
const uint8_t PIN_BUZZER = 35;
// حداقل تعداد لبه (rising+falling) طی یک پنجره‌ی مانیتورینگ (FEEDBACK_WINDOW_MS)
// که برای تایید «بازر فعال بود» لازم است. در پایین‌ترین فرکانس مجاز (۵۰۰هرتز)
// طی ۳ ثانیه، حدوداً ۳۰۰۰ لبه انتظار می‌رود؛ آستانه را خیلی پایین‌تر از این
// می‌گذاریم تا حتی یک فعال‌سازی کوتاه (کسری از پنجره) هم به اندازه‌ی کافی
// نویز را از سیگنال واقعی تفکیک کند.
const uint32_t BUZZER_EDGE_THRESHOLD = 10;

// --- زمان‌بندی تست هر رله ---
const uint32_t RELAY_SETTLE_MS = 50;        // فاصله‌ی فعال شدن رله تا شروع مانیتورینگ
// RELAY_RETRY_GAP_MS از سرور قابل تغییر است (دستور CFG از طریق ESP8266)؛
// مقدار پیش‌فرض و بازه‌ی مجاز پایین تعریف شده، مقدار جاری در gRelayRetryGapMs
// نگه داشته می‌شود و در NVS هم ذخیره می‌شود تا بعد از ریست باقی بماند.
const uint32_t DEFAULT_RELAY_RETRY_GAP_MS = 2000;
const uint32_t MIN_RELAY_RETRY_GAP_MS = 200;      // حداقل مجاز (ایمنی رله)
const uint32_t MAX_RELAY_RETRY_GAP_MS = 60000;    // حداکثر مجاز (۶۰ ثانیه)
volatile uint32_t gRelayRetryGapMs = DEFAULT_RELAY_RETRY_GAP_MS;
// فاصله‌ی «تریگ تا تریگ»: از لحظه‌ی فعال شدن رله‌ی اول تا لحظه‌ی فعال شدن
// رله‌ی دوم دقیقاً همین مقدار طول می‌کشد (شامل مدت مانیتورینگ).
const uint32_t PHASE_TRIGGER_INTERVAL_MS = 5000;

// حداقل فاصله‌ی خاموشی بین دو رله؛ اگر مدت مانیتورینگ از بازه‌ی بالا بیشتر
// شود، دست‌کم این مقدار فاصله رعایت می‌شود تا دو رله پشت سر هم نزنند.
const uint32_t PHASE_MIN_GAP_MS = 300;
const uint32_t FEEDBACK_WINDOW_MS = 3000;   // مهلت پاسخ BCM بعد از تریگ
const uint8_t RELAY_MAX_ATTEMPTS = 3;       // تعداد تلاش برای هر رله
const uint32_t PULSE_CONFIRM_MS = 100;      // حداقل مدت HIGH برای معتبر بودن پالس
// CYCLE_PERIOD_MS هم مثل RELAY_RETRY_GAP_MS از سرور قابل تغییر است؛ همان
// الگو: پیش‌فرض + بازه‌ی مجاز + متغیر سراسری قابل‌تغییر که در NVS می‌ماند.
const uint32_t DEFAULT_CYCLE_PERIOD_MS = 120000;  // ۲ دقیقه
// حداقل مجاز: مقادیر کمتر از این اصلاً قبول نمی‌شوند (نه کلمپ به این
// عدد؛ کلاً رد می‌شوند و مقدار قبلی دست‌نخورده می‌ماند — به درخواست کاربر)
const uint32_t MIN_CYCLE_PERIOD_MS = 35000;       // حداقل مجاز (۳۵ ثانیه)
const uint32_t MAX_CYCLE_PERIOD_MS = 3600000;     // حداکثر مجاز (۱ ساعت)
volatile uint32_t gCyclePeriodMs = DEFAULT_CYCLE_PERIOD_MS;
// فاصله‌ی هر بار «بیدار شدن» در حلقه‌ی انتظار بین سیکل‌ها (TaskRelayControl)
// برای چک کردن اینکه آیا gCyclePeriodMs از سرور عوض شده یا نه. هرچه کوچک‌تر،
// واکنش به تغییر زمان سریع‌تر است؛ ۲۰۰ میلی‌ثانیه برای این منظور کافی است.
const uint32_t CYCLE_WAIT_POLL_MS = 200;

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
// توان فرستنده.
//
// FORCE_MAX_TX_POWER = true  -> همیشه حداکثر (۱۹.۵ dBm) برای بیشترین برد.
// FORCE_MAX_TX_POWER = false -> تطبیقی بر اساس سیگنال اسکن؛ فقط وقتی لازم
//   است که دو برد چسبیده به هم باشند و اشباع گیرنده هندشیک را خراب کند.
const bool FORCE_MAX_TX_POWER = true;
#define STA_TX_POWER_NEAR WIFI_POWER_11dBm
#define STA_TX_POWER_MID WIFI_POWER_15dBm
#define STA_TX_POWER_FAR WIFI_POWER_19_5dBm

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;   // مهلت هر تلاش اتصال
const uint32_t WIFI_BACKOFF_MIN_MS = 2000;       // فاصله‌ی تلاش‌ها: از ۲ ثانیه
const uint32_t WIFI_BACKOFF_MAX_MS = 30000;      // تا سقف ۳۰ ثانیه
const uint32_t TCP_KEEPALIVE_MS = 25000;         // PING برای زنده نگه داشتن سوکت

// --- پروتکل انتقال مطمئن v2 (لینک سه‌مرحله‌ای با گیرنده/سرور) ---
// ESP32 هیچ رکوردی را «ارسال‌شده» نمی‌داند مگر اینکه ACK واقعی سرور
// (از مسیر گیرنده) برایش برگردد. آفست آپلود فقط به اندازه‌ی پیشوندِ
// پیاپیِ تاییدشده جلو می‌رود؛ بقیه دور بعد دوباره ارسال می‌شوند.
const uint8_t  ACK_WINDOW      = 8;      // چند رکورد در هر پنجره می‌رود
const uint32_t ACK_TIMEOUT_MS  = 3000;   // سقف انتظار برای ACK های سرور
const uint32_t WAIT_BACKOFF_MS = 2000;   // عقب‌نشینی وقتی سرور آماده نیست
const uint32_t HELLO_WAIT_MS   = 2000;   // سقف انتظار برای READY بعد از HELLO
// --- زمان‌بندی واچ‌داگ (بازبینی‌شده) ---
// هر تسک یک «ضربان» دارد. اگر ضربانی در بازه‌ی زیر تکان نخورد یعنی قفل کرده
// و برد کنترل‌شده ری‌استارت می‌شود. مقادیر با سرعت طبیعی هر تسک تنظیم شده‌اند:
//   • شبکه   : هر ≤۲ ثانیه یک ضربان  -> ۹۰ ثانیه سکوت = قفل
//   • رله    : هر سیکل یک ضربان      -> ۳ برابر دوره‌ی سیکل
//   • دیجیتال: در هر پنجره‌ی پایش     -> ۳ برابر دوره‌ی سیکل
//   • سنسور  : هر سیکل یک ضربان      -> ۳ برابر دوره‌ی سیکل
const uint32_t WDT_CHECK_PERIOD_MS = 15000;
const uint32_t WDT_TIMEOUT_NET_MS = 90000;
// این سه سقف به gCyclePeriodMs وابسته‌اند که حالا از سرور قابل تغییر است؛
// پس دیگر const نیستند و TaskHealthMonitor هر بار آن‌ها را به‌روز می‌کند
// (نگاه کنید به beats[].timeoutMs داخل همان تسک).
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
// ساختار قبلاً دقیقاً با STRUCT_FORMAT = '<iff????iBBBBB' (۲۵ بایت) در
// app.py سازگار بود. یک فیلد CycleAttempt به آن اضافه شد (۲۶ بایت)، و
// حالا دو فیلد بولی جدید Indicator و Buzzer هم به انتها اضافه شده‌اند
// (سایز نهایی ۲۸ بایت): app.py باید STRUCT_FORMAT_V3='<iff????iBBBBBB??'
// (۲۸ بایت) بخواند. فایل‌های .dat قدیمی‌تر (۲۵ یا ۲۶ بایتی) هنوز
// قابل‌خواندن‌اند چون app.py بر اساس باقیمانده‌ی طول فایل بر ۲۵/۲۶/۲۸
// فرمت را تشخیص می‌دهد.
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
  // تعداد تلاش‌هایی که runTestCycle() طول کشید تا هر دو BCM تایید شوند
  // (یا بعد از ۳ تلاش ناموفق ناقص باقی بماند): مقدار ۱، ۲ یا ۳.
  uint8_t CycleAttempt;
  // GPIO34 (Indicator, سیگنال high-side): مثل ۴ سیگنال BCM در همان
  // پنجره‌ی مانیتورینگِ هر فاز خوانده و debounce می‌شود (PULSE_CONFIRM_MS)،
  // و مثل got[][] در کل سیکل (همه‌ی تلاش‌ها) تجمعی باقی می‌ماند.
  bool Indicator;
  // GPIO35 (Buzzer, پالس مربعی ۵۰۰Hz..4kHz): چون فرکانسش خیلی بالاتر از
  // پولینگ ۱۰ میلی‌ثانیه‌ای TaskDigitalRead است، با وقفه‌ی سخت‌افزاری
  // (attachInterrupt) شمارش لبه می‌شود؛ اگر طی یک پنجره‌ی مانیتورینگ به
  // اندازه‌ی کافی لبه دیده شود (BUZZER_EDGE_THRESHOLD)، همان پنجره تایید
  // می‌شود؛ مثل Indicator در کل سیکل تجمعی باقی می‌ماند.
  bool Buzzer;
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

// =====================================================================
//                     لایه‌ی ذخیره‌سازی دو سطحی
//
//  اولویت با کارت SD است. اگر کارت نبود یا خراب بود، همان ساختار فایل‌ها
//  روی حافظه‌ی داخلی خود ESP32 (LittleFS) نوشته می‌شود تا هیچ رکوردی گم
//  نشود. هر ۶۰ ثانیه دوباره دنبال کارت می‌گردیم؛ به‌محض پیدا شدن، فایل‌های
//  باقی‌مانده از حافظه‌ی داخلی به کارت منتقل و ذخیره‌سازی روی کارت ادامه
//  پیدا می‌کند.
//
//  ظرفیت حافظه‌ی داخلی با پارتیشن ۱ مگابایتی ≈ ۴۰٬۰۰۰ رکورد.
// =====================================================================
fs::FS *gFs = nullptr;         // به SD یا LittleFS اشاره می‌کند
bool usingSD = false;          // الان روی کارت می‌نویسیم؟
uint32_t lastSdProbeMs = 0;    // آخرین باری که دنبال کارت گشتیم

const char *storageName() { return usingSD ? "SD" : "حافظه داخلی"; }

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

// --- وضعیت لینک v2 با سرور (از مسیر گیرنده) ---
// linkSid همان شناسه‌ای است که سرور موقع باز شدن پورت سریال می‌سازد و
// با SRV_READY تا گیرنده و با READY تا همین‌جا می‌رسد — هر سه لایه با
// یک id پیش می‌روند تا معلوم باشد داده‌ی چه نشستی در جریان است.
char linkSid[12] = "";           // شناسه‌ی نشست مشترک (خالی = هنوز ندیده‌ایم)
bool linkServerReady = false;    // سرور واقعاً آماده‌ی دریافت است؟ (READY دیدیم)
uint32_t winNums[ACK_WINDOW];    // شماره‌ی رکوردهای پنجره‌ی ارسال جاری
bool winAcked[ACK_WINDOW];       // کدام اسلات‌ها ACK واقعی سرور گرفتند
uint8_t winLen = 0;              // طول مؤثر پنجره

volatile int currentGlobalID = 0;
volatile WifiData globalSystemState;

// نتیجه‌ی خام آخرین پنجره‌ی مانیتورینگ:  fbSeen[فاز][دستگاه]
volatile bool fbSeen[PHASE_COUNT][DEVICE_COUNT] = { { false, false }, { false, false } };

// نتیجه‌ی خام آخرین پنجره‌ی مانیتورینگ برای Indicator (GPIO34)؛ توسط
// TaskDigitalRead با همان الگوریتم debounce چهار سیگنال BCM پر می‌شود.
volatile bool fbIndicatorSeen = false;

// شمارنده‌ی لبه‌های Buzzer (GPIO35) طی پنجره‌ی مانیتورینگِ جاری؛ توسط
// وقفه‌ی سخت‌افزاری onBuzzerEdge() افزایش می‌یابد، در ابتدای هر پنجره
// (beginFeedbackWindow) صفر می‌شود.
volatile uint32_t buzzerEdgeCount = 0;

// وقفه‌ی GPIO35: چون پریود پالس بازر می‌تواند تا ۲۵۰ میکروثانیه (۴کیلوهرتز)
// کوتاه باشد، پولینگ ۱۰ میلی‌ثانیه‌ایِ TaskDigitalRead قادر به دیدن آن
// نیست؛ به همین دلیل لبه‌شماری با وقفه‌ی سخت‌افزاری CHANGE انجام می‌شود.
void IRAM_ATTR onBuzzerEdge() {
  buzzerEdgeCount++;
}

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
volatile uint32_t writeFailures = 0; // شکست‌های پیاپی نوشتن روی SD
const uint32_t WRITE_FAIL_LIMIT = 5;      // بعد از این تعداد، ری‌استارت
uint32_t lastTxMillis = 0;             // آخرین باری که چیزی روی سوکت فرستادیم

// Prototypes
void TaskHealthMonitor(void *pv);
void loadConfig();
void saveWifiConfig(const String &dSsid, const String &dPass,
                    const String &tSsid, const String &tPass);
void loadTimingConfig();
void saveTimingConfig(uint32_t cycleMs, uint32_t retryGapMs);
bool syncTimeFromNtp();
void runSetupPortal(bool timeAlreadyValid);
bool rtcTimeLooksValid();
void wifiService();
void handleSerialCommands();
int serviceIncomingLink(bool windowActive);   // لینک v2: خواندن خطوط گیرنده
void processLinkLine(const char* line, bool windowActive);
void waitForDataLink(uint32_t timeoutMs);
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void scanForDataAp();
static const char *wifiReasonText(uint8_t reason);
static bool mountInternalFs();
bool initStorage();
void probeSdCard();
void saveRecord(const WifiData &data);

/** نتیجه‌ی پاک‌سازی کارت حافظه */
struct EraseResult {
  bool ok = false;
  uint32_t files = 0;
  uint32_t bytes = 0;
};
EraseResult eraseStorage();
static String dayFilePath(int y, int m, int d);
static String posPathOf(const String &datPath);
static uint32_t readUploadPos(const String &datPath);
static void writeUploadPos(const String &datPath, uint32_t pos);
static String baseNameOf(const char *rawName);
static String pickDayFile(bool oldest);
static int lastRecordIdStored();
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

  // لاگ‌های داخلی درایور وای‌فای (مثل «wifi:Set status to INIT») فقط نویزند
  // و در جریان تلاش‌های مجدد طبیعی هستند؛ فقط هشدارها و بالاتر نشان داده شوند.
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);

  WiFi.onEvent(onWiFiEvent);  // با اطلاعات دلیل قطعی
  loadConfig();
  loadTimingConfig();
  rtc.initClock();

  // ---------------- SD & شماره‌ی رکورد ----------------
  if (xSemaphoreTake(xSDMutex, portMAX_DELAY)) {
    if (!initStorage()) {
      Serial.println("[STORE] هیچ حافظه‌ای در دسترس نیست؛ دیتا فقط ارسال می‌شود");
    } else {

      // مدل ذخیره‌سازی بهینه: به‌جای «یک فایل برای هر رکورد»، هر روز یک فایل
      // /data/YYYYMMDD.dat که رکوردهای ۲۵ بایتی پشت سر هم به آن append می‌شوند.
      // آخرین شماره‌ی رکورد = NUM آخرین رکورد جدیدترین فایل.
      currentGlobalID = lastRecordIdStored();
      int lastSaved = getNextPersistentID();
      if (lastSaved > currentGlobalID) currentGlobalID = lastSaved;
      DEBUG_PRINTF("[STORE] Resuming from ID %d\n", currentGlobalID);
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

  // GPIO34/35 روی ESP32 «ورودی‌خالص» هستند و پول‌آپ/پول‌داون داخلی ندارند؛
  // برخلاف FEEDBACK_PINS بالا با INPUT ساده تنظیم می‌شوند (مقاومت pull باید
  // بیرونی/روی سخت‌افزار باشد).
  pinMode(PIN_INDICATOR, INPUT);
  pinMode(PIN_BUZZER, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_BUZZER), onBuzzerEdge, CHANGE);

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

// =====================================================================
//   تنظیمات زمان‌بندی سیکل (CYCLE_PERIOD_MS / RELAY_RETRY_GAP_MS)
//   این دو مقدار از سرور (از طریق ESP8266، خط «CFG ...») قابل تغییرند و
//   در همان NVS فضای "rfcfg" ذخیره می‌شوند تا بعد از قطع برق/ریست هم بمانند.
// =====================================================================
void loadTimingConfig() {
  prefs.begin("rfcfg", true);  // read-only
  gCyclePeriodMs = prefs.getUInt("cycleMs", DEFAULT_CYCLE_PERIOD_MS);
  gRelayRetryGapMs = prefs.getUInt("retryGapMs", DEFAULT_RELAY_RETRY_GAP_MS);
  prefs.end();

  // اگر مقدار ذخیره‌شده (یا NVS خراب) خارج از بازه‌ی مجاز بود، به پیش‌فرض برگرد
  if (gCyclePeriodMs < MIN_CYCLE_PERIOD_MS || gCyclePeriodMs > MAX_CYCLE_PERIOD_MS)
    gCyclePeriodMs = DEFAULT_CYCLE_PERIOD_MS;
  if (gRelayRetryGapMs < MIN_RELAY_RETRY_GAP_MS || gRelayRetryGapMs > MAX_RELAY_RETRY_GAP_MS)
    gRelayRetryGapMs = DEFAULT_RELAY_RETRY_GAP_MS;

  DEBUG_PRINTF("[CFG] زمان‌بندی سیکل: CYCLE_PERIOD_MS=%u RELAY_RETRY_GAP_MS=%u\n",
               (unsigned)gCyclePeriodMs, (unsigned)gRelayRetryGapMs);
}

/** مقدار جدید را اعمال و در NVS ذخیره می‌کند (مقادیر قبلاً clamp شده‌اند) */
void saveTimingConfig(uint32_t cycleMs, uint32_t retryGapMs) {
  prefs.begin("rfcfg", false);
  prefs.putUInt("cycleMs", cycleMs);
  prefs.putUInt("retryGapMs", retryGapMs);
  prefs.end();
  gCyclePeriodMs = cycleMs;
  gRelayRetryGapMs = retryGapMs;
  DEBUG_PRINTF("[CFG] زمان‌بندی سیکل از سرور به‌روزرسانی و در NVS ذخیره شد: "
               "CYCLE_PERIOD_MS=%u RELAY_RETRY_GAP_MS=%u\n",
               (unsigned)cycleMs, (unsigned)retryGapMs);
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

  EraseResult r = eraseStorage();
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
  // گذار تمیز AP -> STA
  //
  // اگر بلافاصله بعد از خاموش کردن اکسس‌پوینت، حالت STA را روشن کنیم،
  // درایور هنوز در حال توقف است و این خطا را می‌دهد:
  //     wifi_init_default: netstack cb reg failed with 12308
  //     (12308 = 0x3014 = ESP_ERR_WIFI_STOP_STATE)
  // بی‌خطر است ولی نشانه‌ی گذار عجولانه است؛ با مکث و ترتیب درست حذف می‌شود.
  WiFi.softAPdisconnect(true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(500);          // فرصت کامل شدن توقف درایور
  WiFi.mode(WIFI_STA); // حالت بعدی از همین‌جا مشخص می‌شود
  delay(200);
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

  // ---- توان فرستنده ----
  if (FORCE_MAX_TX_POWER) {
    WiFi.setTxPower(STA_TX_POWER_FAR);          // حداکثر برد
  } else if (bestRssi > -25) {
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

  // نردبان تلاش: اگر ترکیب اول جواب نداد، خودکار سراغ بعدی می‌رود.
  //   تلاش ۰ تا ۲ : PMF خاموش + کانال و BSSID مشخص   (حالت درست برای ESP8266)
  //   تلاش ۳       : PMF خاموش، بدون قید کانال
  //   تلاش ۴ و ۵   : PMF روشن (برای اکسس‌پوینت‌هایی که PMF می‌خواهند)
  //   تلاش ۶       : راه‌اندازی مجدد کامل درایور و شروع دوباره‌ی نردبان
  bool pmfCapable = (failStreak == 4 || failStreak == 5);

  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    conf.sta.pmf_cfg.capable = pmfCapable;
    conf.sta.pmf_cfg.required = false;
    conf.sta.threshold.authmode =
        (cfgDataPass.length() == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    conf.sta.scan_method = WIFI_FAST_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
  }

  VERBOSE_PRINTF("[NET] تلاش %u | PMF=%s | BSSID=%s\n",
                 (unsigned)failStreak, pmfCapable ? "on" : "off",
                 haveBssid ? "yes" : "no");

  esp_wifi_connect();

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    const char *quality = (rssi > -55) ? "عالی"
                          : (rssi > -67) ? "خوب"
                          : (rssi > -75) ? "قابل قبول" : "ضعیف";
    DEBUG_PRINTF("[NET] اتصال برقرار شد (تلاش %u) | rssi=%d dBm (%s) | tx=%.1f dBm | PMF=%s\n",
                 (unsigned)failStreak, rssi, quality,
                 WiFi.getTxPower() / 4.0, pmfCapable ? "on" : "off");
    if (rssi <= -75) {
      DEBUG_PRINTLN("[NET] سیگنال ضعیف است: فاصله/مانع را کم کنید یا "
                    "تغذیه‌ی گیرنده را بررسی کنید");
    }
    failStreak = 0;
    return true;
  }

  failStreak++;

  // بعد از ۱۰ شکست، یک بار راهنمای کامل چاپ می‌شود
  if (failStreak == 10) {
    Serial.println("[NET] ---- راهنمای عیب‌یابی اتصال ----");
    Serial.println("  ۱) گیرنده روشن است و SSID در اسکن دیده می‌شود؟");
    Serial.println("  ۲) رمز دو طرف یکی است؟ (DATA_AP_PASS و PASSWORD)");
    Serial.println("  ۳) بردها حداقل نیم متر فاصله دارند؟");
    Serial.println("  ۴) تغذیه‌ی ESP8266 پایدار است؟ (افت ولتاژ = هندشیک ناموفق)");
    Serial.println("  ۵) برای تست قطعی: AP_OPEN_TEST=true و DATA_AP_PASS=\"\"");
  }
  return false;
}

/**
 * سرویس لینک — هر بار که تسک شبکه بیدار می‌شود صدا زده می‌شود.
 * هم در حالت عادی و هم وسط سیکل رله اجرا می‌شود (به SD دست نمی‌زند).
 */
/**
 * فرمان‌های سریال (در ترمینال Arduino تایپ کنید و Enter بزنید):
 *     FORMAT       -> پاک‌سازی کامل حافظه‌ی فعال (SD یا داخلی)
 *     STATUS       -> نمایش وضعیت لحظه‌ای
 */
void handleSerialCommands() {
  static String buf;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      buf.trim();
      if (buf.equalsIgnoreCase("FORMAT SD") || buf.equalsIgnoreCase("FORMAT")) {
        Serial.printf("[CMD] پاک‌سازی %s ...\n", storageName());
        eraseStorage();
      } else if (buf.equalsIgnoreCase("STATUS")) {
        Serial.printf("[CMD] مرحله=%s | wifi=%s | لینک=%s%s | حافظه=%s | رکورد بعدی=%d | heap=%uk\n",
                      relayPhaseText,
                      WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                      linkServerReady ? "READY" : "WAIT",
                      linkSid,
                      storageName(),
                      currentGlobalID + 1, (unsigned)(ESP.getFreeHeap() / 1024));
      } else if (buf.length()) {
        Serial.println("[CMD] فرمان‌ها: FORMAT | STATUS");
      }
      buf = "";
    } else if (buf.length() < 40) {
      buf += c;
    }
  }
}

// =====================================================================
//  لینک v2 — پردازش یک خط کامل از گیرنده (ESP8266)
//  خطوط: ACK <num> / READY <sid> / WAIT / SRV_LOST / ERR:* / PONG
// =====================================================================
void processLinkLine(const char* line, bool windowActive) {
  if (strncmp(line, "ACK ", 4) == 0) {
    // تایید واقعی سرور (گیرنده فقط عین همان ACK ای را فوروارد می‌کند
    // که از سرور آمده) -> اسلات متناظر در پنجره‌ی جاری علامت می‌خورد
    long num = strtol(line + 4, NULL, 10);
    if (windowActive) {
      for (uint8_t i = 0; i < winLen; i++) {
        if (!winAcked[i] && winNums[i] == (uint32_t)num) {
          winAcked[i] = true;
          break;
        }
      }
    }
  }
  else if (strncmp(line, "READY", 5) == 0) {
    const char* sid = line + 5;
    while (*sid == ' ') sid++;
    strlcpy(linkSid, sid, sizeof(linkSid));
    linkServerReady = true;
    VERBOSE_PRINTF("[LINK] سرور آماده است (نشست %s)\n", linkSid);
  }
  else if (strcmp(line, "WAIT") == 0) {
    linkServerReady = false;
  }
  else if (strncmp(line, "SRV_LOST", 8) == 0) {
    linkServerReady = false;
    DEBUG_PRINTLN("[LINK] سرور از دست رفت -> آپلود متوقف تا READY جدید");
  }
  else if (strncmp(line, "ERR:", 4) == 0) {
    // گیرنده فرمت را رد کرد؛ چون ACK نمی‌آید همان رکورد دور بعد
    // دوباره ارسال می‌شود (retry تا موفق — هیچ داده‌ای دور ریخته نمی‌شود)
    DEBUG_PRINTF("[LINK] گیرنده رد کرد: %s\n", line);
  }
  else if (strncmp(line, "CFG ", 4) == 0) {
    // پیکربندی زمان‌بندی از سرور (عیناً توسط ESP8266 فوروارد شده):
    //   CFG CYCLE_PERIOD_MS=<ms>;RELAY_RETRY_GAP_MS=<ms>
    // هر دو کلید اختیاری‌اند؛ هرکدام نبود همان مقدار فعلی باقی می‌ماند.
    long newCycleMs = (long)gCyclePeriodMs;
    long newGapMs = (long)gRelayRetryGapMs;
    bool changed = false;

    char body[80];
    strlcpy(body, line + 4, sizeof(body));
    char *saveptr = nullptr;
    char *tok = strtok_r(body, ";", &saveptr);
    while (tok != nullptr) {
      char *eq = strchr(tok, '=');
      if (eq) {
        *eq = '\0';
        const char *key = tok;
        long val = strtol(eq + 1, NULL, 10);
        if (strcmp(key, "CYCLE_PERIOD_MS") == 0 && val > 0) {
          newCycleMs = val;
          changed = true;
        } else if (strcmp(key, "RELAY_RETRY_GAP_MS") == 0 && val > 0) {
          newGapMs = val;
          changed = true;
        }
      }
      tok = strtok_r(nullptr, ";", &saveptr);
    }

    if (changed) {
      // CYCLE_PERIOD_MS: اگر مقدار درخواستی کمتر از حداقل مجاز باشد، دیگر
      // کلمپ به نزدیک‌ترین حد مجاز نمی‌شود — کلاً رد می‌شود و مقدار فعلی
      // (gCyclePeriodMs) دست‌نخورده باقی می‌ماند (طبق درخواست صریح: زیر
      // این آستانه اصلاً قبول نشود).
      if (newCycleMs < (long)MIN_CYCLE_PERIOD_MS) {
        DEBUG_PRINTF("[CFG] CYCLE_PERIOD_MS=%ld رد شد (کمتر از حداقل مجاز %u) -> "
                     "مقدار قبلی (%u) حفظ شد\n",
                     newCycleMs, (unsigned)MIN_CYCLE_PERIOD_MS, (unsigned)gCyclePeriodMs);
        newCycleMs = (long)gCyclePeriodMs;
      }
      if (newCycleMs > (long)MAX_CYCLE_PERIOD_MS) newCycleMs = MAX_CYCLE_PERIOD_MS;
      if (newGapMs < (long)MIN_RELAY_RETRY_GAP_MS) newGapMs = MIN_RELAY_RETRY_GAP_MS;
      if (newGapMs > (long)MAX_RELAY_RETRY_GAP_MS) newGapMs = MAX_RELAY_RETRY_GAP_MS;

      if ((uint32_t)newCycleMs != gCyclePeriodMs || (uint32_t)newGapMs != gRelayRetryGapMs) {
        saveTimingConfig((uint32_t)newCycleMs, (uint32_t)newGapMs);
      }
    }
  }
  // PONG و هر خط ناشناخته‌ی دیگر: نادیده گرفته می‌شود
}

// =====================================================================
//  لینک v2 — خواندن غیرمسدودکننده‌ی همه‌ی خطوط موجود از گیرنده.
//  فقط خطوط «کامل» (با newline) پردازش می‌شوند تا نیمه‌خط پارس اشتباهی
//  نسازد. تعداد خطوط پردازش‌شده را برمی‌گرداند.
// =====================================================================
int serviceIncomingLink(bool windowActive) {
  static char rxBuf[96];
  static uint8_t rxLen = 0;
  int handled = 0;

  while (uploadClient.available() > 0) {
    char c = (char)uploadClient.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxBuf[rxLen] = '\0';
        processLinkLine(rxBuf, windowActive);
        rxLen = 0;
        handled++;
      }
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxLen = 0;   // سرریز؛ خط دور ریخته شد تا با بعدی قاطی نکند
    }
  }
  return handled;
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
      // جواب (PONG — یا هر خط لینک v2 دیگری مثل READY/SRV_LOST) را
      // با همان پارسر لینک می‌خوانیم؛ سقف ۵۰۰ میلی‌ثانیه
      uint32_t t0 = millis();
      while (millis() - t0 < 500) {
        if (serviceIncomingLink(false) > 0) break;   // جواب آمد
        if (!uploadClient.connected()) break;
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

  // راهنمای هدفمند بر اساس دلیل واقعی قطعی
  if (wifiLink.failures % 5 == 0) {
    scanForDataAp();

    switch (lastDropReason) {
      case 15:  // 4WAY_HANDSHAKE_TIMEOUT
      case 204:
        DEBUG_PRINTLN("[NET] هندشیک کامل نمی‌شود: معمولاً سیگنال ضعیف "
                      "(rssi بدتر از -70) یا افت ولتاژ گیرنده است");
        break;
      case 5:   // ASSOC_TOOMANY
        DEBUG_PRINTLN("[NET] اکسس‌پوینت می‌گوید ظرفیتش پر است: نشست‌های مرده "
                      "روی گیرنده جمع شده‌اند؛ گیرنده خودش تا ۲ دقیقه دیگر "
                      "اکسس‌پوینت را بازسازی می‌کند");
        break;
      case 201:  // NO_AP_FOUND
        DEBUG_PRINTLN("[NET] اکسس‌پوینت دیده نمی‌شود: گیرنده خاموش است یا "
                      "فاصله/مانع زیاد است");
        break;
      default:
        break;
    }
  }

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
  fbIndicatorSeen = false;
  buzzerEdgeCount = 0;  // شمارش لبه‌ی بازر برای همین پنجره از صفر شروع می‌شود

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
                     bool *gotIndicator = nullptr, bool *gotBuzzer = nullptr,
                     uint32_t *triggeredAtMs = nullptr) {
  uint8_t pin = RELAY_PINS[phase];

  digitalWrite(pin, HIGH);                          // 1) تحریک رله
  if (triggeredAtMs) *triggeredAtMs = millis();     // لحظه‌ی دقیق تریگ
  vTaskDelay(pdMS_TO_TICKS(RELAY_SETTLE_MS));       //    پایدار شدن کنتاکت
  beginFeedbackWindow();                            // 2) مانیتورینگ فعال (fbSeen/Indicator/Buzzer صفر می‌شوند)
  vTaskDelay(pdMS_TO_TICKS(FEEDBACK_WINDOW_MS));    // 3) زمان مجاز
  endFeedbackWindow();                              // 4) مانیتورینگ غیرفعال
  digitalWrite(pin, LOW);                           // 5) قطع رله

  for (int d = 0; d < DEVICE_COUNT; d++) {
    if (fbSeen[phase][d]) got[phase][d] = true;
  }

  // Indicator (GPIO34) و Buzzer (GPIO35): دقیقاً در همین پنجره‌ی مانیتورینگ
  // (هر دو فاز OPEN و CLOSE، هر تلاش) سنجیده می‌شوند و مثل got[][] در کل
  // سیکل تجمعی باقی می‌مانند (هیچ‌وقت در طول سیکل به false برنمی‌گردند).
  if (fbIndicatorSeen && gotIndicator) *gotIndicator = true;
  if (buzzerEdgeCount >= BUZZER_EDGE_THRESHOLD && gotBuzzer) *gotBuzzer = true;

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
 *
 * outAttempt: شماره‌ی تلاشی که روی آن متوقف شد (۱ اگر بار اول موفق شد،
 * ۲ اگر بار دوم، یا ۳ اگر حتی بعد از ۳ تلاش هم ناقص ماند). این همان
 * مقداری است که در پروتکل به‌عنوان «cycle=» برای سرور فرستاده می‌شود.
 *
 * outIndicator/outBuzzer: نتیجه‌ی تجمعی GPIO34/GPIO35 طی کل سیکل (همه‌ی
 * فازها و همه‌ی تلاش‌ها)؛ دقیقاً مثل got[][] هرگز در طول سیکل ریست نمی‌شوند.
 */
static void runTestCycle(bool got[PHASE_COUNT][DEVICE_COUNT], uint8_t *outAttempt,
                          bool *outIndicator = nullptr, bool *outBuzzer = nullptr) {
  for (int p = 0; p < PHASE_COUNT; p++)
    for (int d = 0; d < DEVICE_COUNT; d++) got[p][d] = false;
  bool gotIndicator = false;
  bool gotBuzzer = false;

  uint8_t usedAttempt = 1;
  for (uint8_t attempt = 1; attempt <= RELAY_MAX_ATTEMPTS; attempt++) {
    usedAttempt = attempt;
    relayPhaseText = "سیکل تست";
    DEBUG_PRINTF("\n[TEST] ===== سیکل %u/%u =====\n", attempt, RELAY_MAX_ATTEMPTS);

    // --- در هر سیکل، هر دو فرمان به ترتیب داده می‌شوند ---
    for (int phase = 0; phase < PHASE_COUNT; phase++) {
      uint32_t triggeredAt = 0;
      runPhase(phase, got, &gotIndicator, &gotBuzzer, &triggeredAt);

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
      vTaskDelay(pdMS_TO_TICKS(gRelayRetryGapMs));
    }
  }

  if (outAttempt) *outAttempt = usedAttempt;
  if (outIndicator) *outIndicator = gotIndicator;
  if (outBuzzer) *outBuzzer = gotBuzzer;
}

void TaskRelayControl(void *pv) {
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

    // لحظه‌ی شروع این سیکل: CYCLE_PERIOD_MS از همین نقطه سنجیده می‌شود
    // (یعنی کل فاصله‌ی «شروع یک سیکل تا شروع سیکل بعد»)، نه از لحظه‌ی
    // پایان تست. در نتیجه با یک تنظیم مشخص، فاصله‌ی بین لاگ‌ها تقریباً
    // ثابت می‌ماند (مستقل از ۱، ۲ یا ۳ تلاش رله)؛ فقط اگر اجرای خودِ تست
    // (با چند تلاش ناموفق) بیشتر از CYCLE_PERIOD_MS طول بکشد، دیگر صبر
    // اضافه‌ای انجام نمی‌شود و سیکل بعدی بی‌معطلی شروع می‌شود.
    uint32_t cycleStartMs = millis();

    // نتیجه‌ی تفکیکی: برای هر دستگاه، هم «باز شد» و هم «بسته شد»
    bool got[PHASE_COUNT][DEVICE_COUNT];
    uint8_t cycleAttemptUsed = 1;
    bool indicatorOk = false;
    bool buzzerOk = false;
    runTestCycle(got, &cycleAttemptUsed, &indicatorOk, &buzzerOk);

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
      globalSystemState.CycleAttempt = cycleAttemptUsed;
      globalSystemState.Indicator = indicatorOk;
      globalSystemState.Buzzer = buzzerOk;

      WifiData snapshot;
      memcpy(&snapshot, (const void *)&globalSystemState, sizeof(WifiData));
      xSemaphoreGive(xGlobalStateMutex);

      DEBUG_PRINTF("[CYCLE] #%d  %s[open:%s close:%s]  %s[open:%s close:%s]  "
                   "T=%.2f H=%.2f  cycle=%u  @ %04d-%02d-%02d %02d:%02d:%02d\n",
                   snapshot.NUM,
                   DEVICE_NAMES[0], snapshot.BCM1_OPEN ? "OK" : "NOK",
                                    snapshot.BCM1_CLOSE ? "OK" : "NOK",
                   DEVICE_NAMES[1], snapshot.BCM2_OPEN ? "OK" : "NOK",
                                    snapshot.BCM2_CLOSE ? "OK" : "NOK",
                   snapshot.Temp, snapshot.Hum, (unsigned)snapshot.CycleAttempt,
                   snapshot.Year, snapshot.Month, snapshot.Day,
                   snapshot.Hour, snapshot.Minute, snapshot.Second);

      if (xQueueSend(xDataQueue, (void *)&snapshot, pdMS_TO_TICKS(100)) != pdPASS) {
        // صف پر است -> همین‌جا مستقیم روی SD بنویس تا رکورد گم نشود
        DEBUG_PRINTLN("[CYCLE] صف پر بود؛ ذخیره‌ی مستقیم روی SD");
        if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(2000))) {
          saveRecord(snapshot);
          xSemaphoreGive(xSDMutex);
        } else {
          writeFailures++;
        }
      }
      saveNextPersistentID(currentGlobalID);
    } else {
      DEBUG_PRINTLN("[CYCLE] Error: state mutex busy, record skipped.");
    }

    xEventGroupSetBits(xSystemEvents, BIT_WIFI_PERMIT);
    relayPhaseText = "انتظار تا سیکل بعد";

    // ---- انتظار تا سیکل بعد: CYCLE_PERIOD_MS از *شروع همین سیکل* ----
    // مبنای شمارش cycleStartMs (بالای حلقه) است، نه لحظه‌ی پایان تست؛
    // یعنی CYCLE_PERIOD_MS = کل فاصله‌ی شروع یک سیکل تا شروع سیکل بعدی،
    // نه فقط «استراحت اضافه‌ی بعد از تست». دو نتیجه:
    //   ۱) اگر اجرای خودِ تست (با ۱، ۲ یا ۳ تلاش) کمتر از CYCLE_PERIOD_MS
    //      طول بکشد، فقط باقیمانده صبر می‌شود -> فاصله‌ی کل تقریباً همیشه
    //      همان CYCLE_PERIOD_MS تنظیم‌شده می‌ماند.
    //   ۲) اگر اجرای تست (مثلاً به‌خاطر ۳ تلاش ناموفق) بیشتر از
    //      CYCLE_PERIOD_MS طول بکشد، دیگر صبر اضافه‌ای انجام نمی‌شود و
    //      سیکل بعدی بی‌معطلی همان لحظه شروع می‌شود.
    // چون مبنا «شروع سیکل» است نه «شروع انتظار»، اگر کاربر همین حین مقدار
    // CYCLE_PERIOD_MS را از سرور عوض کند، همان لحظه با مقدار تازه بازمحاسبه
    // می‌شود؛ نیازی به ریست از صفر نیست.
    for (;;) {
      uint32_t targetPeriod = gCyclePeriodMs;  // ممکن است حین انتظار از سرور تغییر کرده باشد
      uint32_t elapsedSinceStart = millis() - cycleStartMs;
      if (elapsedSinceStart >= targetPeriod) break;  // دیر شده/دقیقاً رسیده -> بی‌معطلی ادامه
      uint32_t remaining = targetPeriod - elapsedSinceStart;
      uint32_t step = (remaining < CYCLE_WAIT_POLL_MS) ? remaining : CYCLE_WAIT_POLL_MS;
      vTaskDelay(pdMS_TO_TICKS(step));
    }
  }
}

// =====================================================================
//        TASK: خواندن فیدبک‌های دیجیتال در طول پنجره‌ی مانیتورینگ
// =====================================================================
void TaskDigitalRead(void *pv) {
  // هر چهار پین فیدبک هم‌زمان مانیتور می‌شوند (هر دو BCM در آنِ واحد)، به‌علاوه
  // یک پین پنجم: PIN_INDICATOR (GPIO34)، که دقیقاً با همان الگوریتم debounce
  // (PULSE_CONFIRM_MS) خوانده می‌شود — آخرین اندیس آرایه‌ی pins[].
  const int FB_PIN_COUNT = PHASE_COUNT * DEVICE_COUNT;
  const int pinCount = FB_PIN_COUNT + 1;  // +1 برای Indicator
  const int INDICATOR_IDX = FB_PIN_COUNT;
  uint8_t pins[FB_PIN_COUNT + 1];
  for (int p = 0; p < PHASE_COUNT; p++)
    for (int d = 0; d < DEVICE_COUNT; d++)
      pins[p * DEVICE_COUNT + d] = FEEDBACK_PINS[p][d];
  pins[INDICATOR_IDX] = PIN_INDICATOR;

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
    if (confirmed[INDICATOR_IDX]) fbIndicatorSeen = true;

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
/** کارت SD را سوار می‌کند؛ در صورت موفقیت gFs به آن اشاره می‌کند */
static bool mountSdCard() {
  if (!SD.begin(SD_CS_PIN)) return false;
  gFs = &SD;
  usingSD = true;
  if (!gFs->exists("/data")) gFs->mkdir("/data");
  return true;
}

/** حافظه‌ی داخلی را سوار می‌کند (در صورت نیاز فرمت می‌شود) */
static bool mountInternalFs() {
  if (!LittleFS.begin(true)) return false;   // true = اگر خالی بود فرمت کن
  gFs = &LittleFS;
  usingSD = false;
  if (!gFs->exists("/data")) gFs->mkdir("/data");
  return true;
}

/** انتخاب محل ذخیره‌سازی در بوت: اول کارت، بعد حافظه‌ی داخلی */
bool initStorage() {
  if (mountSdCard()) {
    DEBUG_PRINTF("[STORE] کارت SD آماده است (%llu مگابایت)\n",
                 SD.cardSize() / (1024ULL * 1024ULL));
    return true;
  }

  DEBUG_PRINTLN("[STORE] کارت SD پیدا نشد -> استفاده از حافظه‌ی داخلی");
  if (mountInternalFs()) {
    DEBUG_PRINTF("[STORE] حافظه‌ی داخلی آماده است (%u کیلوبایت آزاد)\n",
                 (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
    return true;
  }

  Serial.println("[STORE] بحرانی: هیچ حافظه‌ای در دسترس نیست!");
  gFs = nullptr;
  return false;
}

/** یک فایل را از حافظه‌ی داخلی به کارت کپی می‌کند */
static bool copyFileToSd(const String &path) {
  File src = LittleFS.open(path, FILE_READ);
  if (!src) return false;

  File dst = SD.open(path, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }

  uint8_t buf[256];
  size_t n;
  while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);

  src.close();
  dst.close();
  return true;
}

/**
 * وقتی روی حافظه‌ی داخلی کار می‌کنیم، هر ۶۰ ثانیه دنبال کارت می‌گردیم.
 * به‌محض پیدا شدن، فایل‌های باقی‌مانده منتقل و ذخیره‌سازی روی کارت ادامه
 * پیدا می‌کند.
 */
void probeSdCard() {
  if (usingSD || gFs == nullptr) return;
  if (millis() - lastSdProbeMs < 60000) return;
  lastSdProbeMs = millis();

  if (!SD.begin(SD_CS_PIN)) return;   // هنوز کارتی نیست

  DEBUG_PRINTLN("[STORE] کارت SD پیدا شد -> انتقال داده‌های حافظه‌ی داخلی");
  if (!SD.exists("/data")) SD.mkdir("/data");

  uint32_t moved = 0;
  File root = LittleFS.open("/data");
  if (root) {
    File e = root.openNextFile();
    while (e) {
      String name = String(e.name());
      if (!name.startsWith("/")) name = "/data/" + name;
      e.close();

      if (copyFileToSd(name)) {
        LittleFS.remove(name);
        moved++;
      }
      e = root.openNextFile();
    }
    root.close();
  }
  if (LittleFS.exists("/last_id.txt")) {
    if (copyFileToSd("/last_id.txt")) LittleFS.remove("/last_id.txt");
  }

  gFs = &SD;
  usingSD = true;
  DEBUG_PRINTF("[STORE] از این پس روی کارت نوشته می‌شود (%u فایل منتقل شد)\n",
               (unsigned)moved);
}

int getNextPersistentID() {
  int id = 0;
  if (gFs->exists("/last_id.txt")) {
    File f = gFs->open("/last_id.txt", FILE_READ);
    if (f) {
      id = f.parseInt();
      f.close();
    }
  }
  return id;
}

void saveNextPersistentID(int id) {
  File f = gFs->open("/last_id.txt", FILE_WRITE);
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
  if (!gFs->exists(pp)) return 0;
  File f = gFs->open(pp, FILE_READ);
  if (!f) return 0;
  uint32_t v = (uint32_t)f.parseInt();
  f.close();
  return v;
}

static void writeUploadPos(const String &datPath, uint32_t pos) {
  File f = gFs->open(posPathOf(datPath), FILE_WRITE);
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
  File root = gFs->open("/data");
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
static int lastRecordIdStored() {
  String newest = pickDayFile(false);
  if (!newest.length()) return 0;
  File f = gFs->open(newest, FILE_READ);
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
 *   ۳) شکست‌های پیاپی شمرده می‌شوند؛ ناظر سلامت بعد از WRITE_FAIL_LIMIT بار
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
EraseResult eraseStorage() {
  EraseResult res;

  if (!xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(5000))) {
    Serial.println("[STORE] پاک‌سازی انجام نشد: حافظه مشغول است");
    return res;
  }

  Serial.printf("[STORE] شروع پاک‌سازی %s ...\n", storageName());

  // ۱) همه‌ی فایل‌های پوشه‌ی داده
  File root = gFs->open("/data");
  if (root) {
    File e = root.openNextFile();
    while (e) {
      String path = String(e.name());
      if (!path.startsWith("/")) path = "/data/" + path;
      uint32_t sz = e.size();
      e.close();

      if (gFs->remove(path)) {
        res.files++;
        res.bytes += sz;
      } else {
        Serial.printf("[STORE] حذف نشد: %s\n", path.c_str());
      }
      e = root.openNextFile();
    }
    root.close();
  }

  // ۲) شمارنده‌ی شماره‌ی رکورد
  if (gFs->exists("/last_id.txt") && gFs->remove("/last_id.txt")) res.files++;

  // ۳) ساخت دوباره‌ی ساختار پوشه‌ها
  if (!gFs->exists("/data")) gFs->mkdir("/data");

  // ۴) شماره‌گذاری از صفر
  currentGlobalID = 0;
  saveNextPersistentID(0);
  writeFailures = 0;

  res.ok = true;
  xSemaphoreGive(xSDMutex);

  Serial.printf("[STORE] پاک‌سازی تمام شد: %u فایل (%u کیلوبایت) حذف شد، "
                "شماره‌ی رکورد از ۱ شروع می‌شود\n",
                (unsigned)res.files, (unsigned)(res.bytes / 1024));
  return res;
}

void saveRecord(const WifiData &data) {
  if (gFs == nullptr) {
    // هیچ حافظه‌ای سوار نیست؛ آخرین تلاش برای سوار کردن
    if (!initStorage()) {
      writeFailures++;
      DEBUG_PRINTLN("[STORE] رکورد ذخیره نشد: حافظه‌ای در دسترس نیست");
      return;
    }
  }

  String path = dayFilePath(data.Year, data.Month, data.Day);

  for (int attempt = 0; attempt < 2; attempt++) {
    File f = gFs->open(path, FILE_APPEND);
    if (!f) f = gFs->open(path, FILE_WRITE);  // اولین رکورد امروز

    if (f) {
      size_t written = f.write((const uint8_t *)&data, REC_SIZE);
      f.flush();
      f.close();

      if (written == REC_SIZE) {
        writeFailures = 0;
        VERBOSE_PRINTF("[STORE] ذخیره شد #%d -> %s\n", data.NUM, path.c_str());
        return;
      }
      DEBUG_PRINTF("[STORE] نوشتن ناقص بود (%u از %u بایت)\n",
                   (unsigned)written, (unsigned)REC_SIZE);
    }

    if (attempt == 0) {
      if (usingSD) {
        DEBUG_PRINTLN("[STORE] تلاش دوم: مقداردهی مجدد کارت حافظه");
        SD.end();
        delay(50);
        if (!SD.begin(SD_CS_PIN)) {
          // کارت واقعاً از دسترس خارج شده -> برو روی حافظه‌ی داخلی
          DEBUG_PRINTLN("[STORE] کارت از دسترس خارج شد -> حافظه‌ی داخلی");
          if (!mountInternalFs()) {
            gFs = nullptr;
            break;
          }
        }
      }
      if (gFs && !gFs->exists("/data")) gFs->mkdir("/data");
    }
  }

  writeFailures++;
  DEBUG_PRINTF("[STORE] بحرانی: رکورد #%d ذخیره نشد (شکست پیاپی: %u)\n",
               data.NUM, (unsigned)writeFailures);
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
           "Temp=%.2f,Humidity=%.2f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d,"
           "cycle=%u,Indicator=%s,Buzzer=%s",
           d.NUM,
           d.BCM1_OPEN ? "OK" : "NOK",
           d.BCM1_CLOSE ? "OK" : "NOK",
           d.BCM2_OPEN ? "OK" : "NOK",
           d.BCM2_CLOSE ? "OK" : "NOK",
           d.Temp, d.Hum,
           d.Year, d.Month, d.Day,
           d.Hour, d.Minute, d.Second,
           (unsigned)(d.CycleAttempt ? d.CycleAttempt : 1),
           d.Indicator ? "OK" : "NOK",
           d.Buzzer ? "OK" : "NOK");
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
  File f = gFs->open(newest, FILE_READ);
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
    probeSdCard();            // اگر روی حافظه‌ی داخلی هستیم، دنبال کارت بگرد
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
            saveRecord(q);
            xSemaphoreGive(xSDMutex);
          } else {
            // نتوانستیم قفل بگیریم؛ رکورد را برمی‌گردانیم تا گم نشود
            xQueueSendToFront(xDataQueue, (void *)&q, pdMS_TO_TICKS(100));
            DEBUG_PRINTLN("[STORE] قفل آزاد نشد؛ رکورد در صف ماند");
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

        // ج) Store & Forward با آفست (لینک v2 — پنجره‌ای):
        //    قدیمی‌ترین فایل روزانه را برمی‌داریم، از روی آفستِ ذخیره‌شده
        //    تا ۸ رکورد بعدی را یک‌جا می‌خوانیم (فایل بلافاصله بسته می‌شود)
        //    و پشت سر هم می‌فرستیم. آفست «فقط» به اندازه‌ی پیشوندِ پیاپی‌ای
        //    که ACK واقعی سرور گرفته جلو می‌رود -> هیچ رکورد تاییدنشده‌ای
        //    از دست نمی‌رود و نوشتن روی SD هم به حداقل می‌رسد.
        if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(100))) {
          bool needBackoff = false;
          do {   // فقط برای خروج زودهنگامِ تمیز؛ دقیقاً یک‌بار اجرا می‌شود
            String dayFile = pickDayFile(true);
            if (!dayFile.length()) break;

            uint32_t pos = readUploadPos(dayFile);
            File f = gFs->open(dayFile, FILE_READ);
            if (!f) break;
            size_t fileSize = f.size();

            if (pos + REC_SIZE > fileSize) {
              // فایل کامل آپلود شده؛ اگر مربوط به امروز نیست پاکش کن
              f.close();
              rtc.read();
              String today = dayFilePath(2000 + rtc.getYear(), rtc.getMonth(), rtc.getDay());
              if (dayFile != today) {
                gFs->remove(posPathOf(dayFile));
                gFs->remove(dayFile);
                DEBUG_PRINTF("[UPLOAD] %s fully uploaded -> removed\n", dayFile.c_str());
              }
              break;
            }

            // ۱) پر کردن پنجره از فایل؛ فایل همین‌جا برای این دور بسته می‌شود
            WifiData win[ACK_WINDOW];
            uint8_t n = 0;
            uint32_t sendPos = pos;
            while (n < ACK_WINDOW && sendPos + REC_SIZE <= fileSize) {
              f.seek(sendPos);
              if (f.read((uint8_t *)&win[n], REC_SIZE) != (int)REC_SIZE) break;
              winNums[n] = (uint32_t)win[n].NUM;
              winAcked[n] = false;
              n++;
              sendPos += REC_SIZE;
            }
            f.close();
            winLen = n;
            if (n == 0) break;   // خواندن فایل ناقص بود؛ دور بعد دوباره

            // ۲) اتصال TCP: باز نگه داشته می‌شود؛ فقط اگر قطع بود وصل می‌شویم.
            //    (قبلاً برای هر رکورد یک اتصال تازه باز و بسته می‌شد که هم
            //    کند بود و هم روی گیرنده مدام connect/disconnect می‌ساخت)
            bool linkReady = uploadClient.connected();
            if (!linkReady) {
              linkReady = uploadClient.connect(serverIP, serverPort);
              if (linkReady) {
                uploadClient.setNoDelay(true);
                VERBOSE_PRINTLN("[UPLOAD] اتصال TCP برقرار شد");
              } else {
                break;   // گیرنده در دسترس نیست؛ دور بعد دوباره
              }
            }

            // ۳) گیت قاعده ۱: بدون READY صریح سرور هیچ داده‌ای نمی‌رود.
            //    HELLO می‌فرستیم و حداکثر ۲ ثانیه منتظر «READY <sid>» می‌مانیم
            linkServerReady = false;
            uploadClient.println("HELLO");
            {
              uint32_t t0 = millis();
              while (!linkServerReady && millis() - t0 < HELLO_WAIT_MS) {
                serviceIncomingLink(false);
                vTaskDelay(pdMS_TO_TICKS(5));
              }
            }
            if (!linkServerReady) {
              DEBUG_PRINTLN("[UPLOAD] سرور آماده نیست -> عقب‌نشینی و تلاش بعدی");
              needBackoff = true;
              break;
            }

            // ۴) ارسال پشت‌سرهم همه‌ی پنجره (سرعت کامل). این فرمت باید
            //    دقیقاً با sscanf گیرنده و parse_industrial_line سرور یکی
            //    بماند (۱۳ فیلد) — formatRecordLine همان فرمت واحد است
            for (uint8_t i = 0; i < n; i++) {
              char buf[300];
              formatRecordLine(win[i], buf, sizeof(buf));
              uploadClient.println(buf);
            }
            lastTxMillis = millis();

            // ۵) جمع‌کردن ACK ها تا سقف مهلت. رکوردی که ACK واقعی نگرفته
            //    باشد همان‌جا می‌ماند و در پنجره‌ی بعد دوباره می‌رود
            {
              uint32_t t0 = millis();
              for (;;) {
                serviceIncomingLink(true);
                uint8_t done = 0;
                while (done < winLen && winAcked[done]) done++;
                if (done >= winLen) break;                  // همه تایید شدند
                if (!uploadClient.connected()) break;       // سوکت قطع شد
                if (millis() - t0 >= ACK_TIMEOUT_MS) break; // مهلت تمام
                vTaskDelay(pdMS_TO_TICKS(5));
              }
            }

            // آفست فقط به اندازه‌ی پیشوند پیاپیِ تاییدشده جلو می‌رود
            uint8_t acked = 0;
            while (acked < winLen && winAcked[acked]) acked++;
            if (acked > 0) {
              pos += (uint32_t)acked * REC_SIZE;
              writeUploadPos(dayFile, pos);
              VERBOSE_PRINTF("[UPLOAD] %u/%u تایید شد، آفست -> %u/%u (نشست %s)\n",
                             (unsigned)acked, (unsigned)winLen,
                             (unsigned)pos, (unsigned)fileSize, linkSid);
            }
            if (acked < winLen) {
              if (acked == 0) {
                DEBUG_PRINTLN("[UPLOAD] هیچ ACK نیامد -> سوکت مشکوک؛ دور بعد تازه");
                uploadClient.stop();
              } else {
                DEBUG_PRINTF("[UPLOAD] %u رکورد بی‌ACK ماند -> پنجره‌ی بعد دوباره\n",
                             (unsigned)(winLen - acked));
              }
              if (!linkServerReady) needBackoff = true;   // SRV_LOST/WAIT وسط پنجره
            }
          } while (false);
          xSemaphoreGive(xSDMutex);
          if (needBackoff) vTaskDelay(pdMS_TO_TICKS(WAIT_BACKOFF_MS));   // بدون قفل SD
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
                    File f = gFs->open(newest, FILE_READ);
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
                EraseResult r = eraseStorage();
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

  // سقف‌های رله/دیجیتال/سنسور از gCyclePeriodMs مشتق می‌شوند که از سرور
  // قابل تغییر است؛ مقدار اولیه از همان لحظه‌ی شروع تسک گرفته می‌شود و در
  // حلقه‌ی زیر هر بار به‌روز می‌شود تا تغییرات زمان اجرا هم اعمال شوند.
  Beat beats[] = {
    { "شبکه", &hbNet, WDT_TIMEOUT_NET_MS, 0, millis() },
    { "رله", &hbRelay, gCyclePeriodMs * 3, 0, millis() },
    { "دیجیتال", &hbDigital, gCyclePeriodMs * 3, 0, millis() },
    { "سنسور", &hbSht, gCyclePeriodMs * 3, 0, millis() },
  };
  const int beatCount = sizeof(beats) / sizeof(beats[0]);

  // در حالت نمایش دیتا فقط تسک شبکه زنده است
  bool viewMode = (xEventGroupGetBits(xSystemEvents) & BIT_REQUEST_AP_DATA_VIEW) != 0;

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    // سقف‌های وابسته به دوره‌ی سیکل را تازه نگه دار (ممکن است از سرور
    // در حین اجرا تغییر کرده باشد؛ اندیس‌ها مطابق ترتیب تعریف beats[] بالاست)
    beats[1].timeoutMs = gCyclePeriodMs * 3;
    beats[2].timeoutMs = gCyclePeriodMs * 3;
    beats[3].timeoutMs = gCyclePeriodMs * 3;

    size_t freeHeap = ESP.getFreeHeap();
    size_t minHeap = ESP.getMinFreeHeap();

    // یک خط خلاصه (سطح ۱) — جزئیات کامل فقط در سطح ۲
    uint32_t freeKb = 0;
    if (gFs == &SD) freeKb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024);
    else if (gFs == &LittleFS) freeKb =
        (uint32_t)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);

    DEBUG_PRINTF("[OK] %s | wifi=%s%d | سیکل=%u | heap=%uk | حافظه=%s(%uk آزاد) | up=%lus\n",
                 relayPhaseText,
                 WiFi.status() == WL_CONNECTED ? "UP " : "DOWN ",
                 WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                 (unsigned)hbRelay,
                 (unsigned)(freeHeap / 1024),
                 storageName(), (unsigned)freeKb,
                 (unsigned long)(millis() / 1000));

    VERBOSE_PRINTF("[HEALTH] heapMin=%u | hb D:%u S:%u N:%u | drops=%u sdErr=%u | "
                   "connects=%u failures=%u\n",
                   (unsigned)minHeap,
                   (unsigned)hbDigital, (unsigned)hbSht, (unsigned)hbNet,
                   (unsigned)wifiDropCount, (unsigned)writeFailures,
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
    if (writeFailures >= WRITE_FAIL_LIMIT) {
      Serial.println("[WDT] نوشتن روی SD مکرراً شکست خورد -> ری‌استارت");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
  }
}
