/**
 * Project: ESP32 Robust Industrial Controller (V1.7.1 - Fixed)
 * Feature: Core Isolation & SPI-WiFi Conflict Mitigation + Advanced Debugging
 * Engineer: Peyman Parsa
 * Fixes: Variable Naming & Global State Mutex Protection
 */

#include <Adafruit_SHT31.h>
#include <Wire.h>
#include <WiFi.h>
#include "SD.h"
#include "SPI.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// --- Debug Configuration ---
#define DEBUG_MODE 1  // Set to 0 to disable verbose logging in production

#if DEBUG_MODE
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTF(...)
#endif

// --- RTC PCF8563 Implementation ---
#define RTC_ADDRESS 0x51
class Rtc_Pcf8563 {
public:
  Rtc_Pcf8563() {}
  void initClock() {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x00);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
      Serial.println("[RTC] Error: Failed to communicate with RTC!");
    }
  }
  void setTime(byte hour, byte minute, byte second) {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x02);
    Wire.write(decToBcd(second));
    Wire.write(decToBcd(minute));
    Wire.write(decToBcd(hour));
    Wire.endTransmission();
  }
  void setDate(byte day, byte weekday, byte month, byte century, byte year) {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x05);
    Wire.write(decToBcd(day));
    Wire.write(decToBcd(weekday));
    Wire.write(decToBcd(month) | (century << 7));
    Wire.write(decToBcd(year));
    Wire.endTransmission();
  }
  char *formatTime() {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x02);
    Wire.endTransmission();
    Wire.requestFrom(RTC_ADDRESS, 3);
    if (Wire.available() >= 3) {
      _second = bcdToDec(Wire.read() & 0x7F);
      _minute = bcdToDec(Wire.read() & 0x7F);
      _hour = bcdToDec(Wire.read() & 0x3F);
    }
    sprintf(strTime, "%02d:%02d:%02d", _hour, _minute, _second);
    return strTime;
  }
  char *formatDate() {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x05);
    Wire.endTransmission();
    Wire.requestFrom(RTC_ADDRESS, 4);
    if (Wire.available() >= 4) {
      _day = bcdToDec(Wire.read() & 0x3F);
      _weekday = bcdToDec(Wire.read() & 0x07);
      byte mRaw = Wire.read();
      _month = bcdToDec(mRaw & 0x1F);
      _century = (mRaw & 0x80) >> 7;
      _year = bcdToDec(Wire.read());
    }
    sprintf(strDate, "%02d/%02d/20%02d", _day, _month, _year);
    return strDate;
  }
  byte getSecond() {
    return _second;
  }
  byte getMinute() {
    return _minute;
  }
  byte getHour() {
    return _hour;
  }
  byte getDay() {
    return _day;
  }
  byte getMonth() {
    return _month;
  }
  byte getYear() {
    return _year;
  }

private:
  byte decToBcd(byte val) {
    return ((val / 10) << 4) + (val % 10);
  }
  byte bcdToDec(byte val) {
    return ((val >> 4) * 10) + (val & 0x0F);
  }
  char strTime[9], strDate[11];
  byte _hour, _minute, _second, _day, _weekday, _month, _year, _century;
};

// --- Standard Data Structure ---
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

// =====================================================================
//  RFC v2 — ذخیره‌سازی فشرده با «تضمین حفظ زمان»
//  ---------------------------------------------------------------
//  یک فایل در روز: /data/YYYYMMDD.rfc
//
//  هر رکورد = 6 بایت (بسته‌بندی بیتی):
//    sod  17 bit  ثانیهٔ روز از RTC (0..86399) — لحظه‌ای و مستقل
//                  (نه زنجیره‌ای/jمعتمد بر رکورد قبل → بدون خطای انباشت)
//    temp  8 bit  دما صحیح °C (int8)       ← بدون اعشار
//    hum   7 bit  رطوبت صحیح % (0..100)    ← بدون اعشار
//    nbcm  4 bit  وضعیت NBCM1..4
//    dnum  8 bit  اختلاف شماره سیکل NUM
//    rsvd  4 bit  رزرو
//  = 48 bit = 6 بایت
//
//  تاریخ (Y/M/D) در هدر فایل روز + نام فایل (دو نسخه).
//  v1 (رکورد 8 بایتی قدیمی) همچنان قابل خواندن است.
// =====================================================================
#define RFC_MAGIC   0x31464352u  // "RCF1"
#define RFC_VERSION 2
#define RFC_REC_SIZE_V1 8
#define RFC_REC_SIZE_V2 6
#define RFC_HEADER_SIZE 20

#pragma pack(1)
struct RfcHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t rec_size;
  int32_t  first_num;
  uint16_t year;
  uint8_t  month, day, hour, minute, second, _pad;
};

struct RfcRecV1 {
  uint16_t dt_s;
  int16_t  temp01;
  uint16_t hum01;
  uint8_t  nbcm;
  uint8_t  dnum;
};
#pragma pack()

struct RfcState {
  bool first;
  int num;
};

static void rfcStateInit(RfcState &st, const RfcHeader &hdr) {
  st.first = true;
  st.num = hdr.first_num;
}

// forward declarations (ترتیب کامپایل)
static int32_t rfcDaysFromCivil(int y, unsigned m, unsigned d);
static int64_t rfcEpoch(int y, int mo, int d, int h, int mi, int s);
static uint8_t rfcMaskFromWifi(const WifiData &d);
static void rfcExpandMask(uint8_t m, WifiData &d);

