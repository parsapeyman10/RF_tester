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
void TaskRelayControl(void *pvParameters);
void TaskReadSHT(void *pvParameters);
void TaskDigitalRead(void *pvParameters);
void TaskInternalWiFiConnection(void *pvParameters);

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

// تابع کمکی برای ارسال محتویات یک فایل دیتای خاص به کلاینت متصل
void sendDataFile(WiFiClient &cl, String filePath) {
  File f = SD.open(filePath, FILE_READ);
  if (f) {
    WifiData d;
    if (f.read((uint8_t *)&d, sizeof(WifiData)) == sizeof(WifiData)) {
      char buf[200];
      // فرمت خروجی JSON برای اپلیکیشن
      snprintf(buf, sizeof(buf),
               "{\"ID\":%d,\"T\":%.2f,\"H\":%.2f,\"N1\":%d,\"N2\":%d,\"Time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
               d.NUM, d.Temp, d.Hum, d.NBCM1, d.NBCM2,
               d.Year, d.Month, d.Day, d.Hour, d.Minute, d.Second);
      cl.println(buf);
    }
    f.close();
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

          // ب) آپلود و حذف (Store and Forward)
          if (WiFi.status() == WL_CONNECTED) {
            if (xSemaphoreTake(xSDMutex, pdMS_TO_TICKS(50))) {
              File root = SD.open("/data");
              if (root) {
                File file = root.openNextFile();
                if (file) {
                  String fPath = "/data/" + String(file.name());
                  if (fPath.endsWith(".dat")) {
                    WifiData stored;
                    if (file.read((uint8_t *)&stored, sizeof(WifiData)) == sizeof(WifiData)) {

                      if (client.connect(serverIP, serverPort)) {
                        // مهم: این خط باید دقیقاً با فرمت sscanf سمت ESP8266 یکی باشد
                        // (13 فیلد: NUM,NBCM1..4,Temp,Humidity,Date,Time) وگرنه parseData()
                        // شکست می‌خورد، هیچ‌وقت "OK" برنمی‌گردد و رکورد از SD پاک نمی‌شود.
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

                        // انتظار برای ACK (با vTaskDelay برای رفع خطر Watchdog روی این تسک)
                        unsigned long t = millis();
                        bool ack = false;
                        while (millis() - t < 3000) {
                          if (client.available() && client.readStringUntil('\n').indexOf("OK") != -1) {
                            ack = true;
                            break;
                          }
                          vTaskDelay(pdMS_TO_TICKS(5));
                        }

                        file.close();
                        if (ack) {
                          SD.remove(fPath);  // حذف فقط در این مود انجام می‌شود
                          DEBUG_PRINTLN("[CLIENT] ACK RX. File Deleted.");
                        }
                        client.stop();
                      } else {
                        file.close();
                      }
                    } else file.close();
                  } else file.close();
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
                    // پیدا کردن آخرین فایل
                    File root = SD.open("/data");
                    String lastFile = "";
                    int maxID = -1;

                    // در C++ استاندارد ESP32 برای حلقه دایرکتوری، این روش امن‌تر است
                    File entry = root.openNextFile();
                    while (entry) {
                      String fn = entry.name();
                      if (fn.endsWith(".dat")) {
                        // استخراج ID از نام فایل (فرمت: date_ID.dat)
                        int uIdx = fn.lastIndexOf('_');
                        int id = fn.substring(uIdx + 1, fn.length() - 4).toInt();
                        if (id > maxID) {
                          maxID = id;
                          lastFile = "/data/" + fn;
                        }
                      }
                      entry.close();
                      entry = root.openNextFile();
                    }
                    root.close();

                    if (lastFile != "") {
                      sendDataFile(remoteClient, lastFile);
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
                      String fn = "/data/" + String(entry.name());
                      if (fn.endsWith(".dat")) {
                        last10[count % 10] = fn;  // بافر چرخشی
                        count++;
                      }
                      entry.close();
                      entry = root.openNextFile();
                    }
                    root.close();

                    // ارسال ۱۰ مورد (یا کمتر)
                    int start = (count > 10) ? (count % 10) : 0;
                    int itemsToSend = (count > 10) ? 10 : count;

                    remoteClient.println("[");  // شروع آرایه JSON
                    for (int i = 0; i < itemsToSend; i++) {
                      int idx = (start + i) % 10;
                      if (last10[idx].length() > 0) {
                        sendDataFile(remoteClient, last10[idx]);
                        if (i < itemsToSend - 1) remoteClient.print(",");  // جداکننده
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

void saveToSD(const WifiData &data) {
  char filename[32];
  sprintf(filename, "/data/%04d%02d%02d_%d.dat", data.Year, data.Month, data.Day, data.NUM);
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    f.write((const uint8_t *)&data, sizeof(WifiData));
    f.close();
    DEBUG_PRINTF("[SD] Logged %s\n", filename);
  } else {
    DEBUG_PRINTLN("[SD] Critical: Could not write file!");
  }
}

void loop() {
  vTaskDelete(NULL);
}