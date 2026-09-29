# RF_tester

سیستم تست خودکار درب‌های BCM با ESP32 + ESP8266 و سرور ثبت داده.

```
ESP32 (تست رله‌ها، SD، RTC) ──WiFi──► ESP8266 (گیرنده) ──USB/Serial──► app.py ──► SQLite
```

## ساختار پروژه

```
esp32_controller_FIXED/     ← فلش روی ESP32  (رله‌ها، SHT31، RTC، کارت SD)
esp8266_receiver_FIXED/     ← فلش روی ESP8266 (اکسس‌پوینت گیرنده، به USB کامپیوتر)

app.py                      ← سرور
requirements.txt            ← پیش‌نیازهای پایتون
run_server.bat              ← اجرای یک‌کلیکی روی ویندوز
run_server.sh               ← اجرا روی لینوکس/مک
templates/                  ← صفحات وب (index، history، plot_display، upload)

docs/SERVER_SETUP.md        ← راه‌اندازی سرور
docs/FIELD_TEST.md          ← چک‌لیست تست روی سخت‌افزار
.github/scripts/            ← تست‌های خودکار (اختیاری، برای توسعه)
```

فایل‌هایی که خودکار ساخته می‌شوند: `instance/master_industrial.db`،
`YYYY-MM-DD.db`، `.venv/`، و در صورت خطا `unsaved_records.log`.

## شروع سریع

1. `esp8266_receiver_FIXED.ino` را روی ESP8266 فلش و با USB به کامپیوتر وصل کنید.
2. `esp32_controller_FIXED.ino` را روی ESP32 فلش کنید.
3. با گوشی به وای‌فای `SetClock` (رمز `12345678`) وصل شوید و `http://192.168.1.1`
   را باز کنید؛ ساعت خودکار تنظیم می‌شود. «شروع کار عادی» را بزنید.
4. روی کامپیوتر `run_server.bat` را اجرا و در داشبورد پورت COM را انتخاب کنید.

## فرمت داده

هر سیکل یک رکورد با چهار نتیجه‌ی تفکیکی تولید می‌کند:

```
NUM=1178,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=NOK,
Temp=24.50,Humidity=40.00,Date=2026-09-29,Time=11:00:00
```

همین یک فرمت، هم روی سریال به سرور می‌رسد و هم در حالت دیتا به هر کلاینتی
داده می‌شود. مرجع زمان، RTC خودِ دستگاه است.

## فرمان‌های سریال ESP32

| فرمان | کار |
|---|---|
| `STATUS` | وضعیت لحظه‌ای (مرحله، وای‌فای، شماره‌ی رکورد بعدی، حافظه) |
| `FORMAT SD` | پاک‌سازی کامل کارت حافظه |

## تنظیمات مهم بالای اسکچ ESP32

```cpp
#define LOG_LEVEL 1                 // 0=کمینه ، 1=معمولی ، 2=عیب‌یابی کامل
const uint8_t RELAY_PINS[2] = { 2, 4 };          // رله باز کردن / بستن
const uint8_t FEEDBACK_PINS[2][2] = {
  { 13, 16 },   // فیدبک «باز شد» : BCM1 , BCM2
  { 15, 17 },   // فیدبک «بسته شد»: BCM1 , BCM2
};
const uint32_t CYCLE_PERIOD_MS = 120000;         // فاصله‌ی سیکل‌ها
```