static void rfc2Pack(uint16_t sod, int8_t tempC, uint8_t humPct,
                     uint8_t nbcm, uint8_t dnum, uint8_t out[6]) {
  if (sod > 86399) sod = 86399;
  if (humPct > 100) humPct = 100;
  uint64_t v = 0;
  v |= (uint64_t)(sod & 0x1FFFFu);
  v |= ((uint64_t)(uint8_t)tempC) << 17;
  v |= ((uint64_t)(humPct & 0x7Fu)) << 25;
  v |= ((uint64_t)(nbcm & 0x0Fu)) << 32;
  v |= ((uint64_t)dnum) << 36;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

// خواندن رکورد v2 — زمان مستقل هر رکورد + تاریخ از هدر
static bool rfcNextV2(File &f, const RfcHeader &hdr, RfcState &st, WifiData &out) {
  uint8_t b[6];
  if (f.read(b, 6) != 6) return false;
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v |= ((uint64_t)b[i]) << (8 * i);
  uint16_t sod = (uint16_t)(v & 0x1FFFFu);
  int8_t tempC = (int8_t)((v >> 17) & 0xFFu);
  uint8_t humPct = (uint8_t)((v >> 25) & 0x7Fu);
  uint8_t nbcm = (uint8_t)((v >> 32) & 0x0Fu);
  uint8_t dnum = (uint8_t)((v >> 36) & 0xFFu);

  if (st.first) {
    st.num = hdr.first_num;
    st.first = false;
  } else {
    st.num += dnum;
  }

  memset(&out, 0, sizeof(out));
  out.NUM = st.num;
  out.Temp = (float)tempC;
  out.Hum = (float)humPct;
  rfcExpandMask(nbcm, out);
  out.Year = hdr.year;
  out.Month = hdr.month;
  out.Day = hdr.day;
  out.Hour = (uint8_t)(sod / 3600);
  out.Minute = (uint8_t)((sod % 3600) / 60);
  out.Second = (uint8_t)(sod % 60);
  return true;
}

// خواندن رکورد v1 قدیمی (dt تجمعی) — سازگاری عقب‌رو
static bool rfcNextV1(File &f, const RfcHeader &hdr, int64_t *epoch,
                      RfcState &st, WifiData &out) {
  RfcRecV1 rec;
  if (f.read((uint8_t *)&rec, sizeof(rec)) != sizeof(rec)) return false;
  if (st.first) {
    st.num = hdr.first_num;
    st.first = false;
    *epoch = rfcEpoch(hdr.year, hdr.month, hdr.day, hdr.hour, hdr.minute, hdr.second);
  } else {
    *epoch += rec.dt_s;
    st.num += rec.dnum;
  }
  memset(&out, 0, sizeof(out));
  out.NUM = st.num;
  out.Temp = rec.temp01 / 10.0f;
  out.Hum = rec.hum01 / 10.0f;
  rfcExpandMask(rec.nbcm, out);
  int64_t days = *epoch / 86400;
  int64_t rem = *epoch - days * 86400;
  if (rem < 0) { rem += 86400; days--; }
  out.Hour = (uint8_t)(rem / 3600);
  out.Minute = (uint8_t)((rem % 3600) / 60);
  out.Second = (uint8_t)(rem % 60);
  int z = (int)days + 719468;
  int era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp + (mp < 10 ? 3 : 9);
  y += (m <= 2);
  out.Year = y; out.Month = (uint8_t)m; out.Day = (uint8_t)d;
  return true;
}

// روزهای میلادی از 1970-01-01 (الگوریتم Hinnant) برای محاسبه dt
static int32_t rfcDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int)doe - 719468;
}

static int64_t rfcEpoch(int y, int mo, int d, int h, int mi, int s) {
  return (int64_t)rfcDaysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400
       + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

static uint8_t rfcMaskFromWifi(const WifiData &d) {
  uint8_t m = 0;
  if (d.NBCM1) m |= 0x01;
  if (d.NBCM2) m |= 0x02;
  if (d.NBCM3) m |= 0x04;
  if (d.NBCM4) m |= 0x08;
  return m;
}

static void rfcExpandMask(uint8_t m, WifiData &d) {
  d.NBCM1 = m & 0x01;
  d.NBCM2 = m & 0x02;
  d.NBCM3 = m & 0x04;
  d.NBCM4 = m & 0x08;
}

enum WiFiOperationMode {
  MODE_CLIENT_UPLOAD = 0,  // حالت نرمال: اتصال به مودم و آپلود
  MODE_HOTSPOT_VIEW = 1    // حالت دیباگ: هات‌اسپات و نمایش دیتا
};

// --- FreeRTOS Handles ---
QueueHandle_t xDataQueue;
SemaphoreHandle_t xSDMutex;
SemaphoreHandle_t xGlobalStateMutex;  // ADDED: Mutex for global state protection
EventGroupHandle_t xDoorEvents;

#define RELAY_OPEN_DOORS_PIN 2
#define RELAY_CLOSE_DOORS_PIN 4
#define SD_CS_PIN 5
const int inputPins[] = { 13, 15, 16, 17 };

Adafruit_SHT31 sht31 = Adafruit_SHT31();
Rtc_Pcf8563 rtc;
WiFiClient client;
const int configPort = 81;
const int serverPort = 80;
WiFiServer configServer(configPort);

IPAddress serverIP(192, 168, 4, 1);
IPAddress ap_local_IP(192, 168, 1, 1);
IPAddress ap_gateway(192, 168, 1, 1);
IPAddress ap_subnet(255, 255, 255, 0);

volatile int currentGlobalID = 0;
volatile bool shared_NBCM1_flag = false;
volatile bool shared_NBCM2_flag = false;

// FIXED: Renamed to match usage in tasks and declared properly
volatile WifiData globalSystemState;

volatile bool raw_N1_Open = false;
volatile bool raw_N1_Close = false;
volatile bool raw_N2_Open = false;
volatile bool raw_N2_Close = false;

// Event Group Bits
#define BIT_START_DIGITAL_MONITORING (1UL << 0)
#define BIT_STOP_DIGITAL_MONITORING (1UL << 1)
#define BIT_START_SHT_READ (1UL << 2)
#define BIT_DIGITAL_READ_COMPLETE (1UL << 3)
#define BIT_SHT_READ_COMPLETE (1UL << 4)
#define BIT_NETWORK_BOOT_COMPLETE (1UL << 5)
#define BIT_REQUEST_AP_DATA_VIEW (1UL << 6)
#define BIT_WIFI_PERMIT (1UL << 7)

const unsigned long requiredHighDuration = 100;

// Prototypes
void startupNetworkLogic();
void interactiveClockSetup();
void saveToSD(const WifiData &data);
int getNextPersistentID();
void saveNextPersistentID(int id);
int rfcScanLastNum();
void TaskRelayControl(void *pvParameters);
void TaskReadSHT(void *pvParameters);
void TaskDigitalRead(void *pvParameters);
void TaskInternalWiFiConnection(void *pvParameters);
bool uploadRfcToServer(const String &fPath);
bool uploadLegacyDatToServer(const String &fPath, File &file);
void sendLastRecordOnly(WiFiClient &cl, String filePath);

void setup() {
  Serial.begin(115200);
  DEBUG_PRINTLN("\n[DEBUG] Industrial Controller V1.7.1 Booting...");
  Wire.begin();
  delay(1000);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  xDataQueue = xQueueCreate(20, sizeof(WifiData));
  xSDMutex = xSemaphoreCreateMutex();
  xGlobalStateMutex = xSemaphoreCreateMutex();  // ADDED: Init global mutex
  xDoorEvents = xEventGroupCreate();

  rtc.initClock();

  if (xSemaphoreTake(xSDMutex, portMAX_DELAY)) {
    DEBUG_PRINTLN("[DEBUG] Accessing SD Card for ID initialization...");
    if (!SD.begin(SD_CS_PIN)) {
      Serial.println("[SD] Critical Error: SD Card not detected!");
    } else {
      DEBUG_PRINTLN("[DEBUG] SD Card initialized successfully.");
      if (!SD.exists("/data")) SD.mkdir("/data");

      int maxFileID = 0;
      bool filesFound = false;
      File root = SD.open("/data");
      if (root) {
        File file = root.openNextFile();
        while (file) {
          String fn = file.name();
          int underscoreIdx = fn.lastIndexOf('_');
          if (underscoreIdx != -1 && fn.endsWith(".dat")) {
            filesFound = true;
            int id = fn.substring(underscoreIdx + 1, fn.length() - 4).toInt();
            if (id > maxFileID) maxFileID = id;
          }
          file = root.openNextFile();
        }
        root.close();
      }

      // آخرین NUM از فایل‌های فشرده روزانه (.rfc)
      int rfcNum = rfcScanLastNum();
      if (rfcNum > maxFileID) maxFileID = rfcNum;
      if (rfcNum > 0) filesFound = true;

      if (!filesFound) {
        currentGlobalID = 0;
      } else {
        int lastSaved = getNextPersistentID();
        currentGlobalID = (maxFileID > lastSaved) ? maxFileID : lastSaved;
      }
      DEBUG_PRINTF("[DEBUG] Resuming from Global ID: %d\n", currentGlobalID);
      saveNextPersistentID(currentGlobalID);
    }
    xSemaphoreGive(xSDMutex);
  }

  startupNetworkLogic();

  if (!sht31.begin(0x44)) Serial.println("[SHT31] Error: SHT31 sensor not found!");

  pinMode(RELAY_OPEN_DOORS_PIN, OUTPUT);
  pinMode(RELAY_CLOSE_DOORS_PIN, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(inputPins[i], INPUT_PULLDOWN);

  xTaskCreatePinnedToCore(TaskDigitalRead, "DigiRead", 4096, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(TaskRelayControl, "RelayCtrl", 4096, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(TaskReadSHT, "SHTRead", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskInternalWiFiConnection, "WiFiConn", 8192, NULL, 2, NULL, 1);

  DEBUG_PRINTLN("[DEBUG] All tasks created and pinned to Core 1.");
}

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

void startupNetworkLogic() {
  WiFi.mode(WIFI_STA);
  DEBUG_PRINTLN("[DEBUG] Connecting to ESP8266_AP (60s Timeout)...");
  WiFi.begin("ESP8266_AP", "12345678");
  unsigned long startAttempt = millis();
  bool connected = false;
  while (millis() - startAttempt < 60000) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(500);
    DEBUG_PRINT(".");
  }
  if (connected) {
    DEBUG_PRINTF("\n[DEBUG] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DEBUG_PRINTLN("\n[DEBUG] Connection Failed. Cleaning up stack...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(3000);
    DEBUG_PRINTLN("[DEBUG] Stack cleared. Switching to Hotspot Mode...");
    interactiveClockSetup();
  }
  xEventGroupSetBits(xDoorEvents, BIT_NETWORK_BOOT_COMPLETE);
}

void interactiveClockSetup() {
  DEBUG_PRINTLN("\n[HOTSPOT] Entering Interactive Configuration Mode...");

  // ۱. آماده‌سازی شبکه
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_local_IP, ap_gateway, ap_subnet);
  WiFi.softAP("SetClock", "12345678");
  configServer.begin();

  DEBUG_PRINTLN("[HOTSPOT] AP Started on Port 81");

  // ۲. متغیرهای کنترل
  unsigned long lastActiveTime = millis();
  const unsigned long INACTIVITY_LIMIT = 60000;

  bool timeConfigured = false;
  bool sessionFinished = false;

  // ۳. حلقه اصلی بقا
  while ((millis() - lastActiveTime < INACTIVITY_LIMIT) && !sessionFinished) {

    WiFiClient c = configServer.available();

    if (c) {
      DEBUG_PRINTLN("[HOTSPOT] Client Connected.");
      lastActiveTime = millis();
      while (c.available()) c.read();  // تخلیه بافر

      // نمایش وضعیت اولیه
      if (!timeConfigured) {
        c.println("--- INDUSTRIAL CONTROLLER V1.7 ---");
        c.print("System Time: ");
        c.print(rtc.formatDate());
        c.print(" ");
        c.println(rtc.formatTime());
        c.println("Is this correct? (ok/nok):");
      }

      while (c.connected() && !sessionFinished) {
        // چک کردن تایمر نگهبان
        if (millis() - lastActiveTime > INACTIVITY_LIMIT) {
          DEBUG_PRINTLN("[HOTSPOT] Inactivity Timeout!");
          break;
        }

        if (c.available()) {
          lastActiveTime = millis();  // دیتایی آمد -> ریست تایمر
          String resp = c.readStringUntil('\n');
          resp.trim();

          // ==================================================
          // فاز ۱: تنظیم ساعت
          // ==================================================
          if (!timeConfigured) {
            if (resp.equalsIgnoreCase("ok")) {
              c.println("[INFO] Time Verified.");
              timeConfigured = true;
              goto SHOW_GUIDANCE;  // پرش مجاز به خارج از بلوک‌ها
            } else if (resp.equalsIgnoreCase("nok")) {
              c.println("[SETUP] Enter Date & Time components:");

              int val[6];
              const char *msgs[] = {
                "Year (e.g. 2025): ", "Month (1-12): ", "Day (1-31): ",
                "Hour (0-23): ", "Minute (0-59): ", "Second (0-59): "
              };

              for (int i = 0; i < 6; i++) {
                c.print(msgs[i]);

                while (!c.available()) {
                  if (millis() - lastActiveTime > INACTIVITY_LIMIT) goto LOOP_EXIT;
                  delay(10);
                }

                lastActiveTime = millis();
                int temp = c.parseInt();
                while (c.available() && c.peek() < '0') c.read();

                // اعتبارسنجی
                bool isValid = true;
                if (i == 0 && (temp < 2024 || temp > 2099)) isValid = false;
                if (i == 1 && (temp < 1 || temp > 12)) isValid = false;
                if (i == 2 && (temp < 1 || temp > 31)) isValid = false;
                if (i == 3 && (temp < 0 || temp > 23)) isValid = false;
                if (i > 3 && (temp < 0 || temp > 59)) isValid = false;

                if (!isValid) {
                  c.println("\n[ERROR] Invalid Value! Try again.");
                  i--;
                  continue;
                }
                val[i] = temp;
                c.println(" OK");
              }

              // ثبت در RTC
              rtc.setDate(val[2], 0, val[1], 0, val[0] % 100);
              rtc.setTime(val[3], val[4], val[5]);

              c.println("[SUCCESS] RTC Updated.");
              timeConfigured = true;
            }

SHOW_GUIDANCE:
            if (timeConfigured) {
              c.println("\n--------------------------------");
              c.println(">> CONFIGURATION MENU <<");
              c.println("1. Type 'yes' -> DEBUG MODE (View Last Data on AP)");
              c.println("2. Type 'no'  -> RUN MODE (Start Industrial Logic)");
              c.println("Select Mode (yes/no):");
            }
          }
          // ==================================================
          // فاز ۲: تعیین وضعیت سیستم
          // ==================================================
          else {
            if (resp.equalsIgnoreCase("yes")) {
              xEventGroupSetBits(xDoorEvents, BIT_REQUEST_AP_DATA_VIEW);
              c.println("[CONFIG] Mode Set: DATA VIEW. Rebooting logic...");
              sessionFinished = true;
            } else if (resp.equalsIgnoreCase("no")) {
              xEventGroupClearBits(xDoorEvents, BIT_REQUEST_AP_DATA_VIEW);
              c.println("[CONFIG] Mode Set: NORMAL RUN. Starting...");
              sessionFinished = true;
            } else {
              c.println("[ERROR] Invalid command. Type 'yes' or 'no':");
            }
          }
        }
        delay(10);
      }
      c.stop();
LOOP_EXIT:;  // لیبل خروج اضطراری
      DEBUG_PRINTLN("[HOTSPOT] Client Disconnected.");
    }
    delay(50);
  }

  configServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  if (!sessionFinished) {
    xEventGroupClearBits(xDoorEvents, BIT_REQUEST_AP_DATA_VIEW);
    DEBUG_PRINTLN("[HOTSPOT] Session Timeout. Defaulting to Normal Mode.");
  }

  delay(500);
}

void TaskRelayControl(void *pvParameters) {
  const TickType_t xCycleFrequency = pdMS_TO_TICKS(120000);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  DEBUG_PRINTLN("[RELAY] Waiting for System Boot...");
  xEventGroupWaitBits(xDoorEvents, BIT_NETWORK_BOOT_COMPLETE, pdFALSE, pdTRUE, portMAX_DELAY);

//   if (xEventGroupGetBits(xDoorEvents) & BIT_REQUEST_AP_DATA_VIEW) {
//     vTaskDelete(NULL);
//   }

  for (;;) {
    DEBUG_PRINTLN("\n[RELAY] >>> Cycle Started.");
    xEventGroupClearBits(xDoorEvents, BIT_WIFI_PERMIT);

    bool cycleSuccess = false;

    // --- حلقه تلاش (تا ۳ بار) ---
    for (int attempt = 1; attempt <= 3; attempt++) {
      DEBUG_PRINTF("[RELAY] Attempt %d/3...\n", attempt);

      // 1. شروع مانیتورینگ
      xEventGroupSetBits(xDoorEvents, BIT_START_DIGITAL_MONITORING);

      // 2. فرمان رله ۱
      digitalWrite(RELAY_OPEN_DOORS_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(800));
      digitalWrite(RELAY_OPEN_DOORS_PIN, LOW);

      // 3. صبر
      vTaskDelay(pdMS_TO_TICKS(3000));

      // 4. فرمان رله ۲
      digitalWrite(RELAY_CLOSE_DOORS_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(800));
      vTaskDelay(pdMS_TO_TICKS(200));
      digitalWrite(RELAY_CLOSE_DOORS_PIN, LOW);

      // 5. توقف مانیتورینگ و دریافت گزارش
      xEventGroupSetBits(xDoorEvents, BIT_STOP_DIGITAL_MONITORING);
      xEventGroupWaitBits(xDoorEvents, BIT_DIGITAL_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));

      // ========================================================
      // تحلیل هوشمند خطا (Logic Core)
      // ========================================================

      // بررسی گیر کردن (باز شده ولی بسته نشده)
      bool n1_stuck = (raw_N1_Open && !raw_N1_Close);
      bool n2_stuck = (raw_N2_Open && !raw_N2_Close);

      // بررسی موفقیت کامل
      bool n1_ok = (raw_N1_Open && raw_N1_Close);
      bool n2_ok = (raw_N2_Open && raw_N2_Close);

      if (n1_ok && n2_ok) {
        DEBUG_PRINTLN("[RELAY] Success! Both Channels OK.");
        cycleSuccess = true;
        break;  // خروج از حلقه تلاش
      }

      // اگر هر کدام گیر کرده باشند -> اجرای عملیات چکش‌کاری
      else if (n1_stuck || n2_stuck) {
        DEBUG_PRINTLN("[RELAY] JAM DETECTED! Initiating Hammering (5x50ms)...");

        // روشن کردن دوباره مانیتورینگ برای دیدن نتیجه ضربه‌ها
        xEventGroupSetBits(xDoorEvents, BIT_START_DIGITAL_MONITORING);

        // ارسال ۵ ضربه سریع
        for (int k = 0; k < 5; k++) {
          digitalWrite(RELAY_CLOSE_DOORS_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(50));
          digitalWrite(RELAY_CLOSE_DOORS_PIN, LOW);
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        // پایان چکش‌کاری و بررسی نتیجه
        xEventGroupSetBits(xDoorEvents, BIT_STOP_DIGITAL_MONITORING);
        xEventGroupWaitBits(xDoorEvents, BIT_DIGITAL_READ_COMPLETE, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));

        // بررسی مجدد بعد از ضربه
        if ((raw_N1_Open && raw_N1_Close) && (raw_N2_Open && raw_N2_Close)) {
          DEBUG_PRINTLN("[RELAY] Recovered after hammering!");
          cycleSuccess = true;
          break;
        } else {
          DEBUG_PRINTLN("[RELAY] Hammering Failed. Retrying full cycle...");
        }
      } else {
        DEBUG_PRINTLN("[RELAY] Severe Failure (Not Opened?). Retrying...");
      }

      if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // ============================================================
    // پایان سیکل
    // ============================================================

    // 1. خواندن سنسورها
    xEventGroupSetBits(xDoorEvents, BIT_START_SHT_READ);
    vTaskDelay(pdMS_TO_TICKS(1000));
    xEventGroupClearBits(xDoorEvents, BIT_START_SHT_READ);

    // 2. شماره‌گذاری و ثبت نهایی (Critical Section)
    if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(1000))) {
      currentGlobalID++;
      globalSystemState.NUM = currentGlobalID;

      if (cycleSuccess) {
        globalSystemState.NBCM1 = true;
        globalSystemState.NBCM2 = true;
      } else {
        // ثبت وضعیت واقعی خرابی
        globalSystemState.NBCM1 = (raw_N1_Open && raw_N1_Close);
        globalSystemState.NBCM2 = (raw_N2_Open && raw_N2_Close);
      }

      // 4. ارسال به صف (کپی ایمن)
      WifiData dataToSend;
      // استفاده از memcpy برای کپی بایت‌به‌بایت از متغیر volatile
      memcpy(&dataToSend, (void *)&globalSystemState, sizeof(WifiData));
      xSemaphoreGive(xGlobalStateMutex);

      xQueueSend(xDataQueue, (void *)&dataToSend, pdMS_TO_TICKS(100));
    } else {
      DEBUG_PRINTLN("[RELAY] Error: Could not take Mutex for State Update!");
    }

    // 5. خواب
    xEventGroupSetBits(xDoorEvents, BIT_WIFI_PERMIT);
    vTaskDelayUntil(&xLastWakeTime, xCycleFrequency);
  }
}

void TaskDigitalRead(void *pvParameters) {
  const int numInputs = 4;
  bool lastInputState[numInputs];
  unsigned long inputHighStartTime[numInputs];
  bool pulseConfirmed_window[numInputs];
  bool monitoringActive = false;

  for (;;) {
    xEventGroupWaitBits(xDoorEvents, BIT_START_DIGITAL_MONITORING, pdTRUE, pdFALSE, portMAX_DELAY);
    DEBUG_PRINTLN("[DIGI-READ] Analysis window ACTIVE.");
    monitoringActive = true;

    // متغیرهای محلی (Local)
    bool openSignalConfirmedNBCM1 = false;
    bool closeSignalConfirmedNBCM1 = false;
    bool openSignalConfirmedNBCM2 = false;
    bool closeSignalConfirmedNBCM2 = false;

    for (int i = 0; i < numInputs; i++) {
      pulseConfirmed_window[i] = false;
      lastInputState[i] = digitalRead(inputPins[i]);
      inputHighStartTime[i] = 0;
    }

    while (monitoringActive) {
      for (int i = 0; i < numInputs; i++) {
        bool currentState = digitalRead(inputPins[i]);
        if (currentState && !lastInputState[i]) {
          inputHighStartTime[i] = millis();
        } else if (currentState && lastInputState[i]) {
          if (inputHighStartTime[i] != 0 && !pulseConfirmed_window[i] && (millis() - inputHighStartTime[i] >= requiredHighDuration)) {
            pulseConfirmed_window[i] = true;
          }
        } else if (!currentState && lastInputState[i]) {
          inputHighStartTime[i] = 0;
        }
        lastInputState[i] = currentState;
      }

      openSignalConfirmedNBCM1 |= pulseConfirmed_window[0];
      closeSignalConfirmedNBCM1 |= pulseConfirmed_window[1];
      openSignalConfirmedNBCM2 |= pulseConfirmed_window[2];
      closeSignalConfirmedNBCM2 |= pulseConfirmed_window[3];

      if (xEventGroupGetBits(xDoorEvents) & BIT_STOP_DIGITAL_MONITORING) {
        xEventGroupClearBits(xDoorEvents, BIT_STOP_DIGITAL_MONITORING);
        monitoringActive = false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    // ============================================================
    // پر کردن متغیرهای گلوبال با حفاظت Mutex
    // ============================================================
    raw_N1_Open = openSignalConfirmedNBCM1;
    raw_N1_Close = closeSignalConfirmedNBCM1;
    raw_N2_Open = openSignalConfirmedNBCM2;
    raw_N2_Close = closeSignalConfirmedNBCM2;

    if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(500))) {
      // پر کردن استراکچر نهایی (پیش‌فرض)
      globalSystemState.NBCM1 = (openSignalConfirmedNBCM1 && closeSignalConfirmedNBCM1);
      globalSystemState.NBCM2 = (openSignalConfirmedNBCM2 && closeSignalConfirmedNBCM2);
      globalSystemState.NBCM3 = false;
      globalSystemState.NBCM4 = false;
      xSemaphoreGive(xGlobalStateMutex);
    }

    DEBUG_PRINTLN("[DIGI-READ] Global Flags Updated.");
    xEventGroupSetBits(xDoorEvents, BIT_DIGITAL_READ_COMPLETE);
  }
}

void TaskReadSHT(void *pvParameters) {
  // تنظیمات نمونه‌برداری صنعتی
  const int SAMPLE_COUNT = 10;     // تعداد نمونه‌ها برای میانگین‌گیری
  const int SAMPLE_DELAY_MS = 20;  // فاصله بین هر نمونه (۲۰ میلی‌ثانیه)

  // حافظه تاریخی (History) برای تحلیل روند
  static float lastValidTemp = 25.0;
  static float lastValidHum = 50.0;

  // پارامترهای فیلتر نرم‌کننده
  const float MAX_TEMP_JUMP = 5.0;
  const float MAX_HUM_JUMP = 15.0;

  // حافظه تاریخی ساعت
  static uint8_t last_Year = 25;
  static uint8_t last_Month = 1;
  static uint8_t last_Day = 1;
  static uint8_t last_Hour = 0;
  static uint8_t last_Minute = 0;
  static uint8_t last_Second = 0;

  for (;;) {
    xEventGroupWaitBits(xDoorEvents, BIT_START_SHT_READ, pdTRUE, pdFALSE, portMAX_DELAY);
    DEBUG_PRINTLN("[SHT-TASK] Starting Sampling Sequence...");

    // ============================================================
    // فاز ۱: نمونه‌برداری چندگانه و میانگین‌گیری (Oversampling)
    // ============================================================
    float sumTemp = 0;
    float sumHum = 0;
    int validSamples = 0;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
      float t = sht31.readTemperature();
      float h = sht31.readHumidity();

      if (!isnan(t) && !isnan(h)) {
        sumTemp += t;
        sumHum += h;
        validSamples++;
      }
      vTaskDelay(pdMS_TO_TICKS(SAMPLE_DELAY_MS));  // صبر کوتاه بین نمونه‌ها
    }

    // ============================================================
    // فاز ۲: تحلیل هوشمند روی میانگین (Smart Trend Logic)
    // ============================================================
    if (validSamples > 0) {
      // محاسبه میانگین (دیتای تمیز شده از نویز)
      float avgTemp = sumTemp / validSamples;
      float avgHum = sumHum / validSamples;

      DEBUG_PRINTF("[SHT-TASK] Avg Result (%d samples): T=%.2f, H=%.2f\n", validSamples, avgTemp, avgHum);

      // 1. چک کردن محدوده فیزیکی (Sanity Check)
      bool physicalSanity = (avgTemp > -20 && avgTemp < 85) && (avgHum >= 0 && avgHum <= 100);

      if (physicalSanity) {
        // 2. تحلیل روند (Trend Analysis) - جلوگیری از پرش ناگهانی روی میانگین
        float tempDelta = avgTemp - lastValidTemp;
        float humDelta = avgHum - lastValidHum;

        // محدود کردن تغییرات شدید (Damping)
        if (abs(tempDelta) > MAX_TEMP_JUMP) {
          if (tempDelta > 0) avgTemp = lastValidTemp + MAX_TEMP_JUMP;
          else avgTemp = lastValidTemp - MAX_TEMP_JUMP;
          DEBUG_PRINTLN("[SHT-TASK] Trend Logic: Temp jump clamped.");
        }

        if (abs(humDelta) > MAX_HUM_JUMP) {
          if (humDelta > 0) avgHum = lastValidHum + MAX_HUM_JUMP;
          else avgHum = lastValidHum - MAX_HUM_JUMP;
        }

        // ثبت نهایی در تخته‌سیاه با حفاظت Mutex
        if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(500))) {
          globalSystemState.Temp = avgTemp;
          globalSystemState.Hum = avgHum;
          xSemaphoreGive(xGlobalStateMutex);
        }

        // آپدیت حافظه تاریخی
        lastValidTemp = avgTemp;
        lastValidHum = avgHum;
      } else {
        DEBUG_PRINTLN("[SHT-TASK] Error: Average data out of physical range!");
      }
    } else {
      DEBUG_PRINTLN("[SHT-TASK] Critical Error: Sensor failed all samples!");
    }

    // ============================================================
    // فاز ۳: تحلیل و اعتبارسنجی زمان (RTC Logic)
    // ============================================================
    rtc.formatDate();
    rtc.formatTime();

    int curYear = rtc.getYear();
    int curMonth = rtc.getMonth();
    int curDay = rtc.getDay();

    bool timeIsValid = (curYear >= 24) && (curMonth >= 1 && curMonth <= 12) && (curDay >= 1 && curDay <= 31);

    if (xSemaphoreTake(xGlobalStateMutex, pdMS_TO_TICKS(500))) {
      if (timeIsValid) {
        globalSystemState.Year = 2000 + curYear;
        globalSystemState.Month = curMonth;
        globalSystemState.Day = curDay;
        globalSystemState.Hour = rtc.getHour();
        globalSystemState.Minute = rtc.getMinute();
        globalSystemState.Second = rtc.getSecond();

        // ذخیره در حافظه
        last_Year = curYear;
        last_Month = curMonth;
        last_Day = curDay;
        last_Hour = rtc.getHour();
        last_Minute = rtc.getMinute();
        last_Second = rtc.getSecond();
      } else {
        DEBUG_PRINTLN("[SHT-TASK] RTC Noise detected. Using History.");
        globalSystemState.Year = 2000 + last_Year;
        globalSystemState.Month = last_Month;
        globalSystemState.Day = last_Day;
        globalSystemState.Hour = last_Hour;
        globalSystemState.Minute = last_Minute;
        globalSystemState.Second = last_Second;
      }
      xSemaphoreGive(xGlobalStateMutex);
    }

    xEventGroupSetBits(xDoorEvents, BIT_SHT_READ_COMPLETE);
  }
}

// تابع کمکی: آخرین NUM ذخیره‌شده در فایل‌های فشرده روزانه
int rfcScanLastNum() {
  int best = 0;
  File root = SD.open("/data");
  if (!root) return 0;
  File entry = root.openNextFile();
  while (entry) {
    String fn = String(entry.name());
    if (fn.endsWith(".rfc")) {
      String path = fn.startsWith("/") ? fn : ("/data/" + fn);
      File f = SD.open(path, FILE_READ);
      if (f) {
        RfcHeader hdr;
        if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == RFC_MAGIC) {
          RfcState st;
          rfcStateInit(st, hdr);
          int64_t epoch = 0;
          WifiData d;
          if (hdr.version >= 2) {
            while (rfcNextV2(f, hdr, st, d)) { }
          } else {
            while (rfcNextV1(f, hdr, &epoch, st, d)) { }
          }
          if (st.num > best) best = st.num;
        }
        f.close();
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  return best;
}

// (rfcFillOut حذف شد — بازسازی با rfcNextV2 / rfcNextV1)

// ارسال محتویات فایل به کلاینت هات‌اسپات (.rfc یا .dat قدیمی)
void sendDataFile(WiFiClient &cl, String filePath) {
  File f = SD.open(filePath, FILE_READ);
  if (!f) return;

  if (filePath.endsWith(".dat")) {
    WifiData d;
    if (f.read((uint8_t *)&d, sizeof(WifiData)) == sizeof(WifiData)) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "{\"ID\":%d,\"T\":%.0f,\"H\":%.0f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
               d.NUM, d.Temp, d.Hum, d.NBCM1, d.NBCM2,
               d.Year, d.Month, d.Day, d.Hour, d.Minute, d.Second);
      cl.println(buf);
    }
    f.close();
    return;
  }

  RfcHeader hdr;
  if (f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != RFC_MAGIC) {
    f.close();
    return;
  }
  RfcState st;
  rfcStateInit(st, hdr);
  int64_t epoch = 0;
  WifiData d;
  if (hdr.version >= 2) {
    while (rfcNextV2(f, hdr, st, d)) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "{\"ID\":%d,\"T\":%.0f,\"H\":%.0f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
               d.NUM, d.Temp, d.Hum, d.NBCM1, d.NBCM2,
               d.Year, d.Month, d.Day, d.Hour, d.Minute, d.Second);
      cl.println(buf);
    }
  } else {
    while (rfcNextV1(f, hdr, &epoch, st, d)) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "{\"ID\":%d,\"T\":%.0f,\"H\":%.0f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
               d.NUM, d.Temp, d.Hum, d.NBCM1, d.NBCM2,
               d.Year, d.Month, d.Day, d.Hour, d.Minute, d.Second);
      cl.println(buf);
    }
  }
  f.close();
}

// فقط آخرین رکورد فایل (دستور sync)
void sendLastRecordOnly(WiFiClient &cl, String filePath) {
  File f = SD.open(filePath, FILE_READ);
  if (!f) return;

  if (filePath.endsWith(".dat")) {
    f.close();
    sendDataFile(cl, filePath);
    return;
  }

  RfcHeader hdr;
  if (f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != RFC_MAGIC) {
    f.close();
    return;
  }
  RfcState st;
  rfcStateInit(st, hdr);
  int64_t epoch = 0;
  WifiData d, last;
  bool any = false;
  if (hdr.version >= 2) {
    while (rfcNextV2(f, hdr, st, d)) { last = d; any = true; }
  } else {
    while (rfcNextV1(f, hdr, &epoch, st, d)) { last = d; any = true; }
  }
  f.close();
  if (!any) return;

  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"ID\":%d,\"T\":%.0f,\"H\":%.0f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
           last.NUM, last.Temp, last.Hum, last.NBCM1, last.NBCM2,
           last.Year, last.Month, last.Day, last.Hour, last.Minute, last.Second);
  cl.println(buf);
}

// آپلود فایل فشرده به سرور (هر رکورد یک خط NUM=؛ حذف فقط پس از ACK کامل)
bool uploadRfcToServer(const String &fPath) {
  File file = SD.open(fPath, FILE_READ);
  if (!file) return false;

  RfcHeader hdr;
  if (file.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != RFC_MAGIC) {
    file.close();
    return false;
  }

  if (!client.connect(serverIP, serverPort)) {
    file.close();
    return false;
  }

  RfcState st;
  rfcStateInit(st, hdr);
  int64_t epoch = 0;
  WifiData d;
  bool allAck = true;
  bool any = false;

  auto sendOne = [&](const WifiData &rec) -> bool {
    char buf[300];
    snprintf(buf, sizeof(buf),
             "NUM=%d,NBCM1=%s,NBCM2=%s,NBCM3=%s,NBCM4=%s,Temp=%.0f,Humidity=%.0f,Date=%04d-%02d-%02d,Time=%02d:%02d:%02d",
             rec.NUM,
             rec.NBCM1 ? "OK" : "NOK",
             rec.NBCM2 ? "OK" : "NOK",
             rec.NBCM3 ? "OK" : "NOK",
             rec.NBCM4 ? "OK" : "NOK",
             rec.Temp, rec.Hum,
             rec.Year, rec.Month, rec.Day,
             rec.Hour, rec.Minute, rec.Second);
    client.println(buf);
    unsigned long t = millis();
    while (millis() - t < 3000) {
      if (client.available() && client.readStringUntil('\n').indexOf("OK") != -1) {
        return true;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
  };

  if (hdr.version >= 2) {
    while (rfcNextV2(file, hdr, st, d)) {
      any = true;
      if (!sendOne(d)) { allAck = false; break; }
    }
  } else {
    while (rfcNextV1(file, hdr, &epoch, st, d)) {
      any = true;
      if (!sendOne(d)) { allAck = false; break; }
    }
  }

  file.close();
  client.stop();
  if (allAck && any) {
    SD.remove(fPath);
    DEBUG_PRINTLN("[CLIENT] RFC file fully ACKed. Deleted.");
  }
  return allAck && any;
}

// آپلود فایل قدیمی تک‌رکوردی (سازگاری با فرمت قبلی)
bool uploadLegacyDatToServer(const String &fPath, File &file) {
  WifiData stored;
  if (file.read((uint8_t *)&stored, sizeof(WifiData)) != sizeof(WifiData)) {
    file.close();
    return false;
  }
  file.close();

  if (!client.connect(serverIP, serverPort)) return false;

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
  client.println(buf);

  unsigned long t = millis();
  bool ack = false;
  while (millis() - t < 3000) {
    if (client.available() && client.readStringUntil('\n').indexOf("OK") != -1) {
      ack = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  client.stop();
  if (ack) {
    SD.remove(fPath);
    DEBUG_PRINTLN("[CLIENT] Legacy ACK RX. File Deleted.");
  }
  return ack;
}

// ذخیره فشرده v2: فایل روزانه + رکورد 6 بایتی (زمان لحظه‌ای RTC)
void saveToSD(const WifiData &data) {
  static bool hasLast = false;
  static int lastNum = 0;

  float tRaw = data.Temp;
  float hRaw = data.Hum;
  if (isnan(tRaw) || isinf(tRaw)) tRaw = 0.0f;
  if (isnan(hRaw) || isinf(hRaw)) hRaw = 0.0f;

  int tempI = (int)lroundf(tRaw);
  if (tempI < -128) tempI = -128;
  if (tempI > 127) tempI = 127;
  int humI = (int)lroundf(hRaw);
  if (humI < 0) humI = 0;
  if (humI > 100) humI = 100;

  uint16_t sod = (uint16_t)((int)data.Hour * 3600 + (int)data.Minute * 60 + (int)data.Second);
  if (sod > 86399) sod = 86399;

  char filename[40];
  snprintf(filename, sizeof(filename), "/data/%04d%02d%02d.rfc",
           data.Year, data.Month, data.Day);

  if (!SD.exists(filename)) {
    File hf = SD.open(filename, FILE_WRITE);
    if (!hf) {
      DEBUG_PRINTLN("[RFC] Critical: could not create daily file!");
      return;
    }
    RfcHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = RFC_MAGIC;
    hdr.version = RFC_VERSION;
    hdr.rec_size = RFC_REC_SIZE_V2;
    hdr.first_num = data.NUM;
    hdr.year = (uint16_t)data.Year;
    hdr.month = data.Month;
    hdr.day = data.Day;
    hdr.hour = data.Hour;
    hdr.minute = data.Minute;
    hdr.second = data.Second;
    hdr._pad = 0;
    hf.write((const uint8_t *)&hdr, sizeof(hdr));
    hf.close();
    hasLast = false;
  }

  if (!hasLast) {
    File rf = SD.open(filename, FILE_READ);
    if (rf) {
      RfcHeader hdr;
      if (rf.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == RFC_MAGIC) {
        RfcState st;
        rfcStateInit(st, hdr);
        int64_t ep = 0;
        WifiData tmp;
        if (hdr.version >= 2) {
          while (rfcNextV2(rf, hdr, st, tmp)) { }
        } else {
          while (rfcNextV1(rf, hdr, &ep, st, tmp)) { }
        }
        lastNum = st.num;
        hasLast = (st.first == false);
      }
      rf.close();
    }
  }

  uint8_t dnum = 0;
  if (hasLast) {
    int dn = data.NUM - lastNum;
    if (dn < 0) dn = 0;
    if (dn > 255) dn = 255;
    dnum = (uint8_t)dn;
  }

  uint8_t buf[6];
  rfc2Pack(sod, (int8_t)tempI, (uint8_t)humI, rfcMaskFromWifi(data), dnum, buf);

  File f = SD.open(filename, FILE_APPEND);
  if (f) {
    size_t w = f.write(buf, 6);
    f.close();
    if (w == 6) {
      hasLast = true;
      lastNum = data.NUM;
      DEBUG_PRINTF("[RFC] Appended %s (NUM=%d, %dC, %d%%)\n",
                   filename, data.NUM, tempI, humI);
    } else {
      DEBUG_PRINTLN("[RFC] Short write!");
    }
  } else {
    DEBUG_PRINTLN("[RFC] Critical: Could not append record!");
  }
}

// تسک اصلی وای‌فای با ساختار سوییچ-کیس
void TaskInternalWiFiConnection(void *pvParameters) {
  // انتظار برای بوت اولیه
  xEventGroupWaitBits(xDoorEvents, BIT_NETWORK_BOOT_COMPLETE, pdFALSE, pdTRUE, portMAX_DELAY);

  WifiData q;
  WiFiServer debugServer(80);  // سرور برای حالت هات‌اسپات
  bool serverStarted = false;

  for (;;) {
    // 1. تشخیص مود کاری بر اساس بیت BIT_REQUEST_AP_DATA_VIEW
    // اگر بیت ست شده باشد (1) -> هات‌اسپات | اگر نباشد (0) -> کلاینت
    int currentMode = (xEventGroupGetBits(xDoorEvents) & BIT_REQUEST_AP_DATA_VIEW)
                        ? MODE_HOTSPOT_VIEW
                        : MODE_CLIENT_UPLOAD;

    switch (currentMode) {

      // ============================================================
      // CASE 0: حالت کلاینت (رفتار قدیمی: ذخیره، ارسال، حذف)
      // ============================================================
      case MODE_CLIENT_UPLOAD:
        {
          // خاموش کردن سرور دیباگ اگر روشن مانده باشد
          if (serverStarted) {
            debugServer.stop();
            serverStarted = false;
          }

          // شرط حیاتی: انتظار برای مجوز رله (چون در این مود نباید تداخل ایجاد کند)
          xEventGroupWaitBits(xDoorEvents, BIT_WIFI_PERMIT, pdFALSE, pdTRUE, portMAX_DELAY);

          // الف) ذخیره دیتای جدید در SD
          while (xQueueReceive(xDataQueue, &q, pdMS_TO_TICKS(10)) == pdPASS) {
            if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
              saveToSD(q);
              xSemaphoreGive(xSDMutex);
            }
          }

          // ب) آپلود و حذف (Store and Forward) — اولویت با فایل‌های فشرده روزانه
          if (WiFi.status() == WL_CONNECTED) {
            if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(50))) {
              File root = SD.open("/data");
              if (root) {
                File file = root.openNextFile();
                if (file) {
                  String fPath = String(file.name());
                  if (!fPath.startsWith("/")) fPath = "/data/" + fPath;
                  file.close();

                  if (fPath.endsWith(".rfc")) {
                    // فایل فشرده روزانه: همه رکوردها روی یک اتصال، سپس حذف
                    uploadRfcToServer(fPath);
                  } else if (fPath.endsWith(".dat")) {
                    File lf = SD.open(fPath, FILE_READ);
                    if (lf) {
                      uploadLegacyDatToServer(fPath, lf);
                    }
                  }
                }
                root.close();
              }
              xSemaphoreGive(xSDMutex);
            }
          }
          break;
        }

      // ============================================================
      // CASE 1: حالت هات‌اسپات (فقط خواندن و ارسال، بدون حذف)
      // ============================================================
      case MODE_HOTSPOT_VIEW:
        {
          // در این مود نیازی به BIT_WIFI_PERMIT نیست چون تسک رله کلاً غیرفعال است (Dead)

          // راه‌اندازی سرور اگر بار اول است
          if (!serverStarted) {
            debugServer.begin();
            serverStarted = true;
            DEBUG_PRINTLN("[HOTSPOT] Debug Server Started on Port 80");
          }

          WiFiClient remoteClient = debugServer.available();
          if (remoteClient) {
            DEBUG_PRINTLN("[HOTSPOT] User Connected.");
            while (remoteClient.connected()) {
              if (remoteClient.available()) {
                String cmd = remoteClient.readStringUntil('\n');
                cmd.trim();  // حذف فاصله و اینتر

                // --- دستور ۱: آخرین دیتا (sync) ---
                if (cmd.equalsIgnoreCase("sync")) {
                  DEBUG_PRINTLN("[CMD] Sync Last Requested.");
                  if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                    // اولویت: جدیدترین فایل .rfc سپس .dat قدیمی
                    File root = SD.open("/data");
                    String lastFile = "";
                    String bestName = "";
                    File entry = root.openNextFile();
                    while (entry) {
                      String fn = String(entry.name());
                      bool isRfc = fn.endsWith(".rfc");
                      bool isDat = fn.endsWith(".dat");
                      if (isRfc || isDat) {
                        // ترتیب الفبایی: YYYYMMDD.rfc یا YYYYMMDD_ID.dat
                        if (fn > bestName) {
                          bestName = fn;
                          lastFile = fn.startsWith("/") ? fn : ("/data/" + fn);
                        }
                      }
                      entry.close();
                      entry = root.openNextFile();
                    }
                    root.close();

                    if (lastFile != "") {
                      sendLastRecordOnly(remoteClient, lastFile);
                    } else {
                      remoteClient.println("NO_DATA");
                    }
                    xSemaphoreGive(xSDMutex);
                  }
                }

                // --- دستور ۲: ۱۰ دیتای آخر (sync10) ---
                else if (cmd.equalsIgnoreCase("sync10")) {
                  DEBUG_PRINTLN("[CMD] Sync 10 Requested.");
                  if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(1000))) {
                    File root = SD.open("/data");

                    String last10[10];
                    int count = 0;

                    File entry = root.openNextFile();
                    while (entry) {
                      String fn = String(entry.name());
                      bool ok = fn.endsWith(".dat") || fn.endsWith(".rfc");
                      if (ok) {
                        String full = fn.startsWith("/") ? fn : ("/data/" + fn);
                        last10[count % 10] = full;
                        count++;
                      }
                      entry.close();
                      entry = root.openNextFile();
                    }
                    root.close();

                    // ارسال ۱۰ مورد (یا کمتر) — هر فایل ممکن است چند رکورد داشته باشد
                    int start = (count > 10) ? (count % 10) : 0;
                    int itemsToSend = (count > 10) ? 10 : count;

                    remoteClient.println("[");  // شروع آرایه JSON
                    for (int i = 0; i < itemsToSend; i++) {
                      int idx = (start + i) % 10;
                      if (last10[idx].length() > 0) {
                        // حداکثر ۱۰ رکورد کل: از هر فایل همه رکوردها (فرمت خطی)
                        sendDataFile(remoteClient, last10[idx]);
                        if (i < itemsToSend - 1) remoteClient.print(",\n");
                      }
                    }
                    remoteClient.println("]");  // پایان آرایه
                    xSemaphoreGive(xSDMutex);
                  }
                }
              }
            }
            remoteClient.stop();
            DEBUG_PRINTLN("[HOTSPOT] User Disconnected.");
          }
          break;
        }
    }

    // تاخیر کلی برای جلوگیری از درگیری CPU
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void loop() {
  vTaskDelete(NULL);
}